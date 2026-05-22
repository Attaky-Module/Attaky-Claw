/*
 * Spike A orchestration. No-brick discipline: every acquired /
 * suspended resource is unwound in reverse order on ANY failure so
 * emote rendering is always restored. Never ESP_ERROR_CHECK / abort
 * on this path — log and return an esp_err_t.
 */

#include "setup_ui.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
/* Explicit symbol-providing headers (do not rely on transitive pulls
 * via the esp_board_manager_includes.h umbrella):
 *   esp_board_manager.h      -> esp_board_manager_get_device_handle()
 *   esp_board_manager_defs.h -> ESP_BOARD_DEVICE_NAME_LCD_TOUCH
 *   dev_lcd_touch.h          -> dev_lcd_touch_handles_t
 *   esp_lcd_touch.h          -> esp_lcd_touch_handle_t
 * The umbrella is kept for the other board-manager device/config
 * decls this file uses (dev_display_lcd_*). */
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_board_manager_includes.h"
#include "dev_lcd_touch.h"
#include "esp_lcd_touch.h"

#include "display_arbiter.h"
#include "emote.h"
/* setup_lvgl_port.h provides setup_lvgl_port_should_stop() (used
 * indirectly: the screens driver polls it) plus the port API. */
#include "setup_lvgl_port.h"
#include "setup_model.h"
#include "setup_screens.h"
#include "setup_worker.h"

static const char *TAG = "setup_ui";

/* Mirror emote_should_swap_color() exactly (emote.c): non-DSI/RGB
 * panels (st7789 over spi here) need an RGB565 byte swap. Derived
 * the same way so Spike A colours match the emote path. */
static bool setup_ui_should_swap_color(const dev_display_lcd_config_t *cfg)
{
    if (cfg == NULL || cfg->sub_type == NULL) {
        return true;
    }
    if (strcmp(cfg->sub_type, "dsi") == 0 ||
        strcmp(cfg->sub_type, "mipi_dsi") == 0 ||
        strcmp(cfg->sub_type, "rgb") == 0) {
        return false;
    }
    return true;
}

static void setup_ui_log_mem(const char *when)
{
    ESP_LOGI(TAG, "[mem %s] free_heap=%u PSRAM_free=%u",
             when,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

esp_err_t setup_ui_spike_a(void)
{
    esp_err_t ret = ESP_OK;
    bool acquired = false;
    bool suspended = false;
    bool lvgl_up = false;

    setup_ui_log_mem("before-acquire");

    /* Resolve the panel handle the SAME way emote does: board
     * manager device handle -> dev_display_lcd_handles_t. */
    void *lcd_handle = NULL;
    void *lcd_config = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(
        ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get display_lcd handle failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = esp_board_manager_get_device_config(
        ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get display_lcd config failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    dev_display_lcd_handles_t *h = (dev_display_lcd_handles_t *)lcd_handle;
    dev_display_lcd_config_t *cfg = (dev_display_lcd_config_t *)lcd_config;
    if (!h || !cfg || !h->panel_handle) {
        ESP_LOGE(TAG, "display_lcd handle/config NULL");
        return ESP_ERR_INVALID_STATE;
    }

    int w = cfg->lcd_width;
    int h_res = cfg->lcd_height;
    bool swap = setup_ui_should_swap_color(cfg);
    ESP_LOGI(TAG, "panel %dx%d sub_type=%s swap=%d",
             w, h_res, cfg->sub_type ? cfg->sub_type : "(null)", (int)swap);

    /* --- Step into the session: acquire -> suspend -> LVGL --- */
    err = display_arbiter_acquire_setup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display_arbiter_acquire_setup failed: %s",
                 esp_err_to_name(err));
        ret = err;
        goto unwind;
    }
    acquired = true;

    err = emote_suspend_for_session();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "emote_suspend_for_session failed: %s",
                 esp_err_to_name(err));
        ret = err;
        goto unwind;
    }
    suspended = true;

    /* Residual-DMA window probe is performed by the LVGL pattern's
     * FIRST action (immediate full-screen fill) — see
     * setup_lvgl_port.c. We start the port right now, with NO settle
     * delay after suspend, on purpose. */
    /* Spike A is a pure visual RGB test with no touch interaction:
     * pass NULL so no indev is created (the port runs without one). */
    err = setup_lvgl_port_start(h->panel_handle, h->io_handle, w, h_res,
                                swap, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup_lvgl_port_start failed: %s",
                 esp_err_to_name(err));
        ret = err;
        goto unwind;
    }
    lvgl_up = true;

    setup_ui_log_mem("mid-spike");

    err = setup_lvgl_port_run_rgb_pattern();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB pattern run failed: %s", esp_err_to_name(err));
        ret = err;
        /* fall through to unwind — still restore emote */
    }

