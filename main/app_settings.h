#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "driver/uart.h"
#include "board.h"
#include "app_parse.h"

typedef struct {
    uint32_t baud;
    uart_word_length_t data_bits;
    uart_stop_bits_t stop_bits;
    uart_parity_t parity;
    bool enabled;
    uint8_t parse_proto;   // app_parse_proto_t, OFF = raw hex only
} port_setting_t;

typedef struct {
    uint16_t segment_min;
    bool screen_auto_off;  // auto turn screen off after inactivity
    uint8_t disk_limit_pct; // clean old logs when SD usage exceeds this (1-99)
    port_setting_t port[APP_PORT_COUNT];
} app_settings_t;

void app_settings_init(void);
void app_settings_get(app_settings_t *out);
void app_settings_save(const app_settings_t *in);
void app_settings_format_port(const port_setting_t *port, char *out, size_t out_len);
