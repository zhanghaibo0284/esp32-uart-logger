#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#define APP_SD_MOUNT "/sdcard"

/* 生成指定时间对应的日志日期目录名（格式 YYYY-MM-DD）。
 * 参数 t：日志时间；buf/n 输出缓冲。结果形如 "2026-10-27"。
 */
void app_sd_date_folder(time_t t, char *buf, size_t n);

/* 生成指定通道在给定时间的日志文件完整路径。
 * 目录结构：/sdcard/YYYY-MM-DD/UARTx/YYYYmmdd_HHMMSS.txt
 * 一级目录按自然日切分，二级目录按通道（业务分类）切分，同日内所有日志归入对应日期目录。
 * 参数 t 应为分段起点（ensure_file 对齐后的时间），确保文件名恒定。
 */
void app_sd_log_path(int port, time_t t, char *buf, size_t n);

/* 确保指定通道的日志目录存在：/sdcard/YYYY-MM-DD 与 /sdcard/YYYY-MM-DD/UARTx。
 * 幂等：目录已存在直接成功（mkdir 返回 EEXIST）。
 * 返回 true=目录就绪可写入；false=创建失败（调用方应降级，不写文件）。
 * 内部对 app_fs_lock 加锁（递归锁，调用方即使已持有也安全）。
 */
bool app_sd_ensure_log_dir(int port, time_t t);

esp_err_t app_sd_mount(void);
bool app_sd_is_mounted(void);
void app_sd_usage(uint32_t *total_mb, uint32_t *free_mb);
void app_sd_refresh_usage(void);
const char *app_sd_last_error(void);
void app_fs_lock(void);
void app_fs_unlock(void);
// Non-blocking lock attempt: true on success (pair with app_fs_unlock).
bool app_fs_trylock(void);
