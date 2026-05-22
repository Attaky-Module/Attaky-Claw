#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the minimal LVGL port for a Spike A display session.
 *
 * One-time lv_init(); allocates two partial draw buffers in
 * internal DMA-capable RAM (w*10 px each, RGB565, sized to the SPI
 * max_transfer_sz); creates an LVGL display whose flush
 * cb calls esp_lcd_panel_draw_bitmap() on @p panel; starts the
 * lv_tick esp_timer and the single LVGL service task.
 *
 * Exactly one FreeRTOS task ever calls lv_timer_handler. The caller
 * MUST NOT issue any lv_* call from another task; use
 * setup_lvgl_port_run_rgb_pattern() which marshals object creation
 * onto the LVGL task.
 *
 * If @p tp is non-NULL an LVGL pointer indev driven by that esp_lcd
 * touch panel is created HERE — before the single LVGL task is
 * spawned, the one window in which lv_indev_create() is provably
 * race-free (no other task touches LVGL yet, identical to the
 * lv_display_create above it). The read_cb runs on the LVGL task
 * (via lv_timer_handler), preserving the single-LVGL-task model. The
 * read_cb calls esp_lcd_touch_read_data() + esp_lcd_touch_get_
 * coordinates(); coordinates are fed straight through (the touch
 * driver already applies the board_devices.yaml swap_xy/mirror_x/
 * mirror_y flags inside get_coordinates — no software transform
 * here, that would double-transform). @p tp == NULL is allowed and
 * simply runs the port without touch (no failure); the caller is
 * responsible for any "requires touch" policy of its own.
 *
 * @param panel  esp_lcd panel handle (same one emote uses).
 * @param io     esp_lcd panel io handle (kept for symmetry; unused).
 * @param w      Display width in pixels.
 * @param h      Display height in pixels.
 * @param swap   RGB565 byte-swap (mirror emote_should_swap_color).
 * @param tp     esp_lcd touch handle from the board manager, or NULL
 *               for no touch (created here, before the LVGL task —
 *               the only race-free window for lv_indev_create()).
 * @return ESP_OK on success; an esp_err_t on failure (nothing left
 *         allocated/running on failure — caller need not call _stop).
 */
esp_err_t setup_lvgl_port_start(esp_lcd_panel_handle_t panel,
                                esp_lcd_panel_io_handle_t io,
                                int w, int h, bool swap,
                                esp_lcd_touch_handle_t tp);

/**
 * @brief Run the Spike A RGB test pattern.
 *
 * Marshals object creation onto the LVGL task: an immediate
 * full-screen solid fill (residual-DMA window probe), then full
 * RGB bars plus a box that steps across the screen for ~8 s.
 * Blocks the caller until the pattern completes. No lv_* calls
 * happen on the caller's task.
 *
 * @return ESP_OK on success, else an esp_err_t.
 */
esp_err_t setup_lvgl_port_run_rgb_pattern(void);

/**
 * @brief Run the Spike B 4-corner touch-coordinate test pattern.
 *
 * Marshalled onto the LVGL task exactly like the RGB pattern.
 * Draws four corner target squares at the true pixel corners and a
 * crosshair marker that tracks the registered touch indev for
 * ~45 s, logging "SPIKEB touch x=%d y=%d" (rate-limited) on press.
 * Blocks the caller until the pattern completes. No lv_* calls
 * happen on the caller's task.
 *
 * @return ESP_OK on success, else an esp_err_t.
 */
esp_err_t setup_lvgl_port_run_touch_pattern(void);

/**
 * @brief Run the minimal setup_ui placeholder screen.
 *
 * Marshalled onto the LVGL task exactly like the RGB / touch
 * patterns (same single-LVGL-task / single-caller-blocks contract,
 * no Spike A/B regression). Shows a single centered label with
 * @p text on a solid background for ~@p hold_ms, pumping
 * lv_timer_handler() in its loop (the Spike B fix), then cleans the
 * screen. Blocks the caller until the hold completes. No lv_* calls
 * happen on the caller's task.
 *
 * @param text     NUL-terminated label text (copied; may be NULL ->
 *                  empty).
 * @param hold_ms  How long to keep the placeholder on screen.
 * @return ESP_OK on success, else an esp_err_t.
 */
esp_err_t setup_lvgl_port_run_placeholder(const char *text, int hold_ms);

