#include <stdio.h>
#include <string.h>
#include "setup_validate.h"
#include "setup_sm.h"
#include "setup_model.h"
#include "setup_status_format.h"

static int g_failed = 0;

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) {                                                    \
            printf("FAIL: %s (line %d)\n", #expr, __LINE__);              \
            g_failed = 1;                                                 \
        }                                                                 \
    } while (0)

/* Fill f with a known-VALID set: valid ssid, empty (open) password since
   password is NOT a mandatory field, openai backend + its defaults, api key.
   Each first-use test copies this and clears exactly ONE field. */
static void fill_valid_fields(setup_fields_t *f)
{
    memset(f, 0, sizeof *f);
    snprintf(f->wifi_ssid, sizeof f->wifi_ssid, "%s", "HomeNet");
    snprintf(f->wifi_password, sizeof f->wifi_password, "%s", "");
    snprintf(f->llm_backend_type, sizeof f->llm_backend_type, "%s", "openai_compatible");
    snprintf(f->llm_base_url, sizeof f->llm_base_url, "%s", "https://api.openai.com/v1");
    snprintf(f->llm_model, sizeof f->llm_model, "%s", "gpt-4o-mini");
    snprintf(f->llm_api_key, sizeof f->llm_api_key, "%s", "sk-xxx");
}