unwind:
    /* Reverse-order, best-effort unwind. Each step is guarded so a
     * partial setup still fully restores emote rendering. */
    if (lvgl_up) {
        esp_err_t s = setup_lvgl_port_stop();
        if (s != ESP_OK) {
            /* Most likely ESP_ERR_TIMEOUT: the LVGL task did not exit
             * and setup_lvgl_port_stop() intentionally leaked the
             * buffers/display to avoid a use-after-free. This run is a
             * NO-GO and the board must be power-cycled before the next.
             * Still fall through to resume/release best-effort (no
             * abort) so emote restore is attempted, but surface the
             * error in the function's return so the operator sees it. */
            ESP_LOGE(TAG,
                     "setup_lvgl_port_stop failed: %s — run is a NO-GO; "
                     "power-cycle the board before any further spike run",
                     esp_err_to_name(s));
            if (ret == ESP_OK) {
                ret = s;
            }
        }
    }
    if (suspended) {
        esp_err_t r = emote_resume_after_session();
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "emote_resume_after_session: %s",
                     esp_err_to_name(r));
        }
    }
    if (acquired) {
        esp_err_t r = display_arbiter_release_setup();
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "display_arbiter_release_setup: %s",
                     esp_err_to_name(r));
        }
    }

    setup_ui_log_mem("after-release");
    ESP_LOGI(TAG, "setup_ui_spike_a -> %s", esp_err_to_name(ret));
    return ret;
}

/* Real first-use setup-UI session (Task 4.2: WELCOME -> WIFI_SCAN ->
 * WIFI_PW; LLM / REVIEW / SAVE are Task 4.3 — the screen flow stops at
 * a short "WiFi connected — LLM setup next" interim screen after a
 * real connect and returns cleanly).
 *
 * Identical no-brick discipline to setup_ui_spike_a/b: every acquired/
 * suspended/started resource is unwound in reverse order on ANY
 * failure so emote rendering is always restored. Never
 * ESP_ERROR_CHECK / abort / assert — log and return an esp_err_t.
 *
 * Threading: ALL lv_* run on the single LVGL task. The interactive
 * flow driver (setup_screens_run) is marshalled there via
 * setup_lvgl_port_run_screens(); blocking WiFi calls run on the
 * setup_worker task and results are drained inside the driver's loop.
 * BOTH modes run this same real path: setup_screens_ctx_create(mode)
 * picks the entry screen (FIRST_USE -> WELCOME, SETTINGS -> MENU) and
 * setup_model_init() seeds the draft (SETTINGS loads the CURRENT
 * config); the worker/ctx/model/run_screens path itself is
 * mode-agnostic.
 *
 * Requires-touch fall-through (no-brick, load-bearing): both the
 * FIRST_USE and SETTINGS interactive flows are touch-driven and have
 * no input-less success exit. If there is no working touch indev (no
 * touch handle resolved, so setup_lvgl_port_start() created no indev)
 * the flow is NOT entered; this function logs, unwinds cleanly (lvgl
 * stop ->
 * emote resume -> arbiter release) and returns a non-fatal esp_err_t.
 * FIRST_USE falls through to emote + the (already-up) captive portal
 * — a fully functional phone-provisioning path; SETTINGS simply does
 * not open and emote resumes. Never a brick.
 * setup_ui_run() runs in app_main BEFORE app_claw_start, so an
 * unbounded block here would brick every boot; that is what this and
 * the cooperative-stop / bounded-wait changes prevent. */
