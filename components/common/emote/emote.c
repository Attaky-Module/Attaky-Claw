/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "emote.h"

#include "sdkconfig.h"
#include <string.h>
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_board_manager_includes.h"
#include "expression_emote.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gfx.h"
#include "display_arbiter.h"
#include "status_state.h"

static const char *TAG = "app_emote";

/* Cross-header value-drift guards. emote.c is the single translation
 * unit where BOTH the esp-claw aggregator enum (ss_state_t, status_state.h)
 * and the vendored engine enum (status_state_t, expression_emote.h) are in
 * scope. The Task 4.3 glue maps ss_state_t -> status_state_t with a plain
 * cast and no translation table, which is only correct while the two enums
 * stay numerically identical. If either side is reordered these fail the
 * build instead of silently driving the wrong status colour. */
_Static_assert((int)SS_STATE_NORMAL       == (int)STATUS_STATE_NORMAL,       "ss/engine NORMAL drift");
_Static_assert((int)SS_STATE_RECORDING    == (int)STATUS_STATE_RECORDING,    "ss/engine RECORDING drift");
_Static_assert((int)SS_STATE_SPEAKING     == (int)STATUS_STATE_SPEAKING,     "ss/engine SPEAKING drift");
_Static_assert((int)SS_STATE_NOTIFICATION == (int)STATUS_STATE_NOTIFICATION, "ss/engine NOTIFICATION drift");

#define EMOTE_ASSETS_PARTITION "emote"

static esp_lcd_panel_io_handle_t s_io_handle;
static esp_lcd_panel_handle_t s_panel_handle;
static int s_lcd_width;
static int s_lcd_height;
static emote_handle_t s_emote_handle;

/* Display-session suspend gate (pairs with display_arbiter
 * acquire/release_setup). Defaults to "not suspended" so the existing
 * flush path is unchanged when suspend/resume are never called. */
/* volatile suffices ONLY because the real synchronization barrier is
 * the flush-drain semaphore (a full FreeRTOS barrier), not this flag;
 * it is a standalone boolean with no companion data. Do not "optimize"
 * the drain trusting this flag, and do not weaken it to a plain bool. */
static volatile bool s_emote_suspended;
/* Given at the end of every emote_flush_callback; taken by
 * emote_suspend_for_session() to confirm the emote engine has finished
 * its draw call and parked. Gates the engine/software draw path only;
 * does NOT track panel in-flight DMA completion. */
static SemaphoreHandle_t s_flush_done_sem;
/* Drive-once latch for the status slot, keyed on the status state value.
 * s_status_driven says the slot has been driven during the current emote
 * display-ownership tenure; s_last_driven_status records which state was
 * driven. Reset on ownership loss so regaining the display always re-drives
 * once (a foreign display session may have clobbered the slot). Keyed on the
 * state value (not a plain one-shot) so a future caller passing a different
 * state still re-drives.
 *
 * Concurrency: this pair plus its paired emote_notify_all_refresh() are
 * accessed from THREE concurrent contexts -- the status-state setter's
 * caller thread (status-state change hook), the display-arbiter thread
 * (owner-changed callback), and emote_apply()'s caller. emote_set_status_state()
 * is internally serialized by the engine's recursive render mutex, but
 * emote_notify_all_refresh()'s gfx_disp_refresh_all path does NOT take that
 * mutex, and these two plain statics are otherwise unsynchronized -> a
 * lost-update race that could silently drop a status notification. Every
 * read-modify-write of this pair AND its paired emote_notify_all_refresh()
 * therefore happens under emote_lock(s_emote_handle) (the engine's recursive
 * render mutex, taken explicitly here precisely because the refresh half and
 * this latch are NOT otherwise serialized). The mutex is recursive, so the
 * lock-holder calling emote_drive_status_default() -> emote_set_status_state()
 * (which re-takes the same mutex) is safe re-entrancy. */
static status_state_t s_last_driven_status;
static bool s_status_driven;

#define EMOTE_FLUSH_DRAIN_TIMEOUT_MS 200