int main(void)
{
    setup_backend_t b;

    /* Persisted ids are claw-accepted ids (the runtime authoritative list), NOT the
     * friendly UI labels. OpenAI and Custom share "openai_compatible"
     * (claw has no distinct custom backend). */
    CHECK(strcmp(setup_backend_id(SETUP_BACKEND_OPENAI),
                 "openai_compatible") == 0);
    CHECK(strcmp(setup_backend_id(SETUP_BACKEND_ANTHROPIC),
                 "anthropic_compatible") == 0);
    CHECK(strcmp(setup_backend_id(SETUP_BACKEND_CUSTOM),
                 "openai_compatible") == 0);

    /* Labels are distinct, friendly, UI-only (never persisted). */
    CHECK(strcmp(setup_backend_label(SETUP_BACKEND_OPENAI),
                 "OpenAI") == 0);
    CHECK(strcmp(setup_backend_label(SETUP_BACKEND_ANTHROPIC),
                 "Anthropic") == 0);
    CHECK(strcmp(setup_backend_label(SETUP_BACKEND_CUSTOM),
                 "Custom (OpenAI-compatible)") == 0);
    /* out-of-range -> "" (no crash) */
    CHECK(setup_backend_label(SETUP_BACKEND_COUNT)[0] == '\0');

    /* known persisted id -> true + correct enum (OpenAI/Custom share an
     * id; setup_backend_from_id returns the FIRST match = OPENAI). */
    CHECK(setup_backend_from_id("openai_compatible", &b) == true);
    CHECK(b == SETUP_BACKEND_OPENAI);
    CHECK(setup_backend_from_id("anthropic_compatible", &b) == true);
    CHECK(b == SETUP_BACKEND_ANTHROPIC);

    /* old/UI strings are NOT persisted ids -> false */
    CHECK(setup_backend_from_id("nope", &b) == false);
    CHECK(setup_backend_from_id("openai", &b) == false);
    CHECK(setup_backend_from_id("custom", &b) == false);

    /* OPENAI defaults: non-empty base_url containing https://, non-empty model */
    char bu[SETUP_STR_LEN];
    char model[SETUP_MODEL_LEN];
    setup_backend_defaults(SETUP_BACKEND_OPENAI, bu, sizeof bu, model, sizeof model);
    CHECK(bu[0] != '\0');
    CHECK(strstr(bu, "https://") != NULL);
    CHECK(model[0] != '\0');

    /* ANTHROPIC defaults differ from OPENAI */
    char abu[SETUP_STR_LEN];
    char amodel[SETUP_MODEL_LEN];
    setup_backend_defaults(SETUP_BACKEND_ANTHROPIC, abu, sizeof abu, amodel, sizeof amodel);
    CHECK(abu[0] != '\0');
    CHECK(strstr(abu, "https://") != NULL);
    CHECK(amodel[0] != '\0');
    CHECK(strcmp(abu, bu) != 0);
    CHECK(strcmp(amodel, model) != 0);

    /* CUSTOM yields empty defaults */
    char cbu[SETUP_STR_LEN];
    char cmodel[SETUP_MODEL_LEN];
    setup_backend_defaults(SETUP_BACKEND_CUSTOM, cbu, sizeof cbu, cmodel, sizeof cmodel);
    CHECK(cbu[0] == '\0');
    CHECK(cmodel[0] == '\0');

    /* safety contract: NULL out tolerated, returns true, no crash */
    CHECK(setup_backend_from_id("openai_compatible", NULL) == true);

    /* safety contract: tiny base_url buffer truncates and stays NUL-terminated */
    char tiny[4];
    setup_backend_defaults(SETUP_BACKEND_OPENAI, tiny, sizeof tiny, model, sizeof model);
    CHECK(tiny[sizeof tiny - 1] == '\0');
    CHECK(strlen(tiny) < sizeof tiny);

    /* safety contract: NULL base_url + 0 size tolerated, model still filled */
    char nmodel[SETUP_MODEL_LEN];
    nmodel[0] = '\0';
    setup_backend_defaults(SETUP_BACKEND_OPENAI, NULL, 0, nmodel, sizeof nmodel);
    CHECK(nmodel[0] != '\0');

    /* ---- Task 0.3: setup_validate_wifi ---- */
    {
        setup_fields_t f;
        const char *err;

        /* empty SSID -> false + err set */
        memset(&f, 0, sizeof f);
        snprintf(f.wifi_ssid, sizeof f.wifi_ssid, "%s", "");
        snprintf(f.wifi_password, sizeof f.wifi_password, "%s", "");
        err = NULL;
        CHECK(setup_validate_wifi(&f, &err) == false);
        CHECK(err != NULL);

        /* SSID 33 bytes -> false */
        memset(&f, 0, sizeof f);
        memset(f.wifi_ssid, 'a', 33);
        f.wifi_ssid[33] = '\0';
        err = NULL;
        CHECK(setup_validate_wifi(&f, &err) == false);
        CHECK(err != NULL);

        /* SSID exactly 32 bytes, empty password (open) -> true */
        memset(&f, 0, sizeof f);
        memset(f.wifi_ssid, 'a', 32);
        f.wifi_ssid[32] = '\0';
        CHECK(setup_validate_wifi(&f, NULL) == true);

        /* SSID 1 byte, password empty (open network) -> true */
        memset(&f, 0, sizeof f);
        snprintf(f.wifi_ssid, sizeof f.wifi_ssid, "%s", "x");
        snprintf(f.wifi_password, sizeof f.wifi_password, "%s", "");
        CHECK(setup_validate_wifi(&f, NULL) == true);

        /* password 7 bytes -> false (WPA2 min 8) */
        memset(&f, 0, sizeof f);
        snprintf(f.wifi_ssid, sizeof f.wifi_ssid, "%s", "mynet");
        memset(f.wifi_password, 'p', 7);
        f.wifi_password[7] = '\0';
        err = NULL;
        CHECK(setup_validate_wifi(&f, &err) == false);
        CHECK(err != NULL);

        /* password exactly 8 bytes -> true */
        memset(&f, 0, sizeof f);
        snprintf(f.wifi_ssid, sizeof f.wifi_ssid, "%s", "mynet");
        memset(f.wifi_password, 'p', 8);
        f.wifi_password[8] = '\0';
        CHECK(setup_validate_wifi(&f, NULL) == true);

        /* password exactly 63 bytes -> true */
        memset(&f, 0, sizeof f);
        snprintf(f.wifi_ssid, sizeof f.wifi_ssid, "%s", "mynet");
        memset(f.wifi_password, 'p', 63);
        f.wifi_password[63] = '\0';
        CHECK(setup_validate_wifi(&f, NULL) == true);

        /* password 64 bytes -> false (WPA2 max 63) */
        memset(&f, 0, sizeof f);
        snprintf(f.wifi_ssid, sizeof f.wifi_ssid, "%s", "mynet");
        memset(f.wifi_password, 'p', 64);
        f.wifi_password[64] = '\0';
        err = NULL;
        CHECK(setup_validate_wifi(&f, &err) == false);
        CHECK(err != NULL);

        /* valid SSID + valid password -> true */
        memset(&f, 0, sizeof f);
        snprintf(f.wifi_ssid, sizeof f.wifi_ssid, "%s", "HomeNet");
        snprintf(f.wifi_password, sizeof f.wifi_password, "%s", "secret123");
        CHECK(setup_validate_wifi(&f, NULL) == true);

        /* NULL fields -> false, no crash */
        err = NULL;
        CHECK(setup_validate_wifi(NULL, &err) == false);
    }

    /* ---- Task 0.3: setup_validate_llm ---- */
    {
        setup_fields_t f;
        const char *err;

        /* missing api_key -> false + err */
        memset(&f, 0, sizeof f);
        snprintf(f.llm_backend_type, sizeof f.llm_backend_type, "%s", "openai_compatible");
        snprintf(f.llm_base_url, sizeof f.llm_base_url, "%s", "https://api.openai.com/v1");
        snprintf(f.llm_model, sizeof f.llm_model, "%s", "gpt-4o-mini");
        snprintf(f.llm_api_key, sizeof f.llm_api_key, "%s", "");
        err = NULL;
        CHECK(setup_validate_llm(&f, &err) == false);
        CHECK(err != NULL);

        /* Spike D no-brick gate: a backend_type NOT in the claw-accepted
         * set must be REJECTED so it can never be persisted. These are
         * exactly the toxic strings the old setup_ui used to save and
         * which made app_claw_start() abort -> first-use boot brick. */
        {
            const char *toxic[] = { "openai", "anthropic", "custom",
                                    "bogus", "" };
            for (size_t i = 0; i < sizeof toxic / sizeof toxic[0]; i++) {
                memset(&f, 0, sizeof f);
                snprintf(f.llm_backend_type, sizeof f.llm_backend_type,
                         "%s", toxic[i]);
                snprintf(f.llm_base_url, sizeof f.llm_base_url, "%s",
                         "https://api.openai.com/v1");
                snprintf(f.llm_model, sizeof f.llm_model, "%s",
                         "gpt-4o-mini");
                snprintf(f.llm_api_key, sizeof f.llm_api_key, "%s",
                         "sk-abc");
                err = NULL;
                CHECK(setup_validate_llm(&f, &err) == false);
                CHECK(err != NULL);
            }
        }

        /* base_url without http (ftp) -> false */
        memset(&f, 0, sizeof f);
        snprintf(f.llm_backend_type, sizeof f.llm_backend_type, "%s", "openai_compatible");
        snprintf(f.llm_base_url, sizeof f.llm_base_url, "%s", "ftp://x");
        snprintf(f.llm_model, sizeof f.llm_model, "%s", "gpt-4o-mini");
        snprintf(f.llm_api_key, sizeof f.llm_api_key, "%s", "sk-abc");
        err = NULL;
        CHECK(setup_validate_llm(&f, &err) == false);
        CHECK(err != NULL);

        /* base_url empty -> false */
        memset(&f, 0, sizeof f);
        snprintf(f.llm_backend_type, sizeof f.llm_backend_type, "%s", "openai_compatible");
        snprintf(f.llm_base_url, sizeof f.llm_base_url, "%s", "");
        snprintf(f.llm_model, sizeof f.llm_model, "%s", "gpt-4o-mini");
        snprintf(f.llm_api_key, sizeof f.llm_api_key, "%s", "sk-abc");
        err = NULL;
        CHECK(setup_validate_llm(&f, &err) == false);
        CHECK(err != NULL);

        /* model empty -> false */
        memset(&f, 0, sizeof f);
        snprintf(f.llm_backend_type, sizeof f.llm_backend_type, "%s", "openai_compatible");
        snprintf(f.llm_base_url, sizeof f.llm_base_url, "%s", "https://api.openai.com/v1");
        snprintf(f.llm_model, sizeof f.llm_model, "%s", "");
        snprintf(f.llm_api_key, sizeof f.llm_api_key, "%s", "sk-abc");
        err = NULL;
        CHECK(setup_validate_llm(&f, &err) == false);
        CHECK(err != NULL);

        /* all valid (openai_compatible) -> true */
        memset(&f, 0, sizeof f);
        snprintf(f.llm_backend_type, sizeof f.llm_backend_type, "%s", "openai_compatible");
        snprintf(f.llm_base_url, sizeof f.llm_base_url, "%s", "https://api.openai.com/v1");
        snprintf(f.llm_model, sizeof f.llm_model, "%s", "gpt-4o-mini");
        snprintf(f.llm_api_key, sizeof f.llm_api_key, "%s", "sk-abc123");
        CHECK(setup_validate_llm(&f, NULL) == true);

        /* all valid (anthropic_compatible) -> true */
        memset(&f, 0, sizeof f);
        snprintf(f.llm_backend_type, sizeof f.llm_backend_type, "%s", "anthropic_compatible");
        snprintf(f.llm_base_url, sizeof f.llm_base_url, "%s", "https://api.anthropic.com/v1");
        snprintf(f.llm_model, sizeof f.llm_model, "%s", "claude-3-5-sonnet");
        snprintf(f.llm_api_key, sizeof f.llm_api_key, "%s", "sk-ant-abc");
        CHECK(setup_validate_llm(&f, NULL) == true);

        /* all valid (custom = openai_compatible + user http:// URL) -> true */
        memset(&f, 0, sizeof f);
        snprintf(f.llm_backend_type, sizeof f.llm_backend_type, "%s", "openai_compatible");
        snprintf(f.llm_base_url, sizeof f.llm_base_url, "%s", "http://192.168.1.10:8080/v1");
        snprintf(f.llm_model, sizeof f.llm_model, "%s", "local-model");
        snprintf(f.llm_api_key, sizeof f.llm_api_key, "%s", "key");
        CHECK(setup_validate_llm(&f, NULL) == true);

        /* NULL fields -> false, no crash */
        err = NULL;
        CHECK(setup_validate_llm(NULL, &err) == false);
    }

    /* ---- Task 0.4: setup_first_use_required (Codex B1) ---- */
    {
        setup_fields_t f;

        /* all 5 mandatory set (+ empty password, since password not mandatory) -> false */
        fill_valid_fields(&f);
        CHECK(setup_first_use_required(&f) == false);

        /* wifi_ssid empty -> true */
        fill_valid_fields(&f);
        f.wifi_ssid[0] = '\0';
        CHECK(setup_first_use_required(&f) == true);

        /* llm_backend_type empty -> true */
        fill_valid_fields(&f);
        f.llm_backend_type[0] = '\0';
        CHECK(setup_first_use_required(&f) == true);

        /* llm_base_url empty -> true */
        fill_valid_fields(&f);
        f.llm_base_url[0] = '\0';
        CHECK(setup_first_use_required(&f) == true);

        /* llm_model empty -> true */
        fill_valid_fields(&f);
        f.llm_model[0] = '\0';
        CHECK(setup_first_use_required(&f) == true);

        /* B1: WiFi fully set but llm_api_key empty -> true */
        fill_valid_fields(&f);
        f.llm_api_key[0] = '\0';
        CHECK(setup_first_use_required(&f) == true);

        /* NULL fields -> true (setup required / safe default), no crash */
        CHECK(setup_first_use_required(NULL) == true);
    }

    /* ---- Task 0.5: setup_sm_next pure state machine ---- */
    {
        /* FIRST_USE forward flow (plan Step-1 assertions) */
        CHECK(setup_sm_next(SETUP_ST_WELCOME, SETUP_EV_START, SETUP_MODE_FIRST_USE) == SETUP_ST_WIFI_SCAN);
        CHECK(setup_sm_next(SETUP_ST_WIFI_SCAN, SETUP_EV_PICK_WIFI, SETUP_MODE_FIRST_USE) == SETUP_ST_WIFI_PW);
        CHECK(setup_sm_next(SETUP_ST_WIFI_PW, SETUP_EV_CONNECT_OK, SETUP_MODE_FIRST_USE) == SETUP_ST_LLM);
        CHECK(setup_sm_next(SETUP_ST_WIFI_PW, SETUP_EV_CONNECT_FAIL, SETUP_MODE_FIRST_USE) == SETUP_ST_WIFI_PW);
        CHECK(setup_sm_next(SETUP_ST_LLM, SETUP_EV_NEXT, SETUP_MODE_FIRST_USE) == SETUP_ST_REVIEW);
        CHECK(setup_sm_next(SETUP_ST_REVIEW, SETUP_EV_SAVE, SETUP_MODE_FIRST_USE) == SETUP_ST_SAVE);

        /* FIRST_USE back-edges */
        CHECK(setup_sm_next(SETUP_ST_WIFI_PW, SETUP_EV_BACK, SETUP_MODE_FIRST_USE) == SETUP_ST_WIFI_SCAN);
        CHECK(setup_sm_next(SETUP_ST_LLM, SETUP_EV_BACK, SETUP_MODE_FIRST_USE) == SETUP_ST_WIFI_PW);
        CHECK(setup_sm_next(SETUP_ST_REVIEW, SETUP_EV_BACK, SETUP_MODE_FIRST_USE) == SETUP_ST_LLM);

        /* FIRST_USE: SAVE is terminal-ish, DONE -> EXIT */
        CHECK(setup_sm_next(SETUP_ST_SAVE, SETUP_EV_DONE, SETUP_MODE_FIRST_USE) == SETUP_ST_EXIT);

        /* SETTINGS menu navigation */
        CHECK(setup_sm_next(SETUP_ST_MENU, SETUP_EV_PICK_WIFI, SETUP_MODE_SETTINGS) == SETUP_ST_WIFI_SCAN);
        CHECK(setup_sm_next(SETUP_ST_MENU, SETUP_EV_PICK_LLM, SETUP_MODE_SETTINGS) == SETUP_ST_LLM);
        CHECK(setup_sm_next(SETUP_ST_MENU, SETUP_EV_BACK, SETUP_MODE_SETTINGS) == SETUP_ST_EXIT);

        /* SETTINGS WiFi subflow: scan -> pw -> (CONNECT_OK) back to MENU */
        CHECK(setup_sm_next(SETUP_ST_WIFI_SCAN, SETUP_EV_PICK_WIFI, SETUP_MODE_SETTINGS) == SETUP_ST_WIFI_PW);
        CHECK(setup_sm_next(SETUP_ST_WIFI_PW, SETUP_EV_CONNECT_OK, SETUP_MODE_SETTINGS) == SETUP_ST_MENU);
        CHECK(setup_sm_next(SETUP_ST_WIFI_PW, SETUP_EV_CONNECT_FAIL, SETUP_MODE_SETTINGS) == SETUP_ST_WIFI_PW);
        CHECK(setup_sm_next(SETUP_ST_WIFI_PW, SETUP_EV_BACK, SETUP_MODE_SETTINGS) == SETUP_ST_WIFI_SCAN);
        CHECK(setup_sm_next(SETUP_ST_WIFI_SCAN, SETUP_EV_BACK, SETUP_MODE_SETTINGS) == SETUP_ST_MENU);

        /* SETTINGS LLM subflow */
        CHECK(setup_sm_next(SETUP_ST_LLM, SETUP_EV_NEXT, SETUP_MODE_SETTINGS) == SETUP_ST_REVIEW);
        CHECK(setup_sm_next(SETUP_ST_REVIEW, SETUP_EV_SAVE, SETUP_MODE_SETTINGS) == SETUP_ST_SAVE);
        CHECK(setup_sm_next(SETUP_ST_SAVE, SETUP_EV_DONE, SETUP_MODE_SETTINGS) == SETUP_ST_MENU);
        CHECK(setup_sm_next(SETUP_ST_REVIEW, SETUP_EV_BACK, SETUP_MODE_SETTINGS) == SETUP_ST_LLM);

        /* mode-split: LLM+BACK -> WIFI_PW (first-use) vs -> MENU (settings) */
        CHECK(setup_sm_next(SETUP_ST_LLM, SETUP_EV_BACK, SETUP_MODE_FIRST_USE) == SETUP_ST_WIFI_PW);
        CHECK(setup_sm_next(SETUP_ST_LLM, SETUP_EV_BACK, SETUP_MODE_SETTINGS) == SETUP_ST_MENU);

        /* mode-split: WIFI_PW+CONNECT_OK -> LLM (first-use) vs -> MENU (settings) */
        CHECK(setup_sm_next(SETUP_ST_WIFI_PW, SETUP_EV_CONNECT_OK, SETUP_MODE_FIRST_USE) == SETUP_ST_LLM);
        CHECK(setup_sm_next(SETUP_ST_WIFI_PW, SETUP_EV_CONNECT_OK, SETUP_MODE_SETTINGS) == SETUP_ST_MENU);

        /* unknown (state,event) -> unchanged state, never crash */
        CHECK(setup_sm_next(SETUP_ST_WELCOME, SETUP_EV_CONNECT_OK, SETUP_MODE_FIRST_USE) == SETUP_ST_WELCOME);
        CHECK(setup_sm_next(SETUP_ST_LLM, SETUP_EV_START, SETUP_MODE_FIRST_USE) == SETUP_ST_LLM);
        CHECK(setup_sm_next(SETUP_ST_EXIT, SETUP_EV_BACK, SETUP_MODE_SETTINGS) == SETUP_ST_EXIT);
        CHECK(setup_sm_next(SETUP_ST_WELCOME, SETUP_EV_START, SETUP_MODE_SETTINGS) == SETUP_ST_WELCOME);
    }

    /* ---- Task 1.1: STATUS state + PICK_STATUS event tests ---- */
    {
        /* MENU + PICK_STATUS + SETTINGS -> STATUS */
        CHECK(setup_sm_next(SETUP_ST_MENU,
                            SETUP_EV_PICK_STATUS,
                            SETUP_MODE_SETTINGS) == SETUP_ST_STATUS);

        /* STATUS + BACK + SETTINGS -> MENU */
        CHECK(setup_sm_next(SETUP_ST_STATUS,
                            SETUP_EV_BACK,
                            SETUP_MODE_SETTINGS) == SETUP_ST_MENU);

        /* STATUS is a leaf: every other event leaves STATUS unchanged
         * (note PICK_STATUS while already in STATUS is also inert). */
        {
            static const setup_sm_event_t others[] = {
                SETUP_EV_START, SETUP_EV_NEXT, SETUP_EV_SAVE,
                SETUP_EV_PICK_WIFI, SETUP_EV_PICK_LLM,
                SETUP_EV_PICK_STATUS, SETUP_EV_CONNECT_OK,
                SETUP_EV_CONNECT_FAIL, SETUP_EV_DONE,
            };
            for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
                CHECK(setup_sm_next(SETUP_ST_STATUS, others[i],
                                    SETUP_MODE_SETTINGS) == SETUP_ST_STATUS);
            }
        }

        /* FIRST_USE mode: STATUS is unreachable via the MENU. MENU does
         * not exist in the first-use flow, so MENU+PICK_STATUS stays at
         * MENU (no edge defined for first-use). */
        CHECK(setup_sm_next(SETUP_ST_MENU,
                            SETUP_EV_PICK_STATUS,
                            SETUP_MODE_FIRST_USE) == SETUP_ST_MENU);

        /* FIRST_USE mode: if a harness pokes the SM into STATUS
         * directly, BACK in FIRST_USE has no defined edge -> stay
         * (undefined edges leave the state unchanged). */
        CHECK(setup_sm_next(SETUP_ST_STATUS,
                            SETUP_EV_BACK,
                            SETUP_MODE_FIRST_USE) == SETUP_ST_STATUS);

        /* PICK_STATUS from every other state is inert in both modes. */
        {
            static const setup_sm_state_t others[] = {
                SETUP_ST_WELCOME, SETUP_ST_WIFI_SCAN, SETUP_ST_WIFI_PW,
                SETUP_ST_LLM, SETUP_ST_REVIEW, SETUP_ST_SAVE,
                SETUP_ST_EXIT,
            };
            for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
                CHECK(setup_sm_next(others[i], SETUP_EV_PICK_STATUS,
                                    SETUP_MODE_SETTINGS) == others[i]);
                CHECK(setup_sm_next(others[i], SETUP_EV_PICK_STATUS,
                                    SETUP_MODE_FIRST_USE) == others[i]);
            }
        }
    }

    /* ---- Task 4.1: setup_model pure helpers (Codex M8) ---- */
    {
        /* setup_model_buf_dirty: equal buffers -> not dirty */
        char x[8] = "abcdefg";
        char y[8] = "abcdefg";
        CHECK(setup_model_buf_dirty(x, y, sizeof x) == false);

        /* one byte differs -> dirty */
        y[3] = 'Z';
        CHECK(setup_model_buf_dirty(x, y, sizeof x) == true);

        /* difference outside the compared length -> not dirty */
        char p[4] = {1, 2, 3, 4};
        char q[4] = {1, 2, 9, 9};
        CHECK(setup_model_buf_dirty(p, q, 2) == false);
        CHECK(setup_model_buf_dirty(p, q, 3) == true);

        /* NULL-safety contract: any NULL or len==0 -> not dirty, no crash */
        CHECK(setup_model_buf_dirty(NULL, y, sizeof y) == false);
        CHECK(setup_model_buf_dirty(x, NULL, sizeof x) == false);
        CHECK(setup_model_buf_dirty(x, y, 0) == false);

        /* self-compare is never dirty */
        CHECK(setup_model_buf_dirty(x, x, sizeof x) == false);

        /* pointer-identical AND len==0 -> not dirty (short-circuits
         * before any memcmp; covers the pins ordering) */
        CHECK(setup_model_buf_dirty(x, x, 0) == false);

        /* large buffer (sizeof app_config_t = 6168B), ONLY the LAST byte
         * differs -> dirty. Catches a tail off-by-one in the memcmp
         * length that the 4-byte cases above cannot. */
        {
            static unsigned char big_a[6168];
            static unsigned char big_b[6168];
            memset(big_a, 0x5A, sizeof big_a);
            memset(big_b, 0x5A, sizeof big_b);
            CHECK(setup_model_buf_dirty(big_a, big_b, sizeof big_a) == false);
            big_b[sizeof big_b - 1] = 0x5B;
            CHECK(setup_model_buf_dirty(big_a, big_b, sizeof big_a) == true);
            /* one byte shorter must NOT see the differing tail byte */
            CHECK(setup_model_buf_dirty(big_a, big_b, sizeof big_a - 1)
                  == false);
        }

        /* setup_model_mode_from_sm: pure SM-mode -> model-mode mapping */
        CHECK(setup_model_mode_from_sm(SETUP_MODE_FIRST_USE)
              == SETUP_MODEL_MODE_FIRST_USE);
        CHECK(setup_model_mode_from_sm(SETUP_MODE_SETTINGS)
              == SETUP_MODEL_MODE_SETTINGS);
    }

    /* ---- Task 1.2: format_call_age Δ-time bucket helper ---- */
    {
        /* "just now" at the 0 µs and 4_999_999 µs boundaries (< 5s). */
        {
            char buf[16];
            CHECK(format_call_age(0LL, buf, sizeof buf) == buf);
            CHECK(strcmp(buf, "just now") == 0);
            CHECK(format_call_age(4999999LL, buf, sizeof buf) == buf);
            CHECK(strcmp(buf, "just now") == 0);
        }

        /* "Ns ago" boundaries: 5s and 59s (floor seconds). */
        {
            char buf[16];
            format_call_age(5000000LL, buf, sizeof buf);
            CHECK(strcmp(buf, "5s ago") == 0);
            format_call_age(59999999LL, buf, sizeof buf);
            CHECK(strcmp(buf, "59s ago") == 0);
        }

        /* "Nm ago" boundaries: 1m and 59m (floor minutes). */
        {
            char buf[16];
            format_call_age(60000000LL, buf, sizeof buf);
            CHECK(strcmp(buf, "1m ago") == 0);
            format_call_age(3599999999LL, buf, sizeof buf);
            CHECK(strcmp(buf, "59m ago") == 0);
        }

        /* "Nh ago" boundaries: 1h and 23h (floor hours). */
        {
            char buf[16];
            format_call_age(3600000000LL, buf, sizeof buf);
            CHECK(strcmp(buf, "1h ago") == 0);
            format_call_age(86399999999LL, buf, sizeof buf);
            CHECK(strcmp(buf, "23h ago") == 0);
        }

        /* "Nd ago" at the 1-day boundary (floor days). */
        {
            char buf[16];
            format_call_age(86400000000LL, buf, sizeof buf);
            CHECK(strcmp(buf, "1d ago") == 0);
        }

        /* buflen < 12 -> NULL (deliberately too small to render). */
        {
            char buf[4];
            CHECK(format_call_age(5000000LL, buf, sizeof buf) == NULL);
        }

        /* NULL buf -> NULL. */
        CHECK(format_call_age(0LL, NULL, 16) == NULL);

        /* Negative input clamps to 0 / "just now" (per header doc).
         * INT64_MIN exercises the most-negative case — locks in that
         * a future refactor can't introduce UB by, e.g., computing
         * -delta_us. */
        {
            char buf[16];
            CHECK(format_call_age(-1LL, buf, sizeof buf) == buf);
            CHECK(strcmp(buf, "just now") == 0);
            CHECK(format_call_age(INT64_MIN, buf, sizeof buf) == buf);
            CHECK(strcmp(buf, "just now") == 0);
        }

        /* Extreme positive input (INT64_MAX) must still render
         * cleanly, not return NULL/truncation. s/86400 fits in
         * a few digits inside the 16-byte buffer. */
        {
            char buf[16];
            CHECK(format_call_age(INT64_MAX, buf, sizeof buf) == buf);
            /* Don't pin the exact digit count — just confirm the
             * "Nd ago" suffix is intact. */
            CHECK(strstr(buf, "d ago") != NULL);
        }
    }

    if (g_failed) {
        puts("TESTS FAILED");
        return 1;
    }
    puts("ALL PASS");
    return 0;
}
