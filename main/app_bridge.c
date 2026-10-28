#include "app_bridge.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "driver/uart.h"
#include "app_logger.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"

static const char *TAG = "bridge";

#define NVS_NS "bridge"
#define BLOB_MAGIC 0x42524432u

// Per-port uplink (UART -> TCP client / BLE central) staging ring. The logger
// only appends bytes here; socket sends and BLE notifications run in the
// bridge task, so a slow client can never block UART reading. Bytes form a
// compact prefix; each consumer keeps its own cursor and skipped data is
// reclaimed once both consumers pass it.
#define UP_CAP 2048
typedef struct {
    uint8_t *buf;               // PSRAM
    size_t len;                 // valid bytes
    size_t tcp_pos;             // bytes handed to the TCP client
    size_t ble_pos;             // bytes notified over BLE
} up_ring_t;

typedef struct {
    uint32_t magic;
    uint8_t tcp_on[APP_PORT_COUNT];
    uint16_t tcp_port[APP_PORT_COUNT];
    uint8_t ble_on;
    uint8_t ble_port;
    char ble_name[BRIDGE_NAME_LEN + 1];
} bridge_blob_t;

static SemaphoreHandle_t s_lock;
static bridge_info_t s_info;
static int s_listen_fd[APP_PORT_COUNT];
static up_ring_t s_up[APP_PORT_COUNT];

static uint16_t s_tx_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static bool s_stack_started;
static bool s_synced;
static bool s_adv_wanted;

/* 从 NVS（namespace "bridge"）读取透传配置，并填充默认值。
 * 关键点：blob 必须与 BLOB_MAGIC 完全匹配才采用，否则按默认值运行，
 * 避免结构体布局变化后误读旧数据；端口默认 U0=8081/J2=8082/J3=8083，
 * 仅接受 >=1024 的端口，BLE 默认绑定 J2。
 */
static void load_nvs(void)
{
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        s_info.tcp_port[i] = (uint16_t)(8081 + i);
        s_info.tcp_clients[i] = -1;
    }
    snprintf(s_info.ble_name, sizeof(s_info.ble_name), "UART-LOG-BLE");
    s_info.ble_port = 1;

    nvs_handle_t h;
    bridge_blob_t blob;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = 0;
        if (nvs_get_blob(h, "cfg", NULL, &len) == ESP_OK && len == sizeof(blob)) {
            nvs_get_blob(h, "cfg", &blob, &len);
            if (blob.magic == BLOB_MAGIC) {
                for (int i = 0; i < APP_PORT_COUNT; i++) {
                    s_info.tcp_on[i] = blob.tcp_on[i] != 0;
                    if (blob.tcp_port[i] >= 1024) {
                        s_info.tcp_port[i] = blob.tcp_port[i];
                    }
                }
                s_info.ble_on = blob.ble_on != 0;
                s_info.ble_port = blob.ble_port < APP_PORT_COUNT ? blob.ble_port : 1;
                snprintf(s_info.ble_name, sizeof(s_info.ble_name), "%s", blob.ble_name);
            }
        }
        nvs_close(h);
    }
    s_adv_wanted = s_info.ble_on;
}

/* 把运行状态打包成 NVS blob（补 magic 与固定字段），供持久化。 */
static void fill_blob(bridge_blob_t *blob, const bridge_info_t *info)
{
    memset(blob, 0, sizeof(*blob));
    blob->magic = BLOB_MAGIC;
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        blob->tcp_on[i] = info->tcp_on[i] ? 1 : 0;
        blob->tcp_port[i] = info->tcp_port[i];
    }
    blob->ble_on = info->ble_on ? 1 : 0;
    blob->ble_port = (uint8_t)info->ble_port;
    snprintf(blob->ble_name, sizeof(blob->ble_name), "%s", info->ble_name);
}

static int write_blob(const bridge_blob_t *blob)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    nvs_set_blob(h, "cfg", blob, sizeof(*blob));
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

static void save_nvs(void)
{
    bridge_blob_t blob;
    fill_blob(&blob, &s_info);
    write_blob(&blob);
}

static bool name_ok_persist(const char *name)
{
    if (!name) {
        return false;
    }
    size_t n = strlen(name);
    if (n < 1 || n > BRIDGE_NAME_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 0x20 || ch > 0x7e) {
            return false;
        }
    }
    return true;
}

