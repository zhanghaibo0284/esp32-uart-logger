#include "app_wifi.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "wifi";
static const char *NVS_NS = "logger";
static const char *DEFAULT_SSID = "UART-LOG";
static const char *DEFAULT_PASS = "12345678";

static SemaphoreHandle_t s_lock;
static app_wifi_info_t s_info;
static bool s_inited;

static bool ascii_token(const char *text, int min_len, int max_len)
{
    if (!text) {
        return false;
    }
    int n = (int)strlen(text);
    if (n < min_len || n > max_len) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x21 || ch > 0x7e) {
            return false;
        }
    }
    return true;
}

static void load_saved(char *ssid, char *pass)
{
    snprintf(ssid, APP_WIFI_SSID_LEN + 1, "%s", DEFAULT_SSID);
    snprintf(pass, APP_WIFI_PASS_LEN + 1, "%s", DEFAULT_PASS);
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t ssid_len = APP_WIFI_SSID_LEN + 1;
    size_t pass_len = APP_WIFI_PASS_LEN + 1;
    char loaded_ssid[APP_WIFI_SSID_LEN + 1] = {0};
    char loaded_pass[APP_WIFI_PASS_LEN + 1] = {0};
    esp_err_t ssid_err = nvs_get_str(handle, "ap_ssid", loaded_ssid, &ssid_len);
    esp_err_t pass_err = nvs_get_str(handle, "ap_pass", loaded_pass, &pass_len);
    nvs_close(handle);
    if (ssid_err == ESP_OK && ascii_token(loaded_ssid, 1, APP_WIFI_SSID_LEN)) {
        snprintf(ssid, APP_WIFI_SSID_LEN + 1, "%s", loaded_ssid);
    }
    if (pass_err == ESP_OK && ascii_token(loaded_pass, 8, APP_WIFI_PASS_LEN)) {
        snprintf(pass, APP_WIFI_PASS_LEN + 1, "%s", loaded_pass);
    }
}

static void save_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_str(handle, "ap_ssid", ssid);
    nvs_set_str(handle, "ap_pass", pass);
    nvs_commit(handle);
    nvs_close(handle);
}

static void publish(bool up, const char *ssid, const char *pass, const char *ip)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.up = up;
    snprintf(s_info.ssid, sizeof(s_info.ssid), "%s", ssid ? ssid : "");
    snprintf(s_info.pass, sizeof(s_info.pass), "%s", pass ? pass : "");
    snprintf(s_info.ip, sizeof(s_info.ip), "%s", ip && ip[0] ? ip : "192.168.4.1");
    xSemaphoreGive(s_lock);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.clients++;
        xSemaphoreGive(s_lock);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED && s_info.clients > 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.clients--;
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t start_ap(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), "%s", ssid);
    cfg.ap.ssid_len = strlen(ssid);
    snprintf((char *)cfg.ap.password, sizeof(cfg.ap.password), "%s", pass);
    cfg.ap.channel = 6;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        esp_wifi_stop();
        err = esp_wifi_start();
        if (err != ESP_OK) {
            return err;
        }
    }
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        char text[16];
        snprintf(text, sizeof(text), IPSTR, IP2STR(&ip.ip));
        publish(true, ssid, pass, text);
    } else {
        publish(true, ssid, pass, "192.168.4.1");
    }
    ESP_LOGI(TAG, "AP %s key %s ip %s", ssid, pass, s_info.ip);
    return ESP_OK;
}

void app_wifi_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    char ssid[APP_WIFI_SSID_LEN + 1];
    char pass[APP_WIFI_PASS_LEN + 1];
    load_saved(ssid, pass);
    publish(false, ssid, pass, "192.168.4.1");

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop %s", esp_err_to_name(err));
        return;
    }
    if (!s_inited) {
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&init_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi init %s", esp_err_to_name(err));
            return;
        }
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
        s_inited = true;
    }
    err = start_ap(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP start %s", esp_err_to_name(err));
        publish(false, ssid, pass, "192.168.4.1");
    }
}

void app_wifi_get(app_wifi_info_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        snprintf(out->ssid, sizeof(out->ssid), "%s", DEFAULT_SSID);
        snprintf(out->pass, sizeof(out->pass), "%s", DEFAULT_PASS);
        snprintf(out->ip, sizeof(out->ip), "192.168.4.1");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);
}

int app_wifi_apply(const char *ssid, const char *pass)
{
    if (!ascii_token(ssid, 1, APP_WIFI_SSID_LEN) || !ascii_token(pass, 8, APP_WIFI_PASS_LEN)) {
        return -1;
    }
    if (!s_inited) {
        app_wifi_start();
    }
    save_nvs(ssid, pass);
    if (!s_inited) {
        return -1;
    }
    esp_wifi_stop();
    if (start_ap(ssid, pass) != ESP_OK) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.clients = 0;
    xSemaphoreGive(s_lock);
    return 0;
}
