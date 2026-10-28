#include "app_settings.h"

#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "settings";
static const char *NVS_NS = "logger";
static const char *NVS_KEY = "cfg";

#define CFG_MAGIC 0x4C4F4731u
#define CFG_VERSION 3
#define V2_BLOB_SIZE 32   // size of the version-2 blob (without screen_auto_off)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t segment_min;
    uint32_t baud[APP_PORT_COUNT];
    uint8_t data_bits[APP_PORT_COUNT];
    uint8_t stop_bits[APP_PORT_COUNT];
    uint8_t parity[APP_PORT_COUNT];
    uint8_t enabled[APP_PORT_COUNT];
    uint8_t screen_auto_off;
} cfg_blob_t;

static app_settings_t s_cfg;
static SemaphoreHandle_t s_lock;

/* 填充出厂默认配置：分段 60 分钟，屏幕自动熄屏开启；
 * 三路均 115200-8N1，默认只启用 J2（UART1），其余关闭。
 */
static void set_defaults(app_settings_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->segment_min = 60;
    cfg->screen_auto_off = true;
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        cfg->port[i].baud = 115200;
        cfg->port[i].data_bits = UART_DATA_8_BITS;
        cfg->port[i].stop_bits = UART_STOP_BITS_1;
        cfg->port[i].parity = UART_PARITY_DISABLE;
        cfg->port[i].enabled = false;
    }
    cfg->port[1].enabled = true;
}

/* 校验波特率范围（300..2000000）：超出范围视为非法，回退默认值。 */
static bool baud_ok(uint32_t baud)
{
    return baud >= 300 && baud <= 2000000;
}

/* 校验 NVS blob 是否可信：magic 必须匹配，版本支持 v2（旧布局）与当前版本，
 * 分段 1..1440 分钟，逐路检查波特率/数据位/停止位/校验枚举范围。
 * 任一项越界返回 false，调用方会回退默认配置，避免把非法值送进 UART 驱动。
 */
static bool blob_ok(const cfg_blob_t *blob)
{
    if (blob->magic != CFG_MAGIC ||
        (blob->version != 2 && blob->version != CFG_VERSION)) {
        return false;
    }
    if (blob->segment_min < 1 || blob->segment_min > 24 * 60) {
        return false;
    }
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        if (!baud_ok(blob->baud[i])) {
            return false;
        }
        if (blob->data_bits[i] > UART_DATA_8_BITS) {
            return false;
        }
        if (blob->stop_bits[i] < UART_STOP_BITS_1 || blob->stop_bits[i] > UART_STOP_BITS_2) {
            return false;
        }
        if (blob->parity[i] != UART_PARITY_DISABLE &&
            blob->parity[i] != UART_PARITY_EVEN &&
            blob->parity[i] != UART_PARITY_ODD) {
            return false;
        }
    }
    return true;
}

/* blob → 运行结构体（含 v2 兼容：v2 没有 screen_auto_off 字段，默认开启熄屏）。 */
static void from_blob(const cfg_blob_t *blob, app_settings_t *cfg)
{
    cfg->segment_min = blob->segment_min;
    // v2 blobs have no stored flag (read buffer zeroed); default to enabled.
    cfg->screen_auto_off = blob->version == 2 ? true : blob->screen_auto_off != 0;
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        cfg->port[i].baud = blob->baud[i];
        cfg->port[i].data_bits = (uart_word_length_t)blob->data_bits[i];
        cfg->port[i].stop_bits = (uart_stop_bits_t)blob->stop_bits[i];
        cfg->port[i].parity = (uart_parity_t)blob->parity[i];
        cfg->port[i].enabled = blob->enabled[i] != 0;
    }
}

