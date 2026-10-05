#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_SD_MOUNT "/sdcard"

esp_err_t app_sd_mount(void);
bool app_sd_is_mounted(void);
void app_sd_usage(uint32_t *total_mb, uint32_t *free_mb);
void app_sd_refresh_usage(void);
const char *app_sd_last_error(void);
void app_fs_lock(void);
void app_fs_unlock(void);
