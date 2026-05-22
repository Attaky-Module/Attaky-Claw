#pragma once

#include "esp_err.h"

#include "setup_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run an exclusive setup-UI display session.
 *
 * Placeholder implementation (real first-use / settings screens land
 * in Phase 4). Performs the same no-brick display-session round trip
 * as the Spike A/B orchestrators:
 *   display_arbiter_acquire_setup() -> emote_suspend_for_session()
 *   -> start minimal LVGL port -> show a single centered placeholder
 *      label (text depends on @p mode) for a few seconds
 *   -> stop LVGL port -> emote_resume_after_session()
 *   -> display_arbiter_release_setup().
 *
 * No-brick discipline: ALWAYS unwinds every acquired/suspended
 * resource in reverse order on ANY failure so emote rendering is
 * restored. Never ESP_ERROR_CHECK / abort / assert; every failure
 * path logs and returns an esp_err_t.
 *
 * The mode enum is the existing setup_sm_mode_t (SETUP_MODE_FIRST_USE
 * / SETUP_MODE_SETTINGS) from setup_sm.h — intentionally not a
 * duplicate type.
 *
 * @param mode  SETUP_MODE_FIRST_USE or SETUP_MODE_SETTINGS.
 * @return ESP_OK on a clean run, otherwise the first error
 *         encountered (after best-effort unwind).
 */
esp_err_t setup_ui_run(setup_sm_mode_t mode);

/**
 * @brief Spike A: exclusive setup-session LVGL display test.
 *
 * Orchestrates a full display-session round trip:
 *   display_arbiter_acquire_setup() -> emote_suspend_for_session()
 *   -> start minimal LVGL port -> draw RGB bars + moving box (~8 s)
 *   -> stop LVGL port -> emote_resume_after_session()
 *   -> display_arbiter_release_setup().
 *
 * No-brick discipline: ALWAYS unwinds in reverse order on any
 * failure so emote rendering is restored. Never aborts; logs and
 * returns an esp_err_t.
 *
 * @return ESP_OK on a clean run, otherwise the first error
 *         encountered (after best-effort unwind).
 */
esp_err_t setup_ui_spike_a(void);

/**
 * @brief Spike B: exclusive setup-session LVGL touch coordinate test.
 *
 * Same display-session round trip as Spike A plus an LVGL pointer
 * indev backed by the board-manager touch device:
 *   display_arbiter_acquire_setup() -> emote_suspend_for_session()
 *   -> start minimal LVGL port -> attach touch indev
 *   -> draw 4 corner targets + crosshair tracking touch (~45 s)
 *   -> stop LVGL port -> emote_resume_after_session()
 *   -> display_arbiter_release_setup().
 *
 * Same no-brick discipline as Spike A: ALWAYS unwinds in reverse
 * order on any failure; never aborts. A missing/unusable touch
 * device fails soft (the visual pattern still runs without an
 * indev).
 *
 * @return ESP_OK on a clean run, otherwise the first error
 *         encountered (after best-effort unwind).
 */
esp_err_t setup_ui_spike_b(void);

#ifdef __cplusplus
}
#endif
