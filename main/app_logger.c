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
#define TXQ_DEPTH   2
#define SNAP_BYTES  2048

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
static FILE *s_fp[APP_PORT_COUNT];
static time_t s_file_start[APP_PORT_COUNT];
static frame_builder_t s_builder[APP_PORT_COUNT];
static int64_t s_prev_poll_us[APP_PORT_COUNT];
static tx_req_t s_txq[TXQ_DEPTH];
static bool s_dirty;

#define LIVE_CAP 4096
static char s_live[APP_PORT_COUNT][LIVE_CAP];
static uint32_t s_live_end[APP_PORT_COUNT];

static char s_sd_msg[48] = "等待SD卡";

// --- config change detection ------------------------------------------------
static char *s_last_snap;       // PSRAM
static bool s_snap_inited;
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
}

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
    err = uart_driver_install((uart_port_t)index, 1024, 0, 0, NULL, 0);
    if (err == ESP_ERR_NO_MEM) {
        err = uart_driver_install((uart_port_t)index, 256, 0, 0, NULL, 0);
    }
    if (err != ESP_OK) {
        return err;
    }
    uart_set_rx_full_threshold((uart_port_t)index, 1);
    uart_set_rx_timeout((uart_port_t)index, 1);
    uart_set_always_rx_timeout((uart_port_t)index, true);
    gpio_pullup_en(app_uart_rx(index));
    gpio_pulldown_dis(app_uart_rx(index));
    uart_flush_input((uart_port_t)index);
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

    char line[1600];
    struct tm tm;
    localtime_r(&tv->tv_sec, &tm);
    int used = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03ld %s ",
                        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                        tm.tm_hour, tm.tm_min, tm.tm_sec, tv->tv_usec / 1000, marker);
    for (int i = 0; i < len && used > 0 && used < (int)sizeof(line) - 4; i++) {
        used += snprintf(line + used, sizeof(line) - (size_t)used, "%02X%s",
                         data[i], (i + 1 == len) ? "" : " ");
    }
    if (used > 0 && used < (int)sizeof(line) - 2) {
        line[used++] = '\n';
        line[used] = '\0';
    }

    remember_frame(index, data, len);
    {
        const char *tag = index == 1 ? "J2" : index == 2 ? "J3" : "U0";
        char tagged[1640];
        snprintf(tagged, sizeof(tagged), "%s %s", tag, line);
        live_append(index, tagged);
        if (note[0]) {
            char noted[220];
            snprintf(noted, sizeof(noted), "%s %s\n", tag, note);
            live_append(index, noted);
        }
    }

    if (!ensure_file(index, tv->tv_sec)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_view.port[index].drop_lines++;
        xSemaphoreGive(s_lock);
        return;
    }

    app_fs_lock();
    size_t need = strlen(line);
    bool ok = fwrite(line, 1, need, s_fp[index]) == need;
    if (ok && note[0]) {
        size_t nlen = strlen(note);
        ok = fwrite(note, 1, nlen, s_fp[index]) == nlen;
        if (ok) {
            ok = fputc('\n', s_fp[index]) == '\n';
        }
    }
    app_fs_unlock();

    if (ok) {
        s_dirty = true;
    } else {
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

// Inter-frame silence: Modbus RTU 3.5 char times (>=19200 baud fixed 1.75ms).
static int frame_gap_us(int index)
{
    uint32_t baud = s_port_cfg[index].baud ? s_port_cfg[index].baud : 115200;
    int64_t us = (11LL * 35 * 100000) / (int64_t)baud;
    if (us < 1750) {
        us = 1750;
    }
    return (int)us;
}

// Duration of one byte on the wire: start + 8 data + parity(if used) + stop.
static int64_t byte_time_us(int index)
{
    uint32_t baud = s_port_cfg[index].baud ? s_port_cfg[index].baud : 115200;
    int bits = s_port_cfg[index].parity == UART_PARITY_DISABLE ? 10 : 11;
    return (int64_t)bits * 1000000 / (int64_t)baud;
}

static void push_bytes(int index, const uint8_t *data, int len, int64_t now_us)
{
    frame_builder_t *builder = &s_builder[index];
    bool ascii_mode = builder->active && builder->raw[0] == ':';

    // Bytes arrived only during this poll window. Estimate the bus silence
    // before this chunk as (window length - time to send len bytes); this keeps
    // one frame intact across consecutive polls without per-byte timestamps.
    if (len > 0 && builder->active) {
        int64_t window = now_us - s_prev_poll_us[index];
        int64_t data_time = (int64_t)len * byte_time_us(index);
        int64_t silence = window > data_time ? window - data_time : 0;
        if ((data[0] == ':') ||
            (ascii_mode && data[0] == '\n') ||
            (!ascii_mode && silence >= frame_gap_us(index))) {
            flush_frame(index);
            ascii_mode = false;
        }
    }

    for (int i = 0; i < len; i++) {
        uint8_t ch = data[i];

        if (builder->active && ch == ':' && i > 0) {
            // New ASCII frame beginning inside a chunk: finish the previous one.
            flush_frame(index);
            ascii_mode = true;
        }
        if (!builder->active) {
            gettimeofday(&builder->first, NULL);
            builder->active = true;
            builder->count = 0;
            ascii_mode = ch == ':';
        }
        builder->raw[builder->count++] = ch;
        builder->last_us = now_us;
        if (builder->count >= FRAME_BYTES) {
            flush_frame(index);
        }
    }
    app_bridge_feed_uart(index, data, (size_t)len);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_view.port[index].rx_bytes += (uint32_t)len;
    xSemaphoreGive(s_lock);
    LED_TOGGLE();
}

static void sync_files(void)
{
    app_fs_lock();
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        if (!s_fp[i]) {
            continue;
        }
        fflush(s_fp[i]);
        fsync(fileno(s_fp[i]));
    }
    s_dirty = false;
    app_fs_unlock();
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
        // Discard loopback echo so it is not logged a second time.
        uint8_t echo[FRAME_BYTES];
        uart_read_bytes((uart_port_t)port, echo, sizeof(echo), pdMS_TO_TICKS(5));
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
    last_cfg_check_us = now_us;

    char *cur = heap_caps_malloc(SNAP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cur) {
        return;
    }
    app_config_build_snapshot(cur, SNAP_BYTES);

    if (!s_snap_inited) {
        s_snap_inited = true;
        snprintf(s_last_snap, SNAP_BYTES, "%s", cur);
        app_config_status_t st;
        app_config_get_status(&st);
        if (app_sd_is_mounted() && !st.present) {
            app_config_export();
        }
        free(cur);
        return;
    }

    if (strcmp(cur, s_last_snap) != 0) {
        if (app_config_export() == 0) {
            snprintf(s_last_snap, SNAP_BYTES, "%s", cur);
        }
    } else {
        app_config_status_t st;
        app_config_get_status(&st);
        if (app_sd_is_mounted() && !st.present) {
            app_config_export();
        }
    }
    free(cur);
}