/**
 * @brief Driver callback for an interactive screen flow.
 *
 * Invoked exactly once, ON THE LVGL TASK, by
 * setup_lvgl_port_run_screens(). Because it runs on the single LVGL
 * task it is the ONE place an interactive flow may freely call lv_*
 * (build widgets, attach event callbacks) AND pump the event loop.
 *
 * The driver owns its loop: it MUST repeatedly call lv_timer_handler()
 * (so input/render/animation are serviced) and drain
 * setup_worker_poll_results() (so blocking work completes back onto
 * this task) until the flow is done, then return. It MUST also poll
 * setup_lvgl_port_should_stop() each iteration and return promptly
 * when it is true — its loop MUST NOT be able to run forever with no
 * exit other than success, or a dead/absent input pins the owner task
 * (and app_main) forever. It MUST NOT block the task on any
 * long/blocking call (wifi connect, scan, etc.) — those go through
 * setup_worker_submit(). It MUST clean up every object it created
 * (lv_obj_clean on the active screen) before returning so the
 * start/stop leak gate stays trustworthy, exactly like the built-in
 * patterns. It MUST NOT abort/assert; on a fatal internal error it
 * returns an esp_err_t and the no-brick unwind in the caller still
 * restores emote.
 *
 * @param ctx Opaque context forwarded unchanged from
 *            setup_lvgl_port_run_screens().
 * @return ESP_OK on a clean flow, else an esp_err_t.
 */
typedef esp_err_t (*setup_lvgl_screens_fn_t)(void *ctx);

/**
 * @brief Run an interactive screen flow on the LVGL task.
 *
 * Marshalled onto the single LVGL task exactly like the RGB / touch /
 * placeholder patterns (same single-LVGL-task / single-caller-blocks
 * contract — never triggered concurrently with them). Invokes @p fn
 * once on the LVGL task; @p fn builds the widgets and runs its own
 * event loop (pumping lv_timer_handler() + draining the setup_worker
 * result queue) until the flow completes, then cleans the screen and
 * returns. Blocks the caller until @p fn returns. No lv_* calls
 * happen on the caller's task.
 *
 * Fail-soft: a NULL @p fn or a port that is not running returns an
 * esp_err_t without aborting; whatever @p fn returns is propagated so
 * the caller can run its no-brick unwind.
 *
 * The wait is human-paced but NOT unbounded: while @p fn is making
 * progress (no teardown requested) the caller waits as long as needed
 * (an interactive flow has no fixed duration). It is, however,
 * cooperatively stoppable — @p fn MUST poll setup_lvgl_port_should_stop()
 * and return promptly once a stop is requested. The wait re-loops on a
 * finite tick timeout; once a stop has been requested it grants @p fn a
 * bounded grace window to return and, if @p fn still does not, returns
 * ESP_ERR_TIMEOUT and lets setup_lvgl_port_stop()'s leak-gate NO-GO
 * semantics take over rather than block the owner task (and app_main)
 * forever. A driver that never returns can therefore no longer brick
 * the device. Single-LVGL-task / single-caller-blocks contract is
 * unchanged (never triggered concurrently with the patterns).
 *
 * @param fn   Driver callback (must be non-NULL); runs on LVGL task.
 *             MUST poll setup_lvgl_port_should_stop() and exit when set.
 * @param ctx  Opaque context forwarded to @p fn unchanged.
 * @return ESP_OK if @p fn returned ESP_OK; ESP_ERR_TIMEOUT if a stop
 *         was requested and @p fn did not return within the grace
 *         window; else whatever @p fn returned.
 */
esp_err_t setup_lvgl_port_run_screens(setup_lvgl_screens_fn_t fn,
                                      void *ctx);

/**
 * @brief Whether a port teardown has been requested.
 *
 * Returns true once setup_lvgl_port_stop() has asked the LVGL task to
 * exit (the internal run flag has been cleared). An interactive driver
 * (setup_lvgl_screens_fn_t) MUST poll this from its own loop and return
 * promptly when it becomes true, so a wedged/never-completing flow can
 * no longer pin the owner task forever — it converges to the no-brick
 * unwind instead of an infinite block. Safe to call from the LVGL task
 * (the driver runs there) and from the owner task.
 *
 * @return true if a stop has been requested (driver must unwind), else
 *         false (keep running).
 */
bool setup_lvgl_port_should_stop(void);

/**
 * @brief Stop the LVGL port: stop the task + tick timer, free the
 * internal DMA draw buffers, delete the display. Safe to call after a
 * failed _run. lv_init() global state is intentionally left
 * initialized (lv_init is one-time / not re-entrant-safe to deinit
 * here); buffers and the display object are released.
 *
 * Repeated setup_lvgl_port_start()/setup_lvgl_port_stop() cycles — a
 * fresh display created and deleted on each cycle, on top of the
 * single persistent lv_init() — are EXPECTED and SUPPORTED, not just
 * tolerated: Task 1.4 runs the Spike A session 3x back-to-back to
 * measure the per-cycle heap/PSRAM delta as a leak gate. "lv_init is
 * one-shot" means we cannot deinit LVGL, NOT that the port runs only
 * once.
 *
 * @return ESP_OK on a clean teardown (task provably exited). If the
 *         LVGL task did not exit within the stop timeout, returns
 *         ESP_ERR_TIMEOUT: the draw buffers/display/tick-timer are
 *         intentionally LEAKED (never freed) to avoid a use-after-free
 *         into the still-live task. That run is a NO-GO and the board
 *         MUST be power-cycled before another spike run is trusted.
 */
esp_err_t setup_lvgl_port_stop(void);

#ifdef __cplusplus
}
#endif
