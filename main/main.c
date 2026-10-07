#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "myiic.h"
#include "xl9555.h"
#include "led.h"
#include "app_settings.h"
#include "app_time.h"
#include "app_sd.h"
#include "app_logger.h"
#include "app_config.h"
#include "app_cmd.h"
#include "app_ui.h"
#include "app_wifi.h"
#include "app_httpd.h"
#include "app_bridge.h"
#include "esp_heap_caps.h"
#include "lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "app";

static void ui_task(void *arg)
{
    lvgl_port_init();
    app_ui_init();
    ESP_LOGI(TAG, "ui ready");
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    app_time_init();
    app_settings_init();
    app_time_restore();

    led_init();
    i2c_obj_t i2c0 = iic_init(I2C_NUM_0);
    xl9555_init(i2c0);

    if (app_sd_mount() != ESP_OK) {
        ESP_LOGW(TAG, "SD card not ready");
    }
    app_config_init();
    int cfg_rc = app_config_load();
    if (cfg_rc == 0) {
        ESP_LOGI(TAG, "config.json loaded");
    } else if (cfg_rc == 1) {
        ESP_LOGI(TAG, "config.json absent, will create on first export");
    } else {
        ESP_LOGW(TAG, "config.json invalid, repairing from current settings");
        app_config_export();
    }
    app_logger_start();
    BaseType_t ui_ret = xTaskCreatePinnedToCore(ui_task, "ui", 16384, NULL, 4, NULL, 1);
    if (ui_ret != pdPASS) {
        ESP_LOGE(TAG, "ui task create failed: %d, internal free %u",
                 (int)ui_ret, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    app_bridge_ble_init();
    app_cmd_start();
    if (app_sd_is_mounted()) {
        app_logger_request_sd_test();
    }

    ESP_LOGI(TAG, "UART0 TX43/RX44  UART1 TX5/RX6  UART2 TX8/RX18");
    ESP_LOGI(TAG, "internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    app_wifi_start();
    app_wifi_info_t wifi;
    app_wifi_get(&wifi);
    if (wifi.up) {
        app_httpd_start();
    } else {
        ESP_LOGW(TAG, "wifi not up, web disabled");
    }
    app_bridge_tcp_resume();
}
