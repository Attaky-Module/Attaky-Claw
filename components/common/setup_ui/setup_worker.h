#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * setup_worker: a single FreeRTOS task + bounded job queue whose ONLY
 * purpose is to keep blocking work off the LVGL task.
 *
 * Threading contract (the whole reason this module exists):
 *
 *   - There is exactly ONE LVGL task in this firmware (see
 *     setup_lvgl_port.c). It calls lv_timer_handler() and owns ALL
 *     lv_* / esp_lcd_* draw API. Any blocking call on that task
 *     freezes input and the display — that is the failure this
 *     module prevents.
 *
 *   - A submitted job runs on the worker task, NEVER on the LVGL
 *     task and NEVER on the caller's task. The worker task is where
 *     blocking I/O (wifi connect, HTTP, NVS, etc.) belongs.
 *
 *   - The worker job function MUST NOT call any lv_* or esp_lcd_*
 *     draw API, and MUST NOT touch LVGL objects. The LVGL task owns
 *     the UI exclusively. A worker that needs the UI to change
 *     publishes a result; the LVGL task picks it up.
 *
 *   - Results flow worker -> LVGL one way only, through a bounded
 *     result queue. The LVGL task drains it from inside its existing
 *     loop via setup_worker_poll_results(), which NEVER blocks. The
 *     registered result callback therefore runs on the LVGL task and
 *     IS allowed to touch LVGL objects.
 *
 * Fail-soft everywhere: every entry point logs and returns an
 * esp_err_t. Nothing here aborts/asserts/ESP_ERROR_CHECKs.
 *
 * ============================================================
 * SINGLE-OWNER-TASK CONTRACT (load-bearing — read before use):
 * ============================================================
 *
 *   EVERY public setup_worker_* entry point — setup_worker_start(),
 *   setup_worker_stop(), setup_worker_submit(),
 *   setup_worker_post_result(), setup_worker_set_result_cb(),
 *   setup_worker_poll_results(), setup_worker_try_get_result(), and
 *   setup_worker_is_running() — MUST be called from, and serialized
 *   on, a SINGLE owning task: the LVGL / setup-UI owner task that
 *   drives this module's lifecycle. (Worker job functions calling
 *   setup_worker_post_result() from the worker task are the one
 *   sanctioned exception, and only ever concurrently with the owner
 *   draining results — that worker->LVGL handoff is the bounded
 *   result queue, which is the single point of cross-task contact.)
 *
 *   These functions are NOT mutually thread-safe. They share module
 *   state — the job/result queue handles, the task handle, the
 *   result-callback pair — that is read and written WITHOUT a lock
 *   by design. In particular, calling setup_worker_stop() (which
 *   deletes and NULLs the queue/task handles) on one task while
 *   another task is inside submit / post_result / poll_results /
 *   try_get_result / is_running is a stale-handle use-after-free and
 *   is UNDEFINED BEHAVIOR.
 *
 *   This is intentional: the module is single-owner by design, not
 *   guarded by a mutex. Correctness is the caller's responsibility —
 *   keep all of the above on the one owner task. Do not "fix" a race
 *   by adding a lock here; fix the caller to stop crossing tasks.
 */

/**
 * @brief A unit of blocking work, executed on the worker task.
 *
 * @param arg Opaque caller-owned pointer passed through unchanged.
 *            The worker does not free it; ownership/lifetime is the
 *            submitter's responsibility (it must outlive the job).
 *
 * The function runs on the worker task. It MUST NOT call lv_* /
 * esp_lcd_* draw API or touch LVGL objects. To affect the UI, post a
 * result with setup_worker_post_result() and let the LVGL task act
 * on it via the drained result callback.
 */
typedef void (*setup_worker_fn_t)(void *arg);

/**
 * @brief One completion record handed worker -> LVGL.
 *
 * Plain values only (no pointers the LVGL task must free). @c status
 * is the job's outcome; @c code is a job-defined detail (e.g. an
 * esp_err_t cast to int, or a small enum). @c arg echoes the arg the
 * job was submitted with so the LVGL side can correlate.
 */
typedef struct {
    esp_err_t status;
    int code;
    void *arg;
} setup_worker_result_t;

/**
 * @brief Result sink invoked on the LVGL task during result drain.
 *
 * Called by setup_worker_poll_results() (which runs on the LVGL
 * task), once per drained result. Because it runs on the LVGL task
 * it MAY touch LVGL objects. It MUST NOT block.
 *
 * @param result The completion record (by value; valid only for the
 *               duration of the call).
 * @param ctx    Opaque context registered with the callback.
 */
typedef void (*setup_worker_result_cb_t)(const setup_worker_result_t *result,
                                         void *ctx);

