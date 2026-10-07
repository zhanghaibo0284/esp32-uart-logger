#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "app_settings.h"
#include "app_wifi.h"
#include "app_bridge.h"
#include "app_sd.h"

static const char *TAG = "config";

#define CFG_PATH     APP_SD_MOUNT "/config.json"
#define CFG_BAK_PATH APP_SD_MOUNT "/config.json.bak"
#define CFG_TMP_PATH APP_SD_MOUNT "/.cfg.tmp"

#define CFG_MAGIC_TEXT "uart-logger-config"
#define CFG_VERSION    1

static app_config_status_t s_status = {0};

// cJSON nodes/strings live in PSRAM: keep the internal heap intact for WiFi,
// BLE and DMA allocations.
static void *cfg_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void cfg_free(void *ptr)
{
    heap_caps_free(ptr);
}

void app_config_init(void)
{
    static bool inited;
    if (inited) {
        return;
    }
    cJSON_Hooks hooks = {
        .malloc_fn = cfg_malloc,
        .free_fn = cfg_free,
    };
    cJSON_InitHooks(&hooks);
    inited = true;
}

static void set_status(bool ok, const char *msg)
{
    s_status.last_ok = ok;
    snprintf(s_status.msg, sizeof(s_status.msg), "%s", msg ? msg : "");
    struct stat st;
    s_status.present = stat(CFG_PATH, &st) == 0;
    s_status.backup_present = stat(CFG_BAK_PATH, &st) == 0;
}

// ---------------------------------------------------------------------------
// Serialize
// ---------------------------------------------------------------------------

void app_config_build_snapshot(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';

    app_settings_t settings;
    app_wifi_info_t wifi;
    bridge_info_t bridge;
    app_settings_get(&settings);
    app_wifi_get(&wifi);
    app_bridge_get(&bridge);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "magic", CFG_MAGIC_TEXT);
    cJSON_AddNumberToObject(root, "version", CFG_VERSION);
    cJSON_AddNumberToObject(root, "segment_min", settings.segment_min);

    cJSON *ports = cJSON_AddArrayToObject(root, "ports");
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        port_setting_t *p = &settings.port[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "baud", p->baud);
        int bits = 8;
        if (p->data_bits == UART_DATA_7_BITS) bits = 7;
        else if (p->data_bits == UART_DATA_6_BITS) bits = 6;
        else if (p->data_bits == UART_DATA_5_BITS) bits = 5;
        cJSON_AddNumberToObject(item, "bits", bits);
        const char *stop = "1";
        if (p->stop_bits == UART_STOP_BITS_1_5) stop = "1.5";
        else if (p->stop_bits == UART_STOP_BITS_2) stop = "2";
        cJSON_AddStringToObject(item, "stop", stop);
        const char *parity = "N";
        if (p->parity == UART_PARITY_EVEN) parity = "E";
        else if (p->parity == UART_PARITY_ODD) parity = "O";
        cJSON_AddStringToObject(item, "parity", parity);
        cJSON_AddBoolToObject(item, "enabled", p->enabled);
        cJSON_AddItemToArray(ports, item);
    }

    cJSON *wifi_j = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi_j, "ssid", wifi.ssid);
    cJSON_AddStringToObject(wifi_j, "pass", wifi.pass);

    cJSON *br = cJSON_AddObjectToObject(root, "bridge");
    cJSON *tcp_on = cJSON_AddArrayToObject(br, "tcp_on");
    cJSON *tcp_port = cJSON_AddArrayToObject(br, "tcp_port");
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        cJSON_AddItemToArray(tcp_on, cJSON_CreateBool(bridge.tcp_on[i]));
        cJSON_AddItemToArray(tcp_port, cJSON_CreateNumber(bridge.tcp_port[i]));
    }
    cJSON_AddBoolToObject(br, "ble_on", bridge.ble_on);
    cJSON_AddNumberToObject(br, "ble_port", bridge.ble_port);
    cJSON_AddStringToObject(br, "ble_name", bridge.ble_name);

    char *printed = cJSON_PrintUnformatted(root);
    if (printed) {
        snprintf(out, out_len, "%s", printed);
        cJSON_free(printed);
    }
    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

