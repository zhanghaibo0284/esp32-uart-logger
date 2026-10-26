// Host-side unit tests for main/app_modbus_codec.c and main/app_parse.c.
// Compiled with native gcc (no FreeRTOS/hardware), same harness as test_modbus.c.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "app_modbus_codec.h"
#include "app_parse.h"

static int s_checks;
static int s_fails;

#define CHECK(cond, msg) do {                                          \
    s_checks++;                                                        \
    if (!(cond)) {                                                     \
        s_fails++;                                                     \
        printf("FAIL [%s] line %d\n", (msg), __LINE__);                \
    }                                                                  \
} while (0)

static uint8_t g_coils[64];
static uint8_t g_discrete[64];
static uint16_t g_holding[64];
static uint16_t g_input[64];

static mb_map_t make_map(void)
{
    mb_map_t m;
    memset(g_coils, 0, sizeof(g_coils));
    memset(g_discrete, 0, sizeof(g_discrete));
    memset(g_holding, 0, sizeof(g_holding));
    memset(g_input, 0, sizeof(g_input));
    m.coils = g_coils;
    m.coil_start = 0;
    m.coil_count = 32;
    m.discrete = g_discrete;
    m.disc_start = 0;
    m.disc_count = 32;
    m.holding = g_holding;
    m.hold_start = 100;
    m.hold_count = 32;
    m.input = g_input;
    m.input_start = 0;
    m.input_count = 32;
    return m;
}

static void put_crc(uint8_t *f, int body)
{
    uint16_t c = app_mb_crc16(f, body);
    f[body] = (uint8_t)c;
    f[body + 1] = (uint8_t)(c >> 8);
}

static int parse_into(int proto, const uint8_t *d, int n, ap_kind_t *kind, char *note)
{
    memset(note, 0, 256);
    return app_parse_frame(proto, 2, d, n, false, kind, note, 256) ? 1 : 0;
}

