#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Logical device-status states that drive the breathing status circle.
 *
 * This type is deliberately NOT named status_state_t: the vendored
 * emote engine already defines its own tag-less `status_state_t` enum
 * (managed_components/.../include/expression_emote/emote_api.h) with
 * the same enumerator spellings. components/common/emote/emote.c
 * includes that engine header, so a same-named type/enumerators here
 * would be a hard typedef/enumerator redefinition when the emote glue
 * (plan Task 4.3) includes both. We namespace ours as ss_state_t /
 * SS_STATE_* instead.
 *
 * Value order IS contractual: SS_STATE_* values must stay numerically
 * identical to the engine's status_state_t (NORMAL=0, RECORDING=1,
 * SPEAKING=2, NOTIFICATION=3) so Task 4.3 can map 1:1 with a plain
 * `(status_state_t)status_state_resolved()` cast at the single
 * emote_set_status_state() call site -- no translation table. The
 * host test asserts these exact values; do not reorder. status_state.c
 * also carries _Static_assert guards so a reorder fails the build.
 */
typedef enum {
    SS_STATE_NORMAL = 0,    /* default / fallback (blue) */
    SS_STATE_RECORDING,     /* ASR capture active (green) */
    SS_STATE_SPEAKING,      /* TTS playback (yellow) */
    SS_STATE_NOTIFICATION   /* inbound IM / scheduler reminder (red) */
} ss_state_t;

/* Pure-C status aggregator + precedence state machine.
 *
 * Precedence (resolved state = highest-priority active input):
 *   SPEAKING > RECORDING > NOTIFICATION > NORMAL
 * NORMAL is the resolved value when no input is active.
 *
 * Entering RECORDING or SPEAKING (set_recording(true) /
 * set_speaking(true)) also clears any pending NOTIFICATION, matching
 * design section 2: starting an interaction supersedes a pending alert.
 *
 * Threading: this is a single global aggregator with a single-writer
 * assumption. Per design section 6/section 11.4 the only writer is the single
 * router-level observer tap and the only reader/consumer is the
 * emote UI glue; it is NOT internally locked. Do not call the setters
 * concurrently from multiple threads.
 *
 * Single change-consumer by design: status_state_on_change() holds
 * exactly ONE callback slot, not a list. Task 4.3 needs both the
 * screen (emote) and the onboard LED to react to a resolved-state
 * change; that fan-out is owned by the Task 4.3 glue, which registers
 * a single trampoline here and dispatches to screen + LED itself. This
 * component intentionally does not grow an observer array -- keeping a
 * single slot keeps the aggregator trivially correct and host-testable.
 *
 * Re-entrancy: calling any status_state_* setter (or _reset) from
 * inside the change callback is UNSUPPORTED. The callback is invoked
 * with g_ss.resolved already committed; a setter re-entered from the
 * callback would recurse into ss_recompute() and is not guarded. The
 * Task 4.3 trampoline must only forward the resolved value to screen
 * and LED; it must not feed state back into this aggregator.
 *
 * NOTE: status_state_set_recording() / status_state_set_speaking()
 * are reserved for the future ASR/voice work (design section 2/section 9) and have
 * NO caller in the firmware today. They are validated here so the
 * state machine is correct when that phase lands.
 */

/* Set the RECORDING input on/off. Turning it on clears a pending
 * NOTIFICATION. Reserved for future ASR work  -  no caller today. */
void status_state_set_recording(bool on);

/* Set the SPEAKING input on/off. Turning it on clears a pending
 * NOTIFICATION. Reserved for future voice work  -  no caller today. */
void status_state_set_speaking(bool on);

/* Set the NOTIFICATION (pending) input. pending=false is equivalent
 * to status_state_notification_clear(). */
void status_state_set_notification(bool pending);

/* Clear a pending NOTIFICATION (item processed / device enters
 * interaction). No-op if nothing is pending. */
void status_state_notification_clear(void);

/* The currently resolved state per the precedence rule above. */
ss_state_t status_state_resolved(void);

/* Register a callback fired EXACTLY ONCE per actual resolved-state
 * change (never for a no-op or shadowed input change). cb may be NULL
 * to unregister. ctx is passed back verbatim. Single slot: a second
 * registration replaces the first (see "Single change-consumer by
 * design" above). Do NOT call status_state_* setters from within cb
 * (see "Re-entrancy" above). */
void status_state_on_change(void (*cb)(ss_state_t resolved, void *ctx),
                            void *ctx);

/* Reset all inputs to inactive (resolved -> NORMAL) and clear the
 * registered callback. Primarily for host unit tests; safe to call at
 * init. Does NOT fire the change callback. */
void status_state_reset(void);

#ifdef __cplusplus
}
#endif
