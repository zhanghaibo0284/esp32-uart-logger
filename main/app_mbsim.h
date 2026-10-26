#pragma once

#include <stdbool.h>
#include <stdint.h>

// Modbus RTU simulator. Both roles share the logger's UART ports instead of
// opening the hardware themselves: every RX burst is passed to app_mbsim_rx()
// and every outbound frame goes through app_logger_request_tx(), so the logger
// keeps owning the UART and still records TX frames.
#define MBS_RULE_MAX 8
#define MBS_VALUE_MAX 8

typedef struct {
    uint8_t enabled;
    uint8_t port;        // logger channel the slave listens on
    uint8_t station;     // 1..247
    uint16_t coil_start;
    uint16_t coil_count;
    uint16_t disc_start;
    uint16_t disc_count;
    uint16_t hold_start;
    uint16_t hold_count;
    uint16_t input_start;
    uint16_t input_count;
} mbs_slave_cfg_t;

typedef struct {
    uint8_t enabled;
    uint8_t port;
    uint8_t slave;
    uint8_t fc;              // 1..6, 15 (0x0F), 16 (0x10)
    uint16_t start;
    uint16_t qty;            // registers/coils per request
    uint16_t values[MBS_VALUE_MAX]; // payload for fc 05/06/10
    uint8_t nvalues;
    uint32_t period_ms;      // 0 = single shot only
    uint32_t repeat;         // periodic requests to send, 0 = unlimited
} mbs_rule_cfg_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    mbs_slave_cfg_t slave;
    uint8_t rule_count;
    mbs_rule_cfg_t rules[MBS_RULE_MAX];
} mbs_cfg_t;

typedef struct {
    uint32_t requests;
    uint32_t responses;
    uint32_t timeouts;
    uint32_t errors;
    int8_t last_status;      // 0 ok, -1 timeout, -2 exception/bad crc, -3 no port
    uint8_t last_count;
    uint16_t last_values[MBS_VALUE_MAX];
    uint32_t slave_requests;
    uint32_t slave_responses;
} mbs_stats_t;

void app_mbsim_start(void);
void app_mbsim_get_cfg(mbs_cfg_t *out);
void app_mbsim_get_stats(mbs_stats_t *out);
void app_mbsim_get_rule_stats(int rule, mbs_stats_t *out);

// Apply a full configuration; invalid entries are rejected as a whole.
int app_mbsim_set_cfg(const mbs_cfg_t *cfg);
int app_mbsim_set_rule(int idx, const mbs_rule_cfg_t *rule);
int app_mbsim_del_rule(int idx);
void app_mbsim_trigger(int idx);   // 0..MBS_RULE_MAX-1, -1 = all enabled rules

// Simulated register/coil access: type is one of "coil", "disc", "hold", "input".
bool app_mbsim_value_set(const char *type, uint16_t addr, uint16_t value);
bool app_mbsim_value_get(const char *type, uint16_t addr, uint16_t *value);

// RX hook for the logger task (raw burst, any length).
void app_mbsim_rx(int port, const uint8_t *data, int len);