static bool emote_should_swap_color(const dev_display_lcd_config_t *lcd_cfg)
{
    if (lcd_cfg == NULL || lcd_cfg->sub_type == NULL) {
        return true;
    }

    if (strcmp(lcd_cfg->sub_type, "dsi") == 0 || strcmp(lcd_cfg->sub_type, "mipi_dsi") == 0 || strcmp(lcd_cfg->sub_type, "rgb") == 0) {
        return false;
    }

    return true;
}

/* Drive the status slot to its default state.
 *
 * The status slot defaults to NORMAL (breathing blue circle) here in the
 * board layer, gated by display ownership, because the emote engine cannot
 * render it at asset-load / init time: emote may not own the shared display
 * during WiFi provisioning, so a source set inside the engine would never be
 * rendered. Driving it at the same display-ownership points the engine uses
 * for every other render guarantees the status slot is present whenever
 * emote owns the display.
 *
 * Non-fatal: a failure must never block boot. Re-driving the same status
 * state is state-safe (no corruption), but the underlying status anim
 * restarts from frame 0 on every real drive, so a drive-once latch (keyed on
 * the state value, reset on ownership loss) avoids re-kicking the breathing
 * anim within one display-ownership tenure. The target is the status_state
 * aggregator's resolved state: NORMAL until a real inbound event raises
 * NOTIFICATION (RECORDING/SPEAKING reserved for the deferred ASR/voice work).
 * When no inbound ever arrives the aggregator stays NORMAL, so this path is
 * byte-identical to the original NORMAL-only behaviour.
 *
 * Locking: this reads/writes the s_status_driven / s_last_driven_status latch
 * and calls emote_set_status_state(). It MUST be called with
 * emote_lock(s_emote_handle) already held by the caller (see the latch
 * declaration's concurrency note). It deliberately does NOT take the lock
 * itself: the latch check-and-set must be atomic with the paired
 * emote_notify_all_refresh() in the caller, so the lock is held at the
 * outermost point of each drive+refresh sequence (emote_status_drive_locked()),
 * not here. Re-taking the recursive render mutex inside emote_set_status_state()
 * while the caller already holds emote_lock is safe re-entrancy. */
static void emote_drive_status_default(void)
{
    if (!s_emote_handle) {
        return;
    }

    status_state_t target = (status_state_t)status_state_resolved();
    if (s_status_driven && s_last_driven_status == target) {
        return;
    }

    esp_err_t err = emote_set_status_state(s_emote_handle, target);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set default status state failed: %s", esp_err_to_name(err));
        return;
    }

    s_status_driven = true;
    s_last_driven_status = target;
}

/* Lock-bracketed status drive + refresh. Single mechanism shared by all
 * three drive paths (status-state change hook, owner-changed callback,
 * emote_apply) so the latch check-and-set and the paired refresh are
 * mutually excluded across the router/setter-caller thread, the
 * display-arbiter thread, and the engine render loop.
 *
 * emote_lock() can fail; on failure we log and return WITHOUT touching the
 * latch or refreshing -- no-brick, never blocks boot or the caller's task.
 * emote_unlock() is issued only when emote_lock() succeeded. If reset_latch
 * is true, s_status_driven is cleared under the lock so the drive is
 * unconditional (used when the resolved state may have changed). The
 * critical section is intentionally minimal: no logging beyond the existing
 * warnings, no blocking calls; this runs once per inbound event / ownership
 * transition, not per frame, so briefly holding the recursive render mutex
 * is acceptable and is the same class of work emote_apply already needs.
 *
 * Caller must hold a non-NULL s_emote_handle (guarded at every call site). */
static void emote_status_drive_locked(bool reset_latch)
{
    esp_err_t lock_err = emote_lock(s_emote_handle);
    if (lock_err != ESP_OK) {
        ESP_LOGW(TAG, "emote_lock failed, skipping status drive: %s",
                 esp_err_to_name(lock_err));
        return;
    }

    if (reset_latch) {
        s_status_driven = false;
    }
    emote_drive_status_default();

    esp_err_t err = emote_notify_all_refresh(s_emote_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "refresh after status drive failed: %s",
                 esp_err_to_name(err));
    }

    emote_unlock(s_emote_handle);
}

