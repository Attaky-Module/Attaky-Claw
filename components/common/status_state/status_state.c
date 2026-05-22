#include <stddef.h>

#include "status_state.h"

/* Value-order contract guards (design section 2 / plan Task 4.3): the
 * SS_STATE_* values MUST stay numerically identical to the engine's
 * status_state_t so the Task 4.3 glue can do a plain
 * (status_state_t)status_state_resolved() cast with no translation
 * table. A reorder here fails the build at compile time. */
_Static_assert(SS_STATE_NORMAL == 0,       "SS_STATE_NORMAL must be 0");
_Static_assert(SS_STATE_RECORDING == 1,    "SS_STATE_RECORDING must be 1");
_Static_assert(SS_STATE_SPEAKING == 2,     "SS_STATE_SPEAKING must be 2");
_Static_assert(SS_STATE_NOTIFICATION == 3, "SS_STATE_NOTIFICATION must be 3");

/* Single global aggregator instance (single-writer; see status_state.h).
 * No heap, no ESP-IDF / LVGL / FreeRTOS deps  -  host-compilable. */
static struct {
    bool         recording;
    bool         speaking;
    bool         notification;
    ss_state_t   resolved;
    void       (*cb)(ss_state_t resolved, void *ctx);
    void        *cb_ctx;
} g_ss = {
    .recording    = false,
    .speaking     = false,
    .notification = false,
    .resolved     = SS_STATE_NORMAL,
    .cb           = NULL,
    .cb_ctx       = NULL,
};

/* Pure precedence function: SPEAKING > RECORDING > NOTIFICATION > NORMAL. */
static ss_state_t ss_compute(void)
{
    if (g_ss.speaking) {
        return SS_STATE_SPEAKING;
    }
    if (g_ss.recording) {
        return SS_STATE_RECORDING;
    }
    if (g_ss.notification) {
        return SS_STATE_NOTIFICATION;
    }
    return SS_STATE_NORMAL;
}

/* Recompute the resolved state; fire the callback only on an actual
 * change. Exactly-once is guaranteed by the single compare-and-store. */
static void ss_recompute(void)
{
    ss_state_t next = ss_compute();

    if (next == g_ss.resolved) {
        return;
    }
    g_ss.resolved = next;
    if (g_ss.cb != NULL) {
        g_ss.cb(next, g_ss.cb_ctx);
    }
}

void status_state_set_recording(bool on)
{
    if (g_ss.recording == on) {
        return; /* no-op: no input change, cannot change resolved state */
    }
    g_ss.recording = on;
    if (on) {
        /* Entering RECORDING supersedes a pending alert (design section 2). */
        g_ss.notification = false;
    }
    ss_recompute();
}

void status_state_set_speaking(bool on)
{
    if (g_ss.speaking == on) {
        return;
    }
    g_ss.speaking = on;
    if (on) {
        /* Entering SPEAKING supersedes a pending alert (design section 2). */
        g_ss.notification = false;
    }
    ss_recompute();
}

void status_state_set_notification(bool pending)
{
    if (g_ss.notification == pending) {
        return;
    }
    g_ss.notification = pending;
    ss_recompute();
}

void status_state_notification_clear(void)
{
    if (!g_ss.notification) {
        return;
    }
    g_ss.notification = false;
    ss_recompute();
}

ss_state_t status_state_resolved(void)
{
    return g_ss.resolved;
}

void status_state_on_change(void (*cb)(ss_state_t resolved, void *ctx),
                            void *ctx)
{
    g_ss.cb = cb;
    g_ss.cb_ctx = ctx;
}

void status_state_reset(void)
{
    g_ss.recording    = false;
    g_ss.speaking     = false;
    g_ss.notification = false;
    g_ss.resolved     = SS_STATE_NORMAL;
    g_ss.cb           = NULL;
    g_ss.cb_ctx       = NULL;
}
