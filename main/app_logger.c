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

static void publish_port(int index)
{
    char param[24];
    app_settings_format_port(&s_port_cfg[index], param, sizeof(param));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_view.port[index].open = s_uart_on[index];
    copy_msg(s_view.port[index].param, sizeof(s_view.port[index].param), param);
    xSemaphoreGive(s_lock);
}

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

// Total bit-times per serial character with the given framing.
static int frame_bits(const port_setting_t *ps)
{
    int bits = 1;                                     // start bit
    bits += 5 + (int)ps->data_bits;                   // enum 0..3 -> 5..8 bits
    bits += ps->parity != UART_PARITY_DISABLE ? 1 : 0;
    bits += ps->stop_bits == UART_STOP_BITS_2 ? 2 : 1;
    return bits;
}

static int byte_time_us(int index);   // full definition after frame builder

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

    struct tm tm;
    localtime_r(&start, &tm);
    char name[24];
    strftime(name, sizeof(name), "%Y%m%d_%H%M%S.txt", &tm);
    char path[64];
    snprintf(path, sizeof(path), APP_SD_MOUNT "/UART%d/%s", index, name);
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
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_msg(s_view.port[index].file_name, sizeof(s_view.port[index].file_name), name);
    xSemaphoreGive(s_lock);
    app_fs_unlock();
    return true;
}

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

// One complete frame: marker + HEX line (+ Modbus annotation).
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

// Write the first L assembled bytes as one frame, then compact the builder and
// shift the timestamp for the remainder (used to split glued grammar frames).
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

// True when every port is between frames (nothing in mid-assembly). Any
// blocking maintenance on the logger task must run only in such a window so it
// can never postpone a frame's later bytes past the inter-frame gap.
static bool ports_idle(void)
{
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        if (s_builder[i].active) {
            return false;
        }
    }
    return true;
}

// Wall time to shift one UART byte at the port's current configuration.
static int byte_time_us(int index)
{
    const port_setting_t *ps = &s_port_cfg[index];
    uint32_t baud = ps->baud ? ps->baud : 115200;
    int us = (int)((int64_t)frame_bits(ps) * 1000000 / baud);
    return us < 1 ? 1 : us;
}

// Wall-clock estimate for byte i in a chunk of len read at now_us. The last
// byte finished arriving ~now; earlier bytes preceded it by one byte-time.
// Deterministic (no jitter term), accurate to a fraction of one byte-time.
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

// Append one contiguous byte range; force_flush ends the frame at its end.
// burst_len/range_off describe the containing event for arrival estimation.
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

// Append one driver-defined event's bytes and frame the accumulated builder:
// grammar-valid slices are written as frames; an incomplete frame candidate
// waits for continuation (hard-flushed by the loop gap timeout). Non-Modbus
// data at a true frame gap is logged raw, unchanged.
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

// Consume one UART driver event.
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

// Ordered barrier: kind 1 = flush+sync all files; kind 2 = close all files
// (used before unmount). Writer signals after everything queued before this
// message has been handled.
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

// Transmit one queued outbound frame in the logger context, then record it.
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

void app_logger_get_view(app_view_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_view;
    xSemaphoreGive(s_lock);
}

void app_logger_request_reload(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_reload = true;
    xSemaphoreGive(s_lock);
}

void app_logger_request_remount(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_remount = true;
    xSemaphoreGive(s_lock);
}

void app_logger_request_sd_test(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sd_test = true;
    xSemaphoreGive(s_lock);
}

void app_logger_request_tx_test(int port)
{
    if (port < 0 || port >= APP_PORT_COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tx_test[port] = true;
    xSemaphoreGive(s_lock);
}

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

void app_logger_stop_periodic_tx(void)
{
    for (int port = 0; port < APP_PORT_COUNT; port++) {
        app_logger_stop_periodic_tx_port(port);
    }
}

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

void app_logger_inject(int port, const uint8_t *data, int len)
{
    if (len > 32) {
        len = 32;
    }
    app_logger_request_tx(port, data, len);
}

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

void app_logger_flush(void)
{
    if (s_diskq) {
        writer_barrier(1);
    }
}

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
