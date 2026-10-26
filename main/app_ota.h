#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// OTA state machine. A firmware image is first staged on the TF card
// (/sdcard/ota.bin), then installed into the inactive OTA slot by a dedicated
// task. The boot slot only switches after the whole image has been written and
// validated, so a power loss at any point leaves the previous firmware bootable.
typedef enum {
    APP_OTA_IDLE = 0,        // nothing staged
    APP_OTA_DOWNLOADING = 1, // HTTP upload in progress
    APP_OTA_READY = 2,       // staged file verified, waiting for apply
    APP_OTA_INSTALLING = 3,  // writing/validating the inactive slot
    APP_OTA_APPLIED = 4,     // slot validated, reboot pending
    APP_OTA_FAILED = 5,      // last transfer/install failed
    APP_OTA_PENDING = 6,     // running a freshly flashed image, not confirmed
    APP_OTA_ROLLED_BACK = 7, // explicit rollback was requested
} app_ota_phase_t;

typedef struct {
    bool available;         // partition table exposes an OTA slot
    bool pending_verify;    // new image is running but not confirmed yet
    int running_slot;       // 0/1 = ota_0/ota_1, -1 = factory
    int next_slot;          // slot that the next install would use, -1 none
    char version[32];
    uint32_t running_size;
    uint32_t total;
    uint32_t written;
    uint8_t phase;          // app_ota_phase_t
    char msg[80];
} app_ota_info_t;

void app_ota_start(void);
void app_ota_get_info(app_ota_info_t *out);

// Staged-image transfer (HTTP upload). begin -> write* -> end/abort.
int app_ota_download_begin(size_t total);
int app_ota_download_write(const uint8_t *data, size_t len);
int app_ota_download_end(void);
void app_ota_download_abort(void);
bool app_ota_has_image(void);

// Install the staged image and reboot; executes asynchronously in the OTA task.
void app_ota_request_apply(void);
// Revert to the previous firmware and reboot (no-op without a pending OTA).
void app_ota_request_rollback(void);
// Mark the running image valid, cancelling automatic rollback. Returns 0 on
// success, otherwise the esp_err_t value.
int app_ota_confirm(void);

// Periodic housekeeping: auto-confirms a healthy image after the boot window.
void app_ota_tick(void);
