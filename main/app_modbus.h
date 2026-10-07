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
