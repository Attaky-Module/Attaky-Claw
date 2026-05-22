/*
 * setup_worker implementation.
 *
 * One FreeRTOS task drains a bounded job queue and runs each job
 * (blocking I/O lives here, NEVER on the LVGL task). Jobs publish
 * completion records onto a second bounded queue; the LVGL task
 * drains that queue from its own loop. The worker task never calls
 * any lv_* / esp_lcd_* draw API — the single-LVGL-task model in
 * setup_lvgl_port.c stays intact.
 *
 * Fail-soft contract: every path logs + returns an esp_err_t. No
 * ESP_ERROR_CHECK / abort / assert anywhere.
 */

#include "setup_worker.h"

#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "setup_worker";

/* Bounded depths: enough to absorb a small burst without stalling
 * the LVGL task on submit, small enough that a wedged drain is
 * caught (post_result fails) instead of growing unbounded. */
#define SETUP_WORKER_JOB_QUEUE_DEPTH    6
#define SETUP_WORKER_RESULT_QUEUE_DEPTH 6

/* Worker does blocking I/O; 4 KB is the floor, give headroom. */
#define SETUP_WORKER_TASK_STACK         (5 * 1024)

/* LVGL task = SETUP_LVGL_TASK_PRIO (2); emote engine task = 3. The
 * worker spends almost all its time blocked in I/O and yields
 * readily, so it cannot starve the prio-2 LVGL task even at 3. We
 * match the established emote-engine convention (3) rather than
 * inventing a new level; the worker being >= LVGL only matters while
 * it is runnable, which is brief and self-yielding. */
#define SETUP_WORKER_TASK_PRIO          3

/* Bounded waits. Submit must not stall the LVGL task: short cap then
 * give up with an error. Stop waits a little for the task to exit so
 * queues aren't deleted under a live job. */
#define SETUP_WORKER_SUBMIT_TIMEOUT_MS  50
#define SETUP_WORKER_POST_TIMEOUT_MS    0   /* never block a job's post */
#define SETUP_WORKER_STOP_TIMEOUT_MS    3000

typedef struct {
    setup_worker_fn_t fn;
    void *arg;
} setup_worker_job_t;

/* These handles are read/written WITHOUT a lock. That is sound only
 * under the single-owner-task contract documented in setup_worker.h:
 * every public setup_worker_* call (start/stop/submit/post_result/
 * set_result_cb/poll_results/try_get_result/is_running) is serialized
 * on one owner task. A concurrent stop() (which deletes+NULLs these)
 * vs submit/post/poll from another task is a stale-handle UAF and is
 * undefined — do not add a mutex here, fix the caller. */
static QueueHandle_t s_job_q;
static QueueHandle_t s_result_q;
static TaskHandle_t s_task;
static volatile bool s_run;
static SemaphoreHandle_t s_task_exited;

/* Result callback is module-global so it can be set independent of
 * start/stop. Only ever read/written from the LVGL task (set + poll)
 * in normal use; no extra lock is taken (matches the project's
 * single-consumer convention for these hooks). */
static setup_worker_result_cb_t s_result_cb;
static void *s_result_cb_ctx;

static bool setup_worker_is_running(void)
{
    return s_run && s_task != NULL && s_job_q != NULL &&
           s_result_q != NULL;
}

static void setup_worker_task(void *arg)
{
    (void)arg;
    setup_worker_job_t job;
    while (s_run) {
        /* Wake periodically even with no work so a stop request is
         * observed promptly (the queue receive timeout bounds the
         * worst-case stop latency). */
        if (xQueueReceive(s_job_q, &job,
                          pdMS_TO_TICKS(100)) == pdTRUE) {
            if (job.fn != NULL) {
                job.fn(job.arg);
            } else {
                ESP_LOGE(TAG, "dropped job with NULL fn");
            }
        }
    }
    if (s_task_exited != NULL) {
        xSemaphoreGive(s_task_exited);
    }
    vTaskDelete(NULL);
}

