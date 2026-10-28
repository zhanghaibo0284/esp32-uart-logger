#include "app_modbus.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------

/* Modbus RTU CRC16（多项式 0xA001 反射算法，初值 0xFFFF）。
 * 参数：data/len 参与计算的字节（不含 CRC 本身）。
 * 返回 CRC 值；帧内发送时低字节在前、高字节在后。
 */
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

/* Modbus ASCII 的 LRC 校验：所有字节求和后取二进制补码。
 * 返回单字节校验值，帧中以两个十六进制字符（大写）表示。
 */
static uint8_t mb_lrc(const uint8_t *data, int len)
{
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint8_t)(-(int8_t)sum);
}

/* 单个十六进制字符转数值：0-9/A-F/a-f 有效，其它返回 -1（用于 ASCII 帧解码校验）。 */
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

/* 大端读取 16 位（Modbus 协议所有寄存器/地址字段均为高字节在前）。 */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* 解析一帧的 PDU 结构（输入为去掉校验后的 [addr][fc][data...]）。
 * 参数：pdu/len 原始帧；out 输出结构（内部先清零）。
 * 实现思路：不同功能码的“合法长度”是固定或可推导的，因此按长度判定
 *   是请求还是响应，无法区分时标记 CAND_AMB（由历史配对状态决定）。
 * 返回：无；无法识别时 out->cand = CAND_UNKNOWN。
 * 覆盖功能码：01-08、0B、0C、0F、10、14、15、16、17 及异常响应。
 */
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

/* 清空某通道的“请求-响应对”状态（串口重载/异常重启时调用）。
 * 参数 port 越界时忽略。
 */
void app_modbus_reset(int port)
{
    if (port >= 0 && port < APP_PORT_COUNT) {
        s_pending[port].pending = false;
    }
}

/* 判定一帧的角色（请求 REQ / 响应 RSP），并维护每通道的配对状态。
 * 参数：port 通道；pdu 已解析的帧；own_tx true 表示本机发出（必然为请求）。
 * 返回：角色枚举。
 * 判定规则：
 *   - 本机发送：非异常帧一律视为 REQ，并记住 addr/fc 供后续响应配对；
 *   - 结构明确为响应（长度/字节数自洽）→ RSP，清除 pending；
 *   - 结构明确为请求 → REQ，记录 pending；
 *   - 二义性帧（如 05/06/0F/10 的回显）：若与 pending 的 addr/fc 相同，
 *     视为响应，否则视为新的请求。
 */
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

/* 功能码 → 可读名称（写入日志注释，便于人工阅读）。未收录的返回 "UNKNOWN"。 */
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

/* 生成日志行末尾的 Modbus 注释，例如：
 *   "# MB-RTU RSP addr=1 fc=3 READ_HOLD bytes=8 check=OK"
 * 参数：
 *   pdu       已解析帧；role 角色；is_ascii 是否 ASCII 帧；check_ok 校验是否通过；
 *   note/note_len 输出缓冲（建议 >=96 字节，超长自动截断）。
 * 实现：先写公共前缀（协议/角色/地址/功能码/名称），再按功能码追加
 *       地址、数量、值、异常码等关键字段，最后附加校验结论。
 * 边界：snprintf 的返回值不用于累加，统一用 strlen 求已用长度，避免截断错位。
 */
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
// Raw burst scanning: CRC-valid frame boundaries via sequential grammar
// ---------------------------------------------------------------------------

/* 列出从当前位置起始时，符合语法的候选帧总长度（含 2 字节 CRC）。
 * 参数：p 帧起点；remaining 剩余字节；out[2] 输出候选长度（按优先级排序）。
 * 返回候选数量（0..2）。
 * 说明：同一功能码往往存在“请求固定长、响应可变长”两种可能，
 *       因此这里给出候选，真正的接受与否由调用方用 CRC 校验裁决。
 *       例如 0x03：先试 8 字节请求，再试 5+bytecount 的响应。
 */
static int candidates_at(const uint8_t *p, int remaining, int out[2])
{
    if (remaining < 4) {
        return 0;
    }
    uint8_t fc = p[1];
    int n = 0;

    if (fc & 0x80) {                          // exception response: addr fc|80 code CRC
        if (remaining >= 5) {
            out[n++] = 5;
        }
        return n;
    }
    switch (fc) {
    case 0x01: case 0x02: case 0x03: case 0x04:
        // Prefer request (fixed 8B) when it fits; the variable response is
        // also possible when p[2] is a plausible byte count.
        if (remaining >= 8) {
            out[n++] = 8;
        }
        if (remaining > 4 && (int)p[2] + 5 <= remaining && p[2] > 0) {
            int L = p[2] + 5;
            if (L != 8 && (fc < 3 || p[2] % 2 == 0)) {
                out[n++] = L;
            }
        }
        break;
    case 0x05: case 0x06:
        if (remaining >= 8) {
            out[n++] = 8;
        }
        break;
    case 0x0F: case 0x10:
        // Prefer the write response echo (8B), then the write request.
        if (remaining >= 8) {
            out[n++] = 8;
        }
        if (remaining > 8 && (int)p[6] + 9 <= remaining) {
            int L = p[6] + 9;
            if (L != 8) {
                out[n++] = L;
            }
        }
        break;
    case 0x17:
        if (remaining > 4 && (int)p[2] + 5 <= remaining) {
            out[n++] = p[2] + 5;
        }
        if (remaining > 12 && (int)p[10] + 13 <= remaining) {
            int L = p[10] + 13;
            bool dup = false;
            for (int k = 0; k < n; k++) {
                dup = dup || out[k] == L;
            }
            if (!dup) {
                out[n++] = L;
            }
        }
        break;
    default:
        break;
    }
    return n;
}

