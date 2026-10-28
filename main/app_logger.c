#include "app_logger.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app_settings.h"
#include "app_sd.h"
#include "app_time.h"
#include "app_bridge.h"
#include "app_config.h"
#include "app_modbus.h"
#include "led.h"

static const char *TAG = "logger";

#define FRAME_BYTES 520         // RTU max 256; ASCII ':' + 512 chars + CRLF
#define LINE_BYTES 2048         // per-port formatted line: worst case ASCII 512B (~1570) + note (192)
#define TXQ_DEPTH   2
#define LED_THROTTLE_US 100000  // activity LED: at most one toggle per 100ms

typedef struct {
    uint8_t *raw;               // allocated in PSRAM (large)
    int count;
    struct timeval first;
    int64_t last_us;
    bool active;
} frame_builder_t;

typedef struct {
    bool used;
    int port;
    uint8_t data[FRAME_BYTES];
    int len;
} tx_req_t;

// Messages for the disk writer task: RX path formats frames, writer owns files.
#define DISK_Q_DEPTH 32
typedef struct {
    uint8_t kind;        // 0 = frame bytes, 1 = flush barrier
    uint8_t port;        // frame: source port index
    struct timeval tv;   // frame: timestamp (selects segment file)
    uint16_t len;        // frame: valid bytes in line
    char line[LINE_BYTES];
} disk_msg_t;

static SemaphoreHandle_t s_lock;
static app_view_t s_view;
static port_setting_t s_port_cfg[APP_PORT_COUNT];
static uint16_t s_segment_min = 60;
static bool s_reload;
static bool s_remount;
static bool s_sd_test;
static bool s_tx_test[APP_PORT_COUNT];
static bool s_repeat_on[APP_PORT_COUNT];
static uint32_t s_repeat_ms[APP_PORT_COUNT] = {1000, 1000, 1000};
static int64_t s_repeat_next_us[APP_PORT_COUNT];
static uint32_t s_repeat_seq[APP_PORT_COUNT];
static bool s_uart_on[APP_PORT_COUNT];
static QueueHandle_t s_uart_evt[APP_PORT_COUNT];
static FILE *s_fp[APP_PORT_COUNT];
static time_t s_file_start[APP_PORT_COUNT];
static frame_builder_t s_builder[APP_PORT_COUNT];
static char *s_linebuf[APP_PORT_COUNT];   // persistent PSRAM line buffers
static tx_req_t s_txq[TXQ_DEPTH];

// Disk writer: stack in PSRAM (task never services ISRs; config permits it),
// message queue storage also PSRAM.
static StaticQueue_t s_diskq_ctrl;
static uint8_t *s_diskq_store;
static QueueHandle_t s_diskq;
static SemaphoreHandle_t s_flush_done;
static StaticTask_t s_writer_ctrl;
static StackType_t *s_writer_stack;
static int64_t s_last_led_us;
static bool s_led_on;

#define LIVE_CAP 4096
static char s_live[APP_PORT_COUNT][LIVE_CAP];
static uint32_t s_live_end[APP_PORT_COUNT];

static char s_sd_msg[48] = "等待SD卡";

// --- config change detection ------------------------------------------------
static int64_t last_cfg_check_us = -2000000;

/* 把一段已格式化的文本追加到指定通道的实时环形缓冲（供 Web /api/live 轮询）。
 * 参数：
 *   index  通道索引 0..APP_PORT_COUNT-1，越界直接丢弃；
 *   text   NUL 结尾的字符串，'\r' 会被过滤以统一网页显示。
 * 说明：写指针 s_live_end 单调递增，读取端用 "游标 % LIVE_CAP" 定位，
 *       因此无需搬移数据；缓冲满后旧内容被自然覆盖。全程持 s_lock 保护。
 */
static void live_append(int index, const char *text)
{
    if (index < 0 || index >= APP_PORT_COUNT || !text) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; text[i]; i++) {
        char ch = text[i];
        if (ch == '\r') {
            continue;
        }
        s_live[index][s_live_end[index] % LIVE_CAP] = ch;
        s_live_end[index]++;
    }
    xSemaphoreGive(s_lock);
}

