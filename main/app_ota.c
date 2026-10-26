#include "app_ota.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "app_sd.h"

static const char *TAG = "ota";

#define OTA_FILE        APP_SD_MOUNT "/ota.bin"
#define OTA_MIN_BYTES   (128 * 1024)
#define OTA_MAX_BYTES   (4 * 1024 * 1024)
#define OTA_CHUNK       4096
#define OTA_HEALTH_MS   30000   // confirm window after booting a new image

static SemaphoreHandle_t s_lock;
static app_ota_info_t s_info;
static FILE *s_fp;
static uint8_t *s_buf;
static size_t s_total;
static size_t s_written;
static int64_t s_boot_us;
static volatile int s_cmd;      // 1 = apply staged image, 2 = rollback

// Size of a leftover staging file, 0 when it is missing / not a firmware image.
// A power cut mid-upload can leave a truncated file; the size and magic checks
// here only decide whether resuming is worth offering, the real validation is
// esp_ota_end() after the image has been written to flash.
static size_t staged_image_size(void)
{
    size_t size = 0;
    app_fs_lock();
    FILE *fp = fopen(OTA_FILE, "rb");
    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0) {
            long len = ftell(fp);
            if (len >= (long)OTA_MIN_BYTES && len <= (long)OTA_MAX_BYTES &&
                fseek(fp, 0, SEEK_SET) == 0) {
                uint8_t magic = 0;
                if (fread(&magic, 1, 1, fp) == 1 && magic == 0xE9) {
                    size = (size_t)len;
                }
            }
        }
        fclose(fp);
    }
    app_fs_unlock();
    return size;
}

static void set_state(uint8_t phase, const char *msg)
{
    s_info.phase = phase;
    snprintf(s_info.msg, sizeof(s_info.msg), "%s", msg ? msg : "");
}

static int slot_of(const esp_partition_t *part)
{
    if (!part) {
        return -1;
    }
    if (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        return 0;
    }
    if (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        return 1;
    }
    return -1;
}

static void init_state(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *desc = esp_app_get_description();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_info, 0, sizeof(s_info));
    s_info.available = update != NULL;
    s_info.running_slot = slot_of(running);
    s_info.next_slot = slot_of(update);
    s_info.running_size = running ? running->size : 0;
    if (desc && desc->version[0]) {
        snprintf(s_info.version, sizeof(s_info.version), "%s", desc->version);
    } else {
        snprintf(s_info.version, sizeof(s_info.version), "unknown");
    }
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_info.pending_verify = true;
        set_state(APP_OTA_PENDING, "new image pending verification");
    } else {
        set_state(APP_OTA_IDLE, "idle");
    }
    xSemaphoreGive(s_lock);

    // A power cut during upload/install can leave a complete image in the SD
    // staging file. Offer it again so the user only has to press install; a
    // truncated file is rejected later by esp_ota_end().
    if (!s_info.pending_verify) {
        size_t staged = staged_image_size();
        if (staged > 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_total = staged;
            s_written = staged;
            s_info.phase = APP_OTA_READY;
            s_info.total = (uint32_t)staged;
            s_info.written = (uint32_t)staged;
            set_state(APP_OTA_READY, "staged image recovered after reboot");
            xSemaphoreGive(s_lock);
            ESP_LOGW(TAG, "recovered staged image: %u bytes", (unsigned)staged);
        }
    }

    ESP_LOGI(TAG, "slot running=%d next=%d version=%s pending=%d",
             s_info.running_slot, s_info.next_slot, s_info.version,
             s_info.pending_verify ? 1 : 0);
}

static void remove_staged(void)
{
    app_fs_lock();
    remove(OTA_FILE);
    app_fs_unlock();
}

