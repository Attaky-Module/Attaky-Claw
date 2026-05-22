/*
 * Spike A minimal LVGL port (LVGL v9). Single LVGL task model:
 * lv_timer_handler runs in exactly one FreeRTOS task created here;
 * the RGB-pattern objects are created from that task only. lv_tick
 * is driven by a periodic esp_timer (lv_tick_inc is timer-safe;
 * object creation is not, so it stays in the LVGL task context).
 */

#include "setup_lvgl_port.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "setup_lvgl";

/* Two partial draw buffers in internal DMA-capable RAM
 * (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL): each LCD_W * 10 px RGB565
 * (2 bytes/px) = 6400 B, sized to the board SPI bus max_transfer_sz
 * so the esp_lcd SPI driver DMAs straight from the buffer with no
 * internal bounce buffer (a PSRAM-backed source previously forced a
 * per-transfer bounce alloc that OOM'd when the session opened late).
 * Double-buffering is REQUIRED, not optional: esp_lcd_panel_io_spi
 * tx_color queues the color transfer asynchronously and returns
 * before its DMA completes, while setup_lvgl_flush_cb signals
 * lv_display_flush_ready synchronously — with one buffer LVGL would
 * reuse it mid-DMA and tear. The second buffer gives the
 * render-while-DMA overlap (mirrors emote's double_buffer +
 * buff_dma). */
#define SETUP_LVGL_BUF_LINES   10
#define SETUP_LVGL_TICK_PERIOD_MS 2
/* Priority below the emote engine task (emote uses task_priority 3). */
#define SETUP_LVGL_TASK_PRIO   2
#define SETUP_LVGL_TASK_STACK  (8 * 1024)
#define SETUP_LVGL_PATTERN_MS  8000
/* Spike B human-interaction window: long enough to tap all 4 corners. */
#define SETUP_LVGL_TOUCH_PATTERN_MS 45000
/* Corner target square size in pixels. */
#define SETUP_LVGL_TOUCH_TARGET_SZ  24
/* Crosshair marker diameter in pixels. */
#define SETUP_LVGL_TOUCH_MARK_SZ    20
/* Min interval between SPIKEB press logs (~5/sec). */
#define SETUP_LVGL_TOUCH_LOG_MIN_MS 200

static bool s_lv_inited;                 /* lv_init() done once, ever. */
static lv_display_t *s_disp;
static uint16_t *s_buf1;
static uint16_t *s_buf2;
static esp_lcd_panel_handle_t s_panel;
static int s_w;
static int s_h;
static bool s_swap;

static esp_timer_handle_t s_tick_timer;
static TaskHandle_t s_lvgl_task;
static volatile bool s_task_run;
static SemaphoreHandle_t s_task_exited;  /* given when the LVGL task returns */
/* Latched if the LVGL task ever failed to exit within the stop timeout.
 * When set, buffers/display were intentionally LEAKED (not freed) to
 * avoid a use-after-free into a still-live task; the board MUST be
 * power-cycled before the diagnosis of any subsequent run is trusted. */
static bool s_stop_timed_out;

/* Pattern marshalling: the LVGL task owns all lv_* object creation.
 * The caller sets s_pattern_request and waits on s_pattern_done.
 * Contract: single-shot, single-caller, NOT re-entrant — the caller
 * blocks on s_pattern_done; adding a second/concurrent trigger would
 * require real synchronization (mutex + request queue), not this flag. */
static volatile bool s_pattern_request;
/* Parallel Spike B request flag (touch 4-corner pattern). Same
 * single-caller-blocks contract as s_pattern_request; the two are
 * never triggered concurrently. */
static volatile bool s_pattern_b_request;
/* Placeholder-screen request (setup_ui_run). Same single-caller-
 * blocks contract as the patterns above; never triggered
 * concurrently with them. */
static volatile bool s_pattern_ph_request;
#define SETUP_LVGL_PH_TEXT_MAX 64
static char s_ph_text[SETUP_LVGL_PH_TEXT_MAX];
static volatile int s_ph_hold_ms;
/* Interactive screen-flow request (setup_screens). Same single-
 * caller-blocks contract as the patterns above; never triggered
 * concurrently with them. The driver fn runs ON the LVGL task and
 * owns its own lv_timer_handler() / worker-drain loop. */
