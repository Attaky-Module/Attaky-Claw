/*
 * Attaky Agent Deck 1.0 board-specific factory entries for ESP Board Manager.
 *
 * Provides the AW9523B IO-expander factory (used by every gpio_expander
 * device on the shared I2C bus) and the ST7789 LCD panel factory.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_io_expander.h"
#include "esp_io_expander_aw9523b.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_board_manager_includes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AGENT_DECK_1_0_SETUP_DEVICE";

esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle, const uint16_t dev_addr, esp_io_expander_handle_t *handle_ret)
{
    esp_err_t ret = esp_io_expander_new_aw9523b(i2c_handle, dev_addr, handle_ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create AW9523B IO expander @ 0x%02x: %s",
                 dev_addr, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = esp_lcd_new_panel_st7789(io, panel_dev_config, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel handle: %s", esp_err_to_name(ret));
    }
    return ret;
}

/*
 * FT6636 capacitive touch factory. The CTP_RESET line is NOT a direct
 * ESP32 GPIO: it hangs off internal AW9523B @0x59 (board device
 * "gpio_expander_io") P13. With the driver's linear pin numbering the
 * 16 IOs map datasheet P00..P07 -> bits 0..7 and P10..P17 -> bits
 * 8..15, so P13 = bit 11 = IO_EXPANDER_PIN_NUM_11. P13 is already
 * declared OUTPUT in board_devices.yaml output_io_mask, so we only
 * drive its level here. We must NOT disturb pins 8/9/10 (P10/P11/P12 =
 * RGB R/G/B). Failures fail soft: a touch-init error must never brick
 * boot, so no ESP_ERROR_CHECK / abort / assert on this path.
 */
#define CTP_RESET_PIN IO_EXPANDER_PIN_NUM_11

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *touch_dev_config, esp_lcd_touch_handle_t *ret_touch)
{
    esp_lcd_touch_config_t touch_cfg = {0};
    memcpy(&touch_cfg, touch_dev_config, sizeof(esp_lcd_touch_config_t));

    /* The gpio_expander_io device handle is an esp_io_expander_handle_t *
     * (dev_gpio_expander_init allocates and stores the handle by
     * pointer), so dereference once to get the usable handle. */
    void *exp_dev_handle = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle("gpio_expander_io", &exp_dev_handle);
    if (ret != ESP_OK || exp_dev_handle == NULL) {
        ESP_LOGE(TAG, "Failed to get gpio_expander_io handle for CTP reset: %s",
                 esp_err_to_name(ret));
        return (ret != ESP_OK) ? ret : ESP_ERR_INVALID_STATE;
    }
    esp_io_expander_handle_t exp = *(esp_io_expander_handle_t *)exp_dev_handle;
    if (exp == NULL) {
        ESP_LOGE(TAG, "gpio_expander_io IO expander handle is NULL; skipping CTP reset");
        return ESP_ERR_INVALID_STATE;
    }

    /* Defensively ensure only P13 is an output; never touch the RGB
     * pins (P10/P11/P12 = bits 8/9/10). */
    ret = esp_io_expander_set_dir(exp, CTP_RESET_PIN, IO_EXPANDER_OUTPUT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set CTP_RESET (P13) direction: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Active-low reset pulse: assert LOW, hold, release HIGH, settle.
     * The FT6636 (FT6x36 family) needs ~300 ms after RESET deassert to
     * finish its internal boot/calibration before it senses touch and
     * before its config registers latch; a too-short settle leaves the
     * controller alive on I2C but reporting TD_STATUS == 0 forever. */
    ret = esp_io_expander_set_level(exp, CTP_RESET_PIN, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to assert CTP_RESET (P13) low: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    ret = esp_io_expander_set_level(exp, CTP_RESET_PIN, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to release CTP_RESET (P13) high: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    ret = esp_lcd_touch_new_i2c_ft5x06(io, &touch_cfg, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ft5x06 touch driver: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Created ft5x06 touch driver (FT6636 @ 7-bit 0x38), CTP_RESET via AW9523B@0x59 P13");
    return ESP_OK;
}
