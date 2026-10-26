#include "app_mbsim.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "app_logger.h"
#include "app_modbus_codec.h"

static const char *TAG = "mbsim";

#define MBS_MAGIC       0x4D425331u
#define MBS_VERSION     1
#define MBS_NVS_NS      "mbsim"
#define MBS_NVS_KEY     "cfg"
#define MBS_MAP_SIZE    256
#define MBS_RX_CAP      320
#define MBS_TIMEOUT_MS  600
#define MBS_TICK_MS     10
#define MBS_ECHO_MS     60000

static mbs_cfg_t s_cfg;
static mbs_stats_t s_stats;
static mbs_stats_t s_rule_stats[MBS_RULE_MAX];
static SemaphoreHandle_t s_lock;
static bool s_started;

static uint8_t s_coils[MBS_MAP_SIZE];
static uint8_t s_discrete[MBS_MAP_SIZE];
static uint16_t s_hold[MBS_MAP_SIZE];
static uint16_t s_input[MBS_MAP_SIZE];
static mb_map_t s_map;

static uint32_t s_due_ms[MBS_RULE_MAX];
static uint32_t s_repeat_left[MBS_RULE_MAX];
static bool s_awaiting[MBS_RULE_MAX];
static uint32_t s_await_ms[MBS_RULE_MAX];

static uint8_t s_srx[APP_PORT_COUNT][MBS_RX_CAP];
static int s_srx_len[APP_PORT_COUNT];
static uint8_t s_mrx[APP_PORT_COUNT][MBS_RX_CAP];
static int s_mrx_len[APP_PORT_COUNT];
static uint8_t s_echo[APP_PORT_COUNT][16];
static int s_echo_len[APP_PORT_COUNT];
static int64_t s_echo_us[APP_PORT_COUNT];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void default_cfg(mbs_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = MBS_MAGIC;
    cfg->version = MBS_VERSION;
    cfg->slave.enabled = 0;
    cfg->slave.port = 1;
    cfg->slave.station = 1;
    cfg->slave.coil_start = 0;
    cfg->slave.coil_count = 16;
    cfg->slave.disc_start = 0;
    cfg->slave.disc_count = 16;
    cfg->slave.hold_start = 0;
    cfg->slave.hold_count = 32;
    cfg->slave.input_start = 0;
    cfg->slave.input_count = 16;
}

static bool fc_supported(uint8_t fc)
{
    return (fc >= 1 && fc <= 6) || fc == 0x0F || fc == 0x10;
}

static bool rule_valid(const mbs_rule_cfg_t *rule)
{
    if (!rule->enabled) {
        return true;
    }
    if (rule->port >= APP_PORT_COUNT || rule->slave == 0 || rule->slave > 247 ||
        !fc_supported(rule->fc) || rule->qty == 0) {
        return false;
    }
    switch (rule->fc) {
    case 1:
    case 2:
        return rule->qty <= 256;
    case 3:
    case 4:
        return rule->qty <= 125;
    case 5:
    case 6:
        return rule->qty == 1 && rule->nvalues >= 1;
    case 0x0F:
        return rule->qty <= 64;
    case 0x10:
        return rule->qty <= MBS_VALUE_MAX && rule->nvalues >= (int)rule->qty;
    default:
        return false;
    }
}

static bool cfg_valid(const mbs_cfg_t *cfg)
{
    if (cfg->rule_count > MBS_RULE_MAX) {
        return false;
    }
    if (cfg->slave.enabled && (cfg->slave.port >= APP_PORT_COUNT ||
                               cfg->slave.station == 0 || cfg->slave.station > 247)) {
        return false;
    }
    const uint16_t limits[4] = {
        cfg->slave.coil_count, cfg->slave.disc_count,
        cfg->slave.hold_count, cfg->slave.input_count,
    };
    for (int i = 0; i < 4; i++) {
        if (limits[i] > MBS_MAP_SIZE) {
            return false;
        }
    }
    for (int i = 0; i < cfg->rule_count; i++) {
        if (!rule_valid(&cfg->rules[i])) {
            return false;
        }
        // A slave and a master rule on the same channel would answer each
        // other's frames; refuse the combination.
        if (cfg->slave.enabled && cfg->rules[i].enabled &&
            cfg->rules[i].port == cfg->slave.port) {
            return false;
        }
    }
    return true;
}

