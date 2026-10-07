#include "app_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "esp_console.h"
#include "esp_log.h"
#include "app_logger.h"
#include "app_settings.h"
#include "app_config.h"
#include "app_time.h"

static const char *TAG = "cmd";

static int parse_port(const char *text)
{
    if (!text) {
        return -1;
    }
    int port = atoi(text);
    if (port < 0 || port >= APP_PORT_COUNT) {
        return -1;
    }
    return port;
}

static int cmd_status(int argc, char **argv)
{
    app_view_t view;
    app_logger_get_view(&view);
    char now[24];
    app_time_format(now, sizeof(now));
    printf("TIME trusted=%d %s\n", app_time_is_trusted() ? 1 : 0, now);
    printf("SD mounted=%d total=%luMB free=%luMB %s\n",
           view.sd_mounted ? 1 : 0,
           (unsigned long)view.sd_total_mb,
           (unsigned long)view.sd_free_mb,
           view.sd_msg);
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        printf("PORT%d open=%d err=%d %s rx=%lu drop=%lu file=%s TX=%d RX=%d\n",
               i,
               view.port[i].open ? 1 : 0,
               view.port[i].error,
               view.port[i].param,
               (unsigned long)view.port[i].rx_bytes,
               (unsigned long)view.port[i].drop_lines,
               view.port[i].file_name[0] ? view.port[i].file_name : "-",
               app_uart_tx(i),
               app_uart_rx(i));
    }
    printf("LAST %s\n", view.last_hex[0] ? view.last_hex : "-");
    return 0;
}

static int set_enabled(int port, bool enabled)
{
    app_settings_t cfg;
    app_settings_get(&cfg);
    cfg.port[port].enabled = enabled;
    app_settings_save(&cfg);
    app_logger_request_reload();
    printf("UART%d %s\n", port, enabled ? "open" : "close");
    return 0;
}

static int cmd_open(int argc, char **argv)
{
    int port = argc > 1 ? parse_port(argv[1]) : -1;
    if (port < 0) {
        printf("usage: open <0-2>\n");
        return 1;
    }
    return set_enabled(port, true);
}

static int cmd_close(int argc, char **argv)
{
    int port = argc > 1 ? parse_port(argv[1]) : -1;
    if (port < 0) {
        printf("usage: close <0-2>\n");
        return 1;
    }
    return set_enabled(port, false);
}

static int cmd_baud(int argc, char **argv)
{
    int port = argc > 2 ? parse_port(argv[1]) : -1;
    if (port < 0) {
        printf("usage: baud <0-2> <rate>\n");
        return 1;
    }
    app_settings_t cfg;
    app_settings_get(&cfg);
    cfg.port[port].baud = (uint32_t)atoi(argv[2]);
    app_settings_save(&cfg);
    app_logger_request_reload();
    printf("UART%d baud %lu\n", port, (unsigned long)cfg.port[port].baud);
    return 0;
}

static int cmd_seg(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: seg <minutes>\n");
        return 1;
    }
    app_settings_t cfg;
    app_settings_get(&cfg);
    cfg.segment_min = (uint16_t)atoi(argv[1]);
    app_settings_save(&cfg);
    app_logger_request_reload();
    printf("segment %u min\n", cfg.segment_min);
    return 0;
}

static int cmd_time(int argc, char **argv)
{
    int year, month, day, hour, minute, second;
    if (argc < 3 ||
        sscanf(argv[1], "%d-%d-%d", &year, &month, &day) != 3 ||
        sscanf(argv[2], "%d:%d:%d", &hour, &minute, &second) != 3) {
        printf("usage: time YYYY-MM-DD HH:MM:SS\n");
        return 1;
    }
    if (app_time_set(year, month, day, hour, minute, second) != 0) {
        printf("invalid time\n");
        return 1;
    }
    printf("time set\n");
    return 0;
}

static int cmd_sdtest(int argc, char **argv)
{
    app_logger_request_sd_test();
    printf("sd test requested\n");
    return 0;
}

