/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wifi_manager.h"
#include "wear_levelling.h"
#include "time.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_board_manager_includes.h"
#include "captive_dns.h"
#include "cmd_wifi.h"
#if CONFIG_APP_EMOTE_DEBUG_CONSOLE
#include "esp_console.h"
#include "emote.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#include "app_config.h"
#include "setup_model.h"

/* Phase 5.1: physical SELECT button (AW9523B @0x59 P02, active-low)
 * opens the on-device settings UI. */
#include "freertos/semphr.h"
#include "esp_io_expander.h"
#include "iot_button.h"
#include "button_interface.h"
#include "display_arbiter.h"
#include "setup_ui.h"
#include "setup_validate.h"
#include "status_led.h"

#define APP_FATFS_PARTITION_LABEL "storage"
#define APP_ENABLE_MEM_LOG        (0)

static const char *TAG = "app";

static app_config_t *s_config;
static app_claw_config_t *s_claw_config;
static app_claw_storage_paths_t *s_claw_paths;

static const char *app_fatfs_base_path = "/fatfs";

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = calloc(1, sizeof(*s_claw_config));
    }
    if (!s_claw_paths) {
        s_claw_paths = calloc(1, sizeof(*s_claw_paths));
    }

    if (!s_config || !s_claw_config || !s_claw_paths) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    /* 4.1-I1 lifetime contract: unbind the borrowed live config/claw
     * pointers BEFORE freeing the objects they point at, so a later
     * setup_model_commit() can never dereference freed memory (it
     * degrades to save-only after unbind). */
    setup_model_bind_live_config(NULL, NULL);

    free(s_claw_paths);
    s_claw_paths = NULL;

    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    const char *ap_ssid = status.ap_active ? status.ap_ssid : NULL;

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
             connected,
             status.ap_active,
             status.mode ? status.mode : "off",
             ap_ssid ? ap_ssid : "(none)");

    esp_err_t err = app_claw_set_network_status(connected, ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network emote: %s", esp_err_to_name(err));
    }
}

