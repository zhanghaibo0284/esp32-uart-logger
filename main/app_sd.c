#include "app_sd.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
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

/* 获取文件系统互斥锁（递归锁，可重入）。
 * 用途：日志写入、Web 文件浏览/下载/删除、屏幕文件浏览器之间共享 FAT，
 * 所有访问 SD 卡的文件操作都必须包在 lock/unlock 之间。
 * 首次调用时才创建锁，允许在任何任务上下文中安全调用。
 */
void app_fs_lock(void)
{
    if (!s_fs_lock) {
        s_fs_lock = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(s_fs_lock, portMAX_DELAY);
}

/* 释放文件系统互斥锁；与 app_fs_lock 成对调用，未创建锁时为空操作。 */
void app_fs_unlock(void)
{
    if (s_fs_lock) {
        xSemaphoreGiveRecursive(s_fs_lock);
    }
}

/* 尝试获取文件系统锁（不等待）。
 * 返回 true=已获得（调用方需 unlock）；false=正被占用，调用方应跳过本次操作。
 * 用于不想阻塞的读操作（例如屏幕刷新时的目录预览）。
 */
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

/* 挂载时不再预建 /sdcard/UART0..2。
 * 新结构按日期分层：/sdcard/YYYY-MM-DD/UARTx/，日期目录在写入时按帧时间按需创建，
 * 因此挂载阶段无需（也无法）提前创建未来日期的目录。
 */
static void ensure_dirs(void)
{
    // 留空：日期与通道目录均由 app_sd_ensure_log_dir 在写入前按需创建。
}

/* 生成日志日期目录名 YYYY-MM-DD。 */
void app_sd_date_folder(time_t t, char *buf, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%Y-%m-%d", &tm);
}

/* 生成日志文件完整路径：/sdcard/YYYY-MM-DD/UARTx/YYYYmmdd_HHMMSS.txt。
 * 所有路径拼接集中在此函数，避免散落在多处导致目录层级演进时出错。
 */
void app_sd_log_path(int port, time_t t, char *buf, size_t n)
{
    char date[16];
    char name[24];
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(date, sizeof(date), "%Y-%m-%d", &tm);
    strftime(name, sizeof(name), "%Y%m%d_%H%M%S.txt", &tm);
    snprintf(buf, n, APP_SD_MOUNT "/%s/UART%d/%s", date, port, name);
}

/* 确保日志目录存在（日期目录 + 通道子目录）。
 * 幂等：mkdir 返回 EEXIST 视为成功；失败返回 false 供调用方降级。
 * 内部持 app_fs_lock（递归锁），与 Web/UI 的 SD 访问互斥。
 */
bool app_sd_ensure_log_dir(int port, time_t t)
{
    char date[16];
    char dir[64];
    app_sd_date_folder(t, date, sizeof(date));
    app_fs_lock();
    snprintf(dir, sizeof(dir), APP_SD_MOUNT "/%s", date);
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed errno=%d", dir, errno);
        app_fs_unlock();
        return false;
    }
    snprintf(dir, sizeof(dir), APP_SD_MOUNT "/%s/UART%d", date, port);
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed errno=%d", dir, errno);
        app_fs_unlock();
        return false;
    }
    app_fs_unlock();
    return true;
}

/* 刷新 SD 卡总容量与剩余容量（MB），供界面/Web 显示。
 * 该调用会访问 FAT 层，可能阻塞数毫秒，应在“帧间隙”由 logger 触发。
 * 未挂载或查询失败时保持上一次的值不变。
 */
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

/* 挂载 TF 卡（SD-SPI）：初始化 SPI 总线 → 挂载 FAT → 建目录 → 统计容量。
 * 返回 ESP_OK 成功；其它为 esp_err_t 错误码。
 * 关键步骤与异常处理：
 *   1. 若之前已挂载，先卸载并清空 s_card，保证可重复调用（重挂载）；
 *   2. SPI 总线只在首次初始化（s_bus_ready 记忆），重挂载不重复占用外设；
 *   3. 挂载失败时释放 SPI 总线并复位 s_bus_ready，允许下次冷重试；
 *   4. mount 参数 max_files=8（同时打开的文件数上限）、16KB 簇；
 *      format_if_mount_failed=false，绝不格式化用户卡。
 */
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

/* 返回当前是否已挂载（供 logger/UI/Web 判断能否读写）。 */
bool app_sd_is_mounted(void)
{
    return s_mounted;
}

/* 读取缓存的容量信息（单位 MB）；参数可为 NULL，表示不关心该项。 */
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
