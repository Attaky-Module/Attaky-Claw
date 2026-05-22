/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "llm/claw_llm_runtime.h"

typedef struct {
    const char *api_key;
    const char *backend_type;
    const char *model;
    const char *base_url;
    const char *auth_type;
    const char *max_tokens_field;
    uint32_t timeout_ms;
    uint32_t max_tokens;
    size_t image_max_bytes;
    bool supports_tools;
    bool supports_vision;
    bool image_remote_url_only;
} claw_core_llm_config_t;

typedef struct {
    bool initialized;       /* true only after claw_core_llm_init
                             * + claw_llm_runtime_init both succeed */
    char backend_type[32];
    char model[64];
    char base_url[128];
} claw_core_llm_config_summary_t;

typedef enum {
    CLAW_CORE_LLM_LAST_CALL_NONE = 0,
    CLAW_CORE_LLM_LAST_CALL_IN_FLIGHT,
    CLAW_CORE_LLM_LAST_CALL_OK,
    CLAW_CORE_LLM_LAST_CALL_ERROR,
} claw_core_llm_last_call_status_t;

typedef struct {
    claw_core_llm_last_call_status_t status;
    int64_t                          timestamp_us;
    esp_err_t                        err;
    char                             msg[48];
} claw_core_llm_last_call_t;

typedef claw_llm_tool_call_t claw_core_llm_tool_call_t;
typedef claw_llm_response_t claw_core_llm_response_t;

esp_err_t claw_core_llm_init(const claw_core_llm_config_t *config, char **out_error_message);
void claw_core_llm_get_config_summary(claw_core_llm_config_summary_t *out);
void claw_core_llm_get_last_call(claw_core_llm_last_call_t *out);
esp_err_t claw_core_llm_chat_messages(const char *system_prompt,
                                      cJSON *messages,
                                      const char *tools_json,
                                      claw_core_llm_response_t *out_response,
                                      char **out_error_message);
esp_err_t claw_core_llm_chat(const char *system_prompt,
                             const char *user_text,
                             char **out_text,
                             char **out_error_message);
esp_err_t claw_core_llm_analyze_image(const char *system_prompt,
                                      const char *user_prompt,
                                      const char *image_path,
                                      char **out_text,
                                      char **out_error_message);
esp_err_t claw_core_llm_infer_media(const claw_llm_media_request_t *request,
                                    char **out_text,
                                    char **out_error_message);
esp_err_t claw_core_llm_register_custom_backend(const claw_llm_custom_backend_registration_t *registration);
void claw_core_llm_response_free(claw_core_llm_response_t *response);
