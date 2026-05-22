#pragma once

#include "esp_err.h"

#include "setup_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * setup_screens: the interactive first-use WiFi screens (Task 4.2).
 *
 * Scope (Task 4.2 ONLY): WELCOME -> WIFI_SCAN -> WIFI_PW. After a
 * successful WiFi connect the flow advances to the LLM state per
 * setup_sm, but the LLM / REVIEW / SAVE screens are Task 4.3 — this
 * module STOPS with a short "WiFi connected — LLM setup next"
 * interim screen and returns cleanly so the no-brick session unwinds.
 *
 * Threading contract (load-bearing):
 *   - setup_screens_run() is a setup_lvgl_screens_fn_t: it is invoked
 *     ONCE, ON THE LVGL TASK, by setup_lvgl_port_run_screens(). It is
 *     the only place this module touches lv_*.
 *   - All blocking WiFi work (scan / apply / wait) runs on the
 *     setup_worker task via setup_worker_submit(); results flow back
 *     through the bounded result queue and are drained by
 *     setup_worker_poll_results() from inside this function's loop
 *     (where touching lv_* is safe).
 *   - The caller MUST setup_worker_start() before invoking the flow
 *     and setup_worker_stop() in its unwind. This module never starts
 *     / stops the worker itself.
 *
 * Fail-soft: every path logs + returns an esp_err_t. Never aborts /
 * asserts / ESP_ERROR_CHECKs. A scan/connect failure does NOT brick:
 * the user stays on the screen and can retry; a fatal internal error
 * returns an esp_err_t so the caller's reverse-order unwind restores
 * emote.
 */

/**
 * @brief Heap-owned context for one WiFi-screens session.
 *
 * Holds the SSID / password buffers handed to worker jobs. These
 * MUST outlive job execution, so the owner heap-allocates this with
 * setup_screens_ctx_create() and frees it with
 * setup_screens_ctx_destroy() AFTER the LVGL port has stopped (no
 * worker job can still reference it by then). Opaque to callers.
 */
typedef struct setup_screens_ctx setup_screens_ctx_t;

/**
 * @brief Allocate a session context on the heap.
 *
 * @param mode SETUP_MODE_FIRST_USE or SETUP_MODE_SETTINGS (Task 4.2
 *             builds the FIRST_USE WELCOME->SCAN->PW path; mode is
 *             threaded through to setup_sm_next()).
 * @return Heap context, or NULL on allocation failure (caller must
 *         fail soft — no abort).
 */
setup_screens_ctx_t *setup_screens_ctx_create(setup_sm_mode_t mode);

/**
 * @brief Free a session context.
 *
 * MUST be called only AFTER setup_lvgl_port_stop() has returned (the
 * LVGL task is gone) AND setup_worker_stop() has returned (no worker
 * job can still dereference the buffers). NULL-safe.
 */
void setup_screens_ctx_destroy(setup_screens_ctx_t *ctx);

/**
 * @brief Interactive WiFi screen flow. Runs ON the LVGL task.
 *
 * Signature matches setup_lvgl_screens_fn_t. Pass to
 * setup_lvgl_port_run_screens(setup_screens_run, ctx) where @p ctx is
 * a setup_screens_ctx_t* from setup_screens_ctx_create().
 *
 * Builds WELCOME, then WIFI_SCAN (lv_list from wifi_manager_scan_aps
 * via the worker, spinner while scanning, Rescan, Manual SSID), then
 * WIFI_PW (password lv_textarea + lv_keyboard, show/hide). On Connect
 * it runs setup_validate_wifi() then, via the worker,
 * wifi_manager_apply_sta_config() + wifi_manager_wait_connected();
 * success advances setup_sm to LLM and shows the Task-4.3 interim
 * stop; failure stays on WIFI_PW with an inline error label. Cleans
 * the screen before returning (leak-gate discipline).
 *
 * Cooperatively stoppable: the loop polls setup_lvgl_port_should_stop()
 * every iteration and exits via the same clean-up path the moment a
 * port teardown is requested, returning ESP_OK. It therefore can never
 * run forever with no exit other than WiFi-connect success — required
 * so a stuck flow cannot pin the owner task / app_main.
 *
 * @param ctx setup_screens_ctx_t* (opaque). Must be non-NULL.
 * @return ESP_OK on a clean flow (incl. user-driven WiFi success or a
 *         cooperative stop), else an esp_err_t (caller runs its
 *         no-brick unwind).
 */
esp_err_t setup_screens_run(void *ctx);

#ifdef __cplusplus
}
#endif
