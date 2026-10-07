#pragma once

#include <stdbool.h>

#define APP_WIFI_SSID_LEN 32
#define APP_WIFI_PASS_LEN 63

typedef struct {
    bool up;
    int clients;
    char ssid[APP_WIFI_SSID_LEN + 1];
    char pass[APP_WIFI_PASS_LEN + 1];
    char ip[16];
} app_wifi_info_t;

void app_wifi_start(void);
void app_wifi_get(app_wifi_info_t *out);
int app_wifi_apply(const char *ssid, const char *pass);

// Persist SSID/password to NVS only (no AP restart). Returns 0 on success.
int app_wifi_persist(const char *ssid, const char *pass);