/* 校验并持久化透传配置（Web /api/bridge_tcp、/api/bridge_ble 调用）。
 * 返回 0=成功；-1=参数非法（通道越界、端口 <1024、设备名含不可见字符）。
 * 注意：本函数只写 NVS，不改变运行状态，运行态由 set 接口负责。
 */
int app_bridge_persist(const bridge_info_t *in)
{
    if (!in || in->ble_port < 0 || in->ble_port >= APP_PORT_COUNT || !name_ok_persist(in->ble_name)) {
        return -1;
    }
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        if (in->tcp_port[i] < 1024) {
            return -1;
        }
    }
    bridge_blob_t blob;
    fill_blob(&blob, in);
    return write_blob(&blob);
}

/* 为某通道创建 TCP 监听 socket（bind+listen=1）。
 * 返回 0 成功；-1 失败（用于上层回滚 tcp_on 状态）。
 * 复用 SO_REUSEADDR，便于关闭后立即重新监听同一端口。
 */
static int tcp_listen_start(int idx)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(s_info.tcp_port[idx]),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    s_listen_fd[idx] = fd;
    return 0;
}

/* 断开并关闭某通道当前的 TCP 客户端（不关闭监听 socket）。 */
static void tcp_drop_client(int idx)
{
    if (s_info.tcp_clients[idx] >= 0) {
        close(s_info.tcp_clients[idx]);
        s_info.tcp_clients[idx] = -1;
    }
}

/* 停止某通道的 TCP 透传：先断开客户端，再关闭监听 socket。 */
static void tcp_listen_stop(int idx)
{
    tcp_drop_client(idx);
    if (s_listen_fd[idx] >= 0) {
        close(s_listen_fd[idx]);
        s_listen_fd[idx] = -1;
    }
}

// Downlink frame parked while the logger tx queue is full; retried across a
// bounded number of cycles before being counted as dropped.
typedef struct {
    bool used;
    int port;
    uint8_t data[256];
    int len;
    int tries;
} down_pending_t;

/* 把上行暂存环里的数据分发给 TCP 客户端与 BLE 中心设备（调用方须持 s_lock）。
 * 设计要点：
 *   - 每个消费端有独立游标；未开启/未连接的消费端直接把游标推到末尾，
 *     相当于跳过它的积压，不拖累另一个消费端；
 *   - TCP 用 MSG_DONTWAIT 非阻塞发送，EAGAIN 时保留字节下轮重试；
 *     其它错误视为客户端失效并断开；
 *   - BLE 按当前 MTU-3 分包 notify，分配 mbuf 失败或 host 未接管则下轮重试；
 *   - 两端都已消费的前缀用 memmove 回收，环缓冲始终为紧凑前缀。
 */
static void drain_uplink_locked(void)
{
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        up_ring_t *u = &s_up[i];
        bool tcp_live = s_info.tcp_on[i] && s_info.tcp_clients[i] >= 0;
        bool ble_live = s_info.ble_on && s_info.ble_connected &&
                        s_info.ble_port == i && s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
        if (!tcp_live) {
            u->tcp_pos = u->len;
        }
        if (!ble_live) {
            u->ble_pos = u->len;
        }

        if (tcp_live && u->tcp_pos < u->len) {
            int sent = send(s_info.tcp_clients[i], u->buf + u->tcp_pos,
                            u->len - u->tcp_pos, MSG_DONTWAIT);
            if (sent > 0) {
                s_info.tcp_tx[i] += sent;
                u->tcp_pos += sent;
            } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                tcp_drop_client(i);
                u->tcp_pos = u->len;
            }
            // EAGAIN: keep bytes; retried next cycle (no silent loss).
        }

        if (ble_live && u->ble_pos < u->len) {
            uint16_t mtu = ble_att_mtu(s_conn_handle);
            size_t max = mtu >= 23 ? (size_t)mtu - 3 : 20;
            size_t remaining = u->len - u->ble_pos;
            size_t chunk = remaining > max ? max : remaining;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(u->buf + u->ble_pos, chunk);
            if (om) {
                int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
                if (rc == 0) {
                    s_info.ble_tx += chunk;
                    u->ble_pos += chunk;
                } else {
                    // Host did not take ownership; free and retry next cycle.
                    os_mbuf_free_chain(om);
                }
            }
        }

        size_t done = u->tcp_pos < u->ble_pos ? u->tcp_pos : u->ble_pos;
        if (done > 0) {
            memmove(u->buf, u->buf + done, u->len - done);
            u->len -= done;
            u->tcp_pos -= done;
            u->ble_pos -= done;
        }
    }
}

