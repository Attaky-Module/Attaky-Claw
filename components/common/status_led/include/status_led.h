#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Onboard RGB LED status driver.
 *
 * Drives the Agent Deck 1.0 onboard active-low common-anode RGB LED on
 * AW9523B @0x59 (P10=Red, P11=Green, P12=Blue) so its colour matches
 * the on-screen status circle's resolved state, polled from the
 * committed status_state aggregator (single source of truth, lock-free
 * getter; the aggregator's single change-callback slot is owned by the
 * emote screen glue per Task 4.3a). The driver does NOT call
 * status_state_on_change(); it polls status_state_resolved() on a
 * 100 ms timer and only pushes I2C writes when the resolved state
 * actually changes.
 *
 * SCOPE TODAY: per-state COLOUR only. The original plan (Task 4.3b)
 * also called for brightness breathing on the LED, matching the
 * screen's envelope; on real hardware the AW9523B LED constant-current
 * mode did not produce a visible breathing response from this board so
 * that path is deferred. The screen status circle remains the
 * breathing-amplitude carrier of the status cue. See status_led.c
 * "AW9523B path notes" for the full bring-up rationale and the
 * follow-up gap.
 *
 * STRICT NO-BRICK: every failure path (expander handle absent, any I2C
 * / set_level error, timer allocation failure) only logs a warning and
 * permanently disables the LED feature. It never blocks boot, never
 * aborts, and never touches any AW9523B register other than the OUTPUT
 * level for P10/P11/P12 -- the same call path the buttons / CTP_RESET
 * already use. The screen, buttons, and touch (CTP_RESET = P13) are
 * never disturbed.
 */

/* Start the onboard-LED status driver. Idempotent: a second call after
 * a successful start is a no-op and returns ESP_OK. Must be called
 * after the board manager has initialised "gpio_expander_io"; the
 * existing main.c bring-up site (right after select_button_init)
 * satisfies that. ALWAYS returns ESP_OK -- any failure logs and
 * silently disables the LED feature; callers should not branch on the
 * return value. */
esp_err_t status_led_start(void);

/* Stop the driver and drive the LED off. Safe to call when never
 * started or already stopped. Provided for completeness / future
 * teardown; not on the boot path and currently has no caller.
 *
 * CALLING-CONTEXT CONSTRAINT: must NOT be called concurrently with a
 * live poll timer from a task other than the esp_timer task. It calls
 * esp_timer_stop()/esp_timer_delete(), which are undefined while the
 * tick callback is executing on the esp_timer task; there is no
 * in-flight-callback fence here. The in-tick no-brick disable path is
 * self-safe (same task); an external teardown caller must first ensure
 * the timer is idle before relying on this. */
void status_led_stop(void);

#ifdef __cplusplus
}
#endif
