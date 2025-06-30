#!/bin/bash

# controls how many characters to read at once.
CHUNK_SIZE=20
current_dir=$(pwd)
PAYLOAD="${current_dir}/test_payload.txt"

inject() {
    echo "injecting... $PAYLOAD"
    while IFS= read -r -d '' -n "$CHUNK_SIZE" chunk || [[ -n "$chunk" ]]; do
        echo -n "$chunk" > /dev/hid_injector
    done < "$PAYLOAD"
}

GPIO_PIN=21

# this function resets the pin state to the default state.
# not really required, but good to do.
cleanup() {
    echo -e "\nCtrl+C caught. Cleaning up and shutting down..."
    
    # Kill the backgrounded main logic process
    if kill -0 $CHILD_PID 2>/dev/null; then
        kill $CHILD_PID
    fi
    
    # set the pin back to its default state (input, no pull). using RPI GPIO and pinctrl
    echo "Resetting GPIO $GPIO_PIN state to default (input, no pull)."
    pinctrl set $GPIO_PIN pn
    python3 -c "import RPi.GPIO as GPIO; GPIO.cleanup()"
    
    exit 0
}

#Contains the main loop of the program.
main_loop() {
    while true; do
        echo "Waiting for button press..."
        gpiomon --num-events=1 --falling-edge gpiochip0 $GPIO_PIN > /dev/null
        
        echo "Button pressed! Injecting payload..."
        inject
        echo "Payload injected. Waiting for release..."
        
        gpiomon --num-events=1 --rising-edge gpiochip0 $GPIO_PIN > /dev/null
        echo "Button released. Re-arming."
        echo "----------------------------------------"
    done
}

# set the trap to catch the ctrl-c
trap cleanup INT

echo "--- Definitive Bash Button Trigger ---"
echo "Arming trigger. Press Ctrl+C to exit."

# Set the pin BIAS to pull-up using RPI.GPIO
# Unfortunately, after many tries in pure bash, RPI.GPIO python is the only one that can setup properly.
# my hypothesis is that RPI.GPIO performs a lot of "magic" in the background that we cannot do reliably in shell.
echo "Setting GPIO $GPIO_PIN bias to 'pull-up'..."
python3 -c "import RPi.GPIO as GPIO; GPIO.setmode(GPIO.BCM); GPIO.setup($GPIO_PIN, GPIO.IN, pull_up_down=GPIO.PUD_UP)"

echo "----------------------------------------"

# run the main loop in a separate child process and get the child's process id.
main_loop &
CHILD_PID=$!

# wait for the child process' termination.
wait $CHILD_PID