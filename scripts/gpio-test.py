import RPi.GPIO as GPIO
import time

# Use BCM pin numbers, which refers to the "GPIO xx" name.
# As per the diagram, physical pin 40 is GPIO 21.
BUTTON_PIN = 21

# --- GPIO Setup ---
# Use BCM pin numbering scheme.
GPIO.setmode(GPIO.BCM)

# Set up the button pin as an input.
# We enable the internal pull-up resistor. This means the pin's default
# state is HIGH (3.3V). When the button is pressed, it connects the pin to
# Ground, pulling the state to LOW (0V). This is a very reliable way to
# wire a button.
GPIO.setup(BUTTON_PIN, GPIO.IN, pull_up_down=GPIO.PUD_UP)

print("GPIO Hardware Test Initialized.")
print(f"Watching GPIO Pin: {BUTTON_PIN}")
print("Press Ctrl+C to exit.")

# Store the initial state of the button.
# GPIO.input() returns 1 for HIGH (un-pressed) and 0 for LOW (pressed).
last_state = GPIO.input(BUTTON_PIN)

print(f"Initial state: {'Un-pressed (HIGH)' if last_state == 1 else 'Pressed (LOW)'}")

# --- Main Loop ---
try:
    while True:
        # Read the current state of the button
        current_state = GPIO.input(BUTTON_PIN)

        # Check if the state has changed since the last time we checked
        if current_state != last_state:
            if current_state == 0:
                print("State changed: Button has been PRESSED (state is LOW)")
            else:
                print("State changed: Button has been RELEASED (state is HIGH)")
            
            # Update the last_state to the new current_state
            last_state = current_state

        # Wait a very short time to prevent the script from using all the CPU
        time.sleep(0.05)

except KeyboardInterrupt:
    print("\nExiting test script.")

finally:
    # This part always runs, even if the script crashes or is interrupted.
    # It releases the GPIO pins back to the system.
    GPIO.cleanup()