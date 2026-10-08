#pragma once

// What the status LED says. How it says it comes from the status_led field of boards/<BOARD>/board.yml: a colour on
// a neopixel, blinking on a plain gpio LED, nothing on a board without an LED.
typedef enum {
    STATUS_LED_RECOVERY,   // the board runs the recovery image
    STATUS_LED_LEAVING,    // the board is about to boot the main firmware: dark
} status_led_state_t;

// Shows the state; the first call sets the LED up. Failures are logged and ignored: the LED is a hint, never a
// reason to stop the portal.
void status_led_show(status_led_state_t state);
