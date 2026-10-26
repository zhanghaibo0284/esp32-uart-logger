#include "app_modbus_codec.h"

#include <string.h>

uint16_t app_mb_crc16(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

bool app_mb_crc_ok(const uint8_t *frame, int len)
{
    if (!frame || len < 4) {
        return false;
    }
    uint16_t calc = app_mb_crc16(frame, len - 2);
    uint16_t recv = (uint16_t)(frame[len - 2] | (frame[len - 1] << 8));
    return calc == recv;
}

static void put_crc(uint8_t *frame, int len_without_crc)
{
    uint16_t crc = app_mb_crc16(frame, len_without_crc);
    frame[len_without_crc] = (uint8_t)(crc & 0xFF);
    frame[len_without_crc + 1] = (uint8_t)(crc >> 8);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFF);
}

static int exception_rsp(uint8_t station, uint8_t fc, uint8_t code,
                         uint8_t *rsp, int cap)
{
    if (cap < 5) {
        return 0;
    }
    rsp[0] = station;
    rsp[1] = (uint8_t)(fc | 0x80);
    rsp[2] = code;
    put_crc(rsp, 3);
    return 5;
}

static bool in_range(uint16_t range_start, uint16_t range_count,
                     uint16_t addr, uint16_t qty)
{
    if (qty == 0) {
        return false;
    }
    uint32_t lo = range_start;
    uint32_t hi = (uint32_t)range_start + range_count;
    uint32_t a = addr;
    return a >= lo && a + qty <= hi;
}

static int read_bits(uint8_t station, uint8_t fc, uint16_t qty,
                     const uint8_t *bits, int bit_count, uint8_t *rsp, int cap)
{
    if (qty == 0 || qty > 2000) {
        return exception_rsp(station, fc, 0x03, rsp, cap);
    }
    if (!bits || qty > (uint16_t)bit_count) {
        return exception_rsp(station, fc, 0x02, rsp, cap);
    }
    int bytes = (qty + 7) / 8;
    if (cap < 5 + bytes) {
        return 0;
    }
    rsp[0] = station;
    rsp[1] = fc;
    rsp[2] = (uint8_t)bytes;
    memset(rsp + 3, 0, (size_t)bytes);
    for (int i = 0; i < qty; i++) {
        if (bits[i]) {
            rsp[3 + i / 8] |= (uint8_t)(1u << (i % 8));
        }
    }
    put_crc(rsp, 3 + bytes);
    return 5 + bytes;
}

static int read_regs(uint8_t station, uint8_t fc, uint16_t qty,
                     const uint16_t *regs, int reg_count, uint8_t *rsp, int cap)
{
    if (qty == 0 || qty > 125) {
        return exception_rsp(station, fc, 0x03, rsp, cap);
    }
    if (!regs || qty > (uint16_t)reg_count) {
        return exception_rsp(station, fc, 0x02, rsp, cap);
    }
    int bytes = qty * 2;
    if (cap < 5 + bytes) {
        return 0;
    }
    rsp[0] = station;
    rsp[1] = fc;
    rsp[2] = (uint8_t)bytes;
    for (int i = 0; i < qty; i++) {
        wr16(rsp + 3 + i * 2, regs[i]);
    }
    put_crc(rsp, 3 + bytes);
    return 5 + bytes;
}

