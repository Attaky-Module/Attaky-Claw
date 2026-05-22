#include "status_led.h"
#include "status_led_logic.h"
#include "status_state.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_io_expander.h"
#include "esp_io_expander_aw9523b.h"
#include "esp_board_manager.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "status_led";

/* ---- AW9523B path notes -------------------------------------------------
 *
 * The onboard RGB LED is a 3-pin active-low COMMON-ANODE RGB on the
 * AW9523B IO expander @ 7-bit 0x59 (board device "gpio_expander_io"):
 * P10=Red, P11=Green, P12=Blue. With the driver's linear pin numbering
 * P10..P17 are bits 8..15, so P10/P11/P12 = bits 8/9/10. P13 (bit 11)
 * is CTP_RESET and is intentionally not touched here. board_devices.yaml
 * already drives P10..P13 as plain GPIO outputs (output_io_mask
 * [8,9,10,11], default level HIGH = LED off, active-low).
 *
 * This driver uses the AW9523B's GPIO mode for the LED pins, NOT the
 * chip's LED constant-current mode. Bring-up on real hardware showed
 * that switching P10/P11/P12 into LED mode (LEDMODE1 = 0xF8 with
 * datasheet-implied 0=LED polarity) did NOT produce a visible breathing
 * response on this board, while the GPIO active-low path lights each
 * channel reliably -- proven by a wiring probe that drove P10 LOW only
 * and produced pure red on the LED (matching the documented P10=R wiring). The LED-mode
 * path is intentionally deferred; this GPIO route piggybacks on the
 * same esp_io_expander API the buttons and CTP_RESET already use, so it
 * inherits the chip-driver's serialization and is known-good.
 *
 * KNOWN GAP / FOLLOW-UP: this gives correct PER-STATE COLOUR but no
 * brightness breathing on the LED (GPIO is binary). The screen status
 * circle remains the breathing-amplitude carrier of the status cue.
 * Restoring on-LED brightness-breathing needs a fresh AW9523B LED-mode
 * investigation on this specific board (DIM register map, LEDMODE bit
 * polarity behaviour, and possibly the physical LED + resistor wiring
 * details).
 *
 * STRICT NO-BRICK: every failure path (expander handle absent, any
 * set_level error) only logs a warning and permanently disables the LED
 * feature. It never blocks boot, never aborts, never affects screen /
 * buttons / touch. Only P10/P11/P12 outputs are ever driven; LEDMODE
 * registers, DIM registers, GCR, P0-port, and P13..P17 are NEVER
 * written.
 */

/* Linear IO_EXPANDER_PIN_NUM bit positions per
 * boards/.../board_devices.yaml (P10..P17 = bits 8..15). */
#define STATUS_LED_PIN_R   (1u << 8)   /* P10 = Red   */
#define STATUS_LED_PIN_G   (1u << 9)   /* P11 = Green */
#define STATUS_LED_PIN_B   (1u << 10)  /* P12 = Blue  */

/* Poll cadence for status_state_resolved(). The LED is binary per
 * channel, so we only push I2C writes when the resolved state actually
 * changes -- a 100 ms tick is plenty given how rarely status changes,
 * and is well under any human-noticeable response delay. */
#define STATUS_LED_TICK_PERIOD_US (100 * 1000)

typedef struct {
    bool                      started;  /* idempotent start guard      */
    bool                      enabled;  /* false = feature disabled    */
    esp_io_expander_handle_t  exp;      /* shared expander handle      */
    esp_timer_handle_t        timer;    /* periodic poll timer         */
    ss_state_t                last_state;
    bool                      have_last;
} status_led_ctx_t;

static status_led_ctx_t s_ctx;

/* Drive one channel high or low via the standard io_expander API; the
 * AW9523B driver maintains its own OUTPUT-register shadow and serializes
 * on the i2c_master bus, so this is the same call path the button and
 * CTP_RESET paths already use successfully. */
static esp_err_t led_set_channel(uint32_t pin_mask, bool on)
{
    /* Active-low: LED ON = drive LOW (level 0). */
    return esp_io_expander_set_level(s_ctx.exp, pin_mask, on ? 0 : 1);
}

static void led_disable(const char *why, esp_err_t err)
{
    ESP_LOGW(TAG, "onboard LED disabled: %s (%s)", why, esp_err_to_name(err));
    s_ctx.enabled = false;
    if (s_ctx.timer) {
        (void)esp_timer_stop(s_ctx.timer);
        (void)esp_timer_delete(s_ctx.timer);
        s_ctx.timer = NULL;
    }
    /* Best-effort: drive all three channels HIGH (LED off) so we do
     * not leave the LED stuck in an arbitrary partial state. Ignore
     * write errors; we are already on the disable path. */
    if (s_ctx.exp) {
        (void)esp_io_expander_set_level(s_ctx.exp, STATUS_LED_PIN_R, 1);
        (void)esp_io_expander_set_level(s_ctx.exp, STATUS_LED_PIN_G, 1);
        (void)esp_io_expander_set_level(s_ctx.exp, STATUS_LED_PIN_B, 1);
    }
}