static esp_err_t app_claw_init_storage_paths(app_claw_storage_paths_t *paths)
{
    if (!paths || !app_fatfs_base_path || app_fatfs_base_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(paths, 0, sizeof(*paths));

    if (strlcpy(paths->fatfs_base_path, app_fatfs_base_path, sizeof(paths->fatfs_base_path)) >= sizeof(paths->fatfs_base_path) ||
        snprintf(paths->memory_session_root, sizeof(paths->memory_session_root), "%s/sessions", app_fatfs_base_path) >= sizeof(paths->memory_session_root) ||
        snprintf(paths->memory_root_dir, sizeof(paths->memory_root_dir), "%s/memory", app_fatfs_base_path) >= sizeof(paths->memory_root_dir) ||
        snprintf(paths->skills_root_dir, sizeof(paths->skills_root_dir), "%s/skills", app_fatfs_base_path) >= sizeof(paths->skills_root_dir) ||
        snprintf(paths->lua_root_dir, sizeof(paths->lua_root_dir), "%s/scripts", app_fatfs_base_path) >= sizeof(paths->lua_root_dir) ||
        snprintf(paths->router_rules_path, sizeof(paths->router_rules_path), "%s/router_rules/router_rules.json", app_fatfs_base_path) >= sizeof(paths->router_rules_path) ||
        snprintf(paths->scheduler_rules_path, sizeof(paths->scheduler_rules_path), "%s/scheduler/schedules.json", app_fatfs_base_path) >= sizeof(paths->scheduler_rules_path) ||
        snprintf(paths->im_attachment_root, sizeof(paths->im_attachment_root), "%s/inbox", app_fatfs_base_path) >= sizeof(paths->im_attachment_root)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t main_save_config(const app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = app_config_validate_wifi(config, NULL);
    if (err != ESP_OK) {
        return err;
    }

    return app_config_save(config);
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    status->ap_ip = wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t main_wechat_login_start(const char *account_id, bool force)
{
    return cap_im_wechat_qr_login_start(account_id, force);
}

static esp_err_t main_wechat_login_get_status(http_server_wechat_login_status_t *status)
{
    cap_im_wechat_qr_login_status_t *raw = NULL;
    esp_err_t err;

    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    raw = calloc(1, sizeof(*raw));
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_wechat_qr_login_get_status(raw);
    if (err != ESP_OK) {
        free(raw);
        return err;
    }

    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->completed = raw->completed;
    status->persisted = raw->persisted;
    strlcpy(status->session_key, raw->session_key, sizeof(status->session_key));
    strlcpy(status->status, raw->status, sizeof(status->status));
    strlcpy(status->message, raw->message, sizeof(status->message));
    strlcpy(status->qr_data_url, raw->qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, raw->account_id, sizeof(status->account_id));
    strlcpy(status->user_id, raw->user_id, sizeof(status->user_id));
    strlcpy(status->token, raw->token, sizeof(status->token));
    strlcpy(status->base_url, raw->base_url, sizeof(status->base_url));
    free(raw);
    return ESP_OK;
}

static esp_err_t main_wechat_login_cancel(void)
{
    return cap_im_wechat_qr_login_cancel();
}

static esp_err_t main_wechat_login_mark_persisted(void)
{
    return cap_im_wechat_qr_login_mark_persisted();
}
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_fatfs(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t err;

    err = esp_vfs_fat_spiflash_mount_rw_wl(app_fatfs_base_path,
                                           APP_FATFS_PARTITION_LABEL,
                                           &mount_config,
                                           &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_vfs_fat_info(app_fatfs_base_path, &total, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to query FATFS info: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "FATFS mounted total=%u used=%u",
                 (unsigned int)total,
                 (unsigned int)(total - free_bytes));
    }

    return ESP_OK;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t err = ESP_OK;

    if (!timezone || timezone[0] == '\0') {
        ESP_LOGE(TAG, "Timezone is empty.");
        err = ESP_ERR_INVALID_ARG;
        goto tz_default;
    }

    if (setenv("TZ", timezone, 1) != 0) {
        ESP_LOGE(TAG, "Failed to set TZ env");
        err = ESP_FAIL;
        goto tz_default;
    }
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return err;
}

/* ------------------------------------------------------------------ *
 * Phase 5.1: physical SELECT button -> settings UI.
 *
 * SELECT is AW9523B @ 7-bit 0x59 P02, active-low. That expander is the
 * board-manager device "gpio_expander_io". esp_board_manager dev_button
 * only knows gpio/adc sub_types (no I2C expander), so we build a custom
 * iot_button driver that polls P02 via esp_io_expander_get_level().
 *
 * Callback-context rule: iot_button runs get_key_level + event cbs on
 * its internal esp_timer/button task. setup_ui_run(SETUP_MODE_SETTINGS)
 * is a long blocking LVGL session and MUST NOT run there. So the
 * single-click cb only does a cheap guard + notifies a dedicated
 * launcher task, which actually runs the settings session.
 *
 * No-brick (Spike C heritage): this arms during/after boot. Any failure
 * here is logged and boot CONTINUES (SELECT just won't work); never
 * ESP_ERROR_CHECK / abort / assert on this path.
 * ------------------------------------------------------------------ */

/* P02 in the AW9523B linear pin numbering = bit 2 (P00..P07 -> bits
 * 0..7). esp_io_expander_get_level() returns a level bitmask. */
#define SELECT_BTN_PIN IO_EXPANDER_PIN_NUM_2

/* Custom iot_button driver: button_driver_t MUST be the first member so
 * a button_driver_t* can be cast back to this struct. */
typedef struct {
    button_driver_t drv;
    esp_io_expander_handle_t exp;
} select_btn_driver_t;

static TaskHandle_t s_select_launcher_task = NULL;

/* iot_button get_key_level: runs on the button timer task. Reads P02
 * and returns the LOGICAL pressed state (active-low: pressed when the
 * P02 bit is 0). On any I2C read error return BUTTON_INACTIVE so a bus
 * glitch on the shared @0x59 bus never spuriously fires SELECT. */
static uint8_t select_btn_get_key_level(button_driver_t *button_driver)
{
    select_btn_driver_t *d = (select_btn_driver_t *)button_driver;
    if (d == NULL || d->exp == NULL) {
        return BUTTON_INACTIVE;
    }
    uint32_t level_mask = 0;
    esp_err_t err = esp_io_expander_get_level(d->exp, SELECT_BTN_PIN, &level_mask);
    if (err != ESP_OK) {
        return BUTTON_INACTIVE;
    }
    return (level_mask & SELECT_BTN_PIN) ? BUTTON_INACTIVE : BUTTON_ACTIVE;
}

/* iot_button single-click cb: lightweight only. Cheap pre-check that
 * EMOTE owns the display, then notify the launcher task. NEVER blocks
 * and NEVER calls setup_ui_run() (wrong context). The launcher task
 * re-checks the guard authoritatively. */
static void select_btn_single_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    if (s_select_launcher_task == NULL) {
        return;
    }
    if (display_arbiter_get_owner() != DISPLAY_ARBITER_OWNER_EMOTE) {
        ESP_LOGI(TAG, "SELECT press ignored: display not owned by EMOTE");
        return;
    }
    /* Coalescing: the task notification is binary. If a session is
     * already open the launcher task is not in ulTaskNotifyTake; this
     * give latches one notification, which is consumed on the next
     * loop iteration and then discarded by the launcher's authoritative
     * owner re-check — so a press during a session is a no-op (at most
     * one stale wake, never a nested session or queue buildup). */
    xTaskNotifyGive(s_select_launcher_task);
}

/* Dedicated launcher task: blocks on a notification, then runs the
 * (long, blocking) settings session in this safe context. Re-checks
 * the single-instance guard authoritatively before opening: only open
 * settings if EMOTE owns the display. setup_ui_run() itself acquires
 * the display arbiter and has its own no-brick unwind. */
static void select_launcher_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (display_arbiter_get_owner() != DISPLAY_ARBITER_OWNER_EMOTE) {
            ESP_LOGI(TAG, "SELECT ignored: settings unavailable (display busy)");
            continue;
        }
        ESP_LOGI(TAG, "SELECT pressed; launching settings UI");
        esp_err_t err = setup_ui_run(SETUP_MODE_SETTINGS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "setup_ui_run(SETTINGS) failed: %s",
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Settings UI session ended");
        }
    }
}

/* Arm the SELECT button. Fail-soft at every step: on failure log and
 * return so the device still boots normally (SELECT just won't work).
 * Static driver: iot_button keeps a pointer to button_driver_t for the
 * button lifetime, so it must outlive app_main. */
static void select_button_init(void)
{
    static select_btn_driver_t s_select_drv;

    void *exp_dev_handle = NULL;
    esp_err_t err = esp_board_manager_get_device_handle("gpio_expander_io",
                                                        &exp_dev_handle);
    if (err != ESP_OK || exp_dev_handle == NULL) {
        ESP_LOGW(TAG, "SELECT init skipped: gpio_expander_io handle: %s",
                 esp_err_to_name(err));
        return;
    }
    /* Stored by pointer (see boards/.../setup_device.c) — deref once. */
    esp_io_expander_handle_t exp = *(esp_io_expander_handle_t *)exp_dev_handle;
    if (exp == NULL) {
        ESP_LOGW(TAG, "SELECT init skipped: IO expander handle is NULL");
        return;
    }

    /* Launcher task first so the cb never notifies a NULL handle. Low
     * prio, small stack; it only blocks then calls setup_ui_run(). */
    if (s_select_launcher_task == NULL) {
        BaseType_t ok = xTaskCreate(select_launcher_task, "setup_select",
                                    4096, NULL, 3, &s_select_launcher_task);
        if (ok != pdPASS || s_select_launcher_task == NULL) {
            ESP_LOGW(TAG, "SELECT init skipped: launcher task create failed");
            s_select_launcher_task = NULL;
            return;
        }
    }

    s_select_drv.exp = exp;
    s_select_drv.drv.enable_power_save = false;
    s_select_drv.drv.get_key_level = select_btn_get_key_level;
    s_select_drv.drv.enter_power_save = NULL;
    s_select_drv.drv.del = NULL;

    /* 0 -> Kconfig defaults (short 180 ms / long 1500 ms). The poll
     * cadence is CONFIG_BUTTON_PERIOD_TIME_MS (5 ms) and is not set
     * here. */
    const button_config_t btn_cfg = {
        .long_press_time = 0,
        .short_press_time = 0,
    };
    button_handle_t btn = NULL;
    err = iot_button_create(&btn_cfg, &s_select_drv.drv, &btn);
    if (err != ESP_OK || btn == NULL) {
        ESP_LOGW(TAG, "SELECT init: iot_button_create failed: %s",
                 esp_err_to_name(err));
        return;
    }
    err = iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL,
                                 select_btn_single_click_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SELECT init: register_cb failed: %s",
                 esp_err_to_name(err));
        (void)iot_button_delete(btn);
        return;
    }
    ESP_LOGI(TAG, "SELECT button armed (AW9523B@0x59 P02, active-low)");
}

