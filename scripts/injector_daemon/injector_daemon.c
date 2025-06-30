//_GNU_SOURCE to enable strndup() and other extensions
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <microhttpd.h>

// program statics.
#define PORT 8080
#define KERNEL_DEVICE_PATH "/dev/hid_injector"
#define GPIO_PIN 21
#define BATCH_SIZE 20
#define DEBOUNCE_DELAY_MS 250 // Debounce delay in milliseconds

// --- Global State ---
char *g_staged_payload = NULL;
pthread_mutex_t g_payload_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int keep_running = 1;

// --- Forward Declarations ---
void* web_server_thread_func(void *arg);
int perform_injection(void);

// --- GPIO & System Setup ---
int initialize_gpio(int pin) {
    char command[256];

    // use python to set the pull up resistor in GPIO
    // this is the most reliable method, pure C and pure bash methods have caused issues.
    // I think rpi does some magic under the hood, so no choice, we have to use python here.
    printf("Setting GPIO %d bias to 'pull-up' via Python...\n", pin);
    sprintf(command, "python3 -c \"import RPi.GPIO as GPIO; GPIO.setmode(GPIO.BCM); GPIO.setup(%d, GPIO.IN, pull_up_down=GPIO.PUD_UP)\"", pin);
    
    if (system(command) != 0) {
        fprintf(stderr, "Warning: Python script for pull-up may have failed. check your logs.\n");
    }

    return 0;
}

// leave the gpio in the state we found it, good practice.
void cleanup_gpio(int pin) {
    char command[256];
    int fd;
    
    printf("\nCleaning up GPIO %d...\n", pin);
    sprintf(command, "/sys/class/gpio/gpio%d", pin);
    if (access(command, F_OK) == 0) {
        fd = open("/sys/class/gpio/unexport", O_WRONLY);
        if (fd < 0) {
            perror("Failed to open GPIO unexport");
            return;
        }
        sprintf(command, "%d", pin);
        write(fd, command, strlen(command));
        close(fd);
    }
}

// simple web server state struct
struct PostRequestState {
    char *data;
    size_t size;
};

// Web server component.
enum MHD_Result post_handler(void *cls, struct MHD_Connection *connection,
                          const char *url, const char *method,
                          const char *version, const char *upload_data,
                          size_t *upload_data_size, void **con_cls) {
    // Silence unused parameter warnings
    (void)cls; (void)url; (void)version;

    // We only accept POST requests
    if (0 != strcmp(method, "POST")) {
        return MHD_NO;
    }

    // On the first call for a connection, set up our state structure
    // post_handler gets called multiple times, with chunks of data, this allows it to have basic protection against DOS attacks.
    if (*con_cls == NULL) {
        struct PostRequestState *request_state = malloc(sizeof(struct PostRequestState));
        if (request_state == NULL) {
            return MHD_NO; // Internal server error
        }
        request_state->data = NULL;
        request_state->size = 0;
        *con_cls = (void *)request_state;
        return MHD_YES;
    }

    struct PostRequestState *request_state = *con_cls;

    // If libmicrohttpd is giving us data, accumulate it.
    if (*upload_data_size > 0) {
        // Reallocate buffer to hold the new chunk of data
        char *new_data = realloc(request_state->data, request_state->size + *upload_data_size);

        // if the signal has not been sent to terminate, and a null is received, this is improper, handle it.
        if (new_data == NULL) {
            return MHD_NO;
        }
        request_state->data = new_data;

        // Copy the new data chunk to the end of our buffer
        memcpy(&request_state->data[request_state->size], upload_data, *upload_data_size);
        request_state->size += *upload_data_size;

        // Tell libmicrohttpd that we have processed this chunk
        *upload_data_size = 0;
        return MHD_YES;
    }

    // termination signal is when request state data is null. handle it and give a response.
    if (request_state->data != NULL) {
        pthread_mutex_lock(&g_payload_mutex);
        
        // Free any previously staged payload
        free(g_staged_payload);
        
        // copy the staged data over using strndup (safe).
        g_staged_payload = strndup(request_state->data, request_state->size);
        
        pthread_mutex_unlock(&g_payload_mutex);

        if (g_staged_payload != NULL) {
             printf("Web server received new payload (%zu bytes): %s\n", 
                    strlen(g_staged_payload), g_staged_payload);
        }
    }

    // finally, send the response.
    const char *page = "<html><body>Payload staged for next injection.</body></html>";
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(page), (void *)page, MHD_RESPMEM_PERSISTENT);
    int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    // --- Clean up the connection state ---
    if (request_state != NULL) {
        free(request_state->data);
        free(request_state);
    }
    *con_cls = NULL;

    return ret;
}