esp_err_t setup_ui_run(setup_sm_mode_t mode)
{
    esp_err_t ret = ESP_OK;
    bool acquired = false;
    bool suspended = false;
    bool lvgl_up = false;
    bool worker_up = false;
    setup_screens_ctx_t *scr_ctx = NULL;

    const char *text = (mode == SETUP_MODE_FIRST_USE)
                           ? "First-use setup"
                           : "Setup";
    ESP_LOGI(TAG, "setup_ui_run mode=%d (\"%s\")", (int)mode, text);
    setup_ui_log_mem("before-acquire");

    /* Resolve the panel handle the SAME way emote/Spike A does. */
    void *lcd_handle = NULL;
    void *lcd_config = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(
        ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get display_lcd handle failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = esp_board_manager_get_device_config(
        ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get display_lcd config failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    dev_display_lcd_handles_t *h = (dev_display_lcd_handles_t *)lcd_handle;
    dev_display_lcd_config_t *cfg = (dev_display_lcd_config_t *)lcd_config;
    if (!h || !cfg || !h->panel_handle) {
        ESP_LOGE(TAG, "display_lcd handle/config NULL");
        return ESP_ERR_INVALID_STATE;
    }

    int w = cfg->lcd_width;
    int h_res = cfg->lcd_height;
    bool swap = setup_ui_should_swap_color(cfg);
    ESP_LOGI(TAG, "panel %dx%d sub_type=%s swap=%d",
             w, h_res, cfg->sub_type ? cfg->sub_type : "(null)", (int)swap);

    /* Resolve the touch handle the SAME way Spike B does so the
     * widgets are tappable. A missing touch device does NOT abort the
     * session, but for the FIRST_USE interactive flow it is NOT run
     * without touch (an input-less, success-only-exit flow would pin
     * app_main forever): instead we fall through to emote + captive
     * portal below. Resolution itself is fail-soft (no abort here). */
    esp_lcd_touch_handle_t tp = NULL;
    void *touch_handle = NULL;
    esp_err_t terr = esp_board_manager_get_device_handle(
        ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_handle);
    if (terr != ESP_OK || touch_handle == NULL) {
        ESP_LOGW(TAG, "get lcd_touch handle failed: %s (no touch)",
                 esp_err_to_name(terr));
    } else {
        dev_lcd_touch_handles_t *th =
            (dev_lcd_touch_handles_t *)touch_handle;
        tp = th->touch_handle;
        if (tp == NULL) {
            ESP_LOGW(TAG, "lcd_touch handle present but tp NULL (no touch)");
        } else {
            ESP_LOGI(TAG, "touch handle resolved tp=%p", (void *)tp);
        }
    }

    /* --- Step into the session: acquire -> suspend -> LVGL --- */
    err = display_arbiter_acquire_setup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display_arbiter_acquire_setup failed: %s",
                 esp_err_to_name(err));
        ret = err;
        goto unwind;
    }
    acquired = true;

    err = emote_suspend_for_session();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "emote_suspend_for_session failed: %s",
                 esp_err_to_name(err));
        ret = err;
        goto unwind;
    }
    suspended = true;

    /* Pass the already-resolved touch handle into start: the indev is
     * created INSIDE setup_lvgl_port_start, before the LVGL task is
     * spawned — the only race-free window for lv_indev_create() (LVGL
     * v9 is not thread-safe; creating it on this owner task after the
     * LVGL task was live corrupted LVGL's global lists and wedged the
     * second session). The interactive first-use flow is touch-driven:
     * without a working indev it would render but accept no input, and
     * its loop's only WiFi-success exit could never be reached — that
     * is the dead/absent-touch brick (an unbounded block in app_main
     * on every boot). So for the real FIRST_USE flow a working touch
     * indev is REQUIRED: if there is no touch handle (tp == NULL) the
     * port still starts but with no indev, and we DO NOT enter the
     * unbounded interactive flow. We log clearly and fall through to
     * the no-brick unwind (lvgl stop -> emote resume -> arbiter
     * release), returning a non-fatal esp_err_t so app_main's Spike C
     * guard continues to app_claw_start — the captive portal (already
     * up before first-use setup) remains a fully functional phone
     * provisioning path. (With working touch the interactive flow runs
     * as before; mandatory provisioning staying on screen until the
     * user completes it is intended. An inactivity timeout WITH working
     * touch is explicitly out of scope here.) */
    err = setup_lvgl_port_start(h->panel_handle, h->io_handle, w, h_res,
                                swap, tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup_lvgl_port_start failed: %s",
                 esp_err_to_name(err));
        ret = err;
        goto unwind;
    }
    lvgl_up = true;

    /* touch_ok iff a touch handle was resolved AND the port started
     * (the indev was therefore created in the race-free window above).
     * tp == NULL was already logged ("no touch handle") in start. */
    bool touch_ok = (tp != NULL);

    setup_ui_log_mem("mid-session");

    /* A working touch indev is REQUIRED for BOTH modes: the FIRST_USE
     * provisioning flow and the SETTINGS MENU flow are both touch-driven
     * and have no input-less success exit. Without an indev they would
     * render but accept no input and the loop could only end via the
     * cooperative stop — so we DO NOT enter either flow. We log clearly
     * and fall through to the no-brick unwind (lvgl stop -> emote resume
     * -> arbiter release), returning a non-fatal esp_err_t. FIRST_USE
     * still has the (already-up) captive portal as a phone-provisioning
     * fallback; SETTINGS simply does not open and emote resumes —
     * nothing is persisted either way. */
    if (!touch_ok) {
        ESP_LOGW(TAG,
                 "setup requires a working touch indev — none available "
                 "(mode=%d); skipping the interactive flow and falling "
                 "through to emote%s (non-fatal, no brick)",
                 (int)mode,
                 (mode == SETUP_MODE_FIRST_USE) ? " + captive portal"
                                                : " (settings not opened)");
        ret = ESP_ERR_NOT_SUPPORTED;
        goto unwind;
    }

    /* Start the worker (blocking WiFi ops live there, never
     * on the LVGL task) and heap-own the session ctx (its SSID /
     * password / wifi_manager_config buffers MUST outlive worker
     * jobs, so they live in this heap object, not a stack frame). */
    err = setup_worker_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup_worker_start failed: %s",
                 esp_err_to_name(err));
        ret = err;
        goto unwind;
    }
    worker_up = true;

    scr_ctx = setup_screens_ctx_create(mode);
    if (scr_ctx == NULL) {
        ESP_LOGE(TAG, "setup_screens_ctx_create failed (OOM)");
        ret = ESP_ERR_NO_MEM;
        goto unwind;
    }

    /* Build the setup_model draft for this session BEFORE the screen
     * flow runs (LLM/SAVE screens edit and commit setup_model_draft()).
     * Fail-soft, no brick: on failure log and take the same reverse-
     * order unwind so emote rendering is always restored. */
    {
        int merr = setup_model_init(setup_model_mode_from_sm(mode));
        if (merr != ESP_OK) {
            ESP_LOGE(TAG, "setup_model_init failed: %s",
                     esp_err_to_name((esp_err_t)merr));
            ret = (esp_err_t)merr;
            goto unwind;
        }
    }

    err = setup_lvgl_port_run_screens(setup_screens_run, scr_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi screen flow failed: %s",
                 esp_err_to_name(err));
        ret = err;
        /* fall through to unwind — still restore emote */
    }

