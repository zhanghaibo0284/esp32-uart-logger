#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Serial log protocol parsers. A port with parsing disabled writes only the raw
// hexadecimal frame; enabled ports get one extra structured annotation line.
typedef enum {
    APP_PARSE_OFF = 0,        // no annotation (raw hex only)
    APP_PARSE_AUTO = 1,       // try Modbus RTU/ASCII, DL/T645, CJ/T188, ASCII
    APP_PARSE_MODBUS_RTU = 2,
    APP_PARSE_MODBUS_ASCII = 3,
    APP_PARSE_DLT645 = 4,     // DL/T 645-2007 multi-function meter
    APP_PARSE_CJT188 = 5,     // CJ/T 188 water/gas/heat meter
    APP_PARSE_ASCII_TEXT = 6, // printable ASCII / AT text
    APP_PARSE_PROTO_MAX
} app_parse_proto_t;

// Frame direction inferred by the parser (drives the TX/RX marker).
typedef enum {
    AP_KIND_NONE = 0,
    AP_KIND_REQ,
    AP_KIND_RSP,
    AP_KIND_DATA,
} ap_kind_t;

const char *app_parse_proto_name(int proto);

// Resolve a textual protocol name (config.json / web API). Returns false for
// unknown names.
bool app_parse_proto_from_name(const char *name, uint8_t *out);

// Analyse one complete frame. `port` is only used to pair Modbus requests and
// responses. Returns true when the frame matched the selected protocol.
bool app_parse_frame(int proto, int port, const uint8_t *data, int len, bool own_tx,
                     ap_kind_t *kind, char *note, size_t note_len);