static void copy_msg(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

/* 把 SD 卡容量/挂载状态/提示语汇总进共享视图 s_view，供 UI 与 Web 读取。
 * 关键步骤：先查挂载状态与容量（可能访问 FAT 层），再持锁一次性更新，
 * 避免读到的字段来自不同时刻。无返回值，失败时容量为 0。
 */
static void publish_sd(void)
{
    uint32_t total = 0;
    uint32_t free_mb = 0;
    app_sd_usage(&total, &free_mb);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_view.sd_mounted = app_sd_is_mounted();
    s_view.sd_total_mb = total;
    s_view.sd_free_mb = free_mb;
    copy_msg(s_view.sd_msg, sizeof(s_view.sd_msg), s_sd_msg);
    xSemaphoreGive(s_lock);
}

/* 把某通道的串口参数格式化后写入共享视图（Web/UI 显示“115200 8N1”）。
 * 参数 index：通道索引，调用方保证合法。
 */
static void publish_port(int index)
{
    char param[24];
    app_settings_format_port(&s_port_cfg[index], param, sizeof(param));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_view.port[index].open = s_uart_on[index];
    copy_msg(s_view.port[index].param, sizeof(s_view.port[index].param), param);
    xSemaphoreGive(s_lock);
}

/* 关闭某通道的日志文件：先 flush+fsync 保证已写内容落盘，再 fclose。
 * 全程持 app_fs_lock，避免与 Web 下载/删除/屏幕浏览并发访问 FAT。
 * 异常处理：fsync/fclose 失败时仅记录内核层错误，指针仍被清空，
 * 下次写帧会重新 fopen 追加，不会因单次失败卡死通道。
 */
static void close_file(int index)
{
    app_fs_lock();
    if (s_fp[index]) {
        fflush(s_fp[index]);
        fsync(fileno(s_fp[index]));
        fclose(s_fp[index]);
        s_fp[index] = NULL;
    }
    s_file_start[index] = 0;
    app_fs_unlock();
}

/* 关闭并卸载某通道的 UART 驱动与事件队列。
 * 用于参数变更/关闭通道：先 delete 驱动释放 RX 环形缓冲，再删除事件队列，
 * 置 s_uart_on=false，让 logger_task 跳过该通道。可重复调用（幂等）。
 */
static void close_uart(int index)
{
    if (s_uart_on[index]) {
        uart_driver_delete((uart_port_t)index);
        s_uart_on[index] = false;
    }
    if (s_uart_evt[index]) {
        vQueueDelete(s_uart_evt[index]);
        s_uart_evt[index] = NULL;
    }
}

/* 计算一个字符占用的总位时间（起始位+数据位+校验位+停止位）。
 * 参数 ps：端口参数；返回位数（如 8N1 = 10 位）。
 * 用途：换算 RX 超时（3.5 字符）与每个字节的到达时间偏移。
 */
static int frame_bits(const port_setting_t *ps)
{
    int bits = 1;                                     // start bit
    bits += 5 + (int)ps->data_bits;                   // enum 0..3 -> 5..8 bits
    bits += ps->parity != UART_PARITY_DISABLE ? 1 : 0;
    bits += ps->stop_bits == UART_STOP_BITS_2 ? 2 : 1;
    return bits;
}

static int byte_time_us(int index);   // full definition after frame builder

/* 按当前参数打开某通道 UART（参数配置→引脚→驱动→超时）。
 * 返回 ESP_OK 成功；否则返回 esp_err_t 错误码（参数/引脚/内存不足）。
 * 异常处理：驱动安装内存不足时按 2048→1024→256 字节逐级降级 RX 缓冲；
 * 全部失败则删除事件队列并保持通道关闭，由上层写入 error 供界面显示。
 * 关键点：RX 超时设为 3.5 字符位时间，正好等于 Modbus RTU 帧间隔，
 * 使一个 UART_DATA(timeout) 事件对应一帧，避免任务调度抖动导致粘帧。
 */
static int open_uart(int index)
{
    uart_config_t cfg = {
        .baud_rate = (int)s_port_cfg[index].baud,
        .data_bits = s_port_cfg[index].data_bits,
        .parity = s_port_cfg[index].parity,
        .stop_bits = s_port_cfg[index].stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config((uart_port_t)index, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    gpio_reset_pin(app_uart_tx(index));
    gpio_reset_pin(app_uart_rx(index));
    err = uart_set_pin((uart_port_t)index, app_uart_tx(index), app_uart_rx(index),
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    // Event queue lets the RX TOUT ISR define frame boundaries: a UART_DATA
    // event with timeout_flag=true ends one frame, independently of how late
    // the logger task drains it. This prevents multi-frame gluing during disk
    // stalls or higher-priority WiFi/BLE preemption.
#define EVT_Q_DEPTH 24
    s_uart_evt[index] = xQueueCreate(EVT_Q_DEPTH, sizeof(uart_event_t));
    if (!s_uart_evt[index]) {
        return ESP_ERR_NO_MEM;
    }
    // 2048B RX ring: at 115200 it absorbs ~178ms of worst-case blocked polling
    // (fsync/maintenance), giving 2x the old margin. Falls back on low memory.
    err = uart_driver_install((uart_port_t)index, 2048, 0, EVT_Q_DEPTH,
                              &s_uart_evt[index], 0);
    if (err == ESP_ERR_NO_MEM) {
        err = uart_driver_install((uart_port_t)index, 1024, 0, EVT_Q_DEPTH,
                                  &s_uart_evt[index], 0);
    }
    if (err == ESP_ERR_NO_MEM) {
        err = uart_driver_install((uart_port_t)index, 256, 0, EVT_Q_DEPTH,
                                  &s_uart_evt[index], 0);
    }
    if (err != ESP_OK) {
        vQueueDelete(s_uart_evt[index]);
        s_uart_evt[index] = NULL;
        return err;
    }
    // RX timeout at 3.5 character bit-times: equal to the Modbus inter-frame
    // gap, so one timeout event corresponds to exactly one finished frame.
    int tout = frame_bits(&s_port_cfg[index]) * 7 / 2;   // bits * 3.5
    if (tout < 8) {
        tout = 8;
    }
    if (tout > 250) {
        tout = 250;
    }
    uart_set_rx_timeout((uart_port_t)index, (uint8_t)tout);
    uart_set_always_rx_timeout((uart_port_t)index, true);
    gpio_pullup_en(app_uart_rx(index));
    gpio_pulldown_dis(app_uart_rx(index));
    uart_flush_input((uart_port_t)index);
    // Discard events generated while no consumer existed yet.
    xQueueReset(s_uart_evt[index]);
    s_uart_on[index] = true;
    return ESP_OK;
}

/* 读取 NVS 配置并使其在运行时生效（分段时长 + 每路串口参数/开关）。
 * 关键步骤：
 *   1. 逐路比较新旧参数，未变化则只刷新界面参数，不打断接收；
 *   2. 变化时依次关闭 UART/文件、清空帧组装器与 Modbus 解析状态；
 *   3. 需要开启的通道重新 open_uart，失败记录 error 并保留“已关闭”状态。
 * 该函数只在 logger_task 上下文调用，避免跨任务操作硬件。
 */
static void apply_settings(void)
{
    app_settings_t cfg;
    app_settings_get(&cfg);
    s_segment_min = cfg.segment_min < 1 ? 60 : cfg.segment_min;
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        bool changed = s_port_cfg[i].baud != cfg.port[i].baud ||
                       s_port_cfg[i].data_bits != cfg.port[i].data_bits ||
                       s_port_cfg[i].stop_bits != cfg.port[i].stop_bits ||
                       s_port_cfg[i].parity != cfg.port[i].parity ||
                       s_port_cfg[i].enabled != cfg.port[i].enabled ||
                       s_uart_on[i] != cfg.port[i].enabled;
        s_port_cfg[i] = cfg.port[i];
        if (!changed && s_uart_on[i] == cfg.port[i].enabled) {
            publish_port(i);
            continue;
        }
        close_uart(i);
        close_file(i);
        s_builder[i].active = false;
        s_builder[i].count = 0;
        app_modbus_reset(i);
        int err = 0;
        if (cfg.port[i].enabled) {
            err = open_uart(i);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "UART%d open failed: %s", i, esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "UART%d open TX=%d RX=%d %lu", i, app_uart_tx(i), app_uart_rx(i),
                         (unsigned long)cfg.port[i].baud);
            }
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_view.port[i].error = err;
        s_view.port[i].file_name[0] = '\0';
        if (!cfg.port[i].enabled) {
            s_repeat_on[i] = false;
            s_view.port[i].sending = false;
        }
        xSemaphoreGive(s_lock);
        publish_port(i);
    }
}

/* 确保某通道在 now 时刻有对应的日志文件，必要时切换新分段文件。
 * 参数：index 通道；now 当前时间（秒），用于计算所属分段起点。
 * 返回 true=已打开可用文件；false=未挂载或打开失败（error=-2，界面可见）。
 * 关键步骤：
 *   1. 计算分段起点：把一天的秒数按 segment_min 对齐，得到固定分段时间窗；
 *   2. 当前文件已属于该窗口则直接复用，否则关闭旧文件、按新窗口名新建；
 *   3. 新文件为空时写入 3 行注释头（引脚/参数/格式说明），便于事后分析。
 * 命名：/sdcard/UART<i>/YYYYmmdd_HHMMSS.txt，同一通道同窗口自动追加。
 * 全程持 app_fs_lock，与 Web 的下载/删除互斥。
 */
static bool ensure_file(int index, time_t now)
{
    app_fs_lock();
    if (!app_sd_is_mounted()) {
        close_file(index);
        app_fs_unlock();
        return false;
    }
    long segment = (long)s_segment_min * 60L;
    if (segment < 60) {
        segment = 60;
    }
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    int sec_of_day = now_tm.tm_hour * 3600 + now_tm.tm_min * 60 + now_tm.tm_sec;
    int aligned = segment >= 86400 ? 0 : sec_of_day - (sec_of_day % (int)segment);
    time_t start = now - (sec_of_day - aligned);
    if (s_fp[index] && s_file_start[index] == start) {
        app_fs_unlock();
        return true;
    }
    close_file(index);

    /* 按帧所属自然日按需创建目录 /sdcard/YYYY-MM-DD 与 /sdcard/YYYY-MM-DD/UARTx。
     * 幂等（mkdir 返回 EEXIST 即已存在）。若创建失败则降级：本帧不落盘，error=-3。
     */
    if (!app_sd_ensure_log_dir(index, start)) {
        ESP_LOGE(TAG, "ensure log dir failed for UART%d", index);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_view.port[index].file_name[0] = '\0';
        s_view.port[index].error = -3;
        xSemaphoreGive(s_lock);
        app_fs_unlock();
        return false;
    }

    struct tm tm;
    localtime_r(&start, &tm);
    char name[24];
    strftime(name, sizeof(name), "%Y%m%d_%H%M%S.txt", &tm);
    char path[96];
    app_sd_log_path(index, start, path, sizeof(path));
    FILE *fp = fopen(path, "a");
    if (!fp) {
        ESP_LOGE(TAG, "open %s failed", path);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_view.port[index].file_name[0] = '\0';
        s_view.port[index].error = -2;
        xSemaphoreGive(s_lock);
        app_fs_unlock();
        return false;
    }
    setvbuf(fp, NULL, _IOFBF, 4096);
    fseek(fp, 0, SEEK_END);
    if (ftell(fp) == 0) {
        char param[24];
        char stamp[32];
        app_settings_format_port(&s_port_cfg[index], param, sizeof(param));
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(fp, "# UART%d TX=%d RX=%d %s\n", index, app_uart_tx(index), app_uart_rx(index), param);
        fprintf(fp, "# segment %s\n", stamp);
        fprintf(fp, "# format: YYYY-mm-dd HH:MM:SS.mmm TX|RX[-REQ|-RSP] HEX ; # MB-* annotation\n");
    }
    s_fp[index] = fp;
    s_file_start[index] = start;
    /* 保存相对路径（相对 /sdcard），供活动文件判断与控制台显示：YYYY-MM-DD/UARTx/YYYYmmdd_HHMMSS.txt */
    char date[16];
    strftime(date, sizeof(date), "%Y-%m-%d", &tm);
    char rel[64];
    snprintf(rel, sizeof(rel), "%s/UART%d/%s", date, index, name);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_msg(s_view.port[index].file_name, sizeof(s_view.port[index].file_name), rel);
    xSemaphoreGive(s_lock);
    app_fs_unlock();
    return true;
}

/* 记录最近一帧到共享视图：last_hex 保存 "U<i> XX XX ..."（截断到 72 字节），
 * last_port 标记来源通道，frame_seq 自增作为“有新帧”的事件令牌供 UI 判断。
 */
static void remember_frame(int index, const uint8_t *data, int len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int used = snprintf(s_view.last_hex, sizeof(s_view.last_hex), "U%d", index);
    for (int i = 0; i < len && used > 0 && used < (int)sizeof(s_view.last_hex) - 4; i++) {
        used += snprintf(s_view.last_hex + used, sizeof(s_view.last_hex) - (size_t)used, " %02X", data[i]);
    }
    s_view.last_port = index;
    // Event token for the UI: a new frame is available to display.
    s_view.frame_seq++;
    xSemaphoreGive(s_lock);
}

/* 生成并投递一条完整日志行：时间戳 + TX/RX(-REQ/-RSP) + 大写 HEX，
 * 可选追加 Modbus 解析注释；同时写入实时缓冲，最后交给磁盘写任务。
 * 参数：
 *   index  通道索引；
 *   tv     帧首字节的到达时间（毫秒精度写入时间戳）；
 *   data   原始字节；len 字节数（<=0 直接返回）；
 *   own_tx true=本机发送的帧（由 TX 队列/测试/周期发送产生）。
 * 关键步骤：
 *   1. app_modbus_frame 解析帧角色并生成注释 note；
 *   2. 在固定的 PSRAM 行缓冲中拼装文本（避免每帧 2KB 栈开销）；
 *   3. live_append 先更新网页实时流，再把行拷贝进 disk_msg 投递队列；
 *      队列满（磁盘长时间卡顿）时丢弃该行并累加 drop_lines。
 * 异常处理：字幕转换/格式化失败仅导致行缩短；队列发送失败不阻塞 RX。
 */
static void write_frame(int index, const struct timeval *tv,
                        const uint8_t *data, int len, bool own_tx)
{
    if (len <= 0) {
        return;
    }

    mb_info_t mb;
    char note[192];
    note[0] = '\0';
    app_modbus_frame(index, data, len, own_tx, &mb, note, sizeof(note));

    const char *marker;
    if (own_tx) {
        marker = (mb.parsed && mb.role == MB_ROLE_REQ) ? "TX-REQ" : "TX";
    } else if (!mb.parsed) {
        marker = "RX";
    } else if (mb.role == MB_ROLE_REQ) {
        marker = "RX-REQ";
    } else if (mb.role == MB_ROLE_RSP) {
        marker = "RX-RSP";
    } else {
        marker = "RX";
    }

    // Persistent per-port PSRAM buffer: no per-frame 1.6KB stack frame,
    // reused for the single combined fwrite below.
    char *line = s_linebuf[index];
    struct tm tm;
    localtime_r(&tv->tv_sec, &tm);
    int used = snprintf(line, LINE_BYTES, "%04d-%02d-%02d %02d:%02d:%02d.%03ld %s ",
                        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                        tm.tm_hour, tm.tm_min, tm.tm_sec, tv->tv_usec / 1000, marker);
    for (int i = 0; i < len && used > 0 && used < LINE_BYTES - 4; i++) {
        used += snprintf(line + used, LINE_BYTES - (size_t)used, "%02X%s",
                         data[i], (i + 1 == len) ? "" : " ");
    }
    if (used > 0 && used < LINE_BYTES - 2) {
        line[used++] = '\n';
        line[used] = '\0';
    }

    remember_frame(index, data, len);
    {
        const char *tag = index == 1 ? "J2" : index == 2 ? "J3" : "U0";
        // Two appends in the same byte order; avoids a second 1.6KB stack copy.
        char head[12];
        snprintf(head, sizeof(head), "%s ", tag);
        live_append(index, head);
        live_append(index, line);
        if (note[0]) {
            live_append(index, head);
            live_append(index, note);
            live_append(index, "\n");
        }
    }

    size_t total = strlen(line);
    if (note[0]) {
        size_t nlen = strlen(note);
        memcpy(line + total, note, nlen);
        line[total + nlen] = '\n';
        total += nlen + 1;
    }

    // Hand the finished line to the disk writer task; the RX path never
    // touches files, so disk latency cannot delay frame reception.
    disk_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.port = (uint8_t)index;
    msg.tv = *tv;
    msg.len = (uint16_t)total;
    memcpy(msg.line, line, total);
    if (!xQueueSend(s_diskq, &msg, 0)) {
        // Writer queue (16 messages) full after an extreme FS stall: the line
        // is lost and counted; bytes already recorded in rx counters.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_view.port[index].drop_lines++;
        xSemaphoreGive(s_lock);
    }
}

/* 输出并清空当前通道的帧组装缓冲；无有效数据时只重置状态。 */
static void flush_frame(int index)
{
    frame_builder_t *builder = &s_builder[index];
    if (!builder->active || builder->count <= 0) {
        builder->active = false;
        builder->count = 0;
        return;
    }
    write_frame(index, &builder->first, builder->raw, builder->count, false);
    builder->active = false;
    builder->count = 0;
}

/* 把组装器中前 L 个字节作为一帧写出（用于把粘在一起的多帧切开）。
 * 剩余字节 memmove 到缓冲头部继续等待；时间戳按已消费字节数向后平移，
 * 保证剩余帧的首字节时间仍然正确。参数 L 由 Modbus 扫描给出的边界决定。
 */
static void slice_frame(int index, int L)
{
    frame_builder_t *builder = &s_builder[index];
    write_frame(index, &builder->first, builder->raw, L, false);
    memmove(builder->raw, builder->raw + L, builder->count - L);
    builder->count -= L;
    builder->active = builder->count > 0;
    if (builder->active) {
        int64_t shift = (int64_t)L * byte_time_us(index);
        struct timeval d;
        d.tv_sec = shift / 1000000;
        d.tv_usec = shift % 1000000;
        timeradd(&builder->first, &d, &builder->first);
    }
}

/* 判断所有通道是否都处于“帧间隙”（没有半帧在组装）。
 * 返回 true 表示可以安全执行可能阻塞数毫秒的维护操作（磁盘统计、
 * NVS 落盘、config.json 检查）而不破坏帧边界。
 */
static bool ports_idle(void)
{
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        if (s_builder[i].active) {
            return false;
        }
    }
    return true;
}

/* 计算某通道传输一个字节所需的微秒数（按当前波特率与帧格式）。
 * 返回值最小为 1，避免除零/负值；用于把“字节个数”换算成时间偏移。
 */
static int byte_time_us(int index)
{
    const port_setting_t *ps = &s_port_cfg[index];
    uint32_t baud = ps->baud ? ps->baud : 115200;
    int us = (int)((int64_t)frame_bits(ps) * 1000000 / baud);
    return us < 1 ? 1 : us;
}

/* 估算一批数据中第 i 个字节的到达时间，写入 builder->first。
 * 原理：一批数据读到时最后一个字节刚到达（now_us），它之前的第 i 个字节
 * 早 (chunk_len-1-i) 个字符时间，因此用当前墙钟回推。无随机抖动项，
 * 误差小于 1 个字符时间，足以满足毫秒级时间戳需求。
 */
static void stamp_arrival(frame_builder_t *builder, int index,
                          int chunk_len, int i, int64_t now_us)
{
    int64_t before_us = (int64_t)(chunk_len - 1 - i) * byte_time_us(index);
    struct timeval wall, delta;
    gettimeofday(&wall, NULL);
    if (before_us > 0) {
        delta.tv_sec = before_us / 1000000;
        delta.tv_usec = before_us % 1000000;
        timersub(&wall, &delta, &builder->first);
    } else {
        builder->first = wall;
    }
}

/* 把一段连续字节追加进通道组装器（帧边界的基础逻辑）。
 * 参数：
 *   index       通道索引；
 *   data/range_len 本次读到的字节范围；
 *   burst_len/range_off 该范围所属事件的整体长度与偏移，用于时间戳回推；
 *   now_us      读取时刻（esp_timer 微秒）；
 *   force_flush true 时在范围末尾强制结束当前帧。
 * 关键逻辑：
 *   - 遇到 ASCII 帧起始符 ':' 且当前正在组帧，说明新帧开始，先输出旧帧；
 *   - 缓冲达到 FRAME_BYTES 上限时强制输出，避免越界；
 *   - 每字节都更新 last_us，供“长时间无后续字节”的硬超时判断。
 */
static void append_range(int index, const uint8_t *data, int range_len,
                         int burst_len, int range_off, int64_t now_us, bool force_flush)
{
    frame_builder_t *builder = &s_builder[index];

    for (int i = 0; i < range_len; i++) {
        uint8_t ch = data[i];

        if (builder->active && ch == ':') {
            // New ASCII frame beginning inside a burst: finish previous one.
            flush_frame(index);
        }
        if (!builder->active) {
            stamp_arrival(builder, index, burst_len, range_off + i, now_us);
            builder->active = true;
            builder->count = 0;
        }
        builder->raw[builder->count++] = ch;
        builder->last_us = now_us;
        if (builder->count >= FRAME_BYTES) {
            flush_frame(index);
        }
    }
    if (force_flush) {
        flush_frame(index);
    }
}

/* 处理一个 UART 事件携带的整批字节：先入组装器，再按 Modbus 语法切帧。
 * 参数：
 *   index    通道；
 *   data/len 本次读到的字节；
 *   now_us   读取时刻；
 *   frame_end 该事件是否由 RX 超时产生（即真实帧间隙）。
 * 切帧策略：
 *   1. app_modbus_scan 找出缓冲内所有完整帧边界，逐个 slice_frame 输出；
 *   2. 若尾部还可能是某帧前缀（app_modbus_prefix），继续等待后续字节；
 *      否则在真实帧间隙处按原始 HEX 输出（非 Modbus 数据不做协议臆测）；
 *   3. 最后把原始字节转发给 TCP/BLE 透传，并累加 rx_bytes、翻转活动灯。
 * 异常/边界：语法扫描最多 8 个边界，防止极端输入造成长循环。
 */
static void append_burst(int index, const uint8_t *data, int len,
                         int64_t now_us, bool frame_end)
{
    frame_builder_t *builder = &s_builder[index];
    append_range(index, data, len, len, 0, now_us, false);

    int bnd[8];
    int nf = builder->count ? app_modbus_scan(builder->raw, builder->count, bnd, 8) : 0;
    if (nf > 0) {
        int cut = 0;
        for (int k = 0; k < nf; k++) {
            slice_frame(index, bnd[k] - cut);
            cut = bnd[k];
        }
        // Residual tail after valid frames
        if (builder->count > 0) {
            if (frame_end && !app_modbus_prefix(builder->raw, builder->count)) {
                flush_frame(index);
            }
            // incomplete prefix: keep waiting
        }
    } else if (frame_end && builder->count > 0) {
        if (app_modbus_prefix(builder->raw, builder->count)) {
            // Frame candidate spans beyond this event; wait for more bytes.
        } else {
            flush_frame(index);    // non-Modbus, true inter-frame gap: raw line
        }
    }

    app_bridge_feed_uart(index, data, (size_t)len);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_view.port[index].rx_bytes += (uint32_t)len;
    xSemaphoreGive(s_lock);
    // Throttled: cap at one LED toggle per 100ms to protect the I2C bus
    // shared with touch/XL9555 from being starved.
    if (now_us - s_last_led_us >= LED_THROTTLE_US) {
        s_last_led_us = now_us;
        LED_TOGGLE();
        s_led_on = !s_led_on;
    }
}

/* 消费一个 UART 驱动事件：UART_DATA 读出字节交给 append_burst；
 * UART_BUFFER_FULL/FIFO_OVF 说明 RX 缓冲溢出，打印告警（数据可能丢失）。
 * 一次事件只读一次，保持 ISR 定义的帧身份，不把多个事件合并读取。
 */
static void feed_uart_event(int index, const uart_event_t *ev)
{
    if (ev->type == UART_DATA) {
        if (ev->size <= 0) {
            return;
        }
        // Bytes are already in the driver ring when the event posts; one read
        // per event keeps ISR-defined frame identity intact.
        static uint8_t ebuf[128];    // max FIFO-full batch, single task
        int want = ev->size > (int)sizeof(ebuf) ? (int)sizeof(ebuf) : ev->size;
        int n = uart_read_bytes((uart_port_t)index, ebuf, want,
                                pdMS_TO_TICKS(50));
        if (n > 0) {
            append_burst(index, ebuf, n, esp_timer_get_time(), ev->timeout_flag);
        }
    } else if (ev->type == UART_BUFFER_FULL || ev->type == UART_FIFO_OVF) {
        ESP_LOGW(TAG, "UART%d rx overrun event=%d size=%d", index, ev->type, ev->size);
    }
}

// (full sync handled by the writer task flush barriers)

// --- disk writer task: owns all log files; RX path never blocks on it -------

static int64_t s_last_sync;

static void writer_sync_all_locked(void)
{
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        if (s_fp[i]) {
            fflush(s_fp[i]);
            fsync(fileno(s_fp[i]));
        }
    }
}

