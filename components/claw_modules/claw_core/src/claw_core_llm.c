/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_llm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/portmacro.h"

static const char *TAG = "claw_core_llm";

#ifndef CLAW_CORE_LOG_FULL_LLM_REQUEST
#define CLAW_CORE_LOG_FULL_LLM_REQUEST 0
#endif

static claw_llm_runtime_t *s_runtime = NULL;
static claw_core_llm_config_summary_t s_summary = {0};

/* Concurrent-call-safe LLM-runtime-call snapshot — see design §2.2 / §4.2.
 * - `inflight` counts currently-running claw_llm_runtime_* invocations
 *   bracketed by transport_begin / transport_end.
 * - `last_*` is the most-recently-completed call's outcome.
 * - Hook intentionally lives at the claw_core_llm wrapper layer (NOT at
 *   the generic claw_llm_runtime layer) so the separate claw_memory
 *   extraction runtime instance does not pollute the snapshot. */
static portMUX_TYPE s_last_call_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t                         inflight;
    claw_core_llm_last_call_status_t last_status;
    int64_t                          last_timestamp_us;
    esp_err_t                        last_err;
    char                             last_msg[48];
} s_last_call;

static void claw_core_llm__transport_begin(void)
{
    portENTER_CRITICAL(&s_last_call_lock);
    s_last_call.inflight++;
    portEXIT_CRITICAL(&s_last_call_lock);
}

static void claw_core_llm__transport_end(esp_err_t transport_err)
{
    char msg_local[48] = "";
    if (transport_err != ESP_OK) {
        strlcpy(msg_local, esp_err_to_name(transport_err),
                sizeof msg_local);
    }
    /* esp_timer_get_time() is intentionally called INSIDE the lock so
     * lock-acquisition order == published-timestamp order — the most
     * recently completed call always wins with the largest timestamp,
     * even if two completions race the lock. See design §2.2. */
    portENTER_CRITICAL(&s_last_call_lock);
    s_last_call.last_status =
        (transport_err == ESP_OK) ? CLAW_CORE_LLM_LAST_CALL_OK
                                  : CLAW_CORE_LLM_LAST_CALL_ERROR;
    s_last_call.last_timestamp_us = esp_timer_get_time();
    s_last_call.last_err = transport_err;
    memcpy(s_last_call.last_msg, msg_local, sizeof s_last_call.last_msg);
    s_last_call.inflight--;     /* always paired with begin */
    portEXIT_CRITICAL(&s_last_call_lock);
}

static void summary_copy_field(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (src) {
        strlcpy(dst, src, dst_size);
    } else {
        dst[0] = '\0';
    }
}

static char *dup_printf(const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;
    char *buf;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    buf = calloc(1, (size_t)needed + 1);
    if (!buf) {
        va_end(args);
        return NULL;
    }

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}

esp_err_t claw_core_llm_init(const claw_core_llm_config_t *config, char **out_error_message)
{
    claw_llm_runtime_config_t runtime_config = {0};
    esp_err_t err;

    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!config || !config->api_key || !config->model || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_llm_runtime_deinit(s_runtime);
    s_runtime = NULL;

    /* MAJOR-2-round-4 ordering + stage-and-publish (round-7 reviewer):
     * build the summary in a local stage struct, then publish to the
     * file-static `s_summary` with a single assignment at the very end
     * of the success path. This way a concurrent getter on the re-init
     * path can only ever observe two states — the prior stable summary
     * or the new stable summary — never a torn mix of strlcpy'd fields
     * mid-update. On runtime-init failure we never publish, so
     * s_summary keeps its previous contents (or {0} on first init). */
    claw_core_llm_config_summary_t stage = {0};
    summary_copy_field(stage.backend_type, sizeof(stage.backend_type), config->backend_type);
    summary_copy_field(stage.model, sizeof(stage.model), config->model);
    summary_copy_field(stage.base_url, sizeof(stage.base_url), config->base_url);

    runtime_config.api_key = config->api_key;
    runtime_config.backend_type = config->backend_type;
    runtime_config.model = config->model;
    runtime_config.base_url = config->base_url;
    runtime_config.auth_type = config->auth_type;
    runtime_config.max_tokens_field = config->max_tokens_field;
    runtime_config.timeout_ms = config->timeout_ms;
    runtime_config.max_tokens = config->max_tokens;
    runtime_config.image_max_bytes = config->image_max_bytes;
    runtime_config.supports_tools = config->supports_tools;
    runtime_config.supports_vision = config->supports_vision;
    runtime_config.image_remote_url_only = config->image_remote_url_only;
    err = claw_llm_runtime_init(&s_runtime, &runtime_config, out_error_message);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: runtime init failed err=0x%x", err);
        return err;
    }

    /* FINAL action on the success path: publish the staged summary
     * as a single struct assignment. Until this line runs, s_summary
     * still holds the previous successful publish (or {0} on first
     * init). Every prior return leaves s_summary untouched. */
    stage.initialized = true;
    s_summary = stage;
    return ESP_OK;
}