#if APP_ENABLE_MEM_LOG

static void print_task_stack_info(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t s_task_status_snapshot[24];
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
#endif
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        print_task_stack_info();
    }
}

#endif

#if CONFIG_APP_EMOTE_DEBUG_CONSOLE
/* Dev debug console (CONFIG_APP_EMOTE_DEBUG_CONSOLE): `emote` command
 * for on-device emote review. Usage:
 *   emote <name>     face emoji   e.g. emote happy / sad / confused
 *   emote listen     fire evt_listen (shows listen_anim overlay)
 *   emote speak      fire evt_speak
 *   emote idle       fire evt_idle (clears listen/speak overlays)
 *   emote bat [c,pct]  show charge_icon + battery_label
 *                      (default 1,75 = charging 75%) */
static int emote_debug_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: emote <name|listen|speak|idle|bat [c,pct]|toast <text...>>\n");
        return 1;
    }
    const char *a = argv[1];
    esp_err_t err;
    if (strcmp(a, "bat") == 0) {
        const char *spec = (argc >= 3) ? argv[2] : "1,75";
        err = emote_debug_bat(spec);
        printf("emote_debug_bat(\"%s\") -> %s\n", spec, esp_err_to_name(err));
    } else if (strcmp(a, "listen") == 0 || strcmp(a, "speak") == 0 ||
        strcmp(a, "idle") == 0) {
        err = emote_debug_event(a);
        printf("emote_debug_event(\"%s\") -> %s\n", a, esp_err_to_name(err));
    } else if (strcmp(a, "toast") == 0) {
        /* Join argv[2..argc) with single spaces. */
        char msg[256] = {0};
        size_t off = 0;
        for (int i = 2; i < argc; i++) {
            int n = snprintf(msg + off, sizeof(msg) - off, "%s%s",
                             (i == 2) ? "" : " ", argv[i]);
            if (n < 0 || (size_t)n >= sizeof(msg) - off) break;
            off += (size_t)n;
        }
        err = emote_debug_toast(msg);
        printf("emote_debug_toast(\"%s\") -> %s\n", msg, esp_err_to_name(err));
    } else {
        err = emote_debug_show(a);
        printf("emote_debug_show(\"%s\") -> %s\n", a, esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

static void register_emote_debug_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "emote",
        .help = "emote <name|listen|speak|idle|bat [c,pct]>",
        .func = &emote_debug_cmd,
    };
    esp_err_t err = esp_console_cmd_register(&cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "emote debug command not registered: %s",
                 esp_err_to_name(err));
    }
}
#endif /* CONFIG_APP_EMOTE_DEBUG_CONSOLE */

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_ERROR_CHECK(app_allocate_runtime_state());
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(app_config_load(s_config));
    app_config_to_claw(s_config, s_claw_config);
    /* 4.1-I1: register the live config objects so setup_model_commit()
     * can refresh the running config from NVS after a save. Borrowed,
     * NON-owning binding — unbound before these objects are freed
     * (app_free_runtime_state). */
    setup_model_bind_live_config(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error
    ESP_ERROR_CHECK(esp_board_manager_init());
    ESP_ERROR_CHECK(app_claw_ui_start());
    ESP_ERROR_CHECK(init_fatfs());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fatfs_base_path,
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .restart_device = main_restart_device,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = main_wechat_login_start,
            .wechat_login_get_status = main_wechat_login_get_status,
            .wechat_login_cancel = main_wechat_login_cancel,
            .wechat_login_mark_persisted = main_wechat_login_mark_persisted,
#endif
        },
    }));
    ESP_ERROR_CHECK(wifi_manager_register_state_callback(on_wifi_state_changed, NULL));

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_config->wifi_ssid,
        .sta_password = s_config->wifi_password,
        .ap_ssid = s_config->ap_ssid[0] ? s_config->ap_ssid : NULL,
        .ap_password = s_config->ap_password[0] ? s_config->ap_password : NULL,
        .ap_behavior = s_config->ap_behavior,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        ESP_ERROR_CHECK(http_server_start());
        if (captive_dns_start(&(captive_dns_config_t) {
                .ap_netif = wifi_manager_get_ap_netif(),
                .configure_dhcp_dns = true,
            }) != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
        }

        if (s_config->wifi_ssid[0] != '\0') {
            if (wifi_manager_wait_connected(30000) == ESP_OK) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);
            } else {
                ESP_LOGW(TAG, "STA could not connect, dropped to AP fallback");
            }
        }

        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
        if (status.ap_active) {
            const char *portal_auth = s_config->ap_password[0] ? "wpa2" : "open";
            ESP_LOGW(TAG,
                     "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
                     status.ap_ssid,
                     portal_auth,
                     status.ap_ip,
                     status.ap_ip);
        }
    }

    /* First-use setup: the provisioning portal (wifi_manager_start +
     * http_server_start + captive_dns_start) is ALWAYS up first
     * (Codex N3) before this runs, and the app_claw interactive/agent
     * start follows. If mandatory config is missing, show the
     * placeholder first-use UI. Fail-soft (Codex B1/B2): ANY failure
     * here is logged and boot CONTINUES to the normal emote + portal
     * behavior — first-use setup must NEVER block or brick boot. */
    {
        /* Heap-allocate setup_fields_t (~1.4KB): a stack local here is
         * live for the whole app_main frame, including the earlier deep
         * emote/heap-walk call chain, and overflows the main task stack
         * (-> heap corruption -> boot loop). Fail-soft: if alloc fails,
         * skip the first-use check and continue boot (never brick). */
        setup_fields_t *f = calloc(1, sizeof(*f));
        if (f == NULL) {
            ESP_LOGW(TAG,
                     "first-use check skipped: alloc failed; continuing boot");
        } else {
            if (s_config) {
                strlcpy(f->wifi_ssid, s_config->wifi_ssid,
                        sizeof(f->wifi_ssid));
                strlcpy(f->wifi_password, s_config->wifi_password,
                        sizeof(f->wifi_password));
                strlcpy(f->llm_backend_type, s_config->llm_backend_type,
                        sizeof(f->llm_backend_type));
                strlcpy(f->llm_base_url, s_config->llm_base_url,
                        sizeof(f->llm_base_url));
                strlcpy(f->llm_model, s_config->llm_model,
                        sizeof(f->llm_model));
                strlcpy(f->llm_api_key, s_config->llm_api_key,
                        sizeof(f->llm_api_key));
            } else {
                ESP_LOGW(TAG, "s_config NULL; first-use check on empty fields");
            }
            if (setup_first_use_required(f)) {
                ESP_LOGI(TAG, "First-use setup required; launching setup UI");
                esp_err_t setup_err = setup_ui_run(SETUP_MODE_FIRST_USE);
                if (setup_err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "setup_ui_run failed: %s — continuing boot",
                             esp_err_to_name(setup_err));
                }
            } else {
                ESP_LOGI(TAG, "Config complete; skipping first-use setup");
            }
            free(f);
        }
    }

    ESP_ERROR_CHECK(app_claw_init_storage_paths(s_claw_paths));
    ESP_ERROR_CHECK(app_claw_start(s_claw_config, s_claw_paths));
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_ERROR_CHECK(http_server_webim_bind_im());
#endif

    /* Phase 5.1: arm the physical SELECT button now that the board
     * manager (esp_board_manager_init, ~L424) and the emote/display
     * path (app_claw_ui_start / app_claw_start) are up. Fail-soft:
     * select_button_init never aborts; on any failure SELECT just
     * won't work and boot continues normally. Unconditional in the
     * linear app_main flow -> reachable on every boot. */
    select_button_init();

    /* Task 4.3b: onboard RGB-LED glue. Started here -- after the board
     * manager + gpio_expander_io are up (same prerequisites as
     * select_button_init) -- so the LED mirrors the on-screen status
     * circle from the SAME status_state aggregator. Fail-soft: returns
     * ESP_OK on every path, the LED is cosmetic and never gates boot. */
    (void)status_led_start();

    register_wifi_command();
#if CONFIG_APP_EMOTE_DEBUG_CONSOLE
    register_emote_debug_command();
#endif

#if APP_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif

    app_free_runtime_state();
}
