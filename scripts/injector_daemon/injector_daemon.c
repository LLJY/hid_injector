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

#define PORT 8080
#define KERNEL_DEVICE_PATH "/dev/hid_injector"
#define GPIO_PIN 21
#define BATCH_SIZE 16 // How many chars to write to the driver at a time

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

    // Use the reliable Python script method to set the pull-up resistor
    printf("Setting GPIO %d bias to 'pull-up' via Python...\n", pin);
    sprintf(command, "python3 -c \"import RPi.GPIO as GPIO; GPIO.setmode(GPIO.BCM); GPIO.setup(%d, GPIO.IN, pull_up_down=GPIO.PUD_UP)\"", pin);
    return system(command);
}

// --- Web Server Component ---
enum MHD_Result post_handler(void *cls, struct MHD_Connection *connection,
                          const char *url, const char *method,
                          const char *version, const char *upload_data,
                          size_t *upload_data_size, void **con_cls) {
    if (strcmp(method, "POST") != 0) {
        return MHD_NO;
    }

    // This callback gets called once with data, and once more with size 0.
    // We only care about the call that has data.
    if (*upload_data_size != 0) {
        pthread_mutex_lock(&g_payload_mutex);
        free(g_staged_payload);
        g_staged_payload = strndup(upload_data, *upload_data_size);
        pthread_mutex_unlock(&g_payload_mutex);
        
        printf("Web server received new payload: %s\n", g_staged_payload);
        *upload_data_size = 0; // Signal that we've processed the data
        return MHD_YES;
    }

    const char *page = "<html><body>Payload staged for next injection.</body></html>";
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(page), (void *)page, MHD_RESPMEM_PERSISTENT);
    int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}

void* web_server_thread_func(void *arg) {
    struct MHD_Daemon *daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, PORT, NULL, NULL,
                                                &post_handler, NULL, MHD_OPTION_END);
    if (NULL == daemon) {
        fprintf(stderr, "Failed to start web server daemon.\n");
        return NULL;
    }
    printf("Web server started on port %d.\n", PORT);

    // The daemon runs until it's stopped. We can just let this thread wait here.
    while (keep_running) {
        sleep(1);
    }

    MHD_stop_daemon(daemon);
    printf("Web server stopped.\n");
    return NULL;
}

// --- Injection Component ---
int perform_injection() {
    char *payload_to_inject = NULL;
    int ret = 0;

    // Atomically get the payload and clear the global one
    pthread_mutex_lock(&g_payload_mutex);
    if (g_staged_payload != NULL) {
        payload_to_inject = g_staged_payload;
        g_staged_payload = NULL; // We've taken ownership
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
        size_t batch = (total_len - offset > BATCH_SIZE) ? BATCH_SIZE : (total_len - offset);
        ssize_t written = write(fd, payload_to_inject + offset, batch);
        
        if (written < 0) {
            perror("Kernel module write error during injection");
            ret = -1;
            break;
        }
        offset += written;
        usleep(50000); // 50ms delay between batches
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


// --- Main Program Logic ---
void int_handler(int dummy) {
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

    // Initialize GPIO
    if (initialize_gpio(GPIO_PIN) != 0) {
        fprintf(stderr, "Failed to initialize GPIO. Exiting.\n");
        return 1;
    }

    // Open GPIO value file for polling
    sprintf(gpio_path, "/sys/class/gpio/gpio%d/value", GPIO_PIN);
    pfd.fd = open(gpio_path, O_RDONLY);
    if (pfd.fd < 0) {
        perror("Failed to open GPIO value file");
        return 1;
    }
    pfd.events = POLLPRI | POLLERR;

    // Start the web server thread
    if (pthread_create(&web_server_thread, NULL, web_server_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create web server thread.\n");
        close(pfd.fd);
        return 1;
    }

    printf("--- System Ready. Waiting for GPIO trigger on pin %d. Press Ctrl+C to exit. ---\n", GPIO_PIN);

    while (keep_running) {
        // Dummy read to clear initial state
        lseek(pfd.fd, 0, SEEK_SET);
        read(pfd.fd, &val, 1);

        // Block here until a GPIO event (button press) occurs
        int poll_ret = poll(&pfd, 1, -1); // -1 means wait indefinitely
        
        if (!keep_running) break;

        if (poll_ret > 0) {
            if (pfd.revents & (POLLPRI | POLLERR)) {
                // Event occurred, perform the injection synchronously
                // perform_injection();
                printf("\n--- Injection complete. Re-arming. Waiting for next GPIO trigger... ---\n");
            }
        }
    }
    
    printf("\nShutting down...\n");
    
    // Signal web server thread to stop and clean up
    pthread_cancel(web_server_thread);
    pthread_join(web_server_thread, NULL);
    
    close(pfd.fd);
    free(g_staged_payload);
    
    printf("Shutdown complete.\n");
    return 0;
}