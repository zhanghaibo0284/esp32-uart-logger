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

/* 把 cJSON 的分配器切到 PSRAM（只执行一次，幂等）。
 * 背景：配置文件含大量小节点/字符串，若走默认 malloc 会吃掉内部堆，
 *       导致 WiFi/BLE/httpd 启动时出现 NO_MEM。必须在任何 cJSON 调用前执行。
 */
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

/* 记录最近一次配置操作的结果，并刷新“配置文件/备份是否存在”状态。
 * 参数：ok 是否成功；msg 中文提示（会截断到状态结构体长度）。
 */
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

/* 生成 config.json 的“标准快照”（紧凑 JSON，字段顺序固定）。
 * 参数 out/out_len：输出缓冲（建议 2048 字节，超长会截断）。
 * 用途：① 导出写盘；② 变更检测（与卡上文件逐字节比较）。
 * 字段：magic/version/segment_min/screen_auto_off、ports[3]、
 *       wifi{ssid,pass}、bridge{tcp_on[],tcp_port[],ble_on,ble_port,ble_name}。
 * 注意：数值均以数字/布尔/字符串的规范形式输出，保证同一配置生成
 *       完全一致的文本，避免无意义的重复导出。
 */
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
    cJSON_AddBoolToObject(root, "screen_auto_off", settings.screen_auto_off);

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

/* 校验字符串是否全部为可见 ASCII 且长度在 [min_len, max_len] 内。
 * 返回 true=合法。用于 SSID/密码/BLE 名称，过滤换行、控制字符与中文，
 * 避免写进 WiFi/BLE 协议栈后出现不可预期的截断或乱码。
 */
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

/* 把 JSON 文本导入为运行配置并持久化到 NVS。
 * 参数：text/len 完整的配置文本（长度上限 APP_CONFIG_FILE_BYTES）。
 * 返回 0=成功；-1=失败（SD 未挂载、文本为空/过大、JSON 语法错误、
 *        magic/版本不符、任一字段越界、WiFi/透传持久化失败）。
 * 关键步骤：
 *   1. 解析 JSON（失败即返回，不改动现有配置）；
 *   2. 逐字段校验：分段 1..1440 分钟；每路 baud 300..2000000、
 *      数据位 5..8、停止位 1/1.5/2、校验 N/E/O、enabled 布尔；
 *      SSID 1..32、密码 8..63 可见 ASCII；TCP 端口 >=1024、BLE 绑定通道合法；
 *   3. 全部通过后才依次写 settings/wifi/bridge，保证不会半套生效；
 *   4. 结果（含中文错误原因）写入状态结构供 Web 显示。
 * 注意：导入只更新 NVS，串口参数需调用方再触发 logger reload 才会生效。
 */
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
        cJSON *autooff = cJSON_GetObjectItem(root, "screen_auto_off");
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
        // Missing flag accepted and treated as enabled; present must be bool.
        settings.screen_auto_off = autooff ? cJSON_IsTrue(autooff) : true;
        if (autooff && !cJSON_IsBool(autooff)) {
            err = "screen_auto_off 必须为布尔";
            break;
        }

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

/* 读整个文件到 buf（最多 buf_len-1 字节，末尾补 '\0'）。
 * 返回实际读到的字节数；-1 表示打开失败或读出错。
 * 全程持 app_fs_lock，与日志写入/Web 下载互斥。
 */
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

/* 检测卡上 config.json 是否需要重新导出。
 * 参数 needs：输出“是否需要导出”。
 * 返回 0=检测完成（needs 有效）；-1=内存不足。
 * 判据：文件不存在，或与标准快照文本不完全一致 → needs=true。
 * 说明：该函数会读文件（阻塞数毫秒），仅可由 logger 在“帧间隙”调用。
 */
int app_config_needs_export(bool *needs)
{
    if (!needs) {
        return -1;
    }
    *needs = false;
    if (!app_sd_is_mounted()) {
        return 0;
    }
    char *snap = cfg_malloc(2048);
    char *file = cfg_malloc(APP_CONFIG_FILE_BYTES);
    if (!snap || !file) {
        cfg_free(snap);
        cfg_free(file);
        return -1;
    }
    app_config_build_snapshot(snap, 2048);
    int n = read_file(CFG_PATH, file, APP_CONFIG_FILE_BYTES);
    // Missing file or any mismatch with the canonical snapshot -> export.
    *needs = n < 0 || strcmp(snap, file) != 0;
    cfg_free(snap);
    cfg_free(file);
    return 0;
}

/* 每次覆盖 config.json 前调用：把现有文件流式复制为 config.json.bak。
 * 采用 1KB 分块拷贝而非整读整写，避免占用大块内存。
 * 文件不存在或任一打开失败时静默返回（不阻断导出流程）。
 */
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

/* 把当前运行配置导出到 TF 卡根目录的 config.json。
 * 返回 0=成功；-1=失败（SD 未挂载/内存不足/写临时文件失败/替换失败）。
 * 关键步骤（原子替换）：
 *   1. 生成标准快照；
 *   2. 写入隐藏临时文件 .cfg.tmp 并 fflush，避免写一半断电损坏正式文件；
 *   3. 备份旧文件为 config.json.bak；
 *   4. FatFs 的 rename 不允许目标已存在，因此先 remove 旧文件再 rename。
 * 异常处理：任一步失败都保留原 config.json（或已备份的 .bak），
 *           并通过 set_status 把原因反馈到 Web/屏幕。
 */
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

/* 从卡上 config.json 重新加载配置（Web「从卡重载」/启动时可选调用）。
 * 返回 0=成功；1=文件不存在（保持当前配置）；-1=其它失败（含解析/字段非法）。
 */
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

/* 从 config.json.bak 恢复配置并写入 NVS（Web「恢复备份」）。
 * 返回 0=成功；-1=备份不存在或导入失败。
 */
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

/* 输出配置状态快照：最近一次操作成功与否、提示语、文件与备份是否存在。
 * 参数 out 为空时直接返回；读取时会重新 stat 两个文件，保证显示实时。
 * 内部直接修改静态 s_status 再整体拷贝，避免与并发 stat 的结果交叉。
 */
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