static void install_staged(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_info.phase == APP_OTA_READY;
    if (ready) {
        set_state(APP_OTA_INSTALLING, "installing");
    }
    xSemaphoreGive(s_lock);
    if (!ready) {
        return;
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        set_state(APP_OTA_FAILED, "no OTA slot");
        xSemaphoreGive(s_lock);
        return;
    }

    app_fs_lock();
    FILE *fp = fopen(OTA_FILE, "rb");
    app_fs_unlock();
    if (!fp) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        set_state(APP_OTA_FAILED, "staged image missing");
        xSemaphoreGive(s_lock);
        return;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &handle);
    int chunk = 0;
    if (err == ESP_OK && !s_buf) {
        esp_ota_abort(handle);
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        while (1) {
            app_fs_lock();
            size_t n = fread(s_buf, 1, OTA_CHUNK, fp);
            app_fs_unlock();
            if (n == 0) {
                break;
            }
            chunk += (int)n;
            err = esp_ota_write(handle, s_buf, n);
            if (err != ESP_OK) {
                break;
            }
        }
    }
    if (err == ESP_OK) {
        // Validates the written image (magic, segments, checksum).
        err = esp_ota_end(handle);
    } else {
        esp_ota_abort(handle);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(update);
    }
    app_fs_lock();
    fclose(fp);
    app_fs_unlock();
    remove_staged();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "install failed: %s (written %d)", esp_err_to_name(err), chunk);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        set_state(APP_OTA_FAILED, esp_err_to_name(err));
        xSemaphoreGive(s_lock);
        return;
    }

    ESP_LOGW(TAG, "OTA installed to slot %d (%d bytes), rebooting", slot_of(update), chunk);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_state(APP_OTA_APPLIED, "installed, rebooting");
    xSemaphoreGive(s_lock);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void do_rollback(void)
{
    ESP_LOGW(TAG, "rollback requested");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_state(APP_OTA_ROLLED_BACK, "rollback, rebooting");
    xSemaphoreGive(s_lock);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    // Only reached when there is no pending image to roll back from.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_state(APP_OTA_FAILED, err == ESP_OK ? "rollback failed" : esp_err_to_name(err));
    xSemaphoreGive(s_lock);
}

void app_ota_tick(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool pending = s_info.pending_verify;
    xSemaphoreGive(s_lock);
    if (!pending) {
        return;
    }
    if (esp_timer_get_time() - s_boot_us < (int64_t)OTA_HEALTH_MS * 1000) {
        return;
    }
    if (app_ota_confirm() == 0) {
        ESP_LOGI(TAG, "image confirmed after %d ms health window", OTA_HEALTH_MS);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.pending_verify = false;
        set_state(APP_OTA_IDLE, "image confirmed");
        xSemaphoreGive(s_lock);
    }
}

static void ota_task(void *arg)
{
    (void)arg;
    init_state();
    while (1) {
        int cmd = s_cmd;
        if (cmd == 1) {
            s_cmd = 0;
            install_staged();
        } else if (cmd == 2) {
            s_cmd = 0;
            do_rollback();
        }
        app_ota_tick();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_ota_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_boot_us = esp_timer_get_time();
    s_buf = heap_caps_malloc(OTA_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        s_buf = heap_caps_malloc(OTA_CHUNK, MALLOC_CAP_8BIT);
    }
    if (!s_buf) {
        ESP_LOGE(TAG, "no memory for OTA buffer");
        return;
    }
    // The task only touches flash/SD, never ISRs: PSRAM stack is safe.
#define OTA_STACK 4096
    StackType_t *stack = heap_caps_malloc(OTA_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *ctrl = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stack || !ctrl) {
        ESP_LOGE(TAG, "no memory for OTA task");
        return;
    }
    xTaskCreateStatic(ota_task, "ota", OTA_STACK / sizeof(StackType_t), NULL, 5, stack, ctrl);
}

void app_ota_get_info(app_ota_info_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        snprintf(out->msg, sizeof(out->msg), "not started");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);
}

