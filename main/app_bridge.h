#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "board.h"

#define BRIDGE_NAME_LEN 20

typedef struct {
    bool tcp_on[APP_PORT_COUNT];
    uint16_t tcp_port[APP_PORT_COUNT];
    int tcp_clients[APP_PORT_COUNT];
    uint64_t tcp_rx[APP_PORT_COUNT];  // 64-bit: overflow-proof at any rate
    uint64_t tcp_tx[APP_PORT_COUNT];
    bool ble_on;
    int ble_port;
    bool ble_connected;
    uint64_t ble_rx;
    uint64_t ble_tx;
    char ble_name[BRIDGE_NAME_LEN + 1];
    uint64_t up_drop[APP_PORT_COUNT]; // uart->clients bytes dropped (ring full)
    uint64_t down_drop[APP_PORT_COUNT]; // clients->uart bytes dropped (queue full)
} bridge_info_t;

void app_bridge_ble_init(void);
void app_bridge_tcp_resume(void);
void app_bridge_get(bridge_info_t *out);

// Validate and persist bridge settings to NVS only (no listener/stack change).
int app_bridge_persist(const bridge_info_t *in);
int app_bridge_tcp_set(int port_idx, bool on, uint16_t port_num);
int app_bridge_ble_set(bool on, int port_idx, const char *name);
void app_bridge_feed_uart(int port_idx, const uint8_t *data, size_t len);