static bool visible_ascii(const char *text, int min_len, int max_len)
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
        if (ch < 0x20 || ch > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool str_eq(const cJSON *item, const char *value)
{
    return item && cJSON_IsString(item) && strcmp(item->valuestring, value) == 0;
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

int app_config_import_text(const char *text, size_t len)
{
    if (!app_sd_is_mounted()) {
        set_status(false, "SD未挂载");
        return -1;
    }
    if (!text || len == 0 || len > APP_CONFIG_FILE_BYTES) {
        set_status(false, "配置为空或过大");
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_status(false, "JSON解析失败");
        return -1;
    }

    int rc = -1;
    const char *err = "配置无效";
    app_settings_t settings;
    app_wifi_info_t wifi;
    bridge_info_t bridge;
    app_settings_get(&settings);
    app_wifi_get(&wifi);
    app_bridge_get(&bridge);

    do {
        cJSON *magic = cJSON_GetObjectItem(root, "magic");
        cJSON *version = cJSON_GetObjectItem(root, "version");
        cJSON *seg = cJSON_GetObjectItem(root, "segment_min");
        cJSON *ports = cJSON_GetObjectItem(root, "ports");
        cJSON *wifi_j = cJSON_GetObjectItem(root, "wifi");
        cJSON *br = cJSON_GetObjectItem(root, "bridge");

        if (!str_eq(magic, CFG_MAGIC_TEXT) || !version || !cJSON_IsNumber(version) ||
            version->valueint != CFG_VERSION) {
            err = "magic/version不匹配";
            break;
        }
        if (!seg || !cJSON_IsNumber(seg) || seg->valueint < 1 || seg->valueint > 24 * 60) {
            err = "分段时长无效";
            break;
        }
        settings.segment_min = (uint16_t)seg->valueint;

        if (!ports || !cJSON_IsArray(ports) || cJSON_GetArraySize(ports) != APP_PORT_COUNT) {
            err = "ports字段无效";
            break;
        }
        for (int i = 0; i < APP_PORT_COUNT; i++) {
            cJSON *it = cJSON_GetArrayItem(ports, i);
            cJSON *baud = it ? cJSON_GetObjectItem(it, "baud") : NULL;
            cJSON *bits = it ? cJSON_GetObjectItem(it, "bits") : NULL;
            cJSON *stop = it ? cJSON_GetObjectItem(it, "stop") : NULL;
            cJSON *par = it ? cJSON_GetObjectItem(it, "parity") : NULL;
            cJSON *en = it ? cJSON_GetObjectItem(it, "enabled") : NULL;
            if (!baud || !cJSON_IsNumber(baud) || baud->valuedouble < 300 || baud->valuedouble > 2000000 ||
                !bits || !cJSON_IsNumber(bits) || !en || !cJSON_IsBool(en)) {
                err = "串口参数无效";
                goto invalid;
            }
            settings.port[i].baud = (uint32_t)baud->valuedouble;
            switch (bits->valueint) {
            case 8: settings.port[i].data_bits = UART_DATA_8_BITS; break;
            case 7: settings.port[i].data_bits = UART_DATA_7_BITS; break;
            case 6: settings.port[i].data_bits = UART_DATA_6_BITS; break;
            case 5: settings.port[i].data_bits = UART_DATA_5_BITS; break;
            default: err = "数据位无效"; goto invalid;
            }
            if (str_eq(stop, "1")) settings.port[i].stop_bits = UART_STOP_BITS_1;
            else if (str_eq(stop, "1.5")) settings.port[i].stop_bits = UART_STOP_BITS_1_5;
            else if (str_eq(stop, "2")) settings.port[i].stop_bits = UART_STOP_BITS_2;
            else { err = "停止位无效"; goto invalid; }
            if (str_eq(par, "N")) settings.port[i].parity = UART_PARITY_DISABLE;
            else if (str_eq(par, "E")) settings.port[i].parity = UART_PARITY_EVEN;
            else if (str_eq(par, "O")) settings.port[i].parity = UART_PARITY_ODD;
            else { err = "校验位无效"; goto invalid; }
            settings.port[i].enabled = cJSON_IsTrue(en);
        }

        if (!wifi_j) {
            err = "wifi字段缺失";
            break;
        }
        cJSON *ssid = cJSON_GetObjectItem(wifi_j, "ssid");
        cJSON *pass = cJSON_GetObjectItem(wifi_j, "pass");
        if (!ssid || !cJSON_IsString(ssid) ||
            !visible_ascii(ssid->valuestring, 1, APP_WIFI_SSID_LEN) ||
            !pass || !cJSON_IsString(pass) ||
            !visible_ascii(pass->valuestring, 8, APP_WIFI_PASS_LEN)) {
            err = "WiFi SSID/密码无效";
            break;
        }

        if (!br) {
            err = "bridge字段缺失";
            break;
        }
        cJSON *tcp_on = cJSON_GetObjectItem(br, "tcp_on");
        cJSON *tcp_port = cJSON_GetObjectItem(br, "tcp_port");
        cJSON *ble_on = cJSON_GetObjectItem(br, "ble_on");
        cJSON *ble_port = cJSON_GetObjectItem(br, "ble_port");
        cJSON *ble_name = cJSON_GetObjectItem(br, "ble_name");
        if (!tcp_on || !cJSON_IsArray(tcp_on) || cJSON_GetArraySize(tcp_on) != APP_PORT_COUNT ||
            !tcp_port || !cJSON_IsArray(tcp_port) || cJSON_GetArraySize(tcp_port) != APP_PORT_COUNT ||
            !ble_on || !cJSON_IsBool(ble_on) ||
            !ble_port || !cJSON_IsNumber(ble_port) ||
            ble_port->valueint < 0 || ble_port->valueint >= APP_PORT_COUNT ||
            !ble_name || !cJSON_IsString(ble_name) ||
            !visible_ascii(ble_name->valuestring, 1, BRIDGE_NAME_LEN)) {
            err = "bridge参数无效";
            break;
        }
        for (int i = 0; i < APP_PORT_COUNT; i++) {
            cJSON *on_i = cJSON_GetArrayItem(tcp_on, i);
            cJSON *port_i = cJSON_GetArrayItem(tcp_port, i);
            if (!cJSON_IsBool(on_i) || !cJSON_IsNumber(port_i) || port_i->valueint < 1024) {
                err = "TCP端口无效";
                goto invalid;
            }
            bridge.tcp_on[i] = cJSON_IsTrue(on_i);
            bridge.tcp_port[i] = (uint16_t)port_i->valueint;
        }
        bridge.ble_on = cJSON_IsTrue(ble_on);
        bridge.ble_port = ble_port->valueint;
        snprintf(bridge.ble_name, sizeof(bridge.ble_name), "%s", ble_name->valuestring);

        // All fields valid -> persist to NVS.
        app_settings_save(&settings);
        if (app_wifi_persist(ssid->valuestring, pass->valuestring) != 0) {
            err = "WiFi持久化失败";
            break;
        }
        if (app_bridge_persist(&bridge) != 0) {
            err = "bridge持久化失败";
            break;
        }
        rc = 0;
        err = "配置已加载";
    } while (0);

invalid:
    cJSON_Delete(root);
    set_status(rc == 0, err);
    return rc;
}

// ---------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------

static int read_file(const char *path, char *buf, size_t buf_len)
{
    int rc = -1;
    app_fs_lock();
    FILE *fp = fopen(path, "rb");
    if (fp) {
        size_t n = fread(buf, 1, buf_len - 1, fp);
        if (!ferror(fp)) {
            buf[n] = '\0';
            rc = (int)n;
        }
        fclose(fp);
    }
    app_fs_unlock();
    return rc;
}

static void backup_existing(void)
{
    struct stat st;
    if (stat(CFG_PATH, &st) != 0) {
        return;
    }
    // Stream-copy config.json -> config.json.bak
    char buf[1024];
    app_fs_lock();
    FILE *src = fopen(CFG_PATH, "rb");
    FILE *dst = src ? fopen(CFG_BAK_PATH, "wb") : NULL;
    if (src && dst) {
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
            if (fwrite(buf, 1, n, dst) != n) {
                break;
            }
        }
    }
    if (src) fclose(src);
    if (dst) fclose(dst);
    app_fs_unlock();
}

int app_config_export(void)
{
    if (!app_sd_is_mounted()) {
        set_status(false, "SD未挂载");
        return -1;
    }

    // Canonical snapshot: export and change-detection share the same format.
    char *snap = cfg_malloc(2048);
    if (!snap) {
        set_status(false, "内存不足");
        return -1;
    }
    app_config_build_snapshot(snap, 2048);
    const char *json = snap;

    int result = -1;
    app_fs_lock();
    FILE *fp = fopen(CFG_TMP_PATH, "wb");
    bool ok = false;
    if (fp) {
        size_t n = strlen(json);
        ok = fwrite(json, 1, n, fp) == n;
        fflush(fp);
        fclose(fp);
    }
    app_fs_unlock();
    if (ok) {
        backup_existing();
        app_fs_lock();
        // FatFs rename fails when the destination already exists (unlike POSIX);
        // remove the old file first, then move the temp file into place.
        remove(CFG_PATH);
        int ren = rename(CFG_TMP_PATH, CFG_PATH);
        app_fs_unlock();
        if (ren == 0) {
            set_status(true, "配置已导出");
            ESP_LOGI(TAG, "exported (%u bytes)", (unsigned)strlen(json));
            result = 0;
        } else {
            set_status(false, "替换配置失败");
        }
    } else {
        set_status(false, "临时文件写入失败");
    }

    cfg_free((void *)snap);
    return result;
}

int app_config_load(void)
{
    if (!app_sd_is_mounted()) {
        set_status(false, "SD未挂载");
        return -1;
    }
    char *buf = cfg_malloc(APP_CONFIG_FILE_BYTES);
    if (!buf) {
        set_status(false, "内存不足");
        return -1;
    }
    int n = read_file(CFG_PATH, buf, APP_CONFIG_FILE_BYTES);
    int rc;
    if (n < 0) {
        set_status(false, "config.json不存在");
        rc = 1;
    } else {
        rc = app_config_import_text(buf, (size_t)n);
    }
    cfg_free(buf);
    return rc;
}

int app_config_restore_backup(void)
{
    if (!app_sd_is_mounted()) {
        set_status(false, "SD未挂载");
        return -1;
    }
    char *buf = cfg_malloc(APP_CONFIG_FILE_BYTES);
    if (!buf) {
        set_status(false, "内存不足");
        return -1;
    }
    int n = read_file(CFG_BAK_PATH, buf, APP_CONFIG_FILE_BYTES);
    int rc;
    if (n < 0) {
        set_status(false, "备份不存在");
        rc = -1;
    } else {
        rc = app_config_import_text(buf, (size_t)n);
    }
    cfg_free(buf);
    return rc;
}

void app_config_get_status(app_config_status_t *out)
{
    if (!out) {
        return;
    }
    struct stat st;
    s_status.present = stat(CFG_PATH, &st) == 0;
    s_status.backup_present = stat(CFG_BAK_PATH, &st) == 0;
    *out = s_status;
}
