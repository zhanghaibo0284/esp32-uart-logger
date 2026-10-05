#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "board.h"

typedef struct {
    bool sd_mounted;
    uint32_t sd_total_mb;
    uint32_t sd_free_mb;
    char sd_msg[48];
    char last_hex[72];
    int last_port;
    struct {
        bool open;
        int error;
        uint32_t rx_bytes;
        uint32_t drop_lines;
        uint32_t tx_frames;
        uint32_t tx_bytes;
        bool sending;
        char param[24];
        char file_name[24];
    } port[APP_PORT_COUNT];
} app_view_t;

void app_logger_start(void);
void app_logger_get_view(app_view_t *out);
void app_logger_request_reload(void);
void app_logger_request_remount(void);
void app_logger_request_sd_test(void);
void app_logger_request_tx_test(int port);
void app_logger_set_periodic_tx(int port, uint32_t interval_ms);
void app_logger_stop_periodic_tx(void);
void app_logger_stop_periodic_tx_port(int port);
void app_logger_inject(int port, const uint8_t *data, int len);
void app_logger_live_get(int index, uint32_t since, char *out, size_t out_len, uint32_t *next);
void app_logger_flush(void);
void app_logger_release_files(void);