/* 向磁盘写任务投递有序屏障并等待完成。
 * 参数 kind：1=flush+fsync 所有文件；2=关闭所有文件（卸载卡前使用）。
 * 返回 true=屏障已执行；false=队列满或 3 秒内未收到完成信号（超时）。
 * 依靠消息队列的 FIFO 顺序保证屏障之前的写请求都已处理完毕。
 */
static bool writer_barrier(uint8_t kind)
{
    disk_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = kind;
    if (xQueueSend(s_diskq, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    return xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(3000)) == pdTRUE;
}

/* 磁盘写任务：唯一持有日志 FILE* 的任务，串行消费 RX 路径投递的行。
 * 消息类型：
 *   0=普通行：ensure_file → fwrite，失败只累加 drop_lines（不影响接收）；
 *   1=flush 屏障：flush+fsync 全部文件后回信号；
 *   2=close 屏障：关闭全部文件后回信号（供卸载卡）。
 * 另外每 2 秒做一次 durability sync，避免异常断电丢失过多行。
 * 该任务栈与队列存储均在 PSRAM，且不使用中断 API，节省内部 RAM。
 */
static void writer_task(void *arg)
{
    disk_msg_t msg;
    (void)arg;
    while (1) {
        if (xQueueReceive(s_diskq, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.kind == 1) {
            // Ordered flush barrier: prior messages are already written.
            app_fs_lock();
            writer_sync_all_locked();
            app_fs_unlock();
            xSemaphoreGive(s_flush_done);
            continue;
        }
        if (msg.kind == 2) {
            // Ordered close barrier: finish prior writes, then release files
            // so the logger task can unmount the card.
            for (int i = 0; i < APP_PORT_COUNT; i++) {
                close_file(i);
            }
            xSemaphoreGive(s_flush_done);
            continue;
        }
        if (msg.port >= APP_PORT_COUNT || msg.len == 0) {
            continue;
        }
        bool ok = ensure_file(msg.port, msg.tv.tv_sec);
        if (ok) {
            app_fs_lock();
            size_t w = fwrite(msg.line, 1, msg.len, s_fp[msg.port]);
            app_fs_unlock();
            ok = w == msg.len;
        }
        if (!ok) {
            // File open or disk write failure (FS error/disk full): count the
            // lost line; RX keeps running and later frames are still tried.
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_view.port[msg.port].drop_lines++;
            xSemaphoreGive(s_lock);
        }
        // Periodic durability sync; runs here so it never delays reception.
        int64_t now = esp_timer_get_time();
        if (now - s_last_sync > 2000000) {
            s_last_sync = now;
            app_fs_lock();
            writer_sync_all_locked();
            app_fs_unlock();
        }
    }
}

/* SD 卡自检：写一个 bootcheck.txt → 回读首行 → 判定读写是否正常。
 * 结果写入 s_sd_msg 并发布到界面；无卡或打开失败时给出中文提示。
 * 全程持 app_fs_lock，与日志写入互斥。
 */
static void run_sd_test(void)
{
    if (!app_sd_is_mounted()) {
        copy_msg(s_sd_msg, sizeof(s_sd_msg), "测试失败:无卡");
        publish_sd();
        return;
    }
    app_fs_lock();
    char path[48];
    snprintf(path, sizeof(path), APP_SD_MOUNT "/bootcheck.txt");
    FILE *fp = fopen(path, "w");
    if (!fp) {
        app_fs_unlock();
        copy_msg(s_sd_msg, sizeof(s_sd_msg), "测试失败:无法写");
        publish_sd();
        return;
    }
    char now[24];
    app_time_format(now, sizeof(now));
    fprintf(fp, "sd ok %s\n", now);
    fprintf(fp, "%s.000: 55 AA 01\n", now);
    fclose(fp);
    fp = fopen(path, "r");
    char back[64] = {0};
    if (!fp || !fgets(back, sizeof(back), fp)) {
        if (fp) {
            fclose(fp);
        }
        app_fs_unlock();
        copy_msg(s_sd_msg, sizeof(s_sd_msg), "测试失败:无法读");
        publish_sd();
        return;
    }
    fclose(fp);
    app_fs_unlock();
    copy_msg(s_sd_msg, sizeof(s_sd_msg), "SD读写正常");
    ESP_LOGI(TAG, "sd test ok: %s", back);
    publish_sd();
}

/* 在锁保护下取出并清除各请求标志（reload/remount/sd_test/逐路 tx_test）。
 * 这些接口由 UI、Web、命令行等其它任务调用，只置标志，真正的硬件操作
 * 统一在 logger_task 里执行，避免跨任务操作 UART/文件系统。
 */
static void take_requests(bool *reload, bool *remount, bool *sd_test, bool tx_test[APP_PORT_COUNT])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *reload = s_reload;
    *remount = s_remount;
    *sd_test = s_sd_test;
    s_reload = false;
    s_remount = false;
    s_sd_test = false;
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        tx_test[i] = s_tx_test[i];
        s_tx_test[i] = false;
    }
    xSemaphoreGive(s_lock);
}

