/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t emote_start(void);
esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid);

/**
 * @brief Suspend the emote engine and drain its software draw path.
 *
 * Pairs with display_arbiter_acquire_setup(): a foreign display owner
 * (e.g. the Spike A LVGL session) must not race the emote flush path.
 * Gates emote_flush_callback so it stops issuing
 * esp_lcd_panel_draw_bitmap, then blocks (finite timeout, no brick)
 * until the emote ENGINE has released the frame buffer and parked.
 * This gates the engine/software draw path only; it does NOT
 * guarantee the panel's in-flight DMA transfer has completed (the
 * emote/board display path registers no on_color_trans_done callback
 * and esp_lcd_panel_draw_bitmap returns before transfer completion).
 *
 * Spike A ordering: display_arbiter_acquire_setup() ->
 * emote_suspend_for_session() -> (LVGL draws) ->
 * emote_resume_after_session() -> display_arbiter_release_setup().
 *
 * @return ESP_OK always when the engine is running; ESP_ERR_INVALID_STATE
 *         if the emote engine is not started. Never aborts.
 */
esp_err_t emote_suspend_for_session(void);

/**
 * @brief Re-arm the emote engine after a foreign display session.
 *
 * Pairs with display_arbiter_release_setup(). Clears the suspend gate
 * and, if emote is the current display owner, schedules a full
 * refresh. Idempotent-safe and harmless if called alongside the
 * display_arbiter owner-changed callback (double refresh just
 * re-schedules).
 *
 * @return ESP_OK always when the engine is running; ESP_ERR_INVALID_STATE
 *         if the emote engine is not started.
 */
esp_err_t emote_resume_after_session(void);

#if CONFIG_APP_EMOTE_DEBUG_CONSOLE
/* Dev debug (CONFIG_APP_EMOTE_DEBUG_CONSOLE): show an arbitrary packed
 * emoji by name for on-device layout review. */
esp_err_t emote_debug_show(const char *name);

/* Dev debug (CONFIG_APP_EMOTE_DEBUG_CONSOLE): fire an emote state
 * event; token is "listen", "speak", or "idle". */
esp_err_t emote_debug_event(const char *token);

/* Dev debug (CONFIG_APP_EMOTE_DEBUG_CONSOLE): show battery/charge;
 * spec is "charging,percent" e.g. "1,75". */
esp_err_t emote_debug_bat(const char *spec);

/* Dev debug (CONFIG_APP_EMOTE_DEBUG_CONSOLE): push a literal string
 * into the scrolling toast_label so a known input can be reviewed
 * on hardware. */
esp_err_t emote_debug_toast(const char *msg);
#endif /* CONFIG_APP_EMOTE_DEBUG_CONSOLE */

#ifdef __cplusplus
}
#endif
