#include "app_parse.h"

#include <stdio.h>
#include <string.h>
#include "app_modbus.h"

static const char *const k_proto_names[] = {
    "off", "auto", "modbus_rtu", "modbus_ascii", "dlt645", "cjt188", "ascii",
};

const char *app_parse_proto_name(int proto)
{
    if (proto < 0 || proto >= APP_PARSE_PROTO_MAX) {
        return "off";
    }
    return k_proto_names[proto];
}

bool app_parse_proto_from_name(const char *name, uint8_t *out)
{
    if (!name || !out) {
        return false;
    }
    for (int i = 0; i < APP_PARSE_PROTO_MAX; i++) {
        if (strcmp(name, k_proto_names[i]) == 0) {
            *out = (uint8_t)i;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Modbus RTU / ASCII (reuses the CRC/LRC-validated frame analyser)
// ---------------------------------------------------------------------------

static bool parse_modbus(int port, const uint8_t *data, int len, bool own_tx,
                         bool require_crc, ap_kind_t *kind, char *note, size_t note_len)
{
    mb_info_t info;
    app_modbus_frame(port, data, len, own_tx, &info, note, note_len);
    if (!info.parsed) {
        if (note && note_len) {
            note[0] = '\0';
        }
        return false;
    }
    if (require_crc && !info.check_ok) {
        if (note && note_len) {
            note[0] = '\0';
        }
        return false;
    }
    if (kind) {
        if (info.role == MB_ROLE_REQ) {
            *kind = AP_KIND_REQ;
        } else if (info.role == MB_ROLE_RSP) {
            *kind = AP_KIND_RSP;
        } else {
            *kind = AP_KIND_NONE;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// DL/T 645-2007
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t di;
    const char *name;
    uint8_t frac;
} dlt_di_t;

static const dlt_di_t k_dlt_di[] = {
    {0x00000000u, "combo_active_energy", 2},   // 组合有功总电能 kWh
    {0x00010000u, "forward_active_energy", 2}, // 正向有功总电能 kWh
    {0x00020000u, "reverse_active_energy", 2}, // 反向有功总电能 kWh
    {0x02010100u, "voltage_a", 1},             // A 相电压 V
    {0x02010200u, "voltage_b", 1},             // B 相电压 V
    {0x02010300u, "voltage_c", 1},             // C 相电压 V
    {0x02020100u, "current_a", 3},             // A 相电流 A
    {0x02020200u, "current_b", 3},             // B 相电流 A
    {0x02020300u, "current_c", 3},             // C 相电流 A
    {0x02030000u, "active_power", 4},          // 瞬时总有功功率 kW
    {0x02040000u, "reactive_power", 4},        // 瞬时总无功功率 kvar
    {0x04000401u, "date_week", 0},             // 日期及星期
    {0x04000402u, "time", 0},                  // 时间
};

static int bcd_digit(uint8_t byte)
{
    uint8_t v = (uint8_t)(byte - 0x33);
    if ((v >> 4) > 9 || (v & 0x0F) > 9) {
        return -1;
    }
    return (v >> 4) * 10 + (v & 0x0F);
}

static void format_meter_addr(const uint8_t *low_first, int bytes, char *out, size_t cap)
{
    size_t used = 0;
    out[0] = '\0';
    for (int i = bytes - 1; i >= 0 && used + 3 < cap; i--) {
        int v = bcd_digit(low_first[i]);
        if (v < 0) {
            snprintf(out + used, cap - used, "%02X", (unsigned)(low_first[i] - 0x33));
        } else {
            snprintf(out + used, cap - used, "%02d", v);
        }
        used = strlen(out);
    }
}

static void format_scaled(char *out, size_t cap, unsigned long long raw, int frac)
{
    if (frac <= 0) {
        snprintf(out, cap, "%llu", raw);
        return;
    }
    if (frac > 6) {
        frac = 6;
    }
    unsigned long long div = 1;
    for (int i = 0; i < frac; i++) {
        div *= 10;
    }
    unsigned long long whole = raw / div;
    unsigned long long part = raw % div;
    char tail[8];
    for (int i = frac - 1; i >= 0; i--) {
        tail[i] = (char)('0' + (int)(part % 10));
        part /= 10;
    }
    tail[frac] = '\0';
    snprintf(out, cap, "%llu.%s", whole, tail);
}

static unsigned long long bcd_le(const uint8_t *p, int n)
{
    unsigned long long value = 0;
    for (int i = n - 1; i >= 0; i--) {
        int v = (p[i] - 0x33) & 0xFF;
        int hi = (v >> 4) & 0x0F;
        int lo = v & 0x0F;
        if (hi > 9 || lo > 9) {
            value = value * 100; // non-BCD: keep progressing, value shown as hex below
            continue;
        }
        value = value * 100 + (unsigned)(hi * 10 + lo);
    }
    return value;
}

static bool parse_dlt645(const uint8_t *d, int n, bool require_crc,
                         ap_kind_t *kind, char *note, size_t cap)
{
    if (n < 12 || d[0] != 0x68 || d[7] != 0x68 || d[n - 1] != 0x16) {
        return false;
    }
    int len_field = d[9];
    if (n != 12 + len_field) {
        return false;
    }
    uint8_t sum = 0;
    for (int i = 0; i < n - 2; i++) {
        sum = (uint8_t)(sum + d[i]);
    }
    bool crc_ok = sum == d[n - 2];
    if (require_crc && !crc_ok) {
        return false;
    }

    char addr[20];
    format_meter_addr(d + 1, 6, addr, sizeof(addr));
    uint8_t control = d[8];
    bool response = (control & 0x80) != 0;
    if (kind) {
        *kind = response ? AP_KIND_RSP : AP_KIND_REQ;
    }

    int used = snprintf(note, cap, "# DLT645 %s addr=%s c=0x%02X L=%d",
                        response ? "RSP" : "REQ", addr, control, len_field);
    if (used < 0 || used >= (int)cap) {
        return true;
    }

    if (len_field >= 4) {
        uint8_t di_raw[4];
        for (int i = 0; i < 4; i++) {
            di_raw[i] = (uint8_t)(d[10 + i] - 0x33);
        }
        uint32_t di = (uint32_t)di_raw[0] | ((uint32_t)di_raw[1] << 8) |
                      ((uint32_t)di_raw[2] << 16) | ((uint32_t)di_raw[3] << 24);
        const dlt_di_t *entry = NULL;
        for (size_t k = 0; k < sizeof(k_dlt_di) / sizeof(k_dlt_di[0]); k++) {
            if (k_dlt_di[k].di == di) {
                entry = &k_dlt_di[k];
                break;
            }
        }
        used += snprintf(note + used, cap - (size_t)used, " DI=%08lX",
                         (unsigned long)di);
        if (used >= (int)cap) {
            return true;
        }
        int val_bytes = len_field - 4;
        if (val_bytes > 0) {
            int show = val_bytes > 4 ? 4 : val_bytes;
            if (entry) {
                unsigned long long raw = bcd_le(d + 14, show);
                char num[32];
                format_scaled(num, sizeof(num), raw, entry->frac);
                used += snprintf(note + used, cap - (size_t)used, " %s=%s",
                                 entry->name, num);
                if (used >= (int)cap) {
                    return true;
                }
            }
            used += snprintf(note + used, cap - (size_t)used, " data=");
            for (int i = 0; i < show && used < (int)cap - 4; i++) {
                used += snprintf(note + used, cap - (size_t)used, "%02X",
                                 (unsigned)(d[14 + i] - 0x33));
            }
        }
    }
    if (used < (int)cap) {
        snprintf(note + used, cap - (size_t)used, " check=%s", crc_ok ? "OK" : "BAD");
    }
    return true;
}

// ---------------------------------------------------------------------------
// CJ/T 188 (water / gas / heat meters)
// ---------------------------------------------------------------------------

static bool parse_cjt188(const uint8_t *d, int n, bool require_crc,
                         ap_kind_t *kind, char *note, size_t cap)
{
    if (n < 14 || d[0] != 0x68 || d[3] != 0x68 || d[n - 1] != 0x16) {
        return false;
    }
    uint8_t sum = 0;
    for (int i = 0; i < n - 2; i++) {
        sum = (uint8_t)(sum + d[i]);
    }
    bool crc_ok = sum == d[n - 2];
    if (require_crc && !crc_ok) {
        return false;
    }

    char addr[20];
    format_meter_addr(d + 5, 7, addr, sizeof(addr));
    uint8_t control = d[4];
    if (kind) {
        *kind = (control & 0x80) ? AP_KIND_RSP : AP_KIND_REQ;
    }
    int data_len = d[1]; // advertised length field; kept for diagnostics
    snprintf(note, cap,
             "# CJT188 %s addr=%s c=0x%02X L=%d bytes=%d check=%s",
             (control & 0x80) ? "RSP" : "REQ", addr, control, data_len,
             n - 15, crc_ok ? "OK" : "BAD");
    return true;
}

// ---------------------------------------------------------------------------
// Printable ASCII text (AT commands, plain-text instruments)
// ---------------------------------------------------------------------------

static bool parse_ascii_text(const uint8_t *d, int n, ap_kind_t *kind,
                             char *note, size_t cap)
{
    if (n <= 0) {
        return false;
    }
    int printable = 0;
    for (int i = 0; i < n; i++) {
        uint8_t ch = d[i];
        if ((ch >= 0x20 && ch <= 0x7E) || ch == '\r' || ch == '\n' || ch == '\t') {
            printable++;
        }
    }
    if (printable * 10 < n * 9) {
        return false;
    }
    if (kind) {
        *kind = AP_KIND_DATA;
    }
    int used = snprintf(note, cap, "# ASCII \"");
    for (int i = 0; i < n && used > 0 && used < (int)cap - 4; i++) {
        uint8_t ch = d[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break; // one line per frame is enough
        }
        if (ch == '"' || ch == '\\') {
            note[used++] = '\\';
        } else if (ch < 0x20 || ch > 0x7E) {
            ch = '.';
        }
        note[used++] = (char)ch;
    }
    if (used > 0 && used < (int)cap - 1) {
        note[used++] = '"';
        note[used] = '\0';
    }
    return true;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

bool app_parse_frame(int proto, int port, const uint8_t *data, int len, bool own_tx,
                     ap_kind_t *kind, char *note, size_t note_len)
{
    if (kind) {
        *kind = AP_KIND_NONE;
    }
    if (!note || note_len == 0) {
        return false;
    }
    note[0] = '\0';
    if (!data || len <= 0 || proto <= APP_PARSE_OFF || proto >= APP_PARSE_PROTO_MAX) {
        return false;
    }

    switch (proto) {
    case APP_PARSE_MODBUS_RTU:
        if (data[0] == ':') {
            return false;
        }
        return parse_modbus(port, data, len, own_tx, false, kind, note, note_len);
    case APP_PARSE_MODBUS_ASCII:
        if (data[0] != ':') {
            return false;
        }
        return parse_modbus(port, data, len, own_tx, false, kind, note, note_len);
    case APP_PARSE_DLT645:
        return parse_dlt645(data, len, false, kind, note, note_len);
    case APP_PARSE_CJT188:
        return parse_cjt188(data, len, false, kind, note, note_len);
    case APP_PARSE_ASCII_TEXT:
        return parse_ascii_text(data, len, kind, note, note_len);
    case APP_PARSE_AUTO:
        break;
    default:
        return false;
    }

    ap_kind_t candidate;
    if (data[0] == ':') {
        if (parse_modbus(port, data, len, own_tx, true, &candidate, note, note_len)) {
            if (kind) {
                *kind = candidate;
            }
            return true;
        }
        note[0] = '\0';
        return false;
    }
    if (parse_modbus(port, data, len, own_tx, true, &candidate, note, note_len)) {
        if (kind) {
            *kind = candidate;
        }
        return true;
    }
    note[0] = '\0';
    if (parse_dlt645(data, len, true, &candidate, note, note_len)) {
        if (kind) {
            *kind = candidate;
        }
        return true;
    }
    note[0] = '\0';
    if (parse_cjt188(data, len, true, &candidate, note, note_len)) {
        if (kind) {
            *kind = candidate;
        }
        return true;
    }
    note[0] = '\0';
    return parse_ascii_text(data, len, kind, note, note_len);
}
