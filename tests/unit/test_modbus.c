// Host-side unit tests for main/app_modbus.c (compiled with native gcc, no
// FreeRTOS/hardware). Covers checksums, PDU parsing, REQ/RSP pairing state,
// exception responses and RTU/ASCII annotation output.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "app_modbus.h"

static int s_checks;
static int s_fails;

#define CHECK(cond, msg) do {                                          \
    s_checks++;                                                        \
    if (!(cond)) {                                                     \
        s_fails++;                                                     \
        printf("FAIL [%s] line %d\n", (msg), __LINE__);                \
    }                                                                  \
} while (0)

static uint16_t crc16(const uint8_t *d, int n)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

static void append_crc(uint8_t *f, int body)
{
    uint16_t c = crc16(f, body);
    f[body] = (uint8_t)c;
    f[body + 1] = (uint8_t)(c >> 8);
}

static void analyze(const uint8_t *f, int n, bool own_tx,
                    mb_info_t *info, char *note)
{
    memset(info, 0, sizeof(*info));
    note[0] = '\0';
    app_modbus_frame(2, f, n, own_tx, info, note, 256);
}

int main(void)
{
    mb_info_t info;
    char note[256];

    // --- known CRC vector (Modbus spec 123456789 -> 0x4B37) ---
    const uint8_t seq[] = {0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39};
    CHECK(crc16(seq, 9) == 0x4B37, "crc16 known vector");

    // --- fc03 request: RX REQ ---
    uint8_t req03[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x14};
    append_crc(req03, 6);
    CHECK(req03[6] == 0x45 && req03[7] == 0xC5, "fc03 crc bytes 4421");
    analyze(req03, 8, false, &info, note);
    CHECK(info.parsed && info.check_ok && info.role == MB_ROLE_REQ, "req03 parsed REQ");
    CHECK(info.addr == 1 && info.fc == 3, "req03 addr/fc");
    CHECK(strstr(note, "MB-RTU REQ addr=1 fc=3 READ_HOLD start=0 qty=20 check=OK"),
          "req03 annotation");

    // --- fc03 response (20 registers, values 0x038A...) ---
    uint8_t rsp03[45];
    rsp03[0] = 1; rsp03[1] = 3; rsp03[2] = 40;
    for (int i = 0; i < 38; i++) {
        static const uint8_t v[2] = {0x03, 0x8A};
        rsp03[3 + i] = v[i & 1];
    }
    rsp03[41] = 0x00; rsp03[42] = 0x00;
    append_crc(rsp03, 43);
    analyze(rsp03, 45, false, &info, note);
    CHECK(info.parsed && info.role == MB_ROLE_RSP && info.check_ok, "rsp03 RSP");
    CHECK(strstr(note, "MB-RTU RSP addr=1 fc=3 READ_HOLD bytes=40 check=OK"),
          "rsp03 annotation");

    // --- own TX request followed by matching response (pairing) ---
    analyze(req03, 8, true, &info, note);
    CHECK(info.role == MB_ROLE_REQ, "own tx marked REQ");
    analyze(rsp03, 45, false, &info, note);
    CHECK(info.role == MB_ROLE_RSP, "rsp after own tx = RSP");

    // --- fc16 write multiple: request + response ---
    uint8_t req16[33] = {0x01, 0x10, 0x00, 0x13, 0x00, 0x0A, 0x14};
    for (int i = 0; i < 20; i++) {
        req16[7 + i] = 0;
    }
    append_crc(req16, 27);
    analyze(req16, 29, false, &info, note);
    CHECK(info.parsed && info.role == MB_ROLE_REQ && info.check_ok, "req16 REQ");
    CHECK(strstr(note, "WRITE_REGS start=19 qty=10"), "req16 annotation");

    uint8_t rsp16[8] = {0x01, 0x10, 0x00, 0x13, 0x00, 0x0A};
    append_crc(rsp16, 6);
    analyze(rsp16, 8, false, &info, note);
    CHECK(info.parsed && info.role == MB_ROLE_RSP && info.check_ok, "rsp16 RSP");

    // --- fc06 ambiguous: first occurrence REQ, matching second RSP ---
    uint8_t req06[8] = {0x01, 0x06, 0x00, 0x01, 0x00, 0x05};
    append_crc(req06, 6);
    analyze(req06, 8, false, &info, note);
    CHECK(info.role == MB_ROLE_REQ, "fc06 first = REQ");
    analyze(req06, 8, false, &info, note);
    CHECK(info.role == MB_ROLE_RSP, "fc06 matching repeat = RSP");
    app_modbus_reset(2);
    analyze(req06, 8, false, &info, note);
    CHECK(info.role == MB_ROLE_REQ, "reset restores REQ classification");

    // --- fc05 first occurrence after reset = REQ ---
    uint8_t req05[8] = {0x01, 0x05, 0x00, 0x00, 0xFF, 0x00};
    append_crc(req05, 6);
    analyze(req05, 8, false, &info, note);
    CHECK(info.parsed && info.role == MB_ROLE_REQ && info.check_ok, "fc05 REQ");
    CHECK(strstr(note, "WRITE_COIL coil=0 value=0xFF00(ON)"), "fc05 annotation");

    // --- exception response (fc03 + 0x80, code 2 illegal address) ---
    uint8_t exc[5] = {0x01, 0x83, 0x02};
    append_crc(exc, 3);
    analyze(exc, 5, false, &info, note);
    CHECK(info.parsed && info.role == MB_ROLE_RSP, "exception RSP");
    CHECK(strstr(note, "EXC code=2"), "exception annotation");

    // --- structurally valid request with corrupted CRC: BAD check ---
    uint8_t bad[8];
    memcpy(bad, req03, 8);
    bad[7] ^= 0xFF;
    analyze(bad, 8, false, &info, note);
    CHECK(info.parsed && !info.check_ok, "bad crc parsed check=BAD");
    CHECK(strstr(note, "check=BAD"), "bad crc annotation");

    // --- non-Modbus garbage with unknown function code: not parsed ---
    // 6-byte PDU so it neither matches exception response (len 3) nor any fc.
    uint8_t junk[8] = {0xAA, 0x77, 0xCC, 0xDD, 0xEE, 0xFF};
    analyze(junk, 6, false, &info, note);
    CHECK(!info.parsed && note[0] == '\0', "garbage not parsed");

    // --- ASCII RTU request with LRC ---
    // :01030000000AF2  (addr1 fc3 start0 qty10, LRC F2)
    const char *ascii = ":01030000000AF2\r\n";
    analyze((const uint8_t *)ascii, (int)strlen(ascii), false, &info, note);
    CHECK(info.parsed && info.is_ascii && info.check_ok && info.role == MB_ROLE_REQ,
          "ascii request parsed");
    CHECK(strstr(note, "MB-ASCII REQ addr=1 fc=3 READ_HOLD start=0 qty=10 check=OK"),
          "ascii annotation");

    // --- malformed ASCII (odd hex chars) not parsed ---
    const char *ascii_bad = ":0103000000A\r\n";
    analyze((const uint8_t *)ascii_bad, (int)strlen(ascii_bad), false, &info, note);
    CHECK(!info.parsed, "malformed ascii rejected");

    // --- fc01 read coils request ---
    uint8_t req01[8] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x08};
    append_crc(req01, 6);
    analyze(req01, 8, false, &info, note);
    CHECK(info.parsed && info.role == MB_ROLE_REQ && info.check_ok, "fc01 REQ");
    CHECK(strstr(note, "READ_COIL start=0 qty=8"), "fc01 annotation");

    // --- fc17 read/write multiple request ---
    // addr fc rdstart(2) rdqty(2) wrstart(2) wrqty(2) bc wrdata
    uint8_t req17[20] = {0x01, 0x17, 0x00, 0x00, 0x00, 0x01,
                         0x00, 0x10, 0x00, 0x01, 0x02, 0x12, 0x34};
    append_crc(req17, 13);
    analyze(req17, 15, false, &info, note);
    CHECK(info.parsed && info.role == MB_ROLE_REQ && info.check_ok, "fc17 REQ");
    CHECK(strstr(note, "RD_WR_MULTI"), "fc17 annotation");

    // --- app_modbus_scan: grammar-based burst splitting ---
    int lens[8];
    uint8_t glued[53];
    memcpy(glued, req03, 8);
    memcpy(glued + 8, rsp03, 45);
    int nf = app_modbus_scan(glued, 53, lens, 8);
    CHECK(nf == 2 && lens[0] == 8 && lens[1] == 53, "scan splits glued req+rsp");

    uint8_t two[16];
    memcpy(two, req03, 8);
    memcpy(two + 8, req03, 8);
    CHECK(app_modbus_scan(two, 16, lens, 8) == 2, "scan two requests");
    CHECK(app_modbus_scan(req03, 8, lens, 8) == 1 && lens[0] == 8, "scan single frame");

    uint8_t g2[6] = {0xAA, 0x77, 0xCC, 0xDD, 0xEE, 0xFF};
    CHECK(app_modbus_scan(g2, 6, lens, 8) == 0, "scan garbage returns 0");

    uint8_t g16[37];
    memcpy(g16, req16, 29);
    memcpy(g16 + 29, rsp16, 8);
    nf = app_modbus_scan(g16, 37, lens, 8);
    CHECK(nf == 2 && lens[0] == 29 && lens[1] == 37, "scan splits fc16 req+rsp");

    // glued frame with an unparseable tail: only valid prefix is reported
    uint8_t gt[56];
    memcpy(gt, glued, 53);
    memcpy(gt + 53, g2, 3);
    nf = app_modbus_scan(gt, 56, lens, 8);
    CHECK(nf == 2 && lens[1] == 53, "scan reports valid prefix before tail");

    // --- app_modbus_prefix: incomplete frame candidate detection ---
    uint8_t partial[10] = {0x01, 0x03, 0x28, 0x03, 0x8A, 0x03,
                           0x8A, 0x03, 0x8A, 0x03};
    CHECK(app_modbus_prefix(partial, 10), "partial fc03 response = prefix");
    CHECK(!app_modbus_prefix(req03, 8), "complete frame != prefix");
    CHECK(!app_modbus_prefix(g2, 6), "garbage != prefix");
    uint8_t hdr2[2] = {0x01, 0x10};
    CHECK(app_modbus_prefix(hdr2, 2), "truncated fc16 header = prefix");
    uint8_t p16[8] = {0x01, 0x10, 0x00, 0x13, 0x00, 0x0A, 0x14, 0x00};
    CHECK(app_modbus_prefix(p16, 8), "partial fc16 request = prefix");

    printf("\nunit tests: %d checks, %d failures\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