static int cmd_remount(int argc, char **argv)
{
    app_logger_request_remount();
    printf("remount requested\n");
    return 0;
}

static int parse_hex(const char *text, uint8_t *out, int max_len)
{
    int count = 0;
    const char *p = text;
    while (*p && count < max_len) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        if (!*p) {
            break;
        }
        char *end = NULL;
        long value = strtol(p, &end, 16);
        if (end == p || value < 0 || value > 255) {
            return -1;
        }
        out[count++] = (uint8_t)value;
        p = end;
    }
    return count;
}

static int cmd_inject(int argc, char **argv)
{
    int port = argc > 2 ? parse_port(argv[1]) : -1;
    if (port < 0) {
        printf("usage: inject <0-2> <hex...>\n");
        return 1;
    }
    uint8_t data[32];
    int len = 0;
    for (int i = 2; i < argc && len < (int)sizeof(data); i++) {
        int n = parse_hex(argv[i], data + len, (int)sizeof(data) - len);
        if (n < 0) {
            printf("bad hex\n");
            return 1;
        }
        len += n;
    }
    if (len <= 0) {
        printf("no data\n");
        return 1;
    }
    app_logger_inject(port, data, len);
    printf("injected %d bytes to UART%d\n", len, port);
    return 0;
}

static int cmd_tx(int argc, char **argv)
{
    int port = argc > 1 ? parse_port(argv[1]) : -1;
    if (port < 0) {
        printf("usage: tx <0-2>\n");
        return 1;
    }
    app_logger_request_tx_test(port);
    printf("tx test UART%d\n", port);
    return 0;
}

static int cmd_cfg(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: cfg <save|load|restore|status>\n");
        return 1;
    }
    if (!strcmp(argv[1], "save")) {
        int rc = app_config_export();
        printf("export rc=%d\n", rc);
        return rc == 0 ? 0 : 1;
    }
    if (!strcmp(argv[1], "load")) {
        int rc = app_config_load();
        printf("load rc=%d\n", rc);
        if (rc == 0) {
            app_logger_request_reload();
        }
        return rc == 0 ? 0 : 1;
    }
    if (!strcmp(argv[1], "restore")) {
        int rc = app_config_restore_backup();
        printf("restore rc=%d\n", rc);
        if (rc == 0) {
            app_logger_request_reload();
        }
        return rc == 0 ? 0 : 1;
    }
    if (!strcmp(argv[1], "status")) {
        app_config_status_t st;
        app_config_get_status(&st);
        printf("config present=%d backup=%d last_ok=%d msg=%s\n",
               st.present ? 1 : 0, st.backup_present ? 1 : 0,
               st.last_ok ? 1 : 0, st.msg);
        return 0;
    }
    printf("usage: cfg <save|load|restore|status>\n");
    return 1;
}

static void register_cmd(const char *name, const char *help, esp_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd = {
        .command = name,
        .help = help,
        .func = func,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void app_cmd_start(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "log> ";
    esp_console_dev_usb_serial_jtag_config_t hw_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl));
    register_cmd("status", "show logger status", cmd_status);
    register_cmd("open", "open <0-2>", cmd_open);
    register_cmd("close", "close <0-2>", cmd_close);
    register_cmd("baud", "baud <0-2> <rate>", cmd_baud);
    register_cmd("seg", "seg <minutes>", cmd_seg);
    register_cmd("time", "time YYYY-MM-DD HH:MM:SS", cmd_time);
    register_cmd("sdtest", "write and read SD test file", cmd_sdtest);
    register_cmd("remount", "remount SD card", cmd_remount);
    register_cmd("inject", "inject <0-2> <hex...>", cmd_inject);
    register_cmd("tx", "tx <0-2> send 55 AA 01 02 03", cmd_tx);
    register_cmd("cfg", "cfg <save|load|restore|status>", cmd_cfg);
    ESP_ERROR_CHECK(esp_console_register_help_command());
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "console ready");
#else
    ESP_LOGW(TAG, "USB serial console is disabled");
#endif
}
