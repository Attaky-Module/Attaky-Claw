#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "status_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-channel ON/OFF for the onboard active-low common-anode RGB LED.
 * The AW9523B drives P10/P11/P12 in plain GPIO mode (LOW = LED on); the
 * chip's LED constant-current mode is intentionally not used today (see
 * status_led.c known-limitations -- this path is the safe, reliable
 * route that piggybacks on the same io_expander API the buttons and
 * CTP_RESET already use). Each channel is binary: brightness breathing
 * on the LED is a documented follow-up gap; the screen status circle
 * is the breathing-amplitude carrier of the status cue. */
typedef struct {
    bool r;   /* P10 -- LED on when true (driven LOW on the wire)  */
    bool g;   /* P11 -- ditto                                       */
    bool b;   /* P12 -- ditto                                       */
} status_led_bits_t;

/* Map a resolved status state to the GPIO ON/OFF triplet. The mapping
 * uses logical PRIMARY colours (one or two channels on at full) -- this
 * is what a 3-bit GPIO RGB can faithfully reproduce, and it matches the
 * dominant colour cue of each on-screen status:
 *
 *   NORMAL       -> blue   (B on, R/G off)
 *   RECORDING    -> green  (G on, R/B off)
 *   SPEAKING     -> yellow (R+G on, B off)
 *   NOTIFICATION -> red    (R on, G/B off)
 *
 * Out-of-range states fall back to NORMAL/blue (no-brick parity with
 * the rest of the status pipeline). */
status_led_bits_t status_led_bits_for_state(ss_state_t state);

#ifdef __cplusplus
}
#endif
