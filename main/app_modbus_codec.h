#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MB_MAP_MAX 256

// Register/coil address ranges exposed by the simulated slave. Pointers are
// arrays indexed from *_start (i.e. item at protocol address A lives at
// array[A - start]).
typedef struct {
    uint8_t *coils;
    uint16_t coil_start;
    uint16_t coil_count;
    uint8_t *discrete;
    uint16_t disc_start;
    uint16_t disc_count;
    uint16_t *holding;
    uint16_t hold_start;
    uint16_t hold_count;
    uint16_t *input;
    uint16_t input_start;
    uint16_t input_count;
} mb_map_t;

uint16_t app_mb_crc16(const uint8_t *data, int len);
bool app_mb_crc_ok(const uint8_t *frame, int len);

// Build one Modbus RTU response for a request addressed to `station`.
// Returns the response length, or 0 when no response must be sent (other
// station, bad CRC, broadcast read). Handles fc 01/02/03/04/05/06/0F/10 and
// answers illegal function/address/value with the standard exceptions.
int app_mb_slave_respond(const uint8_t *req, int req_len, uint8_t station,
                         const mb_map_t *map, uint8_t *rsp, int rsp_cap);

// Build a master request. fc 01-04 use start/qty; 05/06 use start + values[0];
// 10 writes qty registers taken from values[].
int app_mb_master_request(uint8_t slave, uint8_t fc, uint16_t start, uint16_t qty,
                          const uint16_t *values, int nvalues,
                          uint8_t *out, int cap);

typedef struct {
    bool crc_ok;
    bool exception;
    uint8_t exc_code;
    int count;              // decoded 16-bit values / packed bytes
    uint16_t values[125];
} mb_rsp_t;

// Decode a complete response frame (used by the simulated master for status).
bool app_mb_parse_response(const uint8_t *frame, int len, mb_rsp_t *out);