/* 在 logger 上下文发送一帧并记录为 TX。
 * 关键步骤：先 flush 当前半帧（避免把 TX 混进 RX 组装）；
 *   uart_write_bytes → 等发送完成 → 读掉回环 echo 并清空对应事件，
 *   防止自己发出的数据被当成 RX 再记一遍；
 *   最后以当前时间写入 TX 帧并累加 tx_bytes/tx_frames。
 * 异常处理：写入失败（wrote<=0）仍记录一条空的 TX 事件便于排查。
 */
static void execute_tx(int port, const uint8_t *data, int len)
{
    flush_frame(port);
    int wrote = uart_write_bytes((uart_port_t)port, data, len);
    if (wrote > 0) {
        uart_wait_tx_done((uart_port_t)port, pdMS_TO_TICKS(100));
        // Discard loopback echo so it is not logged a second time. The 15ms
        // window spans the frame tail plus the frame-gap timeout at 19200.
        uint8_t echo[FRAME_BYTES];
        uart_read_bytes((uart_port_t)port, echo, sizeof(echo), pdMS_TO_TICKS(15));
        // Echo bytes also produced UART_DATA events; drop them so they are
        // neither re-logged nor mistaken for real traffic.
        uart_event_t stale;
        while (s_uart_evt[port] &&
               xQueueReceive(s_uart_evt[port], &stale, 0) == pdTRUE) {
        }
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    write_frame(port, &tv, data, wrote > 0 ? wrote : 0, true);

    if (wrote > 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_view.port[port].tx_bytes += (uint32_t)wrote;
        s_view.port[port].tx_frames++;
        xSemaphoreGive(s_lock);
    }
}

/* 取出所有已排队的出站帧并在本任务发送（TCP/BLE/命令行的下发都走这里）。
 * 每槽在锁内标记 used=false 后释放锁再真正写串口，避免持锁调用阻塞 API。
 * 通道已关闭的请求直接丢弃，不报错。
 */
static void drain_txq(void)
{
    for (int d = 0; d < TXQ_DEPTH; d++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        tx_req_t req = s_txq[d];
        if (req.used) {
            s_txq[d].used = false;
        }
        xSemaphoreGive(s_lock);
        if (req.used) {
            if (req.port >= 0 && req.port < APP_PORT_COUNT && s_uart_on[req.port]) {
                execute_tx(req.port, req.data, req.len);
            }
        }
    }
}

/* 周期发送一步：若该通道使能且到达下次发送时刻，则发一行 "P<port> <seq>\\r\\n"。
 * 参数 port 通道，now_us 当前微秒；间隔被限制为不小于 200ms（默认 1000ms）。
 * 发送走 execute_tx，因此同样按 TX 记录并过滤回环。
 */
static void send_one_periodic(int port, int64_t now_us)
{
    uint32_t interval_ms;
    uint32_t seq;
    bool on;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    on = s_repeat_on[port];
    interval_ms = s_repeat_ms[port] < 200 ? 1000 : s_repeat_ms[port];
    seq = s_repeat_seq[port];
    xSemaphoreGive(s_lock);
    if (!on || !s_uart_on[port] || now_us < s_repeat_next_us[port]) {
        return;
    }
    s_repeat_next_us[port] = now_us + (int64_t)interval_ms * 1000;
    char line[40];
    int len = snprintf(line, sizeof(line), "P%u %04lu\r\n", port, (unsigned long)(seq % 10000));
    if (len > 0) {
        execute_tx(port, (const uint8_t *)line, len);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_repeat_seq[port] = seq + 1;
    s_view.port[port].sending = true;
    xSemaphoreGive(s_lock);
}

static void send_periodic(int64_t now_us)
{
    for (int port = 0; port < APP_PORT_COUNT; port++) {
        send_one_periodic(port, now_us);
    }
}

/* 每 2 秒检查一次 config.json 是否需要从运行时状态重新导出。
 * 重要约束：stat/fopen/fread 会阻塞数毫秒，必须等所有通道处于帧间隙才执行，
 * 否则会把同一帧的后半段推到帧间隙之后，造成一帧被拆成两帧。
 * 未到空闲条件时不推进计时器，下一轮（1ms）立即重试。
 */
static void check_config_changes(int64_t now_us)
{
    if (now_us - last_cfg_check_us < 2000000) {
        return;
    }
    // Same all-idle gate as the periodic fsync: stat/fopen/fread on config.json
    // blocks several ms. If that happened after a frame's first bytes were read,
    // the later bytes would arrive during the block and then be mistaken for a
    // new frame (idle >= 3.5 char gap). Defer until no frame is mid-assembly;
    // do not advance last_cfg_check_us so the next poll retries immediately.
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        if (s_builder[i].active) {
            return;
        }
    }
    last_cfg_check_us = now_us;

    // File is compared with the canonical snapshot: any runtime change,
    // missing field or missing file converges automatically within 2s.
    bool needs = false;
    if (app_config_needs_export(&needs) == 0 && needs) {
        app_config_export();
    }
}

/* 串口记录仪主循环（core 0，优先级 8）。
 * 每轮依次处理：外部请求（重载/重挂载/自检/TX）→ 出站队列 → 周期发送
 * → 逐通道消费 ISR 事件（每通道每轮最多 6 个，保证公平）→ 半帧硬超时
 * → 空闲门控的维护任务（容量统计 30s、RTC 落盘 60s、config 检查 2s）。
 * 调度节拍：有活动时 1ms，全空闲时 8ms，兼顾抓包时序与 CPU 占用。
 */
static void logger_task(void *arg)
{
    int64_t last_sd_try_us = 0;
    int64_t last_usage_us = 0;
    int64_t last_time_save_us = esp_timer_get_time();
    apply_settings();
    publish_sd();

    while (1) {
        bool reload = false;
        bool remount = false;
        bool sd_test = false;
        bool tx_test[APP_PORT_COUNT] = {0};
        take_requests(&reload, &remount, &sd_test, tx_test);

        int64_t now_us = esp_timer_get_time();
        if (remount || (!app_sd_is_mounted() && now_us - last_sd_try_us > 8000000)) {
            last_sd_try_us = now_us;
            // Writer owns the FILE pointers: have it close every file first.
            writer_barrier(2);
            if (app_sd_mount() == ESP_OK) {
                copy_msg(s_sd_msg, sizeof(s_sd_msg), "SD卡正常");
            } else {
                copy_msg(s_sd_msg, sizeof(s_sd_msg), "挂载失败");
            }
            publish_sd();
        }
        if (reload) {
            apply_settings();
        }
        if (sd_test) {
            run_sd_test();
        }
        drain_txq();
        for (int i = 0; i < APP_PORT_COUNT; i++) {
            if (!tx_test[i] || !s_uart_on[i]) {
                continue;
            }
            const uint8_t probe[] = {0x55, 0xAA, 0x01, 0x02, 0x03};
            execute_tx(i, probe, sizeof(probe));
        }

        send_periodic(now_us);

        bool activity = false;
        for (int i = 0; i < APP_PORT_COUNT; i++) {
            if (!s_uart_on[i]) {
                continue;
            }
            // Drain ISR-defined frame events. Bounded per loop so a large
            // backlog (after a stall) still serves all ports, and re-checks
            // the UART between bursts.
            int events = 0;
            uart_event_t ev;
            while (events < 6 &&
                   xQueueReceive(s_uart_evt[i], &ev, 0) == pdTRUE) {
                feed_uart_event(i, &ev);
                events++;
                activity = true;
            }
            // Incomplete frame candidate with no continuation: hard-flush the
            // waiting bytes after 15ms (a corrupt/truncated frame).
            frame_builder_t *b = &s_builder[i];
            if (b->active && now_us - b->last_us > 15000) {
                flush_frame(i);
            }
        }
        // All ports idle: make sure the activity LED does not stay lit.
        if (!activity && s_led_on) {
            LED(1);
            s_led_on = false;
        }

        check_config_changes(now_us);
        // Both operations block (FAT statfs; NVS commit may erase a page).
        // Defer without advancing the timer while a frame is mid-assembly;
        // the next 1ms poll retries, so worst-case delay is one frame gap.
        if (now_us - last_usage_us > 30000000 && ports_idle()) {
            app_sd_refresh_usage();
            publish_sd();
            last_usage_us = now_us;
        }
        if (now_us - last_time_save_us > 60000000 && ports_idle()) {
            app_time_persist_now();
            last_time_save_us = now_us;
        }
        // Fast 1ms polling during traffic keeps frame timing accurate;
        // fall back to 8ms only when every port is idle.
        vTaskDelay(pdMS_TO_TICKS(activity ? 1 : 8));
    }
}

/* 初始化记录仪：创建互斥锁、PSRAM 帧缓冲/行缓冲、磁盘写队列与任务，
 * 最后启动 logger_task（core 0）。大块内存全部指定 MALLOC_CAP_SPIRAM，
 * 保证内部 SRAM 留给 BLE/WiFi/httpd（否则易出现 NO_MEM 或 UI 任务创建失败）。
 * 注意：本函数在 app_main 中必须先于 BLE/WiFi 初始化调用。
 */
void app_logger_start(void)
{
    s_lock = xSemaphoreCreateMutex();

    // Large buffers live in PSRAM so the internal heap stays intact.
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        s_builder[i].raw = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_linebuf[i] = heap_caps_malloc(LINE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    // Disk writer queue: message storage in PSRAM; control block internal.
    s_diskq_store = heap_caps_malloc(DISK_Q_DEPTH * sizeof(disk_msg_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_diskq = xQueueCreateStatic(DISK_Q_DEPTH, sizeof(disk_msg_t),
                                 s_diskq_store, &s_diskq_ctrl);
    s_flush_done = xSemaphoreCreateBinary();

    // Writer stack in PSRAM (8KB; task is ISR-free): keeps 8KB internal heap
    // free so httpd's own task allocation cannot fail.
#define WRITER_STACK 8192
    s_writer_stack = heap_caps_malloc(WRITER_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    copy_msg(s_sd_msg, sizeof(s_sd_msg), app_sd_is_mounted() ? "SD卡正常" : "无卡");
    publish_sd();
    xTaskCreateStatic(writer_task, "diskwr", WRITER_STACK / sizeof(StackType_t),
                      NULL, 6, s_writer_stack, &s_writer_ctrl);
    xTaskCreatePinnedToCore(logger_task, "logger", 8192, NULL, 8, NULL, 0);
}

/* 复制一份当前运行状态快照给调用方（UI/Web/命令行）。
 * 参数 out：外部提供的结构体；持锁整体拷贝，保证字段一致。
 */
void app_logger_get_view(app_view_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_view;
    xSemaphoreGive(s_lock);
}

/* 请求重新加载串口参数（仅置标志，由 logger_task 真正执行）。
 * 典型调用：Web /api/port、屏幕参数页保存、命令行 set 之后。
 */
void app_logger_request_reload(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_reload = true;
    xSemaphoreGive(s_lock);
}

/* 请求重新挂载 SD 卡（热插拔/挂载失败后重试）。
 * 实际动作在 logger_task 中先执行 close 屏障再 mount，避免文件仍在使用。
 */
void app_logger_request_remount(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_remount = true;
    xSemaphoreGive(s_lock);
}

/* 请求执行一次 SD 读写自检（在 logger_task 中调用 run_sd_test）。 */
void app_logger_request_sd_test(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sd_test = true;
    xSemaphoreGive(s_lock);
}

/* 请求在某通道发送一次固定测试帧 55 AA 01 02 03（由 logger_task 执行）。
 * 参数 port 越界时直接忽略。
 */
void app_logger_request_tx_test(int port)
{
    if (port < 0 || port >= APP_PORT_COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tx_test[port] = true;
    xSemaphoreGive(s_lock);
}

/* 开启某通道的周期发送并重置该通道统计。
 * 参数：port 通道；interval_ms 发送间隔（<200ms 时按 200ms 处理）。
 * 同时清零 tx/rx/drop 计数，便于观察本轮周期的收发情况。
 */
void app_logger_set_periodic_tx(int port, uint32_t interval_ms)
{
    if (port < 0 || port >= APP_PORT_COUNT) {
        return;
    }
    if (interval_ms < 200) {
        interval_ms = 200;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_repeat_on[port] = true;
    s_repeat_ms[port] = interval_ms;
    s_repeat_seq[port] = 0;
    s_repeat_next_us[port] = 0;
    s_view.port[port].sending = true;
    s_view.port[port].tx_frames = 0;
    s_view.port[port].tx_bytes = 0;
    s_view.port[port].rx_bytes = 0;
    s_view.port[port].drop_lines = 0;
    xSemaphoreGive(s_lock);
}

/* 停止指定通道的周期发送（仅置标志，发送循环下一轮自然停止）。 */
void app_logger_stop_periodic_tx_port(int port)
{
    if (port < 0 || port >= APP_PORT_COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_repeat_on[port] = false;
    s_view.port[port].sending = false;
    xSemaphoreGive(s_lock);
}

/* 停止全部通道的周期发送。 */
void app_logger_stop_periodic_tx(void)
{
    for (int port = 0; port < APP_PORT_COUNT; port++) {
        app_logger_stop_periodic_tx_port(port);
    }
}

/* 将一帧数据加入出站队列，由 logger_task 发出并记为 TX。
 * 参数：port 通道；data/len 待发数据（len 超过 FRAME_BYTES 截断）。
 * 返回 true=入队成功；false=参数非法或队列（深度 2）已满。
 * 线程安全：可在任意任务调用（TCP/BLE/命令行），队列满时调用方应丢弃或重试。
 */
bool app_logger_request_tx(int port, const uint8_t *data, int len)
{
    if (port < 0 || port >= APP_PORT_COUNT || !data || len <= 0) {
        return false;
    }
    if (len > FRAME_BYTES) {
        len = FRAME_BYTES;
    }
    bool queued = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int d = 0; d < TXQ_DEPTH; d++) {
        if (!s_txq[d].used) {
            s_txq[d].used = true;
            s_txq[d].port = port;
            s_txq[d].len = len;
            memcpy(s_txq[d].data, data, (size_t)len);
            queued = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return queued;
}

/* 调试注入：把最多 32 字节直接排入出站队列（命令行 inject 使用）。
 * 与 app_logger_request_tx 行为一致，仅额外限制长度。
 */
void app_logger_inject(int port, const uint8_t *data, int len)
{
    if (len > 32) {
        len = 32;
    }
    app_logger_request_tx(port, data, len);
}

/* 读取某通道自 since 游标之后的实时文本（增量，供网页轮询）。
 * 参数：
 *   index   通道；since 上次返回的 next 游标（0=从最近 600 字符开始）；
 *   out/out_len 输出缓冲，超出部分截断；next 回传本次结束游标。
 * 边界处理：游标早于环形缓冲有效范围时自动跳到最旧可用位置，
 * 游标超过写指针时钳到末尾，保证不会读到旧数据或越界。
 */
void app_logger_live_get(int index, uint32_t since, char *out, size_t out_len, uint32_t *next)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (next) {
        *next = 0;
    }
    if (index < 0 || index >= APP_PORT_COUNT || !s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t end = s_live_end[index];
    uint32_t start = end > LIVE_CAP ? end - LIVE_CAP : 0;
    if (since == 0 && end > 600) {
        since = end - 600;
    }
    if (since < start) {
        since = start;
    }
    if (since > end) {
        since = end;
    }
    size_t w = 0;
    for (uint32_t pos = since; pos != end && w + 1 < out_len; pos++) {
        out[w++] = s_live[index][pos % LIVE_CAP];
    }
    out[w] = '\0';
    if (next) {
        *next = end;
    }
    xSemaphoreGive(s_lock);
}

/* 强制把队列中已产生的日志全部落盘（下载/打包前调用）。
 * 通过 flush 屏障等待写任务处理完之前的所有消息；最多等待 3 秒，
 * 超时也不阻塞调用方（下载到的文件可能缺少最后一小段）。
 */
void app_logger_flush(void)
{
    if (s_diskq) {
        writer_barrier(1);
    }
}

/* 把各路正在组装的半帧推入写队列并等待全部落盘。
 * 供卸载 SD 卡/OTA 前使用；文件保持打开（由写任务持有），
 * 其它读者仍可并发打开同一文件读取。
 */
void app_logger_release_files(void)
{
    // Push any in-flight assembly into the writer queue, then wait until all
    // of it is on disk. Files stay open (writer owns them); readers can still
    // open them concurrently.
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        flush_frame(i);
    }
    writer_barrier(1);
}