void claw_core_llm_get_config_summary(claw_core_llm_config_summary_t *out)
{
    if (out == NULL) {
        return;
    }
    /* Snapshot copy. Readers may race with re-init publishers, but
     * the stage-and-publish pattern in claw_core_llm_init guarantees
     * the only observable s_summary values are "previous stable" or
     * "new stable" — never a half-strlcpy'd intermediate. */
    *out = s_summary;
}

void claw_core_llm_get_last_call(claw_core_llm_last_call_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_last_call_lock);
    if (s_last_call.inflight > 0) {
        out->status       = CLAW_CORE_LLM_LAST_CALL_IN_FLIGHT;
        out->timestamp_us = 0;
        out->err          = ESP_OK;
        out->msg[0]       = '\0';
    } else if (s_last_call.last_timestamp_us == 0) {
        out->status       = CLAW_CORE_LLM_LAST_CALL_NONE;
        out->timestamp_us = 0;
        out->err          = ESP_OK;
        out->msg[0]       = '\0';
    } else {
        out->status       = s_last_call.last_status;
        out->timestamp_us = s_last_call.last_timestamp_us;
        out->err          = s_last_call.last_err;
        memcpy(out->msg, s_last_call.last_msg, sizeof out->msg);
    }
    portEXIT_CRITICAL(&s_last_call_lock);
}