/**
 * @brief Create the worker task and its job/result queues.
 *
 * Idempotent: a second call while already running logs and returns
 * ESP_OK without creating a duplicate task. Fail-soft: on any
 * allocation/creation failure it cleans up partial state and returns
 * an esp_err_t (never aborts).
 *
 * Must NOT be called after setup_worker_stop() returned
 * ESP_ERR_TIMEOUT: that leaves the module WEDGED (see
 * setup_worker_stop()), and a start() from the wedged state would
 * spawn a duplicate worker task and compound the intentional leak.
 *
 * @return ESP_OK on success (or already running); else an esp_err_t.
 */
esp_err_t setup_worker_start(void);

/**
 * @brief Stop the worker task and delete its queues.
 *
 * Idempotent: safe to call when not running (returns ESP_OK). Asks
 * the worker task to exit, waits briefly for it to actually return
 * (so no job is in flight while queues are deleted), then deletes
 * both queues. Any still-queued, not-yet-started jobs are dropped.
 *
 * @return ESP_OK on a clean stop (or not running); ESP_ERR_TIMEOUT
 *         if the worker task did not exit in time (queues are then
 *         intentionally LEFT to avoid a use-after-free into the
 *         still-live task — fail-soft, no abort).
 *
 * On ESP_ERR_TIMEOUT the module is WEDGED: the worker task and both
 * queues are deliberately leaked (never freed) so the still-live
 * task cannot fault on freed handles. The module must NOT be
 * restarted after this — do not call setup_worker_start() again, as
 * it would spawn a second worker task and compound the leak.
 */
esp_err_t setup_worker_stop(void);

/**
 * @brief Enqueue a job to run on the worker task.
 *
 * Non-blocking-ish: waits at most a short, bounded time for queue
 * space, then gives up rather than stalling the caller (which may be
 * the LVGL task). The job runs later on the worker task, never on
 * the caller's task and never on the LVGL task.
 *
 * @param fn  Job function (must be non-NULL).
 * @param arg Opaque pointer passed to @p fn unchanged; the submitter
 *            owns its lifetime (must outlive job execution).
 * @return ESP_OK if enqueued; ESP_ERR_INVALID_STATE if the worker is
 *         not running; ESP_ERR_INVALID_ARG if @p fn is NULL;
 *         ESP_ERR_TIMEOUT if the job queue stayed full.
 */
esp_err_t setup_worker_submit(setup_worker_fn_t fn, void *arg);

/**
 * @brief Post a completion record from a worker job back to the LVGL
 *        task's drain.
 *
 * Intended to be called from inside a job function (worker task
 * context). Non-blocking enqueue onto the bounded result queue; if
 * the result queue is full the result is dropped with an error log
 * (the LVGL task is responsible for draining promptly via
 * setup_worker_poll_results()).
 *
 * @param result Completion record, copied by value into the queue.
 * @return ESP_OK if queued; ESP_ERR_INVALID_STATE if not running;
 *         ESP_ERR_INVALID_ARG if @p result is NULL; ESP_ERR_NO_MEM
 *         if the result queue was full (result dropped).
 */
esp_err_t setup_worker_post_result(const setup_worker_result_t *result);

/**
 * @brief Register the result callback (called on the LVGL task).
 *
 * Optional. If set, setup_worker_poll_results() invokes it once per
 * drained result. Pass NULL to clear. Safe to call before or after
 * setup_worker_start(); the registration is module-global.
 *
 * Per the single-owner-task contract above, this MUST be called on
 * the same single owning task as setup_worker_poll_results() /
 * setup_worker_try_get_result(). The (cb, ctx) pair is stored
 * non-atomically; a cross-task set vs a concurrent poll/drain can
 * tear (mismatched callback/context). Keep both on the one task.
 *
 * @param cb  Callback, or NULL to clear.
 * @param ctx Opaque context forwarded to @p cb on every call.
 */
void setup_worker_set_result_cb(setup_worker_result_cb_t cb, void *ctx);

/**
 * @brief Drain all pending results. Call from the LVGL task loop.
 *
 * NEVER blocks: pops every currently-queued result (bounded by the
 * queue depth) and, for each, invokes the registered result callback
 * on the calling (LVGL) task. Returns immediately if the queue is
 * empty or the worker is not running. The worker task never calls
 * this and never calls lv_*; this is the ONLY worker->LVGL handoff.
 */
void setup_worker_poll_results(void);

/**
 * @brief Pull a single result without invoking the callback.
 *
 * Non-blocking alternative to setup_worker_poll_results() for
 * callers that prefer to handle results inline in their loop.
 *
 * @param out Receives one result if available (must be non-NULL).
 * @return true if a result was dequeued into @p out; false if the
 *         queue was empty / worker not running / @p out NULL.
 */
bool setup_worker_try_get_result(setup_worker_result_t *out);

#ifdef __cplusplus
}
#endif
