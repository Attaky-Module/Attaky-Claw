#include "status_led_logic.h"

/* Per-state primary-colour GPIO mapping for the onboard RGB LED. See
 * status_led_logic.h for the contract; this file is pure, no I2C/expander
 * deps, host-testable, and exercised by test_host/test_status_led_logic.c. */
status_led_bits_t status_led_bits_for_state(ss_state_t state)
{
    switch (state) {
    case SS_STATE_RECORDING:
        return (status_led_bits_t){ .r = false, .g = true,  .b = false };
    case SS_STATE_SPEAKING:
        return (status_led_bits_t){ .r = true,  .g = true,  .b = false };
    case SS_STATE_NOTIFICATION:
        return (status_led_bits_t){ .r = true,  .g = false, .b = false };
    case SS_STATE_NORMAL:
    default:
        /* No-brick: any unexpected value falls back to NORMAL/blue. */
        return (status_led_bits_t){ .r = false, .g = false, .b = true  };
    }
}