/* 向下行方向提交数据（TCP 收包或 BLE 写入）到 logger 出站队列。
 * 参数 pend：上一帧尚未入队的暂存槽，队列满时可跨周期重试；
 * 返回：无。logger 队列满且已有暂存帧时，说明上一帧重试失败，
 *       累计 down_drop 后覆盖暂存槽（最多再重试 5 轮）。
 * 通过 app_logger_request_tx 发送，保证 TX 统一入日志且不持本模块锁。
 */
static void downlink_submit(down_pending_t *pend, int port, const uint8_t *data, int len)
{
    if (app_logger_request_tx(port, data, len)) {
        pend->used = false;
        return;
    }
    if (pend->used) {
        ESP_LOGW(TAG, "downlink queue full, %d bytes dropped", pend->len);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.down_drop[pend->port] += pend->len;
        xSemaphoreGive(s_lock);
    }
    pend->used = true;
    pend->port = port;
    pend->len = len;
    pend->tries = 0;
    memcpy(pend->data, data, (size_t)len);
}

/* 透传主任务：每 5ms 一轮，负责 TCP accept/recv、上行分发与下行重试。
 * 先重试暂存的下行帧（最多 5 轮≈25ms），再处理监听/客户端，
 * 收包后统一交给 logger 发送；持锁时间仅覆盖 socket/BLE 暂存操作，
 * 实际的 app_logger_request_tx 在锁外调用，避免与 logger 互相等待。
 */
static void bridge_task(void *arg)
{
    uint8_t buf[256];
    down_pending_t pend = {0};
    while (1) {
        int txport = -1;
        int txlen = 0;

        // Retry previously parked downlink data first (max 5 cycles ~25ms).
        if (pend.used) {
            pend.tries++;
            if (app_logger_request_tx(pend.port, pend.data, pend.len)) {
                pend.used = false;
            } else if (pend.tries >= 5) {
                ESP_LOGW(TAG, "downlink retry exhausted, %d bytes dropped", pend.len);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_info.down_drop[pend.port] += pend.len;
                xSemaphoreGive(s_lock);
                pend.used = false;
            }
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < APP_PORT_COUNT; i++) {
            if (s_listen_fd[i] < 0) {
                continue;
            }
            if (s_info.tcp_clients[i] < 0) {
                struct sockaddr_in caddr;
                socklen_t clen = sizeof(caddr);
                int cfd = accept(s_listen_fd[i], (struct sockaddr *)&caddr, &clen);
                if (cfd >= 0) {
                    s_info.tcp_clients[i] = cfd;
                    ESP_LOGI(TAG, "tcp uart%d client connected", i);
                }
            }
            int cfd = s_info.tcp_clients[i];
            if (cfd >= 0 && !pend.used) {
                int n = recv(cfd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n > 0) {
                    // Hand off to the logger (TX record + actual write).
                    txport = i;
                    txlen = n;
                    s_info.tcp_rx[i] += n;
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    tcp_drop_client(i);
                }
            }
        }
        drain_uplink_locked();
        xSemaphoreGive(s_lock);

        if (txlen > 0) {
            downlink_submit(&pend, txport, buf, txlen);
        }
        // 5ms cadence keeps uplink latency low (~200 wakes/s is cheap).
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* logger 收到串口数据时调用：只把字节追加到该通道的上行暂存环，
 * 真正的 socket send / BLE notify 由 bridge_task 完成，
 * 因此客户端变慢或断开都不会阻塞串口读取。暂存环满时丢弃超出部分
 * 并累计 up_drop（不抛错、不等待）。
 */
void app_bridge_feed_uart(int idx, const uint8_t *data, size_t len)
{
    if (idx < 0 || idx >= APP_PORT_COUNT || !data || len == 0 || !s_lock) {
        return;
    }
    // Only enqueue into the staging ring: socket/BLE work runs in bridge_task,
    // so the logger RX path never blocks on a slow client.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    up_ring_t *u = &s_up[idx];
    size_t space = UP_CAP - u->len;
    size_t take = len < space ? len : space;
    if (take > 0) {
        memcpy(u->buf + u->len, data, take);
        u->len += take;
    }
    if (take < len) {
        s_info.up_drop[idx] += len - take;
    }
    xSemaphoreGive(s_lock);
}

static const ble_uuid128_t NUS_SERVICE_UUID =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_RX_UUID =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_TX_UUID =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

/* NUS RX 特征写入回调：把手机/电脑写入的数据转发到已绑定的串口。
 * 参数：conn_handle 连接句柄；attr_handle 特征句柄；ctxt 访问上下文；
 *       arg 未使用。返回 0（NimBLE 协议栈要求）。
 * 处理：按 256 字节截断读取 mbuf → 统计 ble_rx → 交给 logger 出站队列；
 *       队列满时丢弃并累计 down_drop。
 */
static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t raw_len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t tmp[256];
        uint16_t len = raw_len > sizeof(tmp) ? sizeof(tmp) : raw_len;
        uint16_t copied = len;
        ble_hs_mbuf_to_flat(ctxt->om, tmp, len, &copied);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int port = s_info.ble_port;
        s_info.ble_rx += copied;
        xSemaphoreGive(s_lock);
        if (!app_logger_request_tx(port, tmp, copied)) {
            ESP_LOGW(TAG, "tx queue full, ble write dropped");
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_info.down_drop[port] += copied;
            xSemaphoreGive(s_lock);
        }
    }
    return 0;
}