static volatile bool s_pattern_scr_request;
static setup_lvgl_screens_fn_t s_scr_fn;
static void *s_scr_ctx;
static volatile esp_err_t s_pattern_result;
static SemaphoreHandle_t s_pattern_done;

/* Touch indev state. The handle is owned by the board manager; we
 * only borrow it. s_touch_indev is created/deleted on the LVGL
 * task's lifetime and torn down in setup_lvgl_port_stop(). */
static esp_lcd_touch_handle_t s_touch_tp;
static lv_indev_t *s_touch_indev;

static void setup_lvgl_tick_cb(void *arg)
{
    (void)arg;
    /* ISR/timer-safe per LVGL docs. */
    lv_tick_inc(SETUP_LVGL_TICK_PERIOD_MS);
}

/* LVGL v9 flush callback. Renders RGB565; ST7789-over-SPI expects
 * big-endian RGB565 so byte-swap when s_swap (mirrors emote's
 * emote_should_swap_color() == true for st7789/spi). */
static void setup_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                                uint8_t *px_map)
{
    if (s_panel != NULL) {
        if (s_swap) {
            uint16_t *p = (uint16_t *)px_map;
            int32_t count = (area->x2 - area->x1 + 1) *
                            (area->y2 - area->y1 + 1);
            for (int32_t i = 0; i < count; i++) {
                p[i] = (uint16_t)((p[i] << 8) | (p[i] >> 8));
            }
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel,
                                                  area->x1, area->y1,
                                                  area->x2 + 1, area->y2 + 1,
                                                  px_map);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
        }
    }
    lv_display_flush_ready(disp);
}

/* Build + animate the Spike A RGB pattern. Runs ONLY on the LVGL
 * task (called from setup_lvgl_task). */
static void setup_lvgl_draw_pattern(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* 1) Immediate full-screen solid fill: deliberately exercises the
     *    "draw right after emote_suspend_for_session() returns" window
     *    (residual in-flight panel DMA test for Task 1.4). */
    ESP_LOGI(TAG, "spike: drawing immediately post-suspend "
                  "(residual-DMA window test)");
    lv_obj_set_style_bg_color(scr, lv_color_make(0xFF, 0xFF, 0xFF), 0);
    lv_refr_now(s_disp);

    /* 2) Full-screen RGB bars (3 vertical bands). */
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_color_t cols[3];
    cols[0] = lv_color_make(0xFF, 0x00, 0x00);
    cols[1] = lv_color_make(0x00, 0xFF, 0x00);
    cols[2] = lv_color_make(0x00, 0x00, 0xFF);
    int bar_w = s_w / 3;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, i * bar_w, 0);
        lv_obj_set_size(bar, (i == 2) ? (s_w - 2 * bar_w) : bar_w, s_h);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, cols[i], 0);
    }

    /* 3) A moving box stepping across the screen for ~8 s. */
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    int box_sz = s_h / 5;
    lv_obj_set_size(box, box_sz, box_sz);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);

    int y = (s_h - box_sz) / 2;
    int step_ms = 100;
    int steps = SETUP_LVGL_PATTERN_MS / step_ms;
    int span = s_w - box_sz;
    for (int i = 0; i < steps && s_task_run; i++) {
        int x = (span > 0) ? (i * span / steps) : 0;
        lv_obj_set_pos(box, x, y);
        lv_refr_now(s_disp);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }

    /* Explicitly destroy every object we created (3 bars + box) while
     * the display is still valid and we are on the LVGL task. This is
     * what makes the Task 1.4 3x repeated-run heap/PSRAM delta a
     * TRUSTWORTHY leak gate: with this clean, screen-child metadata
     * cannot accumulate across runs, so a non-zero delta is a real
     * leak rather than retained-children noise, and a real leak is not
     * masked by relying on lv_display_delete to reclaim them. The
     * lv_display_delete in setup_lvgl_port_stop() is kept as-is for
     * defense in depth. */
    lv_obj_clean(lv_screen_active());
}