int app_ota_download_begin(size_t total)
{
    if (!s_lock) {
        return -1;
    }
    if (total < OTA_MIN_BYTES || total > OTA_MAX_BYTES) {
        return -3;
    }
    if (!app_sd_is_mounted()) {
        return -2;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_info.phase == APP_OTA_DOWNLOADING || s_info.phase == APP_OTA_INSTALLING) {
        xSemaphoreGive(s_lock);
        return -1;
    }
    app_fs_lock();
    FILE *fp = fopen(OTA_FILE, "wb");
    app_fs_unlock();
    if (!fp) {
        set_state(APP_OTA_FAILED, "cannot create staging file");
        xSemaphoreGive(s_lock);
        return -4;
    }
    s_fp = fp;
    s_total = total;
    s_written = 0;
    s_info.total = (uint32_t)total;
    s_info.written = 0;
    set_state(APP_OTA_DOWNLOADING, "receiving");
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "download begin: %u bytes", (unsigned)total);
    return 0;
}

int app_ota_download_write(const uint8_t *data, size_t len)
{
    if (!s_lock || !data || len == 0) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_fp || s_written + len > s_total) {
        xSemaphoreGive(s_lock);
        return -2;
    }
    app_fs_lock();
    size_t w = fwrite(data, 1, len, s_fp);
    app_fs_unlock();
    if (w != len) {
        set_state(APP_OTA_FAILED, "SD write failed");
        xSemaphoreGive(s_lock);
        return -3;
    }
    s_written += len;
    s_info.written = (uint32_t)s_written;
    xSemaphoreGive(s_lock);
    return 0;
}

int app_ota_download_end(void)
{
    if (!s_lock) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_fp) {
        xSemaphoreGive(s_lock);
        return -1;
    }
    app_fs_lock();
    fflush(s_fp);
    fsync(fileno(s_fp));
    fclose(s_fp);
    app_fs_unlock();
    s_fp = NULL;
    size_t written = s_written;
    size_t total = s_total;
    xSemaphoreGive(s_lock);

    if (written != total) {
        remove_staged();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        set_state(APP_OTA_FAILED, "size mismatch");
        xSemaphoreGive(s_lock);
        return -2;
    }

    // Minimal image sanity check before allowing the install step; the real
    // validation is esp_ota_end() after the image is written to flash.
    uint8_t head[8] = {0};
    app_fs_lock();
    FILE *fp = fopen(OTA_FILE, "rb");
    size_t got = fp ? fread(head, 1, sizeof(head), fp) : 0;
    if (fp) {
        fclose(fp);
    }
    app_fs_unlock();
    if (got < sizeof(head) || head[0] != 0xE9) {
        remove_staged();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        set_state(APP_OTA_FAILED, "not a firmware image");
        xSemaphoreGive(s_lock);
        return -3;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_state(APP_OTA_READY, "image staged, apply to install");
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "download complete: %u bytes staged", (unsigned)total);
    return 0;
}

void app_ota_download_abort(void)
{
    if (!s_lock || !s_fp) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    app_fs_lock();
    fclose(s_fp);
    s_fp = NULL;
    remove(OTA_FILE);
    app_fs_unlock();
    set_state(APP_OTA_IDLE, "transfer aborted");
    xSemaphoreGive(s_lock);
}

bool app_ota_has_image(void)
{
    bool ok = false;
    app_fs_lock();
    FILE *fp = fopen(OTA_FILE, "rb");
    if (fp) {
        fclose(fp);
        ok = true;
    }
    app_fs_unlock();
    return ok;
}

void app_ota_request_apply(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_info.phase == APP_OTA_READY;
    xSemaphoreGive(s_lock);
    if (ready) {
        s_cmd = 1;
    } else {
        ESP_LOGW(TAG, "apply ignored: no staged image");
    }
}

void app_ota_request_rollback(void)
{
    if (!s_lock) {
        return;
    }
    s_cmd = 2;
}

int app_ota_confirm(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return 0;   // already confirmed, nothing to do
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "confirm failed: %s", esp_err_to_name(err));
        return (int)err;
    }
    return 0;
}
