#include "app_time.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "time";
static const char *NVS_NS = "logger";
static RTC_NOINIT_ATTR uint32_t s_trusted_magic;
#define TRUSTED_MAGIC 0x54524D31u

void app_time_init(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
}

bool app_time_is_trusted(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return s_trusted_magic == TRUSTED_MAGIC && (tm.tm_year + 1900) >= 2024;
}

void app_time_restore(void)
{
    if (app_time_is_trusted()) {
        ESP_LOGI(TAG, "clock kept across reset");
        return;
    }
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint32_t epoch = 0;
    esp_err_t err = nvs_get_u32(handle, "epoch", &epoch);
    nvs_close(handle);
    if (err == ESP_OK && epoch > 1700000000u) {
        struct timeval val = {.tv_sec = (time_t)epoch, .tv_usec = 0};
        settimeofday(&val, NULL);
        ESP_LOGW(TAG, "restored stale clock from NVS, please set time");
    }
}

static bool date_ok(int year, int month, int day, int hour, int minute, int second)
{
    if (year < 2020 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }
    static const int mdays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = mdays[month];
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && leap) {
        max_day = 29;
    }
    return day <= max_day;
}

int app_time_set(int year, int month, int day, int hour, int minute, int second)
{
    if (!date_ok(year, month, day, hour, minute, second)) {
        return -1;
    }
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = 0;
    time_t epoch = mktime(&tm);
    if (epoch == (time_t)-1) {
        return -1;
    }
    struct timeval val = {.tv_sec = epoch, .tv_usec = 0};
    if (settimeofday(&val, NULL) != 0) {
        return -1;
    }
    s_trusted_magic = TRUSTED_MAGIC;
    app_time_persist_now();
    ESP_LOGI(TAG, "clock set to %04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return 0;
}

void app_time_persist_now(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_u32(handle, "epoch", (uint32_t)time(NULL));
    nvs_commit(handle);
    nvs_close(handle);
}

void app_time_format(char *out, size_t out_len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    snprintf(out, out_len, "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}
