#include "app_modbus.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------

static uint16_t mb_crc16(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static uint8_t mb_lrc(const uint8_t *data, int len)
{
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint8_t)(-(int8_t)sum);
}

static int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

// ---------------------------------------------------------------------------
// PDU structure
// ---------------------------------------------------------------------------

typedef enum {
    CAND_UNKNOWN = 0,
    CAND_REQ,
    CAND_RSP,
    CAND_AMB, // could be either request or echoed response
} cand_t;

typedef struct {
    cand_t cand;
    bool exception;
    uint8_t addr;
    uint8_t fc;
    uint16_t start;
    uint16_t qty;
    uint16_t value;
    uint16_t subfunc;
    uint8_t bytecount;
    uint8_t status;
} pdu_t;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

// pdu points at the raw frame: [addr][fc][...data...], with no checksum.
static void parse_pdu(const uint8_t *pdu, int len, pdu_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 2) {
        return;
    }
    out->addr = pdu[0];
    out->fc = pdu[1];
    uint8_t fc = out->fc;

    if (fc & 0x80) {
        // Exception response: addr, fc|0x80, code
        if (len == 3) {
            out->cand = CAND_RSP;
            out->exception = true;
            out->status = pdu[2];
        }
        return;
    }

    switch (fc) {
    case 0x01: case 0x02: case 0x03: case 0x04:
        if (len == 6) {
            out->cand = CAND_REQ;
            out->start = rd16(pdu + 2);
            out->qty = rd16(pdu + 4);
        } else if (len >= 3 && pdu[2] == len - 3) {
            out->cand = CAND_RSP;
            out->bytecount = pdu[2];
        }
        break;
    case 0x05:
    case 0x06:
        if (len == 6) {
            out->cand = CAND_AMB;
            out->start = rd16(pdu + 2);
            out->value = rd16(pdu + 4);
        }
        break;
    case 0x07:
        if (len == 2) {
            out->cand = CAND_REQ;
        } else if (len == 3) {
            out->cand = CAND_RSP;
            out->status = pdu[2];
        }
        break;
    case 0x08:
        if (len == 6) {
            out->cand = CAND_AMB;
            out->subfunc = rd16(pdu + 2);
            out->value = rd16(pdu + 4);
        }
        break;
    case 0x0B:
        if (len == 2) {
            out->cand = CAND_REQ;
        } else if (len == 6) {
            out->cand = CAND_RSP;
            out->status = rd16(pdu + 2);
            out->value = rd16(pdu + 4);
        }
        break;
    case 0x0C:
        if (len == 2) {
            out->cand = CAND_REQ;
        } else if (len >= 3 && pdu[2] == len - 3) {
            out->cand = CAND_RSP;
            out->bytecount = pdu[2];
        }
        break;
    case 0x0F:
    case 0x10:
        if (len == 6) {
            out->cand = CAND_RSP;
            out->start = rd16(pdu + 2);
            out->qty = rd16(pdu + 4);
        } else if (len >= 7) {
            out->start = rd16(pdu + 2);
            out->qty = rd16(pdu + 4);
            out->bytecount = pdu[6];
            if (pdu[6] == len - 7) {
                out->cand = CAND_REQ;
            }
        }
        break;
    case 0x16:
        if (len == 8) {
            out->cand = CAND_AMB;
            out->start = rd16(pdu + 2);
            out->value = rd16(pdu + 4);
            out->qty = rd16(pdu + 6);
        }
        break;
    case 0x17:
        // addr,fc, rdstart, rdqty, wrstart, wrqty, bytecount, wrdata
        if (len >= 3 && pdu[2] == len - 3) {
            out->cand = CAND_RSP;
            out->bytecount = pdu[2];
        } else if (len >= 11) {
            out->start = rd16(pdu + 6);
            out->qty = rd16(pdu + 8);
            out->bytecount = pdu[10];
            if (pdu[10] == len - 11) {
                out->cand = CAND_REQ;
            }
        }
        break;
    case 0x14:
    case 0x15:
        // file record PDU and response share fc,bytecount,data shape
        if (len >= 3 && pdu[2] == len - 3) {
            out->cand = CAND_AMB;
            out->bytecount = pdu[2];
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Request/response pairing state
// ---------------------------------------------------------------------------

typedef struct {
    bool pending;
    uint8_t addr;
    uint8_t fc;
} pending_t;

static pending_t s_pending[APP_PORT_COUNT];

void app_modbus_reset(int port)
{
    if (port >= 0 && port < APP_PORT_COUNT) {
        s_pending[port].pending = false;
    }
}

static mb_role_t resolve_role(int port, const pdu_t *pdu, bool own_tx)
{
    pending_t *pend = &s_pending[port];

    if (own_tx) {
        if (!pdu->exception) {
            pend->pending = true;
            pend->addr = pdu->addr;
            pend->fc = pdu->fc;
        }
        return MB_ROLE_REQ;
    }

    switch (pdu->cand) {
    case CAND_RSP:
        pend->pending = false;
        return MB_ROLE_RSP;
    case CAND_REQ:
        pend->pending = true;
        pend->addr = pdu->addr;
        pend->fc = pdu->fc;
        return MB_ROLE_REQ;
    case CAND_AMB:
        if (pend->pending && pend->addr == pdu->addr && pend->fc == pdu->fc) {
            pend->pending = false;
            return MB_ROLE_RSP;
        }
        pend->pending = true;
        pend->addr = pdu->addr;
        pend->fc = pdu->fc;
        return MB_ROLE_REQ;
    default:
        return MB_ROLE_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Annotation
// ---------------------------------------------------------------------------

static const char *fc_name(uint8_t fc)
{
    switch (fc) {
    case 0x01: return "READ_COIL";
    case 0x02: return "READ_DISC";
    case 0x03: return "READ_HOLD";
    case 0x04: return "READ_INPUT";
    case 0x05: return "WRITE_COIL";
    case 0x06: return "WRITE_REG";
    case 0x07: return "READ_STATUS";
    case 0x08: return "DIAGNOSTIC";
    case 0x0B: return "COMM_EVENT_CTR";
    case 0x0C: return "COMM_EVENT_LOG";
    case 0x0F: return "WRITE_COILS";
    case 0x10: return "WRITE_REGS";
    case 0x14: return "READ_FILE";
    case 0x15: return "WRITE_FILE";
    case 0x16: return "MASK_WRITE";
    case 0x17: return "RD_WR_MULTI";
    default:   return "UNKNOWN";
    }
}

static void make_note(const pdu_t *pdu, mb_role_t role, bool is_ascii, bool check_ok,
                      char *note, size_t note_len)
{
    const char *proto = is_ascii ? "MB-ASCII" : "MB-RTU";
    const char *role_s = role == MB_ROLE_REQ ? "REQ" : role == MB_ROLE_RSP ? "RSP" : "??";
    const char *chk = check_ok ? "OK" : "BAD";
    int n = 0;

    if (pdu->exception) {
        n = snprintf(note, note_len, "# %s %s addr=%u fc=%u EXC code=%u check=%s",
                     proto, role_s, pdu->addr, pdu->fc, pdu->status, chk);
        (void)n;
        return;
    }

    n = snprintf(note, note_len, "# %s %s addr=%u fc=%u %s",
                 proto, role_s, pdu->addr, pdu->fc, fc_name(pdu->fc));
    if (n <= 0) {
        return;
    }

    switch (pdu->fc) {
    case 0x01: case 0x02: case 0x03: case 0x04:
        if (role == MB_ROLE_REQ) {
            snprintf(note + n, note_len - (size_t)n, " start=%u qty=%u", pdu->start, pdu->qty);
        } else {
            snprintf(note + n, note_len - (size_t)n, " bytes=%u", pdu->bytecount);
        }
        break;
    case 0x05:
        snprintf(note + n, note_len - (size_t)n, " coil=%u value=0x%04X(%s)",
                 pdu->start, pdu->value, pdu->value ? "ON" : "OFF");
        break;
    case 0x06:
        snprintf(note + n, note_len - (size_t)n, " reg=%u value=0x%04X", pdu->start, pdu->value);
        break;
    case 0x07:
        if (role == MB_ROLE_RSP) {
            snprintf(note + n, note_len - (size_t)n, " status=0x%02X", pdu->status);
        }
        break;
    case 0x08:
        snprintf(note + n, note_len - (size_t)n, " sub=%u data=0x%04X", pdu->subfunc, pdu->value);
        break;
    case 0x0B:
        if (role == MB_ROLE_RSP) {
            snprintf(note + n, note_len - (size_t)n, " status=%u events=%u", pdu->status, pdu->value);
        }
        break;
    case 0x0F: case 0x10:
        snprintf(note + n, note_len - (size_t)n, " start=%u qty=%u", pdu->start, pdu->qty);
        break;
    case 0x16:
        snprintf(note + n, note_len - (size_t)n, " reg=%u and=0x%04X or=0x%04X",
                 pdu->start, pdu->value, pdu->qty);
        break;
    default:
        if (pdu->bytecount) {
            snprintf(note + n, note_len - (size_t)n, " bytes=%u", pdu->bytecount);
        }
        break;
    }

    size_t used = strlen(note);
    snprintf(note + used, note_len - used, " check=%s", chk);
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void app_modbus_frame(int port, const uint8_t *frame, int len, bool own_tx,
                      mb_info_t *info, char *note, size_t note_len)
{
    if (note && note_len) {
        note[0] = '\0';
    }
    if (info) {
        memset(info, 0, sizeof(*info));
    }
    if (port < 0 || port >= APP_PORT_COUNT || !frame || len < 4) {
        return;
    }

    bool is_ascii = frame[0] == ':';
    const uint8_t *pdu = NULL;
    int pdu_len = 0;
    bool check_ok = false;

    if (is_ascii) {
        // ':' hex-encoded bytes CRLF ; last encoded byte is LRC.
        int end = len;
        while (end > 0 && (frame[end - 1] == '\r' || frame[end - 1] == '\n')) {
            end--;
        }
        int hex_chars = end - 1;
        if (hex_chars < 4 || (hex_chars % 2) != 0) {
            return;
        }
        static uint8_t dec[256]; // decoded frame incl. addr/pdu/lrc
        int dn = 0;
        bool bad = false;
        for (int i = 1; i + 1 < end; i += 2) {
            int hi = hex_val((char)frame[i]);
            int lo = hex_val((char)frame[i + 1]);
            if (hi < 0 || lo < 0 || dn >= (int)sizeof(dec)) {
                bad = true;
                break;
            }
            dec[dn++] = (uint8_t)((hi << 4) | lo);
        }
        if (bad || dn < 3) {
            return;
        }
        check_ok = mb_lrc(dec, dn - 1) == dec[dn - 1];
        pdu = dec;
        pdu_len = dn - 1;
    } else {
        if (len < 4) {
            return;
        }
        uint16_t calc = mb_crc16(frame, len - 2);
        // RTU transmits CRC low byte first.
        uint16_t recv = (uint16_t)(frame[len - 2] | (frame[len - 1] << 8));
        check_ok = calc == recv;
        pdu = frame;
        pdu_len = len - 2;
    }

    pdu_t pdu_info;
    parse_pdu(pdu, pdu_len, &pdu_info);
    if (pdu_info.cand == CAND_UNKNOWN) {
        return;
    }

    mb_role_t role = resolve_role(port, &pdu_info, own_tx);
    if (info) {
        info->parsed = true;
        info->check_ok = check_ok;
        info->is_ascii = is_ascii;
        info->addr = pdu_info.addr;
        info->fc = pdu_info.fc;
        info->role = role;
    }
    if (note && note_len) {
        make_note(&pdu_info, role, is_ascii, check_ok, note, note_len);
    }
}