/* Clear the drive-once latch under emote_lock so the write is serialized
 * against the render loop and the other two drive paths (no torn/lost
 * update). Used by the not-owner branches: a drive now would not render,
 * but the latch clear must still be mutually excluded. On lock failure the
 * stale latch is left as-is (no-brick); the next successful drive path
 * re-evaluates the resolved state anyway. */
static void emote_status_clear_latch_locked(void)
{
    esp_err_t lock_err = emote_lock(s_emote_handle);
    if (lock_err != ESP_OK) {
        ESP_LOGW(TAG, "emote_lock failed, skipping latch clear: %s",
                 esp_err_to_name(lock_err));
        return;
    }

    s_status_driven = false;

    emote_unlock(s_emote_handle);
}

/* status_state resolved-state change hook.
 *
 * Runs on the status-state setter's caller thread -- normally the
 * claw_event_router task (or the scheduler), but also ANY caller of the
 * synchronous claw_event_router_handle_event(), which reaches the
 * status-state observer on its own thread. The exact thread identity is
 * immaterial because the latch read-modify-write and the paired
 * emote_notify_all_refresh() are bracketed in emote_lock() via
 * emote_status_drive_locked(): that critical section is what serializes this
 * hook against the engine render loop and the display-arbiter thread.
 * emote_set_status_state() also re-takes the same recursive render mutex
 * internally, which is safe re-entrancy under the held emote_lock. No new
 * task is introduced.
 *
 * Job: make the status slot re-drive to the new resolved state. The
 * drive-once latch in emote_drive_status_default() is keyed on the state
 * value, so on a genuine resolved-state change it re-drives on its own;
 * clearing s_status_driven (reset_latch=true) makes the re-drive
 * unconditional even for the (callback-contract-impossible) same-value
 * case, keeping this hook robust. No-brick: a failed emote_lock or re-drive
 * only logs; it never blocks the caller. Must NOT call any status_state_*
 * setter (re-entrancy is unsupported by the aggregator). Only re-drives
 * while emote owns the display; if it does not, the latch is cleared (under
 * the lock) so the next owner-change re-drives the latest resolved state.
 *
 * Scope of the lock: emote_lock() makes the latch/refresh DATA-race-safe;
 * it does NOT govern callback lifecycle. A callback firing after
 * status_state_on_change(NULL,NULL) is currently impossible only
 * STRUCTURALLY: emote_cleanup() runs solely as emote bring-up rollback,
 * before app_claw registers the router observer, so no event can be in
 * flight when the hook is unregistered. A future runtime emote_cleanup /
 * app_claw_stop path MUST add explicit unregister-then-quiesce; the lock
 * alone would not close that lifecycle gap.
 *
 * The resolved arg is intentionally ignored: emote_drive_status_default()
 * re-reads status_state_resolved() under emote_lock, keeping the aggregator
 * the single source of truth and avoiding a stale snapshot racing the lock. */
static void emote_on_status_state_change(ss_state_t resolved, void *ctx)
{
    (void)resolved;
    (void)ctx;

    if (!s_emote_handle) {
        return;
    }

    if (!display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
        /* Not the display owner: a drive now would not render. Clear the
         * latch (under emote_lock) so emote_on_owner_changed() re-drives
         * the latest resolved state when emote regains the display. */
        emote_status_clear_latch_locked();
        return;
    }

    emote_status_drive_locked(true);
}

static void emote_on_owner_changed(display_arbiter_owner_t owner, void *user_ctx)
{
    (void)user_ctx;

    if (owner != DISPLAY_ARBITER_OWNER_EMOTE) {
        /* Lost the display: a foreign session may clobber the status slot,
         * so clear the drive-once latch (under emote_lock so the write is
         * serialized against the render loop and the other drive paths) and
         * re-drive once on regain. Skip if the handle is gone. */
        if (!s_emote_handle) {
            return;
        }
        emote_status_clear_latch_locked();
        return;
    }

    if (!s_emote_handle) {
        return;
    }

    /* Set the status anim source while emote owns the display so the
     * paired refresh actually renders it. Latch + drive + refresh are
     * bracketed in emote_lock; reset_latch=false keeps the normal
     * drive-once behaviour for this ownership tenure. */
    emote_status_drive_locked(false);
}