/* LVGL pointer indev read_cb. Runs on the LVGL task (called from
 * lv_timer_handler) — single-task model preserved. Coordinates are
 * fed straight from the driver: esp_lcd_touch_get_coordinates()
 * already applies the board_devices.yaml swap_xy/mirror_x/mirror_y
 * flags internally (see managed_components/espressif__esp_lcd_touch/
 * esp_lcd_touch.c esp_lcd_touch_get_coordinates), so any extra
 * software swap/mirror/scale here would double-transform. */
static void setup_lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (s_touch_tp == NULL) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_err_t err = esp_lcd_touch_read_data(s_touch_tp);
    if (err != ESP_OK) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t strength = 0;
    uint8_t num = 0;
    bool pressed = esp_lcd_touch_get_coordinates(s_touch_tp, &x, &y,
                                                 &strength, &num, 1);
    /* strength is not used; num is still read in the press check. */
    (void)strength;
    if (pressed && num > 0) {
        data->point.x = (int32_t)x;
        data->point.y = (int32_t)y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        /* LVGL keeps the last point on release — acceptable. */
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* Build + run the Spike B 4-corner touch-coordinate test. Runs ONLY
 * on the LVGL task (called from setup_lvgl_task). Draws four corner
 * targets at the true pixel corners and a crosshair that tracks the
 * touch indev for ~45 s, logging pressed coordinates (rate-limited).
 */
static void setup_lvgl_draw_touch_pattern(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_make(0x10, 0x10, 0x10), 0);

    /* Four filled corner target squares at the true pixel corners:
     * top-left (0,0), top-right (W-1), bottom-left (0,H-1),
     * bottom-right. Position is the square's top-left, so the
     * right/bottom ones are inset by the square size. */
    int ts = SETUP_LVGL_TOUCH_TARGET_SZ;
    int corners[4][2] = {
        { 0,            0 },
        { s_w - ts,     0 },
        { 0,            s_h - ts },
        { s_w - ts,     s_h - ts },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *t = lv_obj_create(scr);
        lv_obj_remove_style_all(t);
        lv_obj_set_pos(t, corners[i][0], corners[i][1]);
        lv_obj_set_size(t, ts, ts);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t, lv_color_make(0xFF, 0xC0, 0x00), 0);
    }

    /* Crosshair marker: a small contrasting circle. */
    lv_obj_t *mark = lv_obj_create(scr);
    lv_obj_remove_style_all(mark);
    int ms = SETUP_LVGL_TOUCH_MARK_SZ;
    lv_obj_set_size(mark, ms, ms);
    lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(mark, lv_color_make(0x00, 0xE0, 0xFF), 0);
    lv_obj_set_style_border_width(mark, 2, 0);
    lv_obj_set_style_border_color(mark, lv_color_white(), 0);
    lv_obj_set_pos(mark, (s_w - ms) / 2, (s_h - ms) / 2);

    ESP_LOGI(TAG, "SPIKEB: tap the 4 corner targets (%dx%d, ~%d s)",
             s_w, s_h, SETUP_LVGL_TOUCH_PATTERN_MS / 1000);

    int step_ms = 30;
    int steps = SETUP_LVGL_TOUCH_PATTERN_MS / step_ms;
    int64_t last_log_us = 0;
    for (int i = 0; i < steps && s_task_run; i++) {
        /* Pump LVGL each iteration so the indev read timer fires and
         * setup_lvgl_touch_read_cb actually runs — without this the
         * indev point is never refreshed for the whole ~45 s test.
         * lv_timer_handler() also services the display refresh. */
        lv_timer_handler();
        if (s_touch_indev != NULL) {
            lv_point_t p;
            lv_indev_get_point(s_touch_indev, &p);
            lv_indev_state_t st = lv_indev_get_state(s_touch_indev);
            if (st == LV_INDEV_STATE_PRESSED) {
                /* Live touch: show the crosshair and track the point.
                 * Hidden while released so the operator can tell a
                 * live touch from a frozen last-point. */
                lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(mark, (int)p.x - ms / 2,
                               (int)p.y - ms / 2);
                int64_t now_us = esp_timer_get_time();
                if (now_us - last_log_us >=
                    (int64_t)SETUP_LVGL_TOUCH_LOG_MIN_MS * 1000) {
                    last_log_us = now_us;
                    ESP_LOGI(TAG, "SPIKEB touch x=%d y=%d",
                             (int)p.x, (int)p.y);
                }
            } else {
                /* Released/stale: hide the crosshair so a frozen
                 * last-point is not mistaken for a live touch. */
                lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_refr_now(s_disp);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }

    /* Explicitly destroy every object we created (4 targets + mark)
     * while the display is still valid and we are on the LVGL task,
     * mirroring Spike A's clean-teardown leak-gate discipline. */
    lv_obj_clean(lv_screen_active());
}

/* Minimal setup_ui placeholder screen. Runs ONLY on the LVGL task
 * (called from setup_lvgl_task). Real first-use / settings screens
 * land in Phase 4; this just proves the no-brick session round trip
 * with a single centered label held for s_ph_hold_ms. Pumps
 * lv_timer_handler() in its loop (the Spike B fix) so the label is
 * actually serviced and the hold is honoured. */
static void setup_lvgl_draw_placeholder(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_make(0x10, 0x20, 0x40), 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, s_ph_text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);

    ESP_LOGI(TAG, "setup_ui placeholder: \"%s\" (~%d ms)",
             s_ph_text, s_ph_hold_ms);

    int step_ms = 30;
    int hold = (s_ph_hold_ms > 0) ? s_ph_hold_ms : 1;
    int steps = hold / step_ms;
    if (steps < 1) {
        steps = 1;
    }
    for (int i = 0; i < steps && s_task_run; i++) {
        lv_timer_handler();
        lv_refr_now(s_disp);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }

    /* Destroy the label while the display is still valid and we are
     * on the LVGL task, mirroring the Spike A/B clean-teardown
     * leak-gate discipline. */
    lv_obj_clean(lv_screen_active());
}