esp_err_t setup_worker_start(void)
{
    if (setup_worker_is_running()) {
        ESP_LOGW(TAG, "already running");
        return ESP_OK;
    }

    s_job_q = xQueueCreate(SETUP_WORKER_JOB_QUEUE_DEPTH,
                           sizeof(setup_worker_job_t));
    s_result_q = xQueueCreate(SETUP_WORKER_RESULT_QUEUE_DEPTH,
                              sizeof(setup_worker_result_t));
    s_task_exited = xSemaphoreCreateBinary();
    if (s_job_q == NULL || s_result_q == NULL ||
        s_task_exited == NULL) {
        ESP_LOGE(TAG, "queue/semaphore create failed");
        goto fail;
    }

    s_run = true;
    if (xTaskCreate(setup_worker_task, "setup_worker",
                    SETUP_WORKER_TASK_STACK, NULL,
                    SETUP_WORKER_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        s_run = false;
        goto fail;
    }

    ESP_LOGI(TAG,
             "started (job_q=%d result_q=%d prio=%d stack=%d)",
             SETUP_WORKER_JOB_QUEUE_DEPTH,
             SETUP_WORKER_RESULT_QUEUE_DEPTH,
             SETUP_WORKER_TASK_PRIO, SETUP_WORKER_TASK_STACK);
    return ESP_OK;

fail:
    if (s_task_exited != NULL) {
        vSemaphoreDelete(s_task_exited);
        s_task_exited = NULL;
    }
    if (s_job_q != NULL) {
        vQueueDelete(s_job_q);
        s_job_q = NULL;
    }
    if (s_result_q != NULL) {
        vQueueDelete(s_result_q);
        s_result_q = NULL;
    }
    return ESP_FAIL;
}

esp_err_t setup_worker_stop(void)
{
    if (s_task == NULL && s_job_q == NULL && s_result_q == NULL &&
        s_task_exited == NULL) {
        /* Not running / already stopped: idempotent no-op.
         * s_task_exited is included so a partially-torn-down state
         * (handles cleared but semaphore still live, or vice versa)
         * still falls through to the real teardown below. */
        return ESP_OK;
    }

    bool task_exited = true;  /* nothing to wait on => treat as exited */
    if (s_task != NULL) {
        s_run = false;
        if (s_task_exited != NULL) {
            task_exited = (xSemaphoreTake(
                               s_task_exited,
                               pdMS_TO_TICKS(SETUP_WORKER_STOP_TIMEOUT_MS))
                           == pdTRUE);
        }
        s_task = NULL;
    }
    s_run = false;

    if (!task_exited) {
        /* The worker task did not exit in time. It may still be
         * inside a job touching the queues. Deleting them now would
         * be a use-after-free. Fail-soft: leave the queues, latch
         * the error, do not abort. */
        ESP_LOGE(TAG,
                 "worker task did not exit within %d ms — leaving "
                 "queues intact to avoid a use-after-free",
                 SETUP_WORKER_STOP_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    /* Task provably gone: nothing is in flight, safe to delete. */
    if (s_job_q != NULL) {
        vQueueDelete(s_job_q);
        s_job_q = NULL;
    }
    if (s_result_q != NULL) {
        vQueueDelete(s_result_q);
        s_result_q = NULL;
    }
    if (s_task_exited != NULL) {
        vSemaphoreDelete(s_task_exited);
        s_task_exited = NULL;
    }
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

esp_err_t setup_worker_submit(setup_worker_fn_t fn, void *arg)
{
    if (fn == NULL) {
        ESP_LOGE(TAG, "submit: NULL fn");
        return ESP_ERR_INVALID_ARG;
    }
    if (!setup_worker_is_running()) {
        ESP_LOGE(TAG, "submit: worker not running");
        return ESP_ERR_INVALID_STATE;
    }

    setup_worker_job_t job = { .fn = fn, .arg = arg };
    if (xQueueSend(s_job_q, &job,
                   pdMS_TO_TICKS(SETUP_WORKER_SUBMIT_TIMEOUT_MS))
            != pdTRUE) {
        ESP_LOGE(TAG, "submit: job queue full (dropped)");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t setup_worker_post_result(const setup_worker_result_t *result)
{
    if (result == NULL) {
        ESP_LOGE(TAG, "post_result: NULL result");
        return ESP_ERR_INVALID_ARG;
    }
    if (!setup_worker_is_running()) {
        ESP_LOGE(TAG, "post_result: worker not running");
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_result_q, result,
                   pdMS_TO_TICKS(SETUP_WORKER_POST_TIMEOUT_MS))
            != pdTRUE) {
        ESP_LOGE(TAG, "post_result: result queue full (dropped)");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void setup_worker_set_result_cb(setup_worker_result_cb_t cb, void *ctx)
{
    s_result_cb = cb;
    s_result_cb_ctx = ctx;
}

bool setup_worker_try_get_result(setup_worker_result_t *out)
{
    if (out == NULL || s_result_q == NULL) {
        return false;
    }
    return xQueueReceive(s_result_q, out, 0) == pdTRUE;
}

void setup_worker_poll_results(void)
{
    if (s_result_q == NULL) {
        return;
    }
    setup_worker_result_t r;
    /* Bounded by the queue depth: every iteration removes one entry
     * and nothing on the LVGL task adds to it, so this terminates
     * without blocking. */
    while (xQueueReceive(s_result_q, &r, 0) == pdTRUE) {
        if (s_result_cb != NULL) {
            s_result_cb(&r, s_result_cb_ctx);
        }
    }
}
