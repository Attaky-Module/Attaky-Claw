/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_ARBITER_OWNER_NONE = 0,
    DISPLAY_ARBITER_OWNER_LUA,
    DISPLAY_ARBITER_OWNER_EMOTE,
    DISPLAY_ARBITER_OWNER_SETUP,
} display_arbiter_owner_t;

typedef void (*display_arbiter_owner_changed_cb_t)(display_arbiter_owner_t owner, void *user_ctx);

esp_err_t display_arbiter_acquire(display_arbiter_owner_t owner);
esp_err_t display_arbiter_release(display_arbiter_owner_t owner);
display_arbiter_owner_t display_arbiter_get_owner(void);
bool display_arbiter_is_owner(display_arbiter_owner_t owner);
esp_err_t display_arbiter_set_owner_changed_callback(display_arbiter_owner_changed_cb_t callback, void *user_ctx);

/* Exclusive setup-session display ownership (distinct from OWNER_LUA).
 * acquire_setup(): acquires ONLY when current owner is EMOTE; returns
 *   ESP_ERR_INVALID_STATE otherwise. Does not itself drain flush — caller
 *   pairs this with emote drain (Task 1.2).
 * release_setup(): restores EMOTE and SYNCHRONOUSLY fires the owner-changed
 *   callback (which re-arms emote refresh) before returning; callers MUST
 *   complete LVGL teardown / emote_resume_after_session() ordering per the
 *   Spike A sequence before calling this. */
esp_err_t display_arbiter_acquire_setup(void);
esp_err_t display_arbiter_release_setup(void);

#ifdef __cplusplus
}
#endif