int app_mb_slave_respond(const uint8_t *req, int req_len, uint8_t station,
                         const mb_map_t *map, uint8_t *rsp, int rsp_cap)
{
    if (!req || req_len < 4 || !map || !rsp || rsp_cap < 8) {
        return 0;
    }
    if (!app_mb_crc_ok(req, req_len)) {
        return 0;
    }
    uint8_t target = req[0];
    if (target != station && target != 0) {
        return 0;
    }
    bool broadcast = target == 0;
    uint8_t fc = req[1];
    int pdu = req_len - 2;   // bytes without CRC

    switch (fc) {
    case 0x01:
    case 0x02: {
        if (pdu != 6) {
            return exception_rsp(station, fc, 0x03, rsp, rsp_cap);
        }
        uint16_t start = rd16(req + 2);
        uint16_t qty = rd16(req + 4);
        if (broadcast) {
            return 0;
        }
        const uint8_t *bits = fc == 0x01 ? map->coils : map->discrete;
        uint16_t base = fc == 0x01 ? map->coil_start : map->disc_start;
        uint16_t count = fc == 0x01 ? map->coil_count : map->disc_count;
        if (!in_range(base, count, start, qty)) {
            return exception_rsp(station, fc, 0x02, rsp, rsp_cap);
        }
        return read_bits(station, fc, qty, bits + (start - base), count, rsp, rsp_cap);
    }
    case 0x03:
    case 0x04: {
        if (pdu != 6) {
            return exception_rsp(station, fc, 0x03, rsp, rsp_cap);
        }
        uint16_t start = rd16(req + 2);
        uint16_t qty = rd16(req + 4);
        if (broadcast) {
            return 0;
        }
        const uint16_t *regs = fc == 0x03 ? map->holding : map->input;
        uint16_t base = fc == 0x03 ? map->hold_start : map->input_start;
        uint16_t count = fc == 0x03 ? map->hold_count : map->input_count;
        if (!in_range(base, count, start, qty)) {
            return exception_rsp(station, fc, 0x02, rsp, rsp_cap);
        }
        return read_regs(station, fc, qty, regs + (start - base), count, rsp, rsp_cap);
    }
    case 0x05: {
        if (pdu != 6) {
            return exception_rsp(station, fc, 0x03, rsp, rsp_cap);
        }
        uint16_t addr = rd16(req + 2);
        uint16_t value = rd16(req + 4);
        if (!in_range(map->coil_start, map->coil_count, addr, 1)) {
            return broadcast ? 0 : exception_rsp(station, fc, 0x02, rsp, rsp_cap);
        }
        if (value != 0xFF00 && value != 0x0000) {
            return broadcast ? 0 : exception_rsp(station, fc, 0x03, rsp, rsp_cap);
        }
        if (map->coils) {
            map->coils[addr - map->coil_start] = value ? 1 : 0;
        }
        if (broadcast) {
            return 0;
        }
        memcpy(rsp, req, 6);
        rsp[0] = station;
        put_crc(rsp, 6);
        return 8;
    }
    case 0x06: {
        if (pdu != 6) {
            return exception_rsp(station, fc, 0x03, rsp, rsp_cap);
        }
        uint16_t addr = rd16(req + 2);
        if (!in_range(map->hold_start, map->hold_count, addr, 1)) {
            return broadcast ? 0 : exception_rsp(station, fc, 0x02, rsp, rsp_cap);
        }
        if (map->holding) {
            map->holding[addr - map->hold_start] = rd16(req + 4);
        }
        if (broadcast) {
            return 0;
        }
        memcpy(rsp, req, 6);
        rsp[0] = station;
        put_crc(rsp, 6);
        return 8;
    }
    case 0x0F: {
        if (pdu < 7) {
            return exception_rsp(station, fc, 0x03, rsp, rsp_cap);
        }
        uint16_t start = rd16(req + 2);
        uint16_t qty = rd16(req + 4);
        int bytes = req[6];
        if (qty == 0 || qty > 1968 || bytes != (qty + 7) / 8 || pdu != 7 + bytes) {
            return exception_rsp(station, fc, 0x03, rsp, rsp_cap);
        }
        if (!in_range(map->coil_start, map->coil_count, start, qty)) {
            return broadcast ? 0 : exception_rsp(station, fc, 0x02, rsp, rsp_cap);
        }
        if (map->coils) {
            for (int i = 0; i < qty; i++) {
                map->coils[start - map->coil_start + i] =
                    (uint8_t)((req[7 + i / 8] >> (i % 8)) & 1);
            }
        }
        if (broadcast) {
            return 0;
        }
        rsp[0] = station;
        rsp[1] = fc;
        wr16(rsp + 2, start);
        wr16(rsp + 4, qty);
        put_crc(rsp, 6);
        return 8;
    }
    case 0x10: {
        if (pdu < 7) {
            return exception_rsp(station, fc, 0x03, rsp, rsp_cap);
        }
        uint16_t start = rd16(req + 2);
        uint16_t qty = rd16(req + 4);
        int bytes = req[6];
        if (qty == 0 || qty > 123 || bytes != qty * 2 || pdu != 7 + bytes) {
            return exception_rsp(station, fc, 0x03, rsp, rsp_cap);
        }
        if (!in_range(map->hold_start, map->hold_count, start, qty)) {
            return broadcast ? 0 : exception_rsp(station, fc, 0x02, rsp, rsp_cap);
        }
        if (map->holding) {
            for (int i = 0; i < qty; i++) {
                map->holding[start - map->hold_start + i] = rd16(req + 7 + i * 2);
            }
        }
        if (broadcast) {
            return 0;
        }
        rsp[0] = station;
        rsp[1] = fc;
        wr16(rsp + 2, start);
        wr16(rsp + 4, qty);
        put_crc(rsp, 6);
        return 8;
    }
    default:
        return broadcast ? 0 : exception_rsp(station, fc, 0x01, rsp, rsp_cap);
    }
}

