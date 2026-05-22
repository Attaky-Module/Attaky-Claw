idf_component_register(
    SRCS "setup_validate.c" "setup_sm.c" "setup_model.c" "setup_lvgl_port.c" "setup_worker.c" "setup_screens.c" "setup_ui.c"
         "setup_status_format.c"
    INCLUDE_DIRS "include" "."
    REQUIRES app_config wifi_manager claw_core display_arbiter esp_lcd_touch lvgl esp_lcd driver
             esp_timer emote espressif__esp_board_manager
)