// simple function that starts the web server.
void* web_server_thread_func(void *arg) {
    (void)arg;

    struct MHD_Daemon *daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, PORT, NULL, NULL,
                                                &post_handler, NULL, MHD_OPTION_END);
    if (NULL == daemon) {
        fprintf(stderr, "Failed to start web server daemon.\n");
        return NULL;
    }
    printf("Web server started on port %d.\n", PORT);

    while (keep_running) {
        sleep(1);
    }

    MHD_stop_daemon(daemon);
    printf("Web server stopped.\n");
    return NULL;
}

// --- Injection Component ---
int perform_injection(void) {
    printf("inject!!\n");
    char *payload_to_inject = NULL;
    int ret = 0;

    // keep a mutex lock on the resource, we signal to the rest of the program that we are injecting.
    pthread_mutex_lock(&g_payload_mutex);
    if (g_staged_payload != NULL) {
        payload_to_inject = g_staged_payload;
        g_staged_payload = NULL;
    }
    pthread_mutex_unlock(&g_payload_mutex);

    if (payload_to_inject == NULL) {
        printf("Injection triggered, but no payload is staged.\n");
        return 0;
    }

    printf("--- Starting injection of payload: \"%s\" ---\n", payload_to_inject);
    
    int fd = open(KERNEL_DEVICE_PATH, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open kernel device for injection");
        free(payload_to_inject);
        return -1;
    }

    size_t total_len = strlen(payload_to_inject);
    size_t offset = 0;

    while (offset < total_len) {
        // select the batch size based on ternary, do we have plenty left or less than batch size left to inject?
        size_t batch = (total_len - offset > BATCH_SIZE) ? BATCH_SIZE : (total_len - offset);
        // perform the write.
        ssize_t written = write(fd, payload_to_inject + offset, batch);
        
        // error check.
        if (written < 0) {
            perror("Kernel module write error during injection");
            ret = -1;
            break;
        }
        offset += written;
    }

    close(fd);
    free(payload_to_inject);

    if (ret == 0) {
        printf("--- Injection finished successfully. ---\n");
    } else {
        printf("--- Injection failed. ---\n");
    }
    
    return ret;
}


// set the handler for SIG termination.
void int_handler(int dummy) {
    (void)dummy;
    keep_running = 0;
}

int main() {
    pthread_t web_server_thread;
    struct pollfd pfd;
    char gpio_path[64];
    char val;

    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);

    printf("--- C Injector Daemon Initializing ---\n");

    // general GPIO operations. If we cannot access the GPIO, return immediately.
    if (initialize_gpio(GPIO_PIN) != 0) {
        fprintf(stderr, "Failed to initialize GPIO. Exiting.\n");
        return 1;
    }

    sprintf(gpio_path, "/sys/class/gpio/gpio%d/value", GPIO_PIN);
    pfd.fd = open(gpio_path, O_RDONLY);
    if (pfd.fd < 0) {
        perror("Failed to open GPIO value file");
        cleanup_gpio(GPIO_PIN);
        return 1;
    }
    pfd.events = POLLPRI | POLLERR;

    if (pthread_create(&web_server_thread, NULL, web_server_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create web server thread.\n");
        close(pfd.fd);
        cleanup_gpio(GPIO_PIN);
        return 1;
    }

    printf("--- System Ready. Waiting for GPIO trigger on pin %d. Press Ctrl+C to exit. ---\n", GPIO_PIN);

    while (keep_running) {
        // read to clear the initial state before polling
        lseek(pfd.fd, 0, SEEK_SET);
        read(pfd.fd, &val, 1);

        //block until a GPIO event (interrupt) occurs
        int poll_ret = poll(&pfd, 1, -1);
        
        if (!keep_running) break;

        if (poll_ret > 0) {
            if (pfd.revents & (POLLPRI | POLLERR)) {
                lseek(pfd.fd, 0, SEEK_SET);
                read(pfd.fd, &val, 1);

                // when the signal is low, it means the button was pressed, execute.
                if (val == '0') {
                    perform_injection();
                    
                    printf("\n--- Injection complete. Debouncing for %dms... ---\n", DEBOUNCE_DELAY_MS);
                    // button debouncing delay
                    usleep(DEBOUNCE_DELAY_MS * 1000);
                    
                    printf("--- Re-arming. Waiting for next GPIO trigger... ---\n");
                }
                // if val is '1', it's a release event, so we do nothing.
            }
        }
    }
    
    // gracefully shutdown the program.
    printf("\nShutting down...\n");
    
    pthread_cancel(web_server_thread);
    pthread_join(web_server_thread, NULL);
    
    close(pfd.fd);
    cleanup_gpio(GPIO_PIN);
    free(g_staged_payload);
    
    printf("Shutdown complete.\n");
    return 0;
}