static void setup_lvgl_task(void *arg)
{
    (void)arg;
    while (s_task_run) {
        if (s_pattern_request) {
            s_pattern_request = false;
            setup_lvgl_draw_pattern();
            s_pattern_result = ESP_OK;
            xSemaphoreGive(s_pattern_done);
        }
        if (s_pattern_b_request) {
            s_pattern_b_request = false;
            setup_lvgl_draw_touch_pattern();
            s_pattern_result = ESP_OK;
            xSemaphoreGive(s_pattern_done);
        }
        if (s_pattern_ph_request) {
            s_pattern_ph_request = false;
            setup_lvgl_draw_placeholder();
            s_pattern_result = ESP_OK;
            xSemaphoreGive(s_pattern_done);
        }
        if (s_pattern_scr_request) {
            s_pattern_scr_request = false;
            /* The driver runs ON this LVGL task and owns its own
             * lv_timer_handler() + worker-drain loop. It is the one
             * place an interactive flow may call lv_* freely. It is
             * responsible for cleaning the screen before it returns
             * (leak-gate discipline, mirroring the patterns). */
            esp_err_t scr_ret = ESP_FAIL;
            if (s_scr_fn != NULL) {
                scr_ret = s_scr_fn(s_scr_ctx);
            } else {
                ESP_LOGE(TAG, "screen-flow request with NULL fn");
            }
            s_pattern_result = scr_ret;
            xSemaphoreGive(s_pattern_done);
        }
        uint32_t next_ms = lv_timer_handler();
        if (next_ms == LV_NO_TIMER_READY || next_ms > 10) {
            next_ms = 10;
        }
        vTaskDelay(pdMS_TO_TICKS(next_ms ? next_ms : 1));
    }
    xSemaphoreGive(s_task_exited);
    vTaskDelete(NULL);
}

