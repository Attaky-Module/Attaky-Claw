#include "setup_model.h"

#include <string.h>

/* ---- pure, host-testable (no ESP / app_config / I/O) ----------------- *
 * These two helpers carry no ESP dependency and are compiled unguarded so
 * the host test harness can link them directly. Everything below the
 * SETUP_MODEL_HOST_TEST guard needs ESP-IDF (app_config / esp_log) and is
 * verified by the controller's hardware build + serial, not on host. */

bool setup_model_buf_dirty(const void *a, const void *b, size_t len)
{
    if (a == NULL || b == NULL || len == 0) {
        return false;
    }
    return memcmp(a, b, len) != 0;
}

setup_model_mode_t setup_model_mode_from_sm(setup_sm_mode_t sm_mode)
{
    return (sm_mode == SETUP_MODE_SETTINGS) ? SETUP_MODEL_MODE_SETTINGS
                                            : SETUP_MODEL_MODE_FIRST_USE;
}

#ifndef SETUP_MODEL_HOST_TEST

#include "app_config.h"
#include "app_claw.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "setup_model";

/* Transient editable working copy for the open setup session. */
static app_config_t s_draft;
/* Baseline captured at init so is_dirty() can diff without re-loading. */
static app_config_t s_baseline;
static bool s_draft_valid;

/* Borrowed, NON-owning pointers to app_main's live config objects. See
 * the ownership rationale in setup_model.h: setup_model never frees or
 * re-allocates these and never double-owns the live config. */
static app_config_t *s_live_cfg;
static app_claw_config_t *s_live_claw;

void setup_model_bind_live_config(void *live_cfg, void *live_claw)
{
    s_live_cfg = (app_config_t *)live_cfg;
    s_live_claw = (app_claw_config_t *)live_claw;
}

int setup_model_init(setup_model_mode_t mode)
{
    memset(&s_draft, 0, sizeof s_draft);

    if (mode == SETUP_MODEL_MODE_FIRST_USE) {
        app_config_load_defaults(&s_draft);
    } else {
        /* SETTINGS: app_config_load == current s_config at steady state
         * (only the setup flow writes config; see setup_model.h note). */
        esp_err_t err = app_config_load(&s_draft);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "app_config_load failed: %s", esp_err_to_name(err));
            s_draft_valid = false;
            return (int)err;
        }
    }

    memcpy(&s_baseline, &s_draft, sizeof s_baseline);
    s_draft_valid = true;
    return (int)ESP_OK;
}

void *setup_model_draft(void)
{
    return s_draft_valid ? &s_draft : NULL;
}

bool setup_model_is_dirty(void)
{
    if (!s_draft_valid) {
        return false;
    }
    return setup_model_buf_dirty(&s_draft, &s_baseline, sizeof s_draft);
}

int setup_model_commit(void)
{
    if (!s_draft_valid) {
        ESP_LOGE(TAG, "commit before init");
        return (int)ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = app_config_save(&s_draft);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_config_save failed: %s", esp_err_to_name(err));
        return (int)err;
    }

    /* Codex M8: refresh the live in-memory config from NVS so the running
     * system reflects the saved values without requiring a reboot.
     * The save itself already succeeded; from here we can only end up
     * fully-refreshed (ESP_OK) or persisted-but-not-refreshed
     * (ESP_ERR_INVALID_STATE, a non-fatal "reboot to apply" signal). */
    if (s_live_cfg == NULL) {
        ESP_LOGW(TAG,
                 "no live config bound; saved but in-memory not refreshed");
        return (int)ESP_ERR_INVALID_STATE;
    }

    esp_err_t rerr = app_config_load(s_live_cfg);
    if (rerr != ESP_OK) {
        /* Persisted, but the running system is now in an indeterminate
         * partial state: do NOT derive claw and do NOT reset the dirty
         * baseline (so setup_model_is_dirty() keeps reporting true and
         * the caller is told a reboot is needed to apply). */
        ESP_LOGE(TAG, "post-save reload failed: %s", esp_err_to_name(rerr));
        return (int)ESP_ERR_INVALID_STATE;
    }

    if (s_live_claw != NULL) {
        app_config_to_claw(s_live_cfg, s_live_claw);
    }
    /* Fully persisted AND live-refreshed: baseline now matches what was
     * persisted -> draft no longer dirty relative to the saved state. */
    memcpy(&s_baseline, &s_draft, sizeof s_baseline);

    return (int)ESP_OK;
}

#endif /* SETUP_MODEL_HOST_TEST */