static void logger_task(void *arg)
{
    uint8_t buf[256];
    int64_t last_sync_us = 0;
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
            for (int i = 0; i < APP_PORT_COUNT; i++) {
                close_file(i);
            }
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
            int n = uart_read_bytes((uart_port_t)i, buf, sizeof(buf), 0);
            if (n > 0) {
                push_bytes(i, buf, n, now_us);
                activity = true;
            } else if (s_builder[i].active && now_us - s_builder[i].last_us > frame_gap_us(i)) {
                flush_frame(i);
            }
            if (s_builder[i].active) {
                activity = true;
            }
            s_prev_poll_us[i] = now_us;
        }

        if (s_dirty && now_us - last_sync_us > 2000000) {
            sync_files();
            last_sync_us = now_us;
        }
        check_config_changes(now_us);
        if (now_us - last_usage_us > 30000000) {
            app_sd_refresh_usage();
            publish_sd();
            last_usage_us = now_us;
        }
        if (now_us - last_time_save_us > 60000000) {
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
    s_last_snap = heap_caps_malloc(SNAP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        s_builder[i].raw = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_prev_poll_us[i] = t0;
    }

    copy_msg(s_sd_msg, sizeof(s_sd_msg), app_sd_is_mounted() ? "SD卡正常" : "无卡");
    publish_sd();
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
    sync_files();
}

void app_logger_release_files(void)
{
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        flush_frame(i);
        close_file(i);
    }
}
