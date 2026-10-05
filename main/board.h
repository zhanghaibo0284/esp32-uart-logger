#pragma once

#define APP_PORT_COUNT 3

static inline int app_uart_tx(int index)
{
    static const int pins[APP_PORT_COUNT] = {43, 5, 8};
    return pins[index];
}

static inline int app_uart_rx(int index)
{
    static const int pins[APP_PORT_COUNT] = {44, 6, 18};
    return pins[index];
}