/* 扫描一段原始字节，按“语法候选 + CRC 校验”顺序找出所有完整帧边界。
 * 参数：
 *   data/len 输入字节；lens 输出每个帧的结束偏移（绝对，相对 data）；
 *   maxn    lens 容量上限。
 * 返回识别出的帧数。
 * 关键步骤：
 *   1. 从 pos 开始取候选长度；
 *   2. 依次试每个候选的 CRC（低字节在前），第一个通过即为该帧长度；
 *   3. 全部候选都不过则停止扫描（遇到非 Modbus 数据或半个帧），
 *      由调用方决定继续等待或按原始 HEX 记录。
 * 异常处理：不会越界读——候选长度均先检查 remaining。
 */
int app_modbus_scan(const uint8_t *data, int len, int *lens, int maxn)
{
    int count = 0;
    int pos = 0;
    while (pos < len && count < maxn) {
        int cand[2];
        int n = candidates_at(data + pos, len - pos, cand);
        int accepted = 0;
        for (int k = 0; k < n; k++) {
            int L = cand[k];
            if (L >= 4 && mb_crc16(data + pos, L - 2) ==
                          (uint16_t)(data[pos + L - 2] | (data[pos + L - 1] << 8))) {
                accepted = L;
                break;
            }
        }
        if (!accepted) {
            break;
        }
        lens[count++] = pos + accepted;
        pos += accepted;
    }
    return count;
}

/* 判断当前缓冲是否可能是“某个 Modbus 帧的前缀”（即还需要更多字节）。
 * 参数：data/len 当前缓冲（至少 2 字节才有意义）。
 * 返回 true=头部字段声明了更长的帧，应继续等待；false=不可能是前缀。
 * 用途：帧间隙到来时，若仍是前缀则继续组装（避免把长响应拆成两帧）；
 *       否则按原始数据输出。头部信息不足时（如 0x10 只有 6 字节）也返回
 *       true 等待，避免过早判死。
 */
bool app_modbus_prefix(const uint8_t *data, int len)
{
    if (len < 2) {
        return false;
    }
    uint8_t fc = data[1];
    int total = 0;

    if (fc & 0x80) {
        total = 5;
    } else {
        switch (fc) {
        case 0x01: case 0x02: case 0x03: case 0x04:
            // Fixed request (8B) or variable response (5+bc); prefix only when
            // the response variant declares a frame longer than `len`.
            if (len >= 3 && data[2] > 0 && 5 + data[2] > len) {
                total = 5 + data[2];
            }
            break;
        case 0x05: case 0x06:
            total = 8;
            break;
        case 0x0F: case 0x10:
            if (len >= 7) {
                total = 9 + data[6];
            } else {
                total = 9;        // header incomplete; wait
            }
            break;
        case 0x17:
            if (len >= 11) {
                total = 13 + data[10];
            } else {
                total = 13;
            }
            break;
        default:
            return false;
        }
    }
    return total > len;
}

/* 分析一帧并产出结构化信息与日志注释（logger 每帧调用一次）。
 * 参数：
 *   port    通道索引（用于查请求/响应配对状态）；
 *   frame/len 完整帧（RTU 含 2 字节 CRC；ASCII 以 ':' 开头、CRLF 结尾）；
 *   own_tx  是否本机发送；
 *   info    输出：parsed/check_ok/is_ascii/addr/fc/role；
 *   note/note_len 输出注释文本（可为 NULL，表示不需要注释）。
 * 返回：无。帧过短、校验结构非法或功能码无法识别时，info->parsed 保持 false
 *       且 comment 为空，调用方据此只记录原始 HEX（不做协议臆测）。
 * 关键步骤：
 *   1. ASCII 帧先做十六进制解码并校验 LRC；RTU 直接校验 CRC（低字节在前）；
 *   2. parse_pdu 解析字段并判定候选类型；
 *   3. resolve_role 结合历史配对确定 REQ/RSP；
 *   4. make_note 生成人类可读注释。
 */
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