static const struct ble_gatt_chr_def gatt_chrs[] = {
    {
        .uuid = &NUS_RX_UUID.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &NUS_TX_UUID.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_tx_handle,
    },
    {0},
};

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SERVICE_UUID.u,
        .characteristics = gatt_chrs,
    },
    {0},
};

static int gap_event_cb(struct ble_gap_event *event, void *arg);

/* 开始 BLE 广播：广播包只放 flags + 完整设备名。
 * 重要约束：BLE 广播数据上限 31 字节，把 128 位 Service UUID 也放进广播
 * 会超长并返回错误码 4；UUID 保留在 GATT 服务内，连接后仍可枚举。
 * 已在广播时重复调用返回 BLE_HS_EALREADY，按正常处理。
 */
static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    }
}

/* GAP 事件回调：维护连接状态并自动恢复广播。
 * 连接成功记录 conn_handle 并置 ble_connected；断开/广播完成后，
 * 若用户仍希望开启 BLE（s_adv_wanted）则重新开始广播，实现“断开自动可发现”。
 */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_info.ble_connected = true;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "ble connected");
        } else {
            if (s_adv_wanted) {
                start_advertising();
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.ble_connected = false;
        xSemaphoreGive(s_lock);
        if (s_adv_wanted) {
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_adv_wanted) {
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu %d", event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

/* NimBLE 主机同步完成回调：确定本机地址类型；若用户已开 BLE 则开始广播。
 * 地址推断失败只记录错误，不影响其它功能（再次同步时会重试）。
 */
static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "id infer failed: %d", rc);
        return;
    }
    s_synced = true;
    if (s_adv_wanted) {
        start_advertising();
    }
}

static void bridge_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "ble reset reason=%d", reason);
}

/* 初始化 BLE 协议栈（只执行一次，幂等）。
 * 顺序：nimble_port_init（控制器+主机）→ 注册 GAP/GATT 服务 →
 *       设置设备名 → 在 FreeRTOS 任务中跑主机循环。
 * 失败处理：各步骤失败仅记录日志并返回，s_stack_started 保持 false；
 *           重复调用会再次尝试，但不会在 httpd 线程中调用（会阻塞服务器）。
 * 内存：必须在 WiFi 之前初始化，否则控制器拿不到连续内部 DMA 内存。
 */