static void rebuild_map(void)
{
    s_map.coils = s_coils;
    s_map.coil_start = s_cfg.slave.coil_start;
    s_map.coil_count = s_cfg.slave.coil_count;
    s_map.discrete = s_discrete;
    s_map.disc_start = s_cfg.slave.disc_start;
    s_map.disc_count = s_cfg.slave.disc_count;
    s_map.holding = s_hold;
    s_map.hold_start = s_cfg.slave.hold_start;
    s_map.hold_count = s_cfg.slave.hold_count;
    s_map.input = s_input;
    s_map.input_start = s_cfg.slave.input_start;
    s_map.input_count = s_cfg.slave.input_count;
}

static void reset_schedule(void)
{
    uint32_t now = now_ms();
    for (int i = 0; i < MBS_RULE_MAX; i++) {
        s_due_ms[i] = now;
        s_repeat_left[i] = s_cfg.rules[i].repeat;
        s_awaiting[i] = false;
    }
}

static void nvs_save(const mbs_cfg_t *cfg)
{
    nvs_handle_t handle;
    if (nvs_open(MBS_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed");
        return;
    }
    esp_err_t err = nvs_set_blob(handle, MBS_NVS_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs save failed: %s", esp_err_to_name(err));
    }
}

static void nvs_load(void)
{
    nvs_handle_t handle;
    mbs_cfg_t cfg;
    size_t size = sizeof(cfg);
    default_cfg(&s_cfg);
    if (nvs_open(MBS_NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_get_blob(handle, MBS_NVS_KEY, &cfg, &size);
    nvs_close(handle);
    if (err == ESP_OK && size == sizeof(cfg) && cfg.magic == MBS_MAGIC &&
        cfg.version == MBS_VERSION && cfg_valid(&cfg)) {
        s_cfg = cfg;
        ESP_LOGI(TAG, "config loaded: slave=%d port=%u station=%u rules=%u",
                 s_cfg.slave.enabled, s_cfg.slave.port, s_cfg.slave.station,
                 s_cfg.rule_count);
    }
}

// ---------------------------------------------------------------------------
// Slave role
// ---------------------------------------------------------------------------

static int slave_want_len(const uint8_t *b, int avail)
{
    if (avail < 2) {
        return 0;
    }
    uint8_t fc = b[1];
    if (fc >= 1 && fc <= 6) {
        return 8;
    }
    if (fc == 0x0F || fc == 0x10) {
        if (avail < 7) {
            return 0;
        }
        int len = 9 + b[6];
        return len <= 260 ? len : -1;
    }
    return -1;
}

static void slave_process(int port)
{
    while (s_srx_len[port] >= 5) {
        int avail = s_srx_len[port];
        int want = slave_want_len(s_srx[port], avail);
        if (want == 0) {
            return; // need more bytes to know the length
        }
        if (want < 0) {
            memmove(s_srx[port], s_srx[port] + 1, (size_t)(avail - 1));
            s_srx_len[port] = avail - 1;
            continue;
        }
        if (avail < want) {
            // A legal frame can arrive split across bursts; keep waiting.
            return;
        }
        if (!app_mb_crc_ok(s_srx[port], want)) {
            memmove(s_srx[port], s_srx[port] + 1, (size_t)(avail - 1));
            s_srx_len[port] = avail - 1;
            continue;
        }

        uint8_t rsp[300];
        int rsp_len;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        rsp_len = app_mb_slave_respond(s_srx[port], want, s_cfg.slave.station,
                                       &s_map, rsp, sizeof(rsp));
        s_stats.slave_requests++;
        if (rsp_len > 0) {
            s_stats.slave_responses++;
        }
        xSemaphoreGive(s_lock);

        memmove(s_srx[port], s_srx[port] + want, (size_t)(avail - want));
        s_srx_len[port] = avail - want;

        if (rsp_len > 0) {
            uint8_t echo[16];
            int echo_len = rsp_len < (int)sizeof(echo) ? rsp_len : (int)sizeof(echo);
            memcpy(echo, rsp, (size_t)echo_len);
            s_echo_len[port] = echo_len;
            memcpy(s_echo[port], echo, (size_t)echo_len);
            s_echo_us[port] = esp_timer_get_time();
            if (!app_logger_request_tx(port, rsp, rsp_len)) {
                ESP_LOGW(TAG, "slave tx queue full on UART%d", port);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Master role
// ---------------------------------------------------------------------------

static int master_rsp_len(const uint8_t *b, int avail)
{
    if (avail < 5) {
        return 0;
    }
    uint8_t fc = b[1];
    if (fc & 0x80) {
        return 5;
    }
    switch (fc) {
    case 1:
    case 2:
    case 3:
    case 4:
        if (avail < 3) {
            return 0;
        }
        return 5 + b[2];
    case 5:
    case 6:
    case 0x0F:
    case 0x10:
        return 8;
    default:
        return -1;
    }
}

static void master_complete(int rule, int status, const mb_rsp_t *rsp)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_awaiting[rule] = false;
    mbs_stats_t *st = &s_rule_stats[rule];
    st->last_status = (int8_t)status;
    if (status == 0 && rsp) {
        st->responses++;
        s_stats.responses++;
        if (rsp->exception) {
            st->last_status = -2;
            st->last_count = 0;
        } else {
            st->last_count = (uint8_t)(rsp->count > 255 ? 255 : rsp->count);
            for (int i = 0; i < MBS_VALUE_MAX && i < rsp->count; i++) {
                st->last_values[i] = rsp->values[i];
            }
        }
    } else {
        st->timeouts++;
        s_stats.timeouts++;
    }
    xSemaphoreGive(s_lock);
}

static void master_rx(int port)
{
    int rule = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_cfg.rule_count; i++) {
        if (s_awaiting[i] && s_cfg.rules[i].port == port) {
            rule = i;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (rule < 0) {
        s_mrx_len[port] = 0;
        return;
    }

    while (s_mrx_len[port] >= 5) {
        int avail = s_mrx_len[port];
        int want = master_rsp_len(s_mrx[port], avail);
        if (want == 0) {
            return;
        }
        if (want < 0) {
            memmove(s_mrx[port], s_mrx[port] + 1, (size_t)(avail - 1));
            s_mrx_len[port] = avail - 1;
            continue;
        }
        if (avail < want) {
            if (avail >= 260) {   // noise without a valid frame: resynchronise
                memmove(s_mrx[port], s_mrx[port] + 1, (size_t)(avail - 1));
                s_mrx_len[port] = avail - 1;
                continue;
            }
            return;
        }
        if (!app_mb_crc_ok(s_mrx[port], want)) {
            memmove(s_mrx[port], s_mrx[port] + 1, (size_t)(avail - 1));
            s_mrx_len[port] = avail - 1;
            continue;
        }
        mb_rsp_t rsp;
        bool ok = app_mb_parse_response(s_mrx[port], want, &rsp);
        memmove(s_mrx[port], s_mrx[port] + want, (size_t)(avail - want));
        s_mrx_len[port] = avail - want;
        master_complete(rule, ok ? 0 : -2, ok ? &rsp : NULL);
        return;
    }
}

static void master_tick(void)
{
    uint32_t now = now_ms();
    int count;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    count = s_cfg.rule_count;
    xSemaphoreGive(s_lock);

    for (int i = 0; i < count; i++) {
        mbs_rule_cfg_t rule;
        bool awaiting;
        uint32_t due;
        uint32_t left;
        uint32_t await_ms;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        rule = s_cfg.rules[i];
        awaiting = s_awaiting[i];
        due = s_due_ms[i];
        left = s_repeat_left[i];
        await_ms = s_await_ms[i];
        xSemaphoreGive(s_lock);

        if (!rule.enabled) {
            continue;
        }
        if (awaiting) {
            if (now - await_ms >= MBS_TIMEOUT_MS) {
                master_complete(i, -1, NULL);
            }
            continue;
        }
        if (rule.repeat > 0 && left == 0) {
            continue;   // finite burst finished
        }
        if ((int32_t)(now - due) < 0) {
            continue;
        }

        uint8_t frame[300];
        int len = app_mb_master_request(rule.slave, rule.fc, rule.start, rule.qty,
                                        rule.values, rule.nvalues, frame, sizeof(frame));
        if (len <= 0 || !app_logger_request_tx(rule.port, frame, len)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_rule_stats[i].errors++;
            s_stats.errors++;
            s_due_ms[i] = now + 1000;
            xSemaphoreGive(s_lock);
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_mrx_len[rule.port] = 0;   // drop stale bytes before the new request
        s_awaiting[i] = true;
        s_await_ms[i] = now;
        if (rule.repeat > 0 && s_repeat_left[i] > 0) {
            s_repeat_left[i]--;
        }
        s_due_ms[i] = now + (rule.period_ms > 0 ? rule.period_ms : 1000);
        s_rule_stats[i].requests++;
        s_stats.requests++;
        xSemaphoreGive(s_lock);
    }
}

static void mbsim_task(void *arg)
{
    (void)arg;
    while (1) {
        master_tick();
        vTaskDelay(pdMS_TO_TICKS(MBS_TICK_MS));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void app_mbsim_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_load();
    rebuild_map();
    reset_schedule();
    s_started = true;

#define MBS_STACK 4096
    StackType_t *stack = heap_caps_malloc(MBS_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *ctrl = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stack && ctrl) {
        xTaskCreateStatic(mbsim_task, "mbsim", MBS_STACK / sizeof(StackType_t), NULL, 4,
                          stack, ctrl);
    } else {
        ESP_LOGE(TAG, "task alloc failed");
    }
}

void app_mbsim_get_cfg(mbs_cfg_t *out)
{
    if (!out) {
        return;
    }
    if (!s_started) {
        default_cfg(out);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

void app_mbsim_get_stats(mbs_stats_t *out)
{
    if (!out) {
        return;
    }
    if (!s_started) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_lock);
}

void app_mbsim_get_rule_stats(int rule, mbs_stats_t *out)
{
    if (!out) {
        return;
    }
    if (!s_started || rule < 0 || rule >= MBS_RULE_MAX) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_rule_stats[rule];
    xSemaphoreGive(s_lock);
}

int app_mbsim_set_cfg(const mbs_cfg_t *cfg)
{
    if (!s_started || !cfg || !cfg_valid(cfg)) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mbs_cfg_t next = *cfg;
    next.magic = MBS_MAGIC;
    next.version = MBS_VERSION;
    s_cfg = next;
    memset(s_rule_stats, 0, sizeof(s_rule_stats));
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.last_status = 0;
    rebuild_map();
    reset_schedule();
    xSemaphoreGive(s_lock);
    nvs_save(&next);   // NVS commit can stall; keep it out of the RX lock path
    ESP_LOGI(TAG, "config applied: slave=%d rules=%u", next.slave.enabled,
             next.rule_count);
    return 0;
}

int app_mbsim_set_rule(int idx, const mbs_rule_cfg_t *rule)
{
    if (!s_started || !rule || idx < 0 || idx >= MBS_RULE_MAX) {
        return -1;
    }
    mbs_cfg_t cfg;
    app_mbsim_get_cfg(&cfg);
    if (idx > cfg.rule_count) {
        return -1;
    }
    cfg.rules[idx] = *rule;
    if (idx == cfg.rule_count) {
        cfg.rule_count++;
    }
    if (!cfg_valid(&cfg)) {
        return -2;
    }
    return app_mbsim_set_cfg(&cfg);
}

int app_mbsim_del_rule(int idx)
{
    if (!s_started) {
        return -1;
    }
    mbs_cfg_t cfg;
    app_mbsim_get_cfg(&cfg);
    if (idx < 0 || idx >= cfg.rule_count) {
        return -1;
    }
    for (int i = idx; i < cfg.rule_count - 1; i++) {
        cfg.rules[i] = cfg.rules[i + 1];
    }
    cfg.rule_count--;
    memset(&cfg.rules[cfg.rule_count], 0, sizeof(mbs_rule_cfg_t));
    return app_mbsim_set_cfg(&cfg);
}

void app_mbsim_trigger(int idx)
{
    if (!s_started) {
        return;
    }
    uint32_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MBS_RULE_MAX; i++) {
        if (idx >= 0 && i != idx) {
            continue;
        }
        s_due_ms[i] = now;
        s_repeat_left[i] = s_cfg.rules[i].repeat;
        s_awaiting[i] = false;
    }
    xSemaphoreGive(s_lock);
}

static bool type_ok(const char *type)
{
    return type && (!strcmp(type, "coil") || !strcmp(type, "disc") ||
                    !strcmp(type, "hold") || !strcmp(type, "input"));
}

bool app_mbsim_value_set(const char *type, uint16_t addr, uint16_t value)
{
    if (!s_started || !type_ok(type)) {
        return false;
    }
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!strcmp(type, "coil") &&
        addr >= s_cfg.slave.coil_start &&
        addr - s_cfg.slave.coil_start < s_cfg.slave.coil_count) {
        s_coils[addr - s_cfg.slave.coil_start] = value ? 1 : 0;
        ok = true;
    } else if (!strcmp(type, "disc") &&
               addr >= s_cfg.slave.disc_start &&
               addr - s_cfg.slave.disc_start < s_cfg.slave.disc_count) {
        s_discrete[addr - s_cfg.slave.disc_start] = value ? 1 : 0;
        ok = true;
    } else if (!strcmp(type, "hold") &&
               addr >= s_cfg.slave.hold_start &&
               addr - s_cfg.slave.hold_start < s_cfg.slave.hold_count) {
        s_hold[addr - s_cfg.slave.hold_start] = value;
        ok = true;
    } else if (!strcmp(type, "input") &&
               addr >= s_cfg.slave.input_start &&
               addr - s_cfg.slave.input_start < s_cfg.slave.input_count) {
        s_input[addr - s_cfg.slave.input_start] = value;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool app_mbsim_value_get(const char *type, uint16_t addr, uint16_t *value)
{
    if (!s_started || !type_ok(type) || !value) {
        return false;
    }
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!strcmp(type, "coil") &&
        addr >= s_cfg.slave.coil_start &&
        addr - s_cfg.slave.coil_start < s_cfg.slave.coil_count) {
        *value = s_coils[addr - s_cfg.slave.coil_start];
        ok = true;
    } else if (!strcmp(type, "disc") &&
               addr >= s_cfg.slave.disc_start &&
               addr - s_cfg.slave.disc_start < s_cfg.slave.disc_count) {
        *value = s_discrete[addr - s_cfg.slave.disc_start];
        ok = true;
    } else if (!strcmp(type, "hold") &&
               addr >= s_cfg.slave.hold_start &&
               addr - s_cfg.slave.hold_start < s_cfg.slave.hold_count) {
        *value = s_hold[addr - s_cfg.slave.hold_start];
        ok = true;
    } else if (!strcmp(type, "input") &&
               addr >= s_cfg.slave.input_start &&
               addr - s_cfg.slave.input_start < s_cfg.slave.input_count) {
        *value = s_input[addr - s_cfg.slave.input_start];
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void app_mbsim_rx(int port, const uint8_t *data, int len)
{
    if (!s_started || port < 0 || port >= APP_PORT_COUNT || !data || len <= 0) {
        return;
    }

    // Master first: responses are consumed independently of the slave role.
    for (int i = 0; i < len; i++) {
        if (s_mrx_len[port] < MBS_RX_CAP) {
            s_mrx[port][s_mrx_len[port]++] = data[i];
        }
    }
    master_rx(port);

    bool slave_here;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slave_here = s_cfg.slave.enabled && s_cfg.slave.port == port;
    xSemaphoreGive(s_lock);
    if (!slave_here) {
        return;
    }

    if (s_echo_len[port] > 0 &&
        esp_timer_get_time() - s_echo_us[port] < (int64_t)MBS_ECHO_MS * 1000) {
        int echo_len = s_echo_len[port];
        int cmp = len < echo_len ? len : echo_len;
        if (cmp > 0 && memcmp(data, s_echo[port], (size_t)cmp) == 0) {
            // RS-485 transceiver echo of our own response; skip the echoed
            // prefix so a concatenated burst keeps the remaining bytes.
            if (len == cmp) {
                return;
            }
            data += cmp;
            len -= cmp;
        }
    }

    for (int i = 0; i < len; i++) {
        if (s_srx_len[port] < MBS_RX_CAP) {
            s_srx[port][s_srx_len[port]++] = data[i];
        } else {
            memmove(s_srx[port], s_srx[port] + 1, MBS_RX_CAP - 1);
            s_srx[port][MBS_RX_CAP - 1] = data[i];
        }
    }
    slave_process(port);
}