/* Periodic tick: read the aggregator's resolved state and, IF it
 * changed since the last tick, push the three GPIO levels onto the
 * AW9523B. Same-source sync with the screen: both consult the same
 * status_state_resolved() (lock-free getter), so they cannot drift.
 * Any I2C error disables the feature (no-brick). */
static void status_led_tick(void *arg)
{
    (void)arg;
    if (!s_ctx.enabled) {
        return;
    }

    ss_state_t state = status_state_resolved();
    if (s_ctx.have_last && state == s_ctx.last_state) {
        return;   /* No state change -> no I2C, no waste */
    }

    status_led_bits_t bits = status_led_bits_for_state(state);

    esp_err_t e;
    e = led_set_channel(STATUS_LED_PIN_R, bits.r);
    if (e != ESP_OK) { led_disable("P10 (red) set_level failed", e); return; }
    e = led_set_channel(STATUS_LED_PIN_G, bits.g);
    if (e != ESP_OK) { led_disable("P11 (green) set_level failed", e); return; }
    e = led_set_channel(STATUS_LED_PIN_B, bits.b);
    if (e != ESP_OK) { led_disable("P12 (blue) set_level failed", e); return; }

    s_ctx.last_state = state;
    s_ctx.have_last = true;
}

esp_err_t status_led_start(void)
{
    /* Idempotent: a second successful start is a no-op. */
    if (s_ctx.started) {
        return ESP_OK;
    }
    s_ctx.started = true;
    s_ctx.enabled = false;
    s_ctx.have_last = false;

    /* Reuse the board-manager AW9523B handle (do NOT create a new
     * expander). Same accessor + deref pattern as
     * main.c select_button_init and boards/.../setup_device.c. */
    void *dev = NULL;
    esp_err_t err = esp_board_manager_get_device_handle("gpio_expander_io",
                                                        &dev);
    if (err != ESP_OK || dev == NULL) {
        ESP_LOGW(TAG, "onboard LED disabled: gpio_expander_io handle: %s",
                 esp_err_to_name(err));
        return ESP_OK;   /* no-brick: feature off, boot continues */
    }
    s_ctx.exp = *(esp_io_expander_handle_t *)dev;
    if (s_ctx.exp == NULL) {
        ESP_LOGW(TAG, "onboard LED disabled: expander handle is NULL");
        return ESP_OK;
    }

    /* Drive all three channels HIGH first (LED off) so the very first
     * tick deterministically transitions from "off" to the current
     * resolved state's colour -- no flash at an undefined level. */
    if (esp_io_expander_set_level(s_ctx.exp, STATUS_LED_PIN_R, 1) != ESP_OK ||
        esp_io_expander_set_level(s_ctx.exp, STATUS_LED_PIN_G, 1) != ESP_OK ||
        esp_io_expander_set_level(s_ctx.exp, STATUS_LED_PIN_B, 1) != ESP_OK) {
        ESP_LOGW(TAG, "onboard LED disabled: initial LED-off write failed");
        return ESP_OK;
    }

    /* Periodic state-poll timer. Failure -> feature disabled, boot ok. */
    const esp_timer_create_args_t targs = {
        .callback = status_led_tick,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "status_led",
    };
    err = esp_timer_create(&targs, &s_ctx.timer);
    if (err != ESP_OK || s_ctx.timer == NULL) {
        ESP_LOGW(TAG, "onboard LED disabled: timer create failed (%s)",
                 esp_err_to_name(err));
        s_ctx.timer = NULL;
        return ESP_OK;
    }

    err = esp_timer_start_periodic(s_ctx.timer, STATUS_LED_TICK_PERIOD_US);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "onboard LED disabled: timer start failed (%s)",
                 esp_err_to_name(err));
        (void)esp_timer_delete(s_ctx.timer);
        s_ctx.timer = NULL;
        return ESP_OK;
    }

    s_ctx.enabled = true;
    ESP_LOGI(TAG, "onboard RGB LED status driver started "
                  "(AW9523B P10/11/12 GPIO active-low, "
                  "in sync with screen status state)");
    return ESP_OK;
}

void status_led_stop(void)
{
    if (!s_ctx.started) {
        return;
    }
    s_ctx.enabled = false;
    if (s_ctx.timer) {
        (void)esp_timer_stop(s_ctx.timer);
        (void)esp_timer_delete(s_ctx.timer);
        s_ctx.timer = NULL;
    }
    /* Best-effort: drive LED off. */
    if (s_ctx.exp) {
        (void)esp_io_expander_set_level(s_ctx.exp, STATUS_LED_PIN_R, 1);
        (void)esp_io_expander_set_level(s_ctx.exp, STATUS_LED_PIN_G, 1);
        (void)esp_io_expander_set_level(s_ctx.exp, STATUS_LED_PIN_B, 1);
    }
    s_ctx.started = false;
    s_ctx.have_last = false;
    s_ctx.exp = NULL;
}
