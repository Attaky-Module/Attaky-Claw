#include "setup_validate.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *label;     /* friendly dropdown text (UI only) */
    const char *id;        /* persisted llm_backend_type (claw id) */
    const char *base_url;
    const char *model;
} setup_backend_info_t;

/* Indexed by setup_backend_t. `label` is UI-only; `id` is the persisted
 * llm_backend_type and MUST be a claw-accepted id (see setup_validate.h).
 * OpenAI and Custom intentionally share SETUP_LLM_BACKEND_OPENAI_COMPATIBLE:
 * claw_core has no distinct "custom" backend — a custom endpoint IS
 * openai_compatible with a user-supplied base_url. The OpenAI vs Custom
 * distinction is therefore a UI-only convenience and is NOT preserved
 * across a persist round-trip (both persist as "openai_compatible");
 * that is correct and acceptable. CUSTOM has empty base_url/model so the
 * user supplies them. */
static const setup_backend_info_t k_backends[SETUP_BACKEND_COUNT] = {
    [SETUP_BACKEND_OPENAI]    = { "OpenAI",
                                  SETUP_LLM_BACKEND_OPENAI_COMPATIBLE,
                                  "https://api.openai.com/v1",
                                  "gpt-4o-mini" },
    [SETUP_BACKEND_ANTHROPIC] = { "Anthropic",
                                  SETUP_LLM_BACKEND_ANTHROPIC_COMPATIBLE,
                                  "https://api.anthropic.com/v1",
                                  "claude-3-5-sonnet" },
    [SETUP_BACKEND_CUSTOM]    = { "Custom (OpenAI-compatible)",
                                  SETUP_LLM_BACKEND_OPENAI_COMPATIBLE,
                                  "",
                                  "" },
};

bool setup_backend_from_id(const char *id, setup_backend_t *out)
{
    if (id == NULL) {
        return false;
    }
    for (int i = 0; i < SETUP_BACKEND_COUNT; i++) {
        if (strcmp(id, k_backends[i].id) == 0) {
            if (out != NULL) {
                *out = (setup_backend_t)i;
            }
            return true;
        }
    }
    return false;
}

const char *setup_backend_id(setup_backend_t b)
{
    if ((unsigned)b >= SETUP_BACKEND_COUNT) {
        return "";
    }
    return k_backends[b].id;
}

const char *setup_backend_label(setup_backend_t b)
{
    if ((unsigned)b >= SETUP_BACKEND_COUNT) {
        return "";
    }
    return k_backends[b].label;
}

void setup_backend_defaults(setup_backend_t b, char *base_url, size_t bu_sz,
                            char *model, size_t m_sz)
{
    const char *src_url = "";
    const char *src_model = "";
    if ((unsigned)b < SETUP_BACKEND_COUNT) {
        src_url = k_backends[b].base_url;
        src_model = k_backends[b].model;
    }
    if (base_url != NULL && bu_sz > 0) {
        snprintf(base_url, bu_sz, "%s", src_url);
    }
    if (model != NULL && m_sz > 0) {
        snprintf(model, m_sz, "%s", src_model);
    }
}

/* SSID 1..32 bytes; password open (0) or WPA2 8..63 bytes. */
bool setup_validate_wifi(const setup_fields_t *f, const char **err)
{
    if (f == NULL) {
        if (err != NULL) {
            *err = "no fields";
        }
        return false;
    }

    /* 802.11 SSID is an opaque <=32-byte field; byte length (strlen) is correct, not codepoints. */
    size_t ssid_len = strlen(f->wifi_ssid);
    if (f->wifi_ssid[0] == '\0') {
        if (err != NULL) {
            *err = "SSID is empty";
        }
        return false;
    }
    if (ssid_len > 32) {
        if (err != NULL) {
            *err = "SSID too long (max 32 bytes)";
        }
        return false;
    }

    size_t pw_len = strlen(f->wifi_password);
    if (pw_len != 0 && (pw_len < 8 || pw_len > 63)) {
        if (err != NULL) {
            *err = "WiFi password must be empty or 8-63 bytes";
        }
        return false;
    }

    return true;
}

/* backend_type EXACTLY one of the claw-accepted ids; base_url non-empty +
 * starts with "http"; model + api_key non-empty. The backend membership
 * test (against SETUP_LLM_BACKEND_{OPENAI,ANTHROPIC}_COMPATIBLE, the
 * claw runtime authoritative list mirrored in setup_validate.h) is the brick-closing
 * gate: SAVE only commits after this passes, so a claw-toxic
 * backend_type (e.g. "openai"/"anthropic"/"custom"/"") can never be
 * persisted and app_claw_start() can never abort -> no first-use brick.
 * This is deliberately NOT setup_backend_from_id()-based (that is
 * UI-ambiguous and not the validity authority). */
bool setup_validate_llm(const setup_fields_t *f, const char **err)
{
    if (f == NULL) {
        if (err != NULL) {
            *err = "no fields";
        }
        return false;
    }

    if (strcmp(f->llm_backend_type,
               SETUP_LLM_BACKEND_OPENAI_COMPATIBLE) != 0 &&
        strcmp(f->llm_backend_type,
               SETUP_LLM_BACKEND_ANTHROPIC_COMPATIBLE) != 0) {
        if (err != NULL) {
            *err = "backend must be openai_compatible or "
                   "anthropic_compatible";
        }
        return false;
    }

    /* Loose guard: prefix "http" only (rejects ftp/empty). Not a full URL/scheme validator. */
    if (f->llm_base_url[0] == '\0' || strncmp(f->llm_base_url, "http", 4) != 0) {
        if (err != NULL) {
            *err = "base_url must start with http";
        }
        return false;
    }

    if (f->llm_model[0] == '\0') {
        if (err != NULL) {
            *err = "model is empty";
        }
        return false;
    }

    if (f->llm_api_key[0] == '\0') {
        if (err != NULL) {
            *err = "api_key is empty";
        }
        return false;
    }

    return true;
}

/* First-use setup is required when ANY of the 5 mandatory fields is empty.
   wifi_password is intentionally NOT mandatory (open networks). NULL -> true
   (safe default: assume setup needed). */
bool setup_first_use_required(const setup_fields_t *f)
{
    if (f == NULL) {
        return true;
    }
    if (f->wifi_ssid[0] == '\0' ||
        f->llm_backend_type[0] == '\0' ||
        f->llm_base_url[0] == '\0' ||
        f->llm_model[0] == '\0' ||
        f->llm_api_key[0] == '\0') {
        return true;
    }
    return false;
}
