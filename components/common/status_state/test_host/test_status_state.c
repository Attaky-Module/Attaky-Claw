#include <stdbool.h>
#include <stdio.h>
#include "status_state.h"

static int g_failed = 0;

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) {                                                    \
            printf("FAIL: %s (line %d)\n", #expr, __LINE__);              \
            g_failed = 1;                                                 \
        }                                                                 \
    } while (0)

/* ---- callback recorder -------------------------------------------------- */

static int        g_cb_count;
static ss_state_t g_cb_last;
static void      *g_cb_last_ctx;

static void record_cb(ss_state_t resolved, void *ctx)
{
    g_cb_count++;
    g_cb_last = resolved;
    g_cb_last_ctx = ctx;
}

/* Reset the aggregator + recorder to a known NORMAL/no-callback baseline.
 * status_state_reset() returns the aggregator to all-inputs-inactive
 * (resolved == NORMAL) and clears the registered callback so each test
 * group starts clean without observing prior-group change callbacks. */
static void fresh(void)
{
    status_state_on_change(NULL, NULL);
    status_state_reset();
    g_cb_count = 0;
    g_cb_last = SS_STATE_NORMAL;
    g_cb_last_ctx = NULL;
}

int main(void)
{
    /* ---- enum value order must match emote_api.h status_state_t -------- */
    CHECK(SS_STATE_NORMAL == 0);
    CHECK(SS_STATE_RECORDING == 1);
    CHECK(SS_STATE_SPEAKING == 2);
    CHECK(SS_STATE_NOTIFICATION == 3);

    /* ---- NORMAL fallback / default ------------------------------------ */
    fresh();
    CHECK(status_state_resolved() == SS_STATE_NORMAL);

    /* ---- single-input resolves to that state -------------------------- */
    fresh();
    status_state_set_notification(true);
    CHECK(status_state_resolved() == SS_STATE_NOTIFICATION);

    fresh();
    status_state_set_recording(true);
    CHECK(status_state_resolved() == SS_STATE_RECORDING);

    fresh();
    status_state_set_speaking(true);
    CHECK(status_state_resolved() == SS_STATE_SPEAKING);

    /* ---- precedence: SPEAKING > RECORDING > NOTIFICATION > NORMAL ------ */
    /* NOTIFICATION vs RECORDING -> RECORDING (entering RECORDING also
     * clears the pending notification, so it stays RECORDING). */
    fresh();
    status_state_set_notification(true);
    status_state_set_recording(true);
    CHECK(status_state_resolved() == SS_STATE_RECORDING);

    /* RECORDING vs SPEAKING -> SPEAKING */
    fresh();
    status_state_set_recording(true);
    status_state_set_speaking(true);
    CHECK(status_state_resolved() == SS_STATE_SPEAKING);

    /* all three active -> SPEAKING (highest) */
    fresh();
    status_state_set_recording(true);
    status_state_set_speaking(true);
    status_state_set_notification(true); /* shadowed by SPEAKING; stays pending, never surfaces */
    CHECK(status_state_resolved() == SS_STATE_SPEAKING);

    /* SPEAKING off with RECORDING still on -> RECORDING */
    fresh();
    status_state_set_recording(true);
    status_state_set_speaking(true);
    status_state_set_speaking(false);
    CHECK(status_state_resolved() == SS_STATE_RECORDING);

    /* RECORDING off with nothing else -> NORMAL */
    fresh();
    status_state_set_recording(true);
    status_state_set_recording(false);
    CHECK(status_state_resolved() == SS_STATE_NORMAL);

    /* ---- entering RECORDING clears a pending NOTIFICATION ------------- */
    fresh();
    status_state_set_notification(true);
    CHECK(status_state_resolved() == SS_STATE_NOTIFICATION);
    status_state_set_recording(true);
    status_state_set_recording(false);
    /* notification was cleared on RECORDING entry -> back to NORMAL,
     * NOT NOTIFICATION. */
    CHECK(status_state_resolved() == SS_STATE_NORMAL);

    /* ---- entering SPEAKING clears a pending NOTIFICATION -------------- */
    fresh();
    status_state_set_notification(true);
    status_state_set_speaking(true);
    status_state_set_speaking(false);
    CHECK(status_state_resolved() == SS_STATE_NORMAL);

    /* ---- notification_clear() clears a pending NOTIFICATION ----------- */
    fresh();
    status_state_set_notification(true);
    CHECK(status_state_resolved() == SS_STATE_NOTIFICATION);
    status_state_notification_clear();
    CHECK(status_state_resolved() == SS_STATE_NORMAL);

    /* notification_clear() with nothing pending is a harmless no-op */
    fresh();
    status_state_notification_clear();
    CHECK(status_state_resolved() == SS_STATE_NORMAL);

    /* set_notification(false) is equivalent to clearing it */
    fresh();
    status_state_set_notification(true);
    status_state_set_notification(false);
    CHECK(status_state_resolved() == SS_STATE_NORMAL);

    /* ---- callback fires exactly once per actual resolved change ------- */
    fresh();
    status_state_on_change(record_cb, NULL);
    status_state_set_notification(true);          /* NORMAL -> NOTIFICATION */
    CHECK(g_cb_count == 1);
    CHECK(g_cb_last == SS_STATE_NOTIFICATION);
    status_state_notification_clear();             /* NOTIFICATION -> NORMAL */
    CHECK(g_cb_count == 2);
    CHECK(g_cb_last == SS_STATE_NORMAL);

    /* ---- no callback on a no-op set (same input value) ---------------- */
    fresh();
    status_state_on_change(record_cb, NULL);
    status_state_set_recording(true);              /* NORMAL -> RECORDING */
    CHECK(g_cb_count == 1);
    status_state_set_recording(true);              /* already recording: no-op */
    CHECK(g_cb_count == 1);
    status_state_set_recording(false);             /* RECORDING -> NORMAL */
    CHECK(g_cb_count == 2);
    status_state_set_recording(false);             /* already off: no-op */
    CHECK(g_cb_count == 2);

    /* ---- de-dup: setting the same input twice -> one resolved, one cb - */
    fresh();
    status_state_on_change(record_cb, NULL);
    status_state_set_notification(true);
    status_state_set_notification(true);
    CHECK(g_cb_count == 1);
    CHECK(status_state_resolved() == SS_STATE_NOTIFICATION);

    /* ---- shadowed input change does NOT fire; later un-shadow does ---- */
    /* NOTIFICATION arriving while SPEAKING is ALREADY active must NOT
     * fire a change (resolved stays SPEAKING). The clear-on-entry rule
     * fires only on the false->true *entry* of RECORDING/SPEAKING; a
     * notification that arrives afterwards is NOT cleared. So when
     * SPEAKING ends the resolved state recomputes to NOTIFICATION and
     * fires exactly once for that un-shadow. */
    fresh();
    status_state_on_change(record_cb, NULL);
    status_state_set_speaking(true);               /* NORMAL -> SPEAKING (cb1) */
    CHECK(g_cb_count == 1);
    CHECK(g_cb_last == SS_STATE_SPEAKING);
    status_state_set_notification(true);           /* shadowed: no cb */
    CHECK(g_cb_count == 1);
    CHECK(status_state_resolved() == SS_STATE_SPEAKING);
    status_state_set_speaking(false);              /* SPEAKING -> NOTIFICATION (cb2) */
    CHECK(g_cb_count == 2);
    CHECK(g_cb_last == SS_STATE_NOTIFICATION);

    /* Same but RECORDING shadows a notification that arrived AFTER the
     * RECORDING entry, then RECORDING ends -> the notification is
     * revealed (it was never cleared, since it post-dated the entry). */
    fresh();
    status_state_on_change(record_cb, NULL);
    status_state_set_recording(true);              /* cb1: -> RECORDING */
    status_state_set_notification(true);           /* shadowed, not cleared */
    CHECK(g_cb_count == 1);
    status_state_set_recording(false);             /* cb2: -> NOTIFICATION */
    CHECK(g_cb_count == 2);
    CHECK(g_cb_last == SS_STATE_NOTIFICATION);

    /* And the genuine clear-on-ENTRY case: notification pending FIRST,
     * then RECORDING entry clears it; ending RECORDING -> NORMAL. */
    fresh();
    status_state_on_change(record_cb, NULL);
    status_state_set_notification(true);           /* cb1: -> NOTIFICATION */
    status_state_set_recording(true);              /* cb2: -> RECORDING, notif cleared */
    CHECK(g_cb_count == 2);
    status_state_set_recording(false);             /* cb3: -> NORMAL */
    CHECK(g_cb_count == 3);
    CHECK(g_cb_last == SS_STATE_NORMAL);

    /* Un-shadow that DOES change: SPEAKING over an active NOTIFICATION is
     * not representable (entry clears it), so exercise RECORDING under
     * SPEAKING instead  -  ending SPEAKING reveals RECORDING. */
    fresh();
    status_state_on_change(record_cb, NULL);
    status_state_set_recording(true);              /* cb1: -> RECORDING */
    status_state_set_speaking(true);               /* cb2: -> SPEAKING */
    CHECK(g_cb_count == 2);
    status_state_set_speaking(false);              /* cb3: -> RECORDING */
    CHECK(g_cb_count == 3);
    CHECK(g_cb_last == SS_STATE_RECORDING);

    /* ---- callback can be unset, then re-set --------------------------- */
    fresh();
    status_state_on_change(record_cb, NULL);
    status_state_set_notification(true);           /* cb1 */
    CHECK(g_cb_count == 1);
    status_state_on_change(NULL, NULL);            /* unset */
    status_state_notification_clear();             /* change, but no cb */
    CHECK(g_cb_count == 1);
    status_state_on_change(record_cb, NULL);       /* re-set */
    status_state_set_notification(true);           /* cb2 */
    CHECK(g_cb_count == 2);
    CHECK(g_cb_last == SS_STATE_NOTIFICATION);

    /* ---- ctx is passed through verbatim -------------------------------- */
    fresh();
    {
        int sentinel = 0;
        status_state_on_change(record_cb, &sentinel);
        status_state_set_notification(true);
        CHECK(g_cb_count == 1);
        CHECK(g_cb_last_ctx == &sentinel);
    }

    if (g_failed) {
        puts("TESTS FAILED");
        return 1;
    }
    puts("ALL PASS");
    return 0;
}
