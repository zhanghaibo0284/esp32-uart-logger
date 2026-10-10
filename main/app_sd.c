#include "app_sd.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sd";
#define SD_PIN_MOSI 16
#define SD_PIN_MISO 15
#define SD_PIN_CLK  7
#define SD_PIN_CS   17

static sdmmc_card_t *s_card;
static bool s_bus_ready;
static bool s_mounted;
static uint32_t s_total_mb;
static uint32_t s_free_mb;
static char s_error[48] = "未挂载";
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_fs_lock;

void app_fs_lock(void)
{
    if (!s_fs_lock) {
        s_fs_lock = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(s_fs_lock, portMAX_DELAY);
}

void app_fs_unlock(void)
{
    if (s_fs_lock) {
        xSemaphoreGiveRecursive(s_fs_lock);
    }
}

bool app_fs_trylock(void)
{
    if (!s_fs_lock) {
        s_fs_lock = xSemaphoreCreateRecursiveMutex();
    }
    return xSemaphoreTakeRecursive(s_fs_lock, 0) == pdTRUE;
}

static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text);
}

static void ensure_dirs(void)
{
    for (int i = 0; i < 3; i++) {
        char path[32];
        snprintf(path, sizeof(path), APP_SD_MOUNT "/UART%d", i);
        if (mkdir(path, 0777) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s failed errno=%d", path, errno);
        }
    }
}

void app_sd_refresh_usage(void)
{
    if (!s_mounted) {
        return;
    }
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(APP_SD_MOUNT, &total, &free_bytes) != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_total_mb = (uint32_t)(total / (1024 * 1024));
    s_free_mb = (uint32_t)(free_bytes / (1024 * 1024));
    xSemaphoreGive(s_lock);
}

esp_err_t app_sd_mount(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(APP_SD_MOUNT, s_card);
        s_card = NULL;
        s_mounted = false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 20000;
    if (!s_bus_ready) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = SD_PIN_MOSI,
            .miso_io_num = SD_PIN_MISO,
            .sclk_io_num = SD_PIN_CLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 16384,
        };
        esp_err_t bus_err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "spi bus init failed: %s", esp_err_to_name(bus_err));
            set_error("SPI失败");
            return bus_err;
        }
        s_bus_ready = true;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = host.slot;
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(APP_SD_MOUNT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        s_mounted = false;
        s_card = NULL;
        s_bus_ready = false;
        esp_err_t free_err = spi_bus_free(host.slot);
        if (free_err != ESP_OK && free_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "spi bus free: %s", esp_err_to_name(free_err));
        }
        set_error("挂载失败");
        ESP_LOGW(TAG, "SD未挂载。64GB卡通常是exFAT，已打开exFAT支持");
        return err;
    }

    s_mounted = true;
    ensure_dirs();
    app_sd_refresh_usage();
    set_error("正常");
    ESP_LOGI(TAG, "mounted, total=%luMB free=%luMB", (unsigned long)s_total_mb, (unsigned long)s_free_mb);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool app_sd_is_mounted(void)
{
    return s_mounted;
}

void app_sd_usage(uint32_t *total_mb, uint32_t *free_mb)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (total_mb) {
        *total_mb = s_total_mb;
    }
    if (free_mb) {
        *free_mb = s_free_mb;
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

const char *app_sd_last_error(void)
{
    return s_error;
}
