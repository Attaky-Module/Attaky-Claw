#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "setup_sm.h"

/* Setup-UI draft model: owns the editable working copy ("draft") of the
 * device configuration while a setup session is open, and commits it.
 *
 * Ownership / layering rationale
 * ------------------------------
 * The live, running configuration is owned EXCLUSIVELY by app_main as two
 * heap objects reached through file-static pointers (`s_config` /
 * `s_claw_config`). setup_model deliberately does NOT take a second owning
 * copy of that state: double-owning the live config would be a latent
 * desync bug. Instead setup_model keeps only a transient `draft` (its own
 * editable scratch) plus a NON-owning binding to the caller's live objects,
 * registered once via setup_model_bind_live_config(). The binding is a
 * borrowed pointer pair; setup_model never frees it. The bound pointers
 * are borrowed and non-owning: app_main allocates the live config/claw
 * objects on the heap and may free them (e.g. app_free_runtime_state()
 * free()s and NULLs s_config / s_claw_config in any runtime-state
 * teardown). setup_model does NOT track that lifetime, so the OWNER MUST
 * call setup_model_bind_live_config(NULL, NULL) BEFORE freeing the live
 * config/claw objects. See setup_model_bind_live_config() for the exact
 * contract.
 *
 * Codex M8 (post-save in-memory refresh)
 * --------------------------------------
 * setup_model_commit() persists `draft` via app_config_save() and then,
 * if a live binding is registered, re-derives the in-memory state from
 * NVS: app_config_load(live_cfg) followed by app_config_to_claw(live_cfg,
 * live_claw). This makes the running system reflect saved values without
 * depending on a reboot (first-use still reboots in a later task, but the
 * model layer satisfies M8 on its own). The refresh outcome is reported
 * via a tri-state return (see setup_model_commit()): ESP_OK only when the
 * save AND the in-memory refresh both succeeded; ESP_ERR_INVALID_STATE
 * (non-fatal, "reboot to apply") when the save succeeded but no binding
 * was registered or the post-save reload failed. On reload failure the
 * dirty baseline is deliberately NOT reset, so a partial reload does not
 * masquerade as a clean, fully-applied state. The model never bricks and
 * stays usable in isolation.
 *
 * SETTINGS-mode "copy of s_config"
 * --------------------------------
 * The plan says SETTINGS mode seeds the draft from `s_config`. setup_model
 * cannot reach app_main's static; instead it uses app_config_load(&draft).
 * At steady state these are equivalent: only the setup flow ever writes
 * config (via setup_model_commit, which re-loads s_config from the same
 * NVS), so NVS == s_config. app_config_load also layers defaults under the
 * persisted values, exactly as app_main built s_config at boot.
 *
 * The dirty-diff helper is intentionally pure (no ESP / app_config / I/O)
 * so it is host-unit-testable on its own. */

typedef enum {
    SETUP_MODEL_MODE_FIRST_USE,
    SETUP_MODEL_MODE_SETTINGS
} setup_model_mode_t;

/* Pure: true iff the first `len` bytes of `a` and `b` differ. NULL-safe
 * (any NULL or len==0 -> false / "not dirty"). No ESP types, no I/O;
 * host-unit-tested. setup_model uses this over sizeof(app_config_t) to
 * decide whether the draft diverged from the loaded baseline. */
bool setup_model_buf_dirty(const void *a, const void *b, size_t len);

/* Map the setup-UI state-machine mode onto the model mode. Pure. */
setup_model_mode_t setup_model_mode_from_sm(setup_sm_mode_t sm_mode);

/* Register the caller-owned live config objects so setup_model_commit()
 * can refresh them after a save (Codex M8). Call once from the owner
 * (app_main) before any session. Safe to call before/without
 * setup_model_init(). The live objects are app_config_t* /
 * app_claw_config_t*; taken as void* so this header stays usable from
 * non-ESP TUs (e.g. host tests).
 *
 * Lifetime contract: the bound pointers are borrowed and non-owning. The
 * OWNER MUST call setup_model_bind_live_config(NULL, NULL) BEFORE freeing
 * the live config/claw objects (e.g. in any runtime-state teardown).
 * After unbinding, setup_model_commit() safely degrades to save-only (no
 * in-memory refresh). Calling commit with stale/freed bound pointers is
 * undefined behavior -- unbind first. */
void setup_model_bind_live_config(void *live_cfg, void *live_claw);

/* Build the draft for `mode`. FIRST_USE -> app_config_load_defaults();
 * SETTINGS -> app_config_load(&draft) (== current s_config, see header
 * note). Returns ESP_OK / esp_err_t (declared int here to keep the header
 * usable from non-ESP TUs; real ESP TUs get esp_err_t via setup_model.c). */
int setup_model_init(setup_model_mode_t mode);

/* Accessors for the live draft (NULL before setup_model_init()). The
 * concrete type is app_config_t; exposed as void* so screen code in
 * non-ESP unit context need not pull ESP headers. */
void *setup_model_draft(void);

/* true iff the draft differs from the baseline captured at init. */
bool setup_model_is_dirty(void);

/* Persist draft then refresh the bound live config (Codex M8).
 * Tri-state esp_err_t result (declared int here to keep the header
 * usable from non-ESP TUs):
 *   - app_config_save() itself failed -> returns that esp_err_t; the
 *     draft is untouched and nothing was persisted.
 *   - save OK, a live binding is present, and the post-save reload
 *     (app_config_load(live_cfg) + app_config_to_claw) succeeded ->
 *     returns ESP_OK. ESP_OK STRICTLY means fully persisted AND the
 *     in-memory live config was refreshed; the dirty baseline was reset.
 *   - save OK, but no live binding is registered OR the post-save reload
 *     failed -> returns ESP_ERR_INVALID_STATE. This is a NON-FATAL
 *     "saved to NVS successfully but the in-memory live config was NOT
 *     refreshed" signal: the caller should require a reboot to apply.
 *     The dirty baseline is NOT reset (setup_model_is_dirty() keeps
 *     reporting true) and the live claw is left as-is.
 * Never aborts/bricks: every path returns an esp_err_t. */
int setup_model_commit(void);