/* 运行结构体 → blob（写入 magic 与当前版本，清零未使用字节保证布局稳定）。 */
static void to_blob(const app_settings_t *cfg, cfg_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->magic = CFG_MAGIC;
    blob->version = CFG_VERSION;
    blob->segment_min = cfg->segment_min;
    blob->screen_auto_off = cfg->screen_auto_off ? 1 : 0;
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        blob->baud[i] = cfg->port[i].baud;
        blob->data_bits[i] = (uint8_t)cfg->port[i].data_bits;
        blob->stop_bits[i] = (uint8_t)cfg->port[i].stop_bits;
        blob->parity[i] = (uint8_t)cfg->port[i].parity;
        blob->enabled[i] = cfg->port[i].enabled ? 1 : 0;
    }
}

/* 把端口参数格式化为简短文本（如 "115200 8N1"），供界面显示。
 * 参数：port 端口参数；out/out_len 输出缓冲（建议 >=24 字节）。
 */
void app_settings_format_port(const port_setting_t *port, char *out, size_t out_len)
{
    int bits = 8;
    if (port->data_bits == UART_DATA_7_BITS) {
        bits = 7;
    } else if (port->data_bits == UART_DATA_6_BITS) {
        bits = 6;
    } else if (port->data_bits == UART_DATA_5_BITS) {
        bits = 5;
    }
    const char *parity = "N";
    if (port->parity == UART_PARITY_EVEN) {
        parity = "E";
    } else if (port->parity == UART_PARITY_ODD) {
        parity = "O";
    }
    const char *stop = "1";
    if (port->stop_bits == UART_STOP_BITS_1_5) {
        stop = "1.5";
    } else if (port->stop_bits == UART_STOP_BITS_2) {
        stop = "2";
    }
    snprintf(out, out_len, "%lu %d%s%s", (unsigned long)port->baud, bits, parity, stop);
}

/* 初始化配置模块：创建锁、装入默认值，再尝试从 NVS（namespace "logger",
 * key "cfg"）读取并校验；任何一步不合法都保留默认值继续启动，不阻塞开机。
 */
void app_settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    set_defaults(&s_cfg);

    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "use default settings");
        return;
    }
    cfg_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    size_t size = sizeof(blob);
    esp_err_t err = nvs_get_blob(handle, NVS_KEY, &blob, &size);
    nvs_close(handle);
    bool size_ok = size == sizeof(blob) || size == V2_BLOB_SIZE;
    if (err == ESP_OK && size_ok && blob_ok(&blob)) {
        from_blob(&blob, &s_cfg);
        ESP_LOGI(TAG, "settings loaded v%u, segment=%u auto_off=%d",
                 blob.version, s_cfg.segment_min, s_cfg.screen_auto_off ? 1 : 0);
    } else {
        ESP_LOGW(TAG, "settings invalid, use defaults");
    }
}

/* 复制当前配置快照（持锁整体拷贝，保证字段一致）。 */
void app_settings_get(app_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

/* 保存配置：先做合法性钳制（分段 1..1440 分钟、波特率 300..2000000），
 * 再更新内存快照，最后整体写入 NVS。
 * 注意：本函数只改配置，不会立刻按新参数重开串口；调用方通常随后执行
 *       app_logger_request_reload() 让参数生效。
 * 异常处理：NVS 打开失败记录错误并返回（内存值已更新，下次启动回退旧值）；
 *           set/commit 失败由 ESP_ERROR_CHECK 直接复位，保证数据一致。
 */
void app_settings_save(const app_settings_t *in)
{
    app_settings_t cfg = *in;
    if (cfg.segment_min < 1 || cfg.segment_min > 24 * 60) {
        cfg.segment_min = 60;
    }
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        if (!baud_ok(cfg.port[i].baud)) {
            cfg.port[i].baud = 115200;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = cfg;
    xSemaphoreGive(s_lock);

    cfg_blob_t blob;
    to_blob(&cfg, &blob);
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return;
    }
    ESP_ERROR_CHECK(nvs_set_blob(handle, NVS_KEY, &blob, sizeof(blob)));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
}