esp_err_t setup_lvgl_port_start(esp_lcd_panel_handle_t panel,
                                esp_lcd_panel_io_handle_t io,
                                int w, int h, bool swap,
                                esp_lcd_touch_handle_t tp)
{
    (void)io;
    ESP_RETURN_ON_FALSE(panel && w > 0 && h > 0, ESP_ERR_INVALID_ARG, TAG,
                        "bad args");

    s_panel = panel;
    s_w = w;
    s_h = h;
    s_swap = swap;
    s_task_run = false;
    /* Per-session touch indev state starts clean every start; the
     * indev is (re)created below in the pre-task race-free window and
     * torn down in setup_lvgl_port_stop(). */
    s_touch_tp = NULL;
    s_touch_indev = NULL;

    if (!s_lv_inited) {
        lv_init();
        s_lv_inited = true;
    }

    size_t buf_px = (size_t)w * SETUP_LVGL_BUF_LINES;
    size_t buf_bytes = buf_px * sizeof(uint16_t);
    s_buf1 = heap_caps_malloc(buf_bytes,
                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_buf2 = heap_caps_malloc(buf_bytes,
                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf1 || !s_buf2) {
        ESP_LOGE(TAG, "internal DMA draw buffer alloc failed (%u B x2)",
                 (unsigned)buf_bytes);
        goto fail;
    }

    s_disp = lv_display_create(w, h);
    if (!s_disp) {
        ESP_LOGE(TAG, "lv_display_create failed");
        goto fail;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, setup_lvgl_flush_cb);
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Create the touch indev HERE — after the display exists but
     * STRICTLY BEFORE xTaskCreate(setup_lvgl_task): the same
     * proven-safe window as lv_display_create above. No other task
     * touches LVGL yet, so lv_indev_create()/set_* cannot race
     * lv_timer_handler() (LVGL v9 is not thread-safe). Doing this on
     * the caller task AFTER the LVGL task is live corrupted the global
     * indev/display lists and wedged the second session — that is the
     * bug this placement fixes. tp == NULL => run without touch (NOT a
     * failure); the caller owns any "requires touch" policy. */
    if (tp != NULL) {
        lv_indev_t *indev = lv_indev_create();
        if (indev == NULL) {
            ESP_LOGE(TAG, "lv_indev_create failed");
            goto fail;
        }
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, setup_lvgl_touch_read_cb);
        lv_indev_set_display(indev, s_disp);
        /* Publish the touch handle only after its owning indev exists
         * so the read_cb can never see s_touch_tp without a valid
         * s_touch_indev. */
        s_touch_tp = tp;
        s_touch_indev = indev;
        ESP_LOGI(TAG, "touch indev attached (tp=%p)", (void *)tp);
    } else {
        ESP_LOGW(TAG, "no touch handle");
    }

    s_pattern_done = xSemaphoreCreateBinary();
    s_task_exited = xSemaphoreCreateBinary();
    if (!s_pattern_done || !s_task_exited) {
        ESP_LOGE(TAG, "semaphore create failed");
        goto fail;
    }

    const esp_timer_create_args_t targs = {
        .callback = setup_lvgl_tick_cb,
        .name = "setup_lv_tick",
    };
    esp_err_t err = esp_timer_create(&targs, &s_tick_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_timer_start_periodic(s_tick_timer,
                                   SETUP_LVGL_TICK_PERIOD_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start failed: %s", esp_err_to_name(err));
        goto fail;
    }

    s_task_run = true;
    if (xTaskCreate(setup_lvgl_task, "setup_lvgl", SETUP_LVGL_TASK_STACK,
                    NULL, SETUP_LVGL_TASK_PRIO, &s_lvgl_task) != pdPASS) {
        ESP_LOGE(TAG, "LVGL task create failed");
        s_task_run = false;
        goto fail;
    }

    ESP_LOGI(TAG,
             "LVGL port started (%dx%d swap=%d, buf=%u B x2 internal DMA)",
             w, h, (int)swap, (unsigned)buf_bytes);
    return ESP_OK;

fail:
    setup_lvgl_port_stop();
    return ESP_FAIL;
}

esp_err_t setup_lvgl_port_run_rgb_pattern(void)
{
    if (!s_lvgl_task || !s_task_run) {
        ESP_LOGE(TAG, "run_rgb_pattern: port not running");
        return ESP_ERR_INVALID_STATE;
    }
    s_pattern_result = ESP_FAIL;
    s_pattern_request = true;
    /* Wait generously: pattern is ~8 s plus margins. */
    if (xSemaphoreTake(s_pattern_done,
                       pdMS_TO_TICKS(SETUP_LVGL_PATTERN_MS + 10000))
            != pdTRUE) {
        ESP_LOGE(TAG, "run_rgb_pattern: timed out");
        return ESP_ERR_TIMEOUT;
    }
    return s_pattern_result;
}

esp_err_t setup_lvgl_port_run_touch_pattern(void)
{
    if (!s_lvgl_task || !s_task_run) {
        ESP_LOGE(TAG, "run_touch_pattern: port not running");
        return ESP_ERR_INVALID_STATE;
    }
    s_pattern_result = ESP_FAIL;
    s_pattern_b_request = true;
    /* Wait generously: pattern is ~45 s plus margins. */
    if (xSemaphoreTake(s_pattern_done,
                       pdMS_TO_TICKS(SETUP_LVGL_TOUCH_PATTERN_MS + 15000))
            != pdTRUE) {
        ESP_LOGE(TAG, "run_touch_pattern: timed out");
        return ESP_ERR_TIMEOUT;
    }
    return s_pattern_result;
}

esp_err_t setup_lvgl_port_run_placeholder(const char *text, int hold_ms)
{
    if (!s_lvgl_task || !s_task_run) {
        ESP_LOGE(TAG, "run_placeholder: port not running");
        return ESP_ERR_INVALID_STATE;
    }
    s_ph_text[0] = '\0';
    if (text != NULL) {
        strlcpy(s_ph_text, text, sizeof(s_ph_text));
    }
    s_ph_hold_ms = (hold_ms > 0) ? hold_ms : 1;
    s_pattern_result = ESP_FAIL;
    s_pattern_ph_request = true;
    /* Wait the hold plus a generous margin. */
    if (xSemaphoreTake(s_pattern_done,
                       pdMS_TO_TICKS(s_ph_hold_ms + 10000)) != pdTRUE) {
        ESP_LOGE(TAG, "run_placeholder: timed out");
        return ESP_ERR_TIMEOUT;
    }
    return s_pattern_result;
}

esp_err_t setup_lvgl_port_run_screens(setup_lvgl_screens_fn_t fn,
                                      void *ctx)
{
    if (!s_lvgl_task || !s_task_run) {
        ESP_LOGE(TAG, "run_screens: port not running");
        return ESP_ERR_INVALID_STATE;
    }
    if (fn == NULL) {
        ESP_LOGE(TAG, "run_screens: NULL fn");
        return ESP_ERR_INVALID_ARG;
    }
    s_scr_fn = fn;
    s_scr_ctx = ctx;
    s_pattern_result = ESP_FAIL;
    s_pattern_scr_request = true;
    /* Interactive flow is human-paced: no fixed total duration while
     * it is making progress. But the wait is NOT portMAX_DELAY: a
     * driver that never returns (dead/absent touch, internal wedge)
     * must not pin the owner task — and therefore app_main — forever.
     * Re-loop on a finite tick timeout. While no stop has been
     * requested, keep waiting (human-paced). Once a stop IS requested
     * (setup_lvgl_port_should_stop()), give the driver a bounded grace
     * window to observe it and return cleanly; if it still has not, we
     * break with ESP_ERR_TIMEOUT and let setup_lvgl_port_stop()'s
     * leak-gate semantics take over (NO-GO, power-cycle) rather than
     * block forever. Single-LVGL-task contract and no-abort discipline
     * are preserved: we never touch lv_* here and never assert. */
    const TickType_t wait_slice = pdMS_TO_TICKS(200);
    const int stop_grace_slices = 25; /* ~5 s after a stop request */
    int stop_waited = 0;
    for (;;) {
        if (xSemaphoreTake(s_pattern_done, wait_slice) == pdTRUE) {
            return s_pattern_result;
        }
        if (setup_lvgl_port_should_stop()) {
            if (++stop_waited >= stop_grace_slices) {
                ESP_LOGE(TAG,
                         "run_screens: driver did not return within "
                         "the stop grace window — giving up to avoid "
                         "an infinite owner-task block (NO-GO)");
                return ESP_ERR_TIMEOUT;
            }
        }
        /* else: no stop requested yet — human-paced, keep waiting. */
    }
}

bool setup_lvgl_port_should_stop(void)
{
    /* True once setup_lvgl_port_stop() cleared the run flag. The
     * interactive driver polls this so a wedged flow unwinds instead
     * of pinning the owner task forever (no infinite app_main block).
     * s_task_run is volatile; a plain read is the intended signal. */
    return !s_task_run;
}

#define SETUP_LVGL_STOP_TIMEOUT_MS 2000

esp_err_t setup_lvgl_port_stop(void)
{
    /* Stop the LVGL task and wait for it to actually exit so no
     * lv_* / flush call is in flight while we free buffers. */
    bool task_exited = true;  /* nothing to wait on => treat as exited */
    if (s_lvgl_task) {
        s_task_run = false;
        if (s_task_exited) {
            task_exited = (xSemaphoreTake(
                               s_task_exited,
                               pdMS_TO_TICKS(SETUP_LVGL_STOP_TIMEOUT_MS))
                           == pdTRUE);
        }
        s_lvgl_task = NULL;
    }
    s_task_run = false;

    if (!task_exited) {
        /* The LVGL task did NOT exit in time. It may still be inside
         * lv_timer_handler / the flush cb, touching s_disp, s_buf1,
         * s_buf2 and the tick timer. Freeing/deleting any of those now
         * would be a use-after-free that corrupts the diagnosis of the
         * NEXT spike run (exactly the wedged-flush mode Spike A exists
         * to detect). Choose leaking-but-safe over UAF: do NOT
         * lv_display_delete, do NOT heap_caps_free the draw buffers,
         * do NOT delete the tick timer (it may still be firing into a
         * live task). Latch the failure, drop only the caller-facing
         * task handle (already NULLed above) and the panel pointer (we
         * no longer own it; the live flush cb will see NULL and skip
         * the draw, which is the safe degradation), and return an
         * error so the operator power-cycles before trusting anything. */
        s_stop_timed_out = true;
        ESP_LOGE(TAG,
                 "setup_lvgl_port_stop: LVGL task did not exit within "
                 "%d ms — LEAKING draw buffers/display/tick-timer to "
                 "avoid a use-after-free into the still-live task. This "
                 "run is a NO-GO. The board MUST be power-cycled before "
                 "another setup spike run; results of subsequent runs "
                 "WITHOUT a power cycle are UNTRUSTWORTHY.",
                 SETUP_LVGL_STOP_TIMEOUT_MS);
        s_panel = NULL;
        return ESP_ERR_TIMEOUT;
    }

    /* Normal path: the task has provably exited; nothing is in flight,
     * so the full clean teardown is safe. */
    if (s_tick_timer) {
        esp_timer_stop(s_tick_timer);
        esp_timer_delete(s_tick_timer);
        s_tick_timer = NULL;
    }

    /* Delete the touch indev BEFORE the display it references.
     * Guarded for the not-attached case so Spike A's stop path is
     * unaffected. */
    if (s_touch_indev) {
        lv_indev_delete(s_touch_indev);
        s_touch_indev = NULL;
    }
    s_touch_tp = NULL;

    if (s_disp) {
        lv_display_delete(s_disp);
        s_disp = NULL;
    }

    if (s_buf1) {
        heap_caps_free(s_buf1);
        s_buf1 = NULL;
    }
    if (s_buf2) {
        heap_caps_free(s_buf2);
        s_buf2 = NULL;
    }

    if (s_pattern_done) {
        vSemaphoreDelete(s_pattern_done);
        s_pattern_done = NULL;
    }
    if (s_task_exited) {
        vSemaphoreDelete(s_task_exited);
        s_task_exited = NULL;
    }

    s_panel = NULL;
    s_pattern_request = false;
    s_pattern_b_request = false;
    s_pattern_ph_request = false;
    s_pattern_scr_request = false;
    s_scr_fn = NULL;
    s_scr_ctx = NULL;
    ESP_LOGI(TAG, "LVGL port stopped");
    return ESP_OK;
}