static void ble_stack_init_once(void)
{
    if (s_stack_started) {
        return;
    }
    esp_err_t eret = nimble_port_init();
    if (eret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s (%d)", esp_err_to_name(eret), eret);
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count failed: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add failed: %d", rc);
        return;
    }
    rc = ble_svc_gap_device_name_set(s_info.ble_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "name_set failed: %d", rc);
        return;
    }
    nimble_port_freertos_init(bridge_host_task);
    s_stack_started = true;
    ESP_LOGI(TAG, "ble stack ready, internal free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

/* 开关某通道的 WiFi TCP 透传并持久化。
 * 参数：idx 通道；on 开关；port_num 期望端口（>=1024 时才更新）。
 * 返回 0 成功；-1 通道非法；-2 监听端口创建失败（可用端口被占用）。
 * 开启时若已在监听则只更新端口配置（下次开启生效）。
 */
int app_bridge_tcp_set(int idx, bool on, uint16_t port_num)
{
    if (idx < 0 || idx >= APP_PORT_COUNT) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (on) {
        if (port_num >= 1024) {
            s_info.tcp_port[idx] = port_num;
        }
        if (!s_info.tcp_on[idx]) {
            if (tcp_listen_start(idx) != 0) {
                xSemaphoreGive(s_lock);
                return -2;
            }
            s_info.tcp_on[idx] = true;
        }
    } else {
        tcp_listen_stop(idx);
        s_info.tcp_on[idx] = false;
    }
    xSemaphoreGive(s_lock);
    save_nvs();
    return 0;
}

/* 开关 BLE 透传 / 修改绑定串口与设备名（运行态 + NVS）。
 * 参数：on 是否开启；port_idx 绑定的串口通道；name 非空时更新设备名。
 * 返回 0 成功；-1 参数非法。
 * 注意：只切换广播与连接，不在本函数里重新初始化协议栈
 * （协议栈在开机时已建好，重初始化会阻塞 httpd 单线程服务器）。
 */
int app_bridge_ble_set(bool on, int port_idx, const char *name)
{
    if (port_idx < 0 || port_idx >= APP_PORT_COUNT) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.ble_port = port_idx;
    if (name && name[0]) {
        snprintf(s_info.ble_name, sizeof(s_info.ble_name), "%.20s", name);
    }
    s_info.ble_on = on;
    s_adv_wanted = on;
    xSemaphoreGive(s_lock);

    if (on) {
        ble_svc_gap_device_name_set(s_info.ble_name);
        if (s_synced) {
            start_advertising();
        }
    } else {
        ble_gap_adv_stop();
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    save_nvs();
    return 0;
}

/* 复制一份透传状态快照（Web /api/bridge 使用）。
 * 参数 out：外部结构体；在 s_lock 保护下整体拷贝。
 * 特殊情形：若在 ble_init 之前调用（s_lock 为空），返回与 NVS 默认一致的
 *           默认值，避免调用方读到未初始化数据。
 */
void app_bridge_get(bridge_info_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock) {
        // Called before app_bridge_ble_init: return defaults (same as load_nvs).
        memset(out, 0, sizeof(*out));
        for (int i = 0; i < APP_PORT_COUNT; i++) {
            out->tcp_port[i] = (uint16_t)(8081 + i);
            out->tcp_clients[i] = -1;
        }
        out->ble_port = 1;
        snprintf(out->ble_name, sizeof(out->ble_name), "UART-LOG-BLE");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);
}

/* 初始化透传模块：创建锁、上行暂存环（PSRAM）、读取 NVS，然后初始化 BLE。
 * 必须由 app_main 在 WiFi 之前调用；TCP 监听不在此时创建（等网络栈就绪
 * 后由 app_bridge_tcp_resume 恢复），否则会因 lwIP tcpip 线程未创建而
 * 触发 "Invalid mbox" 断言重启。
 */
void app_bridge_ble_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        s_listen_fd[i] = -1;
        s_up[i].buf = heap_caps_malloc(UP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_up[i].len = 0;
        s_up[i].tcp_pos = s_up[i].ble_pos = 0;
    }
    load_nvs();

    // Initialize the BT controller + NimBLE host BEFORE WiFi. The BLE
    // controller needs a sizable chunk of contiguous internal DMA memory;
    // once WiFi/lwIP is up that memory is no longer available and controller
    // init fails with ESP_ERR_NO_MEM. NimBLE itself does not need lwIP.
    // The web UI only toggles advertising later.
    ble_stack_init_once();
}

/* 网络栈就绪后恢复 TCP 监听并启动透传任务（在 WiFi/httpd 之后调用）。
 * 任一通道监听失败时把该通道 tcp_on 置 false，避免状态与实际不一致。
 */
void app_bridge_tcp_resume(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        if (s_info.tcp_on[i]) {
            if (tcp_listen_start(i) != 0) {
                s_info.tcp_on[i] = false;
            }
        }
    }
    xSemaphoreGive(s_lock);
    xTaskCreate(bridge_task, "bridge", 4096, NULL, 6, NULL);
}