int app_mb_master_request(uint8_t slave, uint8_t fc, uint16_t start, uint16_t qty,
                          const uint16_t *values, int nvalues,
                          uint8_t *out, int cap)
{
    if (!out || cap < 8 || slave == 0 || slave > 247) {
        return 0;
    }
    switch (fc) {
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x04:
        if (qty == 0 || qty > 125) {
            return 0;
        }
        out[0] = slave;
        out[1] = fc;
        wr16(out + 2, start);
        wr16(out + 4, qty);
        put_crc(out, 6);
        return 8;
    case 0x05:
    case 0x06:
        if (!values || nvalues < 1) {
            return 0;
        }
        out[0] = slave;
        out[1] = fc;
        wr16(out + 2, start);
        if (fc == 0x05) {
            wr16(out + 4, values[0] ? 0xFF00 : 0x0000);
        } else {
            wr16(out + 4, values[0]);
        }
        put_crc(out, 6);
        return 8;
    case 0x10: {
        if (qty == 0 || qty > 123 || !values || nvalues < (int)qty) {
            return 0;
        }
        int total = 9 + qty * 2;
        if (cap < total) {
            return 0;
        }
        out[0] = slave;
        out[1] = fc;
        wr16(out + 2, start);
        wr16(out + 4, qty);
        out[6] = (uint8_t)(qty * 2);
        for (int i = 0; i < qty; i++) {
            wr16(out + 7 + i * 2, values[i]);
        }
        put_crc(out, total - 2);
        return total;
    }
    default:
        return 0;
    }
}

bool app_mb_parse_response(const uint8_t *frame, int len, mb_rsp_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!frame || len < 5) {
        return false;
    }
    out->crc_ok = app_mb_crc_ok(frame, len);
    if (!out->crc_ok) {
        return false;
    }
    uint8_t fc = frame[1];
    if (fc & 0x80) {
        if (len < 5) {
            return false;
        }
        out->exception = true;
        out->exc_code = frame[2];
        return true;
    }
    switch (fc) {
    case 0x01:
    case 0x02: {
        int bytes = frame[2];
        if (len != 5 + bytes) {
            return false;
        }
        out->count = bytes;
        for (int i = 0; i < bytes && i < 125; i++) {
            out->values[i] = frame[3 + i];
        }
        return true;
    }
    case 0x03:
    case 0x04: {
        int bytes = frame[2];
        if (bytes % 2 != 0 || len != 5 + bytes) {
            return false;
        }
        out->count = bytes / 2;
        for (int i = 0; i < out->count && i < 125; i++) {
            out->values[i] = rd16(frame + 3 + i * 2);
        }
        return true;
    }
    case 0x05:
    case 0x06:
    case 0x0F:
    case 0x10:
        if (len != 8) {
            return false;
        }
        out->count = 1;
        out->values[0] = rd16(frame + 4);
        return true;
    default:
        return true;
    }
}
