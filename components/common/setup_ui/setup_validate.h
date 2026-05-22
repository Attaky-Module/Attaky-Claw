#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { SETUP_BACKEND_OPENAI, SETUP_BACKEND_ANTHROPIC, SETUP_BACKEND_CUSTOM, SETUP_BACKEND_COUNT } setup_backend_t;

/* Persisted llm_backend_type values. These MUST stay byte-equal to
 * claw_core's accepted backend ids (the runtime authoritative list):
 *   CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_ID  in
 *     components/claw_modules/claw_core/src/llm/backends/claw_llm_backend_openai_compatible.h
 *   CLAW_LLM_BACKEND_ANTHROPIC_ID          in
 *     .../claw_llm_backend_anthropic.h
 * app_config.c presets use the same strings. claw_llm_find_backend_
 * registration() is the runtime authority; if a new builtin backend
 * is added there, add it here too. A backend_type not in this set
 * makes app_claw_start() abort -> first-use boot brick. */
#define SETUP_LLM_BACKEND_OPENAI_COMPATIBLE    "openai_compatible"
#define SETUP_LLM_BACKEND_ANTHROPIC_COMPATIBLE "anthropic_compatible"

/* 320 = generous vs WPA2 SSID 32 / pwd 63; mirrors app_config string sizing. */
#define SETUP_STR_LEN 320
#define SETUP_BACKEND_ID_LEN 32
#define SETUP_MODEL_LEN 64

typedef struct {
    char wifi_ssid[SETUP_STR_LEN];
    char wifi_password[SETUP_STR_LEN];
    char llm_backend_type[SETUP_BACKEND_ID_LEN];
    char llm_base_url[SETUP_STR_LEN];
    char llm_model[SETUP_MODEL_LEN];
    char llm_api_key[SETUP_STR_LEN];
} setup_fields_t;

/* Resolve a persisted id back to a setup_backend_t. NOTE: OpenAI and
 * Custom share the persisted id SETUP_LLM_BACKEND_OPENAI_COMPATIBLE
 * (claw has no distinct custom backend; "Custom" is just
 * OpenAI-compatible + a user-supplied base_url), so this is ambiguous
 * by id and returns the FIRST matching backend. It is a UI convenience
 * only and is NOT the validity authority — setup_validate_llm() gates
 * validity against the claw-accepted id set directly. */
bool setup_backend_from_id(const char *id, setup_backend_t *out);
/* Persisted llm_backend_type for a backend (a claw-accepted id). */
const char *setup_backend_id(setup_backend_t b);
/* Friendly dropdown label for a backend (UI text, never persisted). */
const char *setup_backend_label(setup_backend_t b);
/* ALWAYS writes base_url/model (empty string for CUSTOM or invalid b); never leaves buffers untouched. */
void setup_backend_defaults(setup_backend_t b, char *base_url, size_t bu_sz, char *model, size_t m_sz);
bool setup_validate_wifi(const setup_fields_t *f, const char **err);
bool setup_validate_llm(const setup_fields_t *f, const char **err);
bool setup_first_use_required(const setup_fields_t *f); /* true if ANY mandatory field missing */
