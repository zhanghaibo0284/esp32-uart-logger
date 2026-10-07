idf_component_register(SRCS
                       "main.c"
                       "app_settings.c"
                       "app_time.c"
                       "app_sd.c"
                       "app_logger.c"
                       "lvgl_port.c"
                       "app_ui.c"
                       "app_files.c"
                       "app_cmd.c"
                       "app_wifi.c"
                       "app_httpd.c"
                       "app_bridge.c"
                       "app_modbus.c"
                       "app_config.c"
                       "dns_server.c"
                       "font_cn_16.c"
                    INCLUDE_DIRS "."
                    REQUIRES BSP nvs_flash fatfs sdmmc esp_timer driver console esp_wifi esp_event esp_netif esp_http_server bt json)

target_compile_options(${COMPONENT_LIB} PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
