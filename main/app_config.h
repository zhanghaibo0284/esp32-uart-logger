#pragma once

#include <stdbool.h>
#include <stddef.h>

#define APP_CONFIG_FILE_BYTES 8192

// Install PSRAM-backed cJSON hooks; call before any other config function.
void app_config_init(void);

// Return codes: 0 = applied, 1 = file absent, <0 = error/invalid.
int app_config_load(void);

// Export current settings to config.json atomically (old file becomes .bak).
int app_config_export(void);

// needs=true when the file is absent or its content differs from the current
// settings snapshot (used by automatic change detection).
int app_config_needs_export(bool *needs);

// Validate and apply JSON text (upload / shared import path).
int app_config_import_text(const char *text, size_t len);

// Restore config.json.bak and apply it.
int app_config_restore_backup(void);

// Build the canonical one-line JSON snapshot of the current configuration.
void app_config_build_snapshot(char *out, size_t out_len);

typedef struct {
    bool present;
    bool backup_present;
    bool last_ok;
    char msg[80];
} app_config_status_t;

void app_config_get_status(app_config_status_t *out);