static void emote_flush_callback(int x_start, int y_start, int x_end, int y_end,
                                 const void *data, emote_handle_t handle)
{
    /* Suspended for a foreign display session, or emote is not the
     * display owner, or the panel is gone: skip the actual draw but
     * still complete the engine's flush bookkeeping so its frame
     * pipeline does not stall, and signal drain so a pending
     * emote_suspend_for_session() can return promptly. */
    if (s_emote_suspended || !s_panel_handle ||
            !display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
        if (handle) {
            emote_notify_flush_finished(handle);
        }
        if (s_flush_done_sem) {
            xSemaphoreGive(s_flush_done_sem);
        }
        return;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel_handle, x_start, y_start, x_end, y_end, data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_draw_bitmap failed: %s", esp_err_to_name(err));
    }

    if (handle) {
        emote_notify_flush_finished(handle);
    }

    if (s_flush_done_sem) {
        xSemaphoreGive(s_flush_done_sem);
    }
}

static void emote_update_callback(gfx_disp_event_t event, const void *obj,
                                  emote_handle_t handle)
{
    if (!handle) {
        return;
    }

    gfx_obj_t *wait_obj = emote_get_obj_by_name(handle, EMT_DEF_ELEM_EMERG_DLG);
    if (wait_obj == obj && event == GFX_DISP_EVENT_ALL_FRAME_DONE) {
        ESP_LOGI(TAG, "Emergency dialog finished");
    }
}

static esp_err_t emote_load_board_display(void)
{
#if !CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    return ESP_ERR_NOT_SUPPORTED;
#else
    void *lcd_handle = NULL;
    void *lcd_config = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config);
    if (err != ESP_OK) {
        return err;
    }

    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)lcd_config;

    ESP_RETURN_ON_FALSE(lcd_handles && lcd_cfg && lcd_handles->panel_handle,
                        ESP_ERR_INVALID_STATE, TAG, "display_lcd handle/config is NULL");

    s_panel_handle = lcd_handles->panel_handle;
    s_io_handle = lcd_handles->io_handle;
    s_lcd_width = lcd_cfg->lcd_width;
    s_lcd_height = lcd_cfg->lcd_height;
    return ESP_OK;
#endif
}

static emote_config_t emote_get_default_config(void)
{
    void *lcd_config = NULL;
    bool swap = true;
    if (esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config) == ESP_OK) {
        swap = emote_should_swap_color((const dev_display_lcd_config_t *)lcd_config);
    }

    emote_config_t config = {
        .flags = {
            .swap = swap,
            .double_buffer = true,
            .buff_dma = true,
        },
        .gfx_emote = {
            .h_res = s_lcd_width,
            .v_res = s_lcd_height,
            .fps = 10,
        },
        .buffers = {
            .buf_pixels = (size_t)s_lcd_width * 16,
        },
        .task = {
            .task_priority = 3,
            .task_stack = 12 * 1024,
            .task_affinity = -1,
#ifdef CONFIG_SPIRAM_XIP_FROM_PSRAM
            .task_stack_in_ext = true,
#else
            .task_stack_in_ext = false,
#endif
        },
        .flush_cb = emote_flush_callback,
        .update_cb = emote_update_callback,
    };

    return config;
}

static esp_err_t emote_apply(const char *idle, const char *msg)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "emote handle is NULL");

    ESP_RETURN_ON_ERROR(emote_set_event_msg(s_emote_handle, EMOTE_MGR_EVT_SYS, msg), TAG, "set emote message failed");
    ESP_RETURN_ON_ERROR(emote_set_anim_emoji(s_emote_handle, idle), TAG, "set emote idle animation failed");

    if (display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
        /* Covers the already-provisioned boot where emote owns the
         * display from the first apply and emote_on_owner_changed never
         * fires a transition. Same ownership gate as the refresh. Latch
         * access + drive + refresh are bracketed in emote_lock (shared
         * mechanism) so this path cannot lose-update against the render
         * loop, the owner-changed callback, or the status-state hook.
         * reset_latch=false keeps drive-once for this tenure; a refresh
         * failure here logs (no-brick) instead of failing emote_apply,
         * matching the other two drive paths. */
        emote_status_drive_locked(false);
    }

    return ESP_OK;
}

esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "emote handle is NULL");

    const bool ap_present = (ap_ssid != NULL && ap_ssid[0] != '\0');
    /* Upstream 320_240 emote set has no swim/offline; map to its
     * own idle names (neutral when online, sleepy when offline). */
    const char *idle = sta_connected ? "neutral" : "sleepy";

    char msg[96];
    if (sta_connected && ap_present) {
        snprintf(msg, sizeof(msg), "Online * AP: %s", ap_ssid);
    } else if (sta_connected) {
        snprintf(msg, sizeof(msg), "Wi-Fi connected");
    } else if (ap_present) {
        snprintf(msg, sizeof(msg), "Setup WiFi: %s", ap_ssid);
    } else {
        snprintf(msg, sizeof(msg), "Wi-Fi offline");
    }

    return emote_apply(idle, msg);
}

esp_err_t emote_suspend_for_session(void)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "emote handle is NULL");

    /* Gate the flush path first so no NEW draw is issued from here on. */
    s_emote_suspended = true;

    /* Drain: a draw may already be mid-flight in the emote task. Take
     * the flush-done signal with a finite timeout so a wedged/slow
     * panel can never brick boot. Clear any stale count first so we
     * actually wait for a fresh post-suspend flush completion. */
    if (s_flush_done_sem) {
        xSemaphoreTake(s_flush_done_sem, 0);
        if (xSemaphoreTake(s_flush_done_sem,
                           pdMS_TO_TICKS(EMOTE_FLUSH_DRAIN_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "emote flush drain timed out after %d ms; proceeding",
                     EMOTE_FLUSH_DRAIN_TIMEOUT_MS);
        }
    }

    return ESP_OK;
}

esp_err_t emote_resume_after_session(void)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "emote handle is NULL");

    /* Clear the gate so the flush path resumes drawing. Idempotent. */
    s_emote_suspended = false;

    /* Re-arm only if emote owns the display. display_arbiter's
     * owner-changed callback may also schedule a refresh after
     * release_setup(); emote_notify_all_refresh just schedules, so a
     * double call here is harmless. */
    if (display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
        esp_err_t err = emote_notify_all_refresh(s_emote_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "refresh after session resume failed: %s",
                     esp_err_to_name(err));
        }
    }

    return ESP_OK;
}

#if CONFIG_APP_EMOTE_DEBUG_CONSOLE

/* Dev debug (CONFIG_APP_EMOTE_DEBUG_CONSOLE): force an arbitrary
 * packed emoji by name for on-device layout review. */
esp_err_t emote_debug_show(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /* Empty system message: don't pollute the scrolling toast_label
     * with the (out-of-subset-font) emote name during layout review. */
    return emote_apply(name, "");
}

/* Dev debug: push a literal string into the scrolling toast_label so a
 * known input can be reviewed on hardware. Used to isolate whether
 * garbled toast output is a font-coverage problem (text valid, glyph
 * missing) versus a byte-level corruption upstream (invalid UTF-8 in
 * the pipeline). */
esp_err_t emote_debug_toast(const char *msg)
{
    /* Use the same path as emote_set_network_status() so the SYS msg
     * sticks: emote_apply() drives both the toast label AND the face
     * anim refresh in one atomic step. Side effect: the face animates
     * to "neutral". Note that calling EMOTE_MGR_EVT_IDLE separately
     * would HIDE the toast (event_table marks IDLE as not-skip-hide). */
    return emote_apply("idle", msg ? msg : "");
}

/* Dev debug (CONFIG_APP_EMOTE_DEBUG_CONSOLE): fire an emote manager
 * state event (listen/speak/idle) so overlay anims like listen_anim
 * can be reviewed on hardware, and evt_listen/evt_speak wiring can be
 * validated for the deferred status-circle feature. */