esp_err_t claw_core_llm_chat_messages(const char *system_prompt,
                                      cJSON *messages,
                                      const char *tools_json,
                                      claw_core_llm_response_t *out_response,
                                      char **out_error_message)
{
    claw_llm_chat_request_t request = {0};
#if CLAW_CORE_LOG_FULL_LLM_REQUEST
    char *messages_json = NULL;
#endif

    if (!s_runtime) {
        if (out_error_message) {
            *out_error_message = dup_printf("LLM runtime is not initialized");
        }
        ESP_LOGE(TAG, "chat_messages: runtime is not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!system_prompt || !messages || !out_response || !out_error_message || !cJSON_IsArray(messages)) {
        return ESP_ERR_INVALID_ARG;
    }

    request.system_prompt = system_prompt;
    request.messages = messages;
    request.tools_json = tools_json;

#if CLAW_CORE_LOG_FULL_LLM_REQUEST
    messages_json = cJSON_PrintUnformatted(messages);
    if (messages_json) {
        ESP_LOGI(TAG, "llm_request system_prompt=%s", system_prompt);
        ESP_LOGI(TAG, "llm_request messages=%s", messages_json);
        ESP_LOGI(TAG, "llm_request tools=%s", tools_json ? tools_json : "[]");
        free(messages_json);
    } else {
        ESP_LOGE(TAG, "failed to render full LLM request messages");
    }
#endif

    /* ── TRANSPORT WINDOW: paired begin/end around the runtime call.
     * NO early-return or goto between begin and end. The snapshot
     * tracks ONLY this runtime invocation; pre-transport validation
     * (above) and post-transport processing (none in this function;
     * unsupported-tool repackaging happens in the higher-level
     * claw_core_llm_chat wrapper, deliberately outside the bracket)
     * do not touch the snapshot. */
    claw_core_llm__transport_begin();
    esp_err_t transport_err = claw_llm_runtime_chat(s_runtime, &request, out_response, out_error_message);
    claw_core_llm__transport_end(transport_err);

    if (transport_err != ESP_OK) {
        ESP_LOGE(TAG, "chat_messages: runtime chat failed err=0x%x", transport_err);
    }
    return transport_err;
}

esp_err_t claw_core_llm_chat(const char *system_prompt,
                             const char *user_text,
                             char **out_text,
                             char **out_error_message)
{
    claw_core_llm_response_t response = {0};
    cJSON *messages = NULL;
    cJSON *user_msg = NULL;
    esp_err_t err;

    if (out_text) {
        *out_text = NULL;
    }
    if (!system_prompt || !user_text || !out_text || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    messages = cJSON_CreateArray();
    user_msg = cJSON_CreateObject();
    if (!messages || !user_msg) {
        cJSON_Delete(messages);
        cJSON_Delete(user_msg);
        *out_error_message = dup_printf("Out of memory building messages");
        ESP_LOGE(TAG, "chat: out of memory building messages");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_text);
    cJSON_AddItemToArray(messages, user_msg);

    err = claw_core_llm_chat_messages(system_prompt, messages, NULL, &response, out_error_message);
    cJSON_Delete(messages);
    if (err != ESP_OK) {
        claw_core_llm_response_free(&response);
        return err;
    }
    if (response.tool_call_count > 0) {
        ESP_LOGE(TAG, "chat: unsupported tool calls returned count=%zu", response.tool_call_count);
        claw_core_llm_response_free(&response);
        *out_error_message = dup_printf("LLM returned unsupported tool calls");
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out_text = response.text;
    response.text = NULL;
    claw_core_llm_response_free(&response);
    return ESP_OK;
}

esp_err_t claw_core_llm_infer_media(const claw_llm_media_request_t *request,
                                    char **out_text,
                                    char **out_error_message)
{
    if (!s_runtime) {
        if (out_error_message) {
            *out_error_message = dup_printf("LLM runtime is not initialized");
        }
        ESP_LOGE(TAG, "infer_media: runtime is not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!request || !out_text || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ── TRANSPORT WINDOW: paired begin/end around the runtime call.
     * Same invariants as claw_core_llm_chat_messages — no early-return
     * or goto between begin and end. */
    claw_core_llm__transport_begin();
    esp_err_t transport_err = claw_llm_runtime_infer_media(s_runtime, request, out_text, out_error_message);
    claw_core_llm__transport_end(transport_err);

    if (transport_err != ESP_OK) {
        ESP_LOGE(TAG, "infer_media: runtime inference failed err=0x%x media_count=%zu",
                 transport_err, request->media_count);
    }
    return transport_err;
}

esp_err_t claw_core_llm_analyze_image(const char *system_prompt,
                                      const char *user_prompt,
                                      const char *image_path,
                                      char **out_text,
                                      char **out_error_message)
{
    claw_media_asset_t asset = {0};
    claw_llm_media_request_t request = {0};

    if (!system_prompt || !user_prompt || !image_path || !out_text || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    asset.kind = CLAW_MEDIA_ASSET_KIND_LOCAL_PATH;
    asset.path = image_path;

    request.system_prompt = system_prompt;
    request.user_prompt = user_prompt;
    request.media = &asset;
    request.media_count = 1;
    return claw_core_llm_infer_media(&request, out_text, out_error_message);
}

esp_err_t claw_core_llm_register_custom_backend(const claw_llm_custom_backend_registration_t *registration)
{
    esp_err_t err = claw_llm_register_custom_backend(registration);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_custom_backend: failed err=0x%x", err);
    }
    return err;
}

void claw_core_llm_response_free(claw_core_llm_response_t *response)
{
    claw_llm_response_free(response);
}