int main(void)
{
    // --- CRC vectors ---
    const uint8_t seq[] = {0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39};
    CHECK(app_mb_crc16(seq, 9) == 0x4B37, "crc16 known vector");

    uint8_t req03[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02};
    put_crc(req03, 6);
    CHECK(app_mb_crc_ok(req03, 8), "crc_ok accepts valid frame");
    uint8_t broken[8];
    memcpy(broken, req03, 8);
    broken[7] ^= 0xFF;
    CHECK(!app_mb_crc_ok(broken, 8), "crc_ok rejects corrupt frame");

    // --- fc03 read holding registers ---
    mb_map_t map = make_map();
    g_holding[0] = 0x1234;
    g_holding[1] = 0xABCD;
    uint8_t req[16];
    uint8_t rsp[300];
    int n = app_mb_master_request(1, 3, 100, 2, NULL, 0, req, sizeof(req));
    CHECK(n == 8, "master fc03 request length");
    CHECK(app_mb_crc_ok(req, n), "master request crc");
    int rn = app_mb_slave_respond(req, n, 1, &map, rsp, sizeof(rsp));
    CHECK(rn == 9, "fc03 response length");
    mb_rsp_t r;
    CHECK(app_mb_parse_response(rsp, rn, &r), "fc03 response parses");
    CHECK(!r.exception && r.count == 2, "fc03 response count");
    CHECK(r.values[0] == 0x1234 && r.values[1] == 0xABCD, "fc03 response values");

    // --- out-of-range address -> exception 02 ---
    n = app_mb_master_request(1, 3, 500, 1, NULL, 0, req, sizeof(req));
    rn = app_mb_slave_respond(req, n, 1, &map, rsp, sizeof(rsp));
    CHECK(rn == 5 && app_mb_parse_response(rsp, rn, &r) && r.exception && r.exc_code == 2,
          "fc03 out of range exception 02");

    // --- qty overflow inside range -> exception 02 ---
    n = app_mb_master_request(1, 4, 30, 4, NULL, 0, req, sizeof(req));
    rn = app_mb_slave_respond(req, n, 1, &map, rsp, sizeof(rsp));
    CHECK(rn == 5 && app_mb_parse_response(rsp, rn, &r) && r.exc_code == 2,
          "fc04 range overflow exception 02");

    // --- fc01 coils bit packing ---
    g_coils[0] = 1;
    g_coils[3] = 1;
    n = app_mb_master_request(1, 1, 0, 8, NULL, 0, req, sizeof(req));
    rn = app_mb_slave_respond(req, n, 1, &map, rsp, sizeof(rsp));
    CHECK(rn == 6, "fc01 response length");
    CHECK(app_mb_parse_response(rsp, rn, &r) && r.count == 1 && r.values[0] == 0x09,
          "fc01 packed bits 0b1001");

    // --- fc05 write single coil ---
    uint16_t one = 1;
    n = app_mb_master_request(1, 5, 5, 1, &one, 1, req, sizeof(req));
    rn = app_mb_slave_respond(req, n, 1, &map, rsp, sizeof(rsp));
    CHECK(rn == 8 && g_coils[5] == 1, "fc05 sets coil");
    one = 0;
    n = app_mb_master_request(1, 5, 5, 1, &one, 1, req, sizeof(req));
    rn = app_mb_slave_respond(req, n, 1, &map, rsp, sizeof(rsp));
    CHECK(rn == 8 && g_coils[5] == 0, "fc05 clears coil");

    // --- fc06 write single register + echo ---
    uint16_t v = 0x55AA;
    n = app_mb_master_request(1, 6, 101, 1, &v, 1, req, sizeof(req));
    rn = app_mb_slave_respond(req, n, 1, &map, rsp, sizeof(rsp));
    CHECK(rn == 8 && g_holding[1] == 0x55AA, "fc06 writes register");
    CHECK(app_mb_parse_response(rsp, rn, &r) && r.values[0] == 0x55AA, "fc06 echo value");

    // --- fc0F write multiple coils ---
    uint8_t req0f[16] = {0x01, 0x0F, 0x00, 0x08, 0x00, 0x03, 0x01, 0x05};
    put_crc(req0f, 8);
    rn = app_mb_slave_respond(req0f, 10, 1, &map, rsp, sizeof(rsp));
    CHECK(rn == 8 && g_coils[8] == 1 && g_coils[9] == 0 && g_coils[10] == 1,
          "fc0F writes coil pattern");

    // --- fc10 write multiple registers ---
    uint16_t vals[2] = {0x0102, 0x0304};
    n = app_mb_master_request(1, 0x10, 102, 2, vals, 2, req, sizeof(req));
    CHECK(n == 13, "fc10 request length");
    rn = app_mb_slave_respond(req, n, 1, &map, rsp, sizeof(rsp));
    CHECK(rn == 8 && g_holding[2] == 0x0102 && g_holding[3] == 0x0304,
          "fc10 writes two registers");

    // --- frame for another station is ignored ---
    n = app_mb_master_request(9, 3, 100, 1, NULL, 0, req, sizeof(req));
    CHECK(app_mb_slave_respond(req, n, 1, &map, rsp, sizeof(rsp)) == 0,
          "other station ignored");

    // --- corrupt CRC is ignored ---
    memcpy(broken, req03, 8);
    broken[6] ^= 0x55;
    CHECK(app_mb_slave_respond(broken, 8, 1, &map, rsp, sizeof(rsp)) == 0,
          "bad crc ignored");

    // --- broadcast write is applied but not answered ---
    v = 0x2222;
    n = app_mb_master_request(0, 6, 100, 1, &v, 1, req, sizeof(req));
    CHECK(n == 0 || (n == 8 && req[0] == 0), "broadcast request build");
    // master_request rejects slave 0, so craft the frame directly
    uint8_t bcast[8] = {0x00, 0x06, 0x00, 0x64, 0x22, 0x22};
    put_crc(bcast, 6);
    CHECK(app_mb_slave_respond(bcast, 8, 1, &map, rsp, sizeof(rsp)) == 0,
          "broadcast not answered");
    CHECK(g_holding[0] == 0x2222, "broadcast write applied");

    // --- illegal function code -> exception 01 ---
    uint8_t ill[8] = {0x01, 0x07, 0x00, 0x00, 0x00, 0x01};
    put_crc(ill, 6);
    rn = app_mb_slave_respond(ill, 8, 1, &map, rsp, sizeof(rsp));
    CHECK(rn == 5 && app_mb_parse_response(rsp, rn, &r) && r.exc_code == 1,
          "illegal fc exception 01");

    // --- exception response decode ---
    uint8_t exc[5] = {0x01, 0x83, 0x03};
    put_crc(exc, 3);
    CHECK(app_mb_parse_response(exc, 5, &r) && r.exception && r.exc_code == 3,
          "exception response decode");

    // --- protocol name mapping ---
    uint8_t proto = 0xFF;
    CHECK(app_parse_proto_from_name("modbus_rtu", &proto) && proto == APP_PARSE_MODBUS_RTU,
          "proto name lookup");
    CHECK(!app_parse_proto_from_name("nope", &proto), "unknown proto rejected");
    CHECK(strcmp(app_parse_proto_name(APP_PARSE_DLT645), "dlt645") == 0,
          "proto name output");
    CHECK(strcmp(app_parse_proto_name(APP_PARSE_PROTO_MAX), "off") == 0,
          "proto name clamped");

    // --- parser: Modbus RTU annotation on a valid request ---
    ap_kind_t kind = AP_KIND_NONE;
    char note[256];
    CHECK(parse_into(APP_PARSE_MODBUS_RTU, req03, 8, &kind, note), "modbus_rtu parsed");
    CHECK(kind == AP_KIND_REQ && strstr(note, "MB-RTU REQ"), "modbus_rtu REQ annotation");

    // corrupt CRC still annotated with check=BAD (raw logging mode)
    broken[7] ^= 0xFF;
    CHECK(parse_into(APP_PARSE_MODBUS_RTU, broken, 8, &kind, note) &&
          strstr(note, "check=BAD"), "modbus_rtu bad crc note");
    CHECK(!parse_into(APP_PARSE_MODBUS_ASCII, req03, 8, NULL, note),
          "modbus_ascii rejects binary frame");

    const char *ascii_req = ":01030000000AF2\r\n";
    CHECK(parse_into(APP_PARSE_MODBUS_ASCII, (const uint8_t *)ascii_req,
                     (int)strlen(ascii_req), &kind, note) &&
          strstr(note, "MB-ASCII"), "modbus_ascii annotation");

    // --- parser: DL/T 645-2007 voltage frame (AUTO) ---
    uint8_t dlt[18];
    memset(dlt, 0, sizeof(dlt));
    dlt[0] = 0x68;
    const uint8_t addr_le[6] = {0x12, 0x90, 0x78, 0x56, 0x34, 0x12};
    for (int i = 0; i < 6; i++) {
        dlt[1 + i] = (uint8_t)(addr_le[i] + 0x33);
    }
    dlt[7] = 0x68;
    dlt[8] = 0x91;          // response, read data
    dlt[9] = 6;             // DI(4) + data(2)
    dlt[10] = 0x33;         // DI 0x02010100 low byte -> drift after -0x33
    dlt[11] = 0x34;
    dlt[12] = 0x34;
    dlt[13] = 0x35;
    dlt[14] = 0x89;         // value BCD 0x34 0x56 -> 3456 -> 345.6
    dlt[15] = 0x67;
    uint8_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum = (uint8_t)(sum + dlt[i]);
    }
    dlt[16] = sum;
    dlt[17] = 0x16;
    kind = AP_KIND_NONE;
    CHECK(parse_into(APP_PARSE_AUTO, dlt, 18, &kind, note), "auto detects DLT645");
    CHECK(kind == AP_KIND_RSP && strstr(note, "# DLT645 RSP") &&
          strstr(note, "DI=02010100") && strstr(note, "voltage_a=345.6"),
          "DLT645 annotation");
    CHECK(strstr(note, "check=OK"), "DLT645 checksum OK");

    // corrupted checksum must not be detected as DLT645 under AUTO
    dlt[16] ^= 0x01;
    CHECK(!parse_into(APP_PARSE_AUTO, dlt, 18, NULL, note), "auto rejects bad DLT645 sum");
    // explicit protocol still annotates with check=BAD
    CHECK(parse_into(APP_PARSE_DLT645, dlt, 18, &kind, note) && strstr(note, "check=BAD"),
          "dlt645 explicit bad checksum");
    dlt[16] ^= 0x01;

    // --- parser: CJ/T 188 frame (AUTO) ---
    uint8_t cjt[16];
    memset(cjt, 0, sizeof(cjt));
    cjt[0] = 0x68;
    cjt[1] = 0x12;
    cjt[2] = 0x34;
    cjt[3] = 0x68;
    cjt[4] = 0x01;          // request, read
    for (int i = 0; i < 7; i++) {
        cjt[5 + i] = 0x33;  // meter address digits 0
    }
    cjt[12] = 0x90;         // data
    cjt[13] = 0x01;
    sum = 0;
    for (int i = 0; i < 14; i++) {
        sum = (uint8_t)(sum + cjt[i]);
    }
    cjt[14] = sum;
    cjt[15] = 0x16;
    kind = AP_KIND_NONE;
    CHECK(parse_into(APP_PARSE_AUTO, cjt, 16, &kind, note), "auto detects CJT188");
    CHECK(kind == AP_KIND_REQ && strstr(note, "# CJT188 REQ"), "CJT188 annotation");

    // --- parser: printable ASCII ---
    const char *text = "AT+GMR\r\n";
    kind = AP_KIND_NONE;
    CHECK(parse_into(APP_PARSE_AUTO, (const uint8_t *)text, (int)strlen(text),
                     &kind, note), "auto detects ASCII text");
    CHECK(kind == AP_KIND_DATA && strstr(note, "# ASCII \"AT+GMR\""), "ASCII annotation");
    CHECK(parse_into(APP_PARSE_ASCII_TEXT, (const uint8_t *)text, (int)strlen(text),
                     &kind, note), "explicit ascii proto");

    // binary garbage must not parse
    uint8_t junk[6] = {0xAA, 0x77, 0xCC, 0xDD, 0xEE, 0xFF};
    CHECK(!parse_into(APP_PARSE_AUTO, junk, 6, NULL, note), "garbage rejected by auto");
    CHECK(!parse_into(APP_PARSE_OFF, req03, 8, NULL, note), "off never parses");

    printf("\nunit tests: %d checks, %d failures\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