esp_err_t emote_debug_event(const char *token)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "emote handle is NULL");

    const char *evt;
    if (token != NULL && strcmp(token, "listen") == 0) {
        evt = EMOTE_MGR_EVT_LISTEN;
    } else if (token != NULL && strcmp(token, "speak") == 0) {
        evt = EMOTE_MGR_EVT_SPEAK;
    } else if (token != NULL && strcmp(token, "idle") == 0) {
        evt = EMOTE_MGR_EVT_IDLE;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(emote_set_event_msg(s_emote_handle, evt, NULL), TAG, "set emote event failed");

    if (display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
        ESP_RETURN_ON_ERROR(emote_notify_all_refresh(s_emote_handle), TAG, "refresh emote display failed");
    }
    return ESP_OK;
}

/* Dev debug (CONFIG_APP_EMOTE_DEBUG_CONSOLE): push a battery state.
 * spec is "charging,percent" e.g. "1,75". NOTE: the status-cluster
 * redesign retired battery_label/charge_icon (the status slot is now a
 * state-driven breathing anim), so evt_bat now only caches
 * bat_is_charging/bat_percent and the idle re-render no longer paints
 * any battery UI -- this command is effectively a no-op on screen,
 * kept for the cached-state path and any future consumer. */
esp_err_t emote_debug_bat(const char *spec)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "emote handle is NULL");
    ESP_RETURN_ON_ERROR(emote_set_event_msg(s_emote_handle, EMOTE_MGR_EVT_BAT, spec), TAG, "set emote bat failed");
    ESP_RETURN_ON_ERROR(emote_set_event_msg(s_emote_handle, EMOTE_MGR_EVT_IDLE, NULL), TAG, "render bat via idle failed");

    if (display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
        ESP_RETURN_ON_ERROR(emote_notify_all_refresh(s_emote_handle), TAG, "refresh emote display failed");
    }
    return ESP_OK;
}

#endif /* CONFIG_APP_EMOTE_DEBUG_CONSOLE */

static void emote_cleanup(void)
{
    if (s_emote_handle) {
        emote_deinit(s_emote_handle);
        s_emote_handle = NULL;
    }
    s_emote_suspended = false;
    if (s_flush_done_sem) {
        vSemaphoreDelete(s_flush_done_sem);
        s_flush_done_sem = NULL;
    }
    display_arbiter_set_owner_changed_callback(NULL, NULL);
    status_state_on_change(NULL, NULL);
}

static esp_err_t emote_init_internal(void)
{
    emote_data_t data = {
        .type = EMOTE_SOURCE_PARTITION,
        .source = {
            .partition_label = EMOTE_ASSETS_PARTITION,
        },
        .flags = {
#ifdef CONFIG_SPIRAM_XIP_FROM_PSRAM
            .mmap_enable = false,
#else
            .mmap_enable = true,
#endif
        },
    };

    if (s_emote_handle) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(emote_load_board_display(), TAG, "Failed to get board display handles");

    s_emote_suspended = false;
    if (!s_flush_done_sem) {
        /* BINARY (not counting) is a correctness requirement: the
         * prime-take + binary cap is what neutralizes stale/multi-give
         * early-return in the drain. Switching to a counting semaphore
         * would silently reintroduce the early-return bug. */
        s_flush_done_sem = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_flush_done_sem != NULL, ESP_ERR_NO_MEM, TAG,
                            "create flush-done semaphore failed");
    }

    emote_config_t config = emote_get_default_config();
    ESP_RETURN_ON_ERROR(display_arbiter_set_owner_changed_callback(emote_on_owner_changed, NULL), TAG, "register display owner callback failed");
    /* One-time: drive the status slot whenever the resolved state changes
     * (NORMAL <-> NOTIFICATION today). Single callback slot by design. */
    status_state_on_change(emote_on_status_state_change, NULL);
    s_emote_handle = emote_init(&config);
    if (!s_emote_handle || !emote_is_initialized(s_emote_handle)) {
        emote_cleanup();
        return ESP_FAIL;
    }

    esp_err_t err = emote_mount_and_load_assets(s_emote_handle, &data);
    if (err != ESP_OK) {
        emote_cleanup();
        return err;
    }

    return emote_set_network_status(false, NULL);
}

esp_err_t emote_start(void)
{
    esp_err_t err = emote_init_internal();
    if (err != ESP_OK) {
        emote_cleanup();
    }
    return err;
}