unwind:
    /* Reverse-order, best-effort unwind. Same no-brick discipline as
     * Spike A/B: every acquired/suspended/started resource is restored
     * so emote rendering always comes back; never abort.
     *
     * Teardown order is load-bearing: stop the LVGL port FIRST (its
     * task is the only thing still running the screen driver), then
     * stop the worker (so no job is in flight), and ONLY THEN free
     * the screens ctx — the ctx holds the SSID/password/cfg buffers a
     * worker connect job may dereference, so it must outlive both the
     * LVGL task and the worker task. */
    if (lvgl_up) {
        esp_err_t s = setup_lvgl_port_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG,
                     "setup_lvgl_port_stop failed: %s — run is a NO-GO; "
                     "power-cycle the board before any further session",
                     esp_err_to_name(s));
            if (ret == ESP_OK) {
                ret = s;
            }
        }
    }
    if (worker_up) {
        esp_err_t s = setup_worker_stop();
        if (s != ESP_OK) {
            /* ESP_ERR_TIMEOUT => worker WEDGED (queues intentionally
             * leaked). A wedged worker may still hold scr_ctx; do NOT
             * free it (use-after-free risk) and surface the error. */
            ESP_LOGE(TAG,
                     "setup_worker_stop failed: %s — worker wedged; "
                     "leaking screens ctx to avoid a use-after-free; "
                     "power-cycle the board before any further session",
                     esp_err_to_name(s));
            if (ret == ESP_OK) {
                ret = s;
            }
            scr_ctx = NULL; /* deliberately leak: do not free below */
        }
    }
    if (scr_ctx != NULL) {
        /* Safe now: LVGL task gone + worker provably stopped, so no
         * job can still dereference the ctx buffers. */
        setup_screens_ctx_destroy(scr_ctx);
        scr_ctx = NULL;
    }
    if (suspended) {
        esp_err_t r = emote_resume_after_session();
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "emote_resume_after_session: %s",
                     esp_err_to_name(r));
        }
    }
    if (acquired) {
        esp_err_t r = display_arbiter_release_setup();
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "display_arbiter_release_setup: %s",
                     esp_err_to_name(r));
        }
    }

    setup_ui_log_mem("after-release");
    ESP_LOGI(TAG, "setup_ui_run -> %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t setup_ui_spike_b(void)
{
    esp_err_t ret = ESP_OK;
    bool acquired = false;
    bool suspended = false;
    bool lvgl_up = false;

    setup_ui_log_mem("before-acquire");

    /* Resolve the panel handle the SAME way emote/Spike A does. */
    void *lcd_handle = NULL;
    void *lcd_config = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(
        ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get display_lcd handle failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = esp_board_manager_get_device_config(
        ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get display_lcd config failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    dev_display_lcd_handles_t *h = (dev_display_lcd_handles_t *)lcd_handle;
    dev_display_lcd_config_t *cfg = (dev_display_lcd_config_t *)lcd_config;
    if (!h || !cfg || !h->panel_handle) {
        ESP_LOGE(TAG, "display_lcd handle/config NULL");
        return ESP_ERR_INVALID_STATE;
    }

    int w = cfg->lcd_width;
    int h_res = cfg->lcd_height;
    bool swap = setup_ui_should_swap_color(cfg);
    ESP_LOGI(TAG, "panel %dx%d sub_type=%s swap=%d",
             w, h_res, cfg->sub_type ? cfg->sub_type : "(null)", (int)swap);

    /* Resolve the touch handle the SAME way: board manager device
     * handle -> dev_lcd_touch_handles_t (handle returned directly,
     * cast then ->touch_handle, exactly as the board manager test
     * does). Fail soft: a missing touch device must not abort the
     * session — log and run the pattern without an indev. */
    esp_lcd_touch_handle_t tp = NULL;
    void *touch_handle = NULL;
    esp_err_t terr = esp_board_manager_get_device_handle(
        ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_handle);
    if (terr != ESP_OK || touch_handle == NULL) {
        ESP_LOGW(TAG, "get lcd_touch handle failed: %s (no touch)",
                 esp_err_to_name(terr));
    } else {
        dev_lcd_touch_handles_t *th =
            (dev_lcd_touch_handles_t *)touch_handle;
        tp = th->touch_handle;
        if (tp == NULL) {
            ESP_LOGW(TAG, "lcd_touch handle present but tp NULL (no touch)");
        } else {
            ESP_LOGI(TAG, "touch handle resolved tp=%p", (void *)tp);
        }
    }

    /* --- Step into the session: acquire -> suspend -> LVGL --- */
    err = display_arbiter_acquire_setup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display_arbiter_acquire_setup failed: %s",
                 esp_err_to_name(err));
        ret = err;
        goto unwind;
    }
    acquired = true;

    err = emote_suspend_for_session();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "emote_suspend_for_session failed: %s",
                 esp_err_to_name(err));
        ret = err;
        goto unwind;
    }
    suspended = true;

    /* Pass the resolved touch handle into start so the indev is
     * created before the LVGL task is spawned (the only race-free
     * window for lv_indev_create(); doing it after the task was live
     * is the latent concurrency bug fixed here). Fail soft: tp == NULL
     * just runs the visual pattern without touch (start logs "no touch
     * handle") so the operator still sees the corner targets. */
    err = setup_lvgl_port_start(h->panel_handle, h->io_handle, w, h_res,
                                swap, tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup_lvgl_port_start failed: %s",
                 esp_err_to_name(err));
        ret = err;
        goto unwind;
    }
    lvgl_up = true;

    setup_ui_log_mem("mid-spike");

    err = setup_lvgl_port_run_touch_pattern();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch pattern run failed: %s",
                 esp_err_to_name(err));
        ret = err;
        /* fall through to unwind — still restore emote */
    }

unwind:
    /* Reverse-order, best-effort unwind. Identical no-brick
     * discipline as Spike A: every acquired/suspended resource is
     * restored so emote rendering always comes back; never abort. */
    if (lvgl_up) {
        esp_err_t s = setup_lvgl_port_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG,
                     "setup_lvgl_port_stop failed: %s — run is a NO-GO; "
                     "power-cycle the board before any further spike run",
                     esp_err_to_name(s));
            if (ret == ESP_OK) {
                ret = s;
            }
        }
    }
    if (suspended) {
        esp_err_t r = emote_resume_after_session();
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "emote_resume_after_session: %s",
                     esp_err_to_name(r));
        }
    }
    if (acquired) {
        esp_err_t r = display_arbiter_release_setup();
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "display_arbiter_release_setup: %s",
                     esp_err_to_name(r));
        }
    }

    setup_ui_log_mem("after-release");
    ESP_LOGI(TAG, "setup_ui_spike_b -> %s", esp_err_to_name(ret));
    return ret;
}
