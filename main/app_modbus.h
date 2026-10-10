#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "board.h"

typedef enum {
    MB_ROLE_UNKNOWN = 0,
    MB_ROLE_REQ,
    MB_ROLE_RSP,
} mb_role_t;

typedef struct {
    bool parsed;      // structure recognized as a Modbus frame
    bool check_ok;    // RTU CRC / ASCII LRC valid
    bool is_ascii;
    uint8_t addr;
    uint8_t fc;
    mb_role_t role;
} mb_info_t;

// Analyse one completed frame. own_tx=true when this device transmitted it.
// Writes an ASCII annotation (no trailing newline) into note.
void app_modbus_frame(int port, const uint8_t *frame, int len, bool own_tx,
                      mb_info_t *info, char *note, size_t note_len);

// Clear the per-port request/response pairing state.
void app_modbus_reset(int port);

// Walk a raw byte burst with Modbus grammar and find complete checksum-valid
// frame boundaries (no pairing state change). lens[] receives cumulative end
// offsets (must be strictly increasing). Returns the number of frames found,
// 0 when no valid frame starts at position 0. Only CRC/LRC-valid boundaries
// are accepted, so callers never split on protocol guesswork.
int app_modbus_scan(const uint8_t *data, int len, int *lens, int maxn);

// True when `data` starts with a plausible Modbus frame whose declared total
// length extends beyond the bytes currently present (incomplete candidate).
// Lets callers wait for more bytes rather than flush a partial frame.
bool app_modbus_prefix(const uint8_t *data, int len);
