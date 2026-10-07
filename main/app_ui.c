#include "app_ui.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "font_cn_16.h"
#include "app_logger.h"
#include "app_settings.h"
#include "app_time.h"
#include "app_files.h"
#include "app_wifi.h"
#include "app_screen.h"
#include "xl9555.h"

static const uint32_t BAUD_TABLE[] = {
    115200, 9600, 19200, 38400, 57600, 230400, 460800, 921600, 4800, 2400, 1200
};
static const uart_word_length_t DATA_TABLE[] = {
    UART_DATA_8_BITS, UART_DATA_7_BITS, UART_DATA_6_BITS, UART_DATA_5_BITS
};
static const uart_stop_bits_t STOP_TABLE[] = {
    UART_STOP_BITS_1, UART_STOP_BITS_1_5, UART_STOP_BITS_2
};
static const uart_parity_t PARITY_TABLE[] = {
    UART_PARITY_DISABLE, UART_PARITY_EVEN, UART_PARITY_ODD
};
static const uint16_t SEG_TABLE[] = {1, 10, 30, 60, 120, 360, 720, 1440};

static lv_obj_t *s_time;
static lv_obj_t *s_trust;
static lv_obj_t *s_sd;
static lv_obj_t *s_port[APP_PORT_COUNT];
static lv_obj_t *s_last;
static lv_obj_t *s_tv;
static lv_obj_t *s_port_dd;
static lv_obj_t *s_baud_dd;
static lv_obj_t *s_data_dd;
static lv_obj_t *s_stop_dd;
static lv_obj_t *s_par_dd;
static lv_obj_t *s_serial_msg;
static lv_obj_t *s_tx_sec_dd;
static lv_obj_t *s_tx_lbl[APP_PORT_COUNT];
static lv_obj_t *s_tx_msg;
static bool s_tx_on[APP_PORT_COUNT];
static lv_obj_t *s_open_btn[APP_PORT_COUNT];
static lv_obj_t *s_open_lbl[APP_PORT_COUNT];
static int s_clk[6];
static lv_obj_t *s_clk_lbl[6];
static lv_obj_t *s_clock_msg;
static lv_obj_t *s_seg_dd;
static lv_obj_t *s_log_msg;
static lv_obj_t *s_wifi_line;
static lv_obj_t *s_ssid;
static lv_obj_t *s_key;
static lv_obj_t *s_wifi_msg;
static lv_obj_t *s_kb;
static lv_obj_t *s_kb_ta;
static bool s_kb_upper;
static int s_edit_port;

static void use_cn_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &font_cn_16, LV_PART_MAIN);
    lv_obj_set_style_text_font(obj, &font_cn_16, LV_PART_ITEMS);
    lv_obj_set_style_text_font(obj, &font_cn_16, LV_PART_SELECTED);
}

static int index_of_u32(const uint32_t *table, int count, uint32_t value)
{
    for (int i = 0; i < count; i++) {
        if (table[i] == value) {
            return i;
        }
    }
    return 0;
}

static int index_of_u16(const uint16_t *table, int count, uint16_t value)
{
    for (int i = 0; i < count; i++) {
        if (table[i] == value) {
            return i;
        }
    }
    return 3;
}

static void dd_ready(lv_event_t *event)
{
    lv_obj_t *dd = lv_event_get_target(event);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_set_style_text_font(list, &font_cn_16, 0);
        lv_obj_set_style_max_height(list, 120, 0);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    use_cn_font(label);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *make_dd(lv_obj_t *parent, const char *options)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, options);
    lv_obj_set_width(dd, lv_pct(100));
    use_cn_font(dd);
    lv_obj_add_event_cb(dd, dd_ready, LV_EVENT_READY, NULL);
    return dd;
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 34);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    use_cn_font(btn);
    use_cn_font(label);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void style_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, 4, 0);
    lv_obj_set_style_pad_row(tab, 4, 0);
}

static void load_port_fields(int index)
{
    app_settings_t cfg;
    app_settings_get(&cfg);
    s_edit_port = index;
    lv_dropdown_set_selected(s_baud_dd, index_of_u32(BAUD_TABLE, 11, cfg.port[index].baud));
    int data_index = 0;
    for (int i = 0; i < 4; i++) {
        if (DATA_TABLE[i] == cfg.port[index].data_bits) {
            data_index = i;
        }
    }
    lv_dropdown_set_selected(s_data_dd, data_index);
    int stop_index = 0;
    for (int i = 0; i < 3; i++) {
        if (STOP_TABLE[i] == cfg.port[index].stop_bits) {
            stop_index = i;
        }
    }
    lv_dropdown_set_selected(s_stop_dd, stop_index);
    int parity_index = 0;
    for (int i = 0; i < 3; i++) {
        if (PARITY_TABLE[i] == cfg.port[index].parity) {
            parity_index = i;
        }
    }
    lv_dropdown_set_selected(s_par_dd, parity_index);
}

static void on_port_changed(lv_event_t *event)
{
    load_port_fields((int)lv_dropdown_get_selected(s_port_dd));
    lv_label_set_text(s_serial_msg, "未应用");
}

static void apply_current_port(bool force_open);

static void ensure_port_open(int index)
{
    app_settings_t cfg;
    app_settings_get(&cfg);
    if (s_port_dd && index == (int)lv_dropdown_get_selected(s_port_dd)) {
        cfg.port[index].baud = BAUD_TABLE[lv_dropdown_get_selected(s_baud_dd)];
        cfg.port[index].data_bits = DATA_TABLE[lv_dropdown_get_selected(s_data_dd)];
        cfg.port[index].stop_bits = STOP_TABLE[lv_dropdown_get_selected(s_stop_dd)];
        cfg.port[index].parity = PARITY_TABLE[lv_dropdown_get_selected(s_par_dd)];
    }
    if (!cfg.port[index].enabled) {
        cfg.port[index].enabled = true;
        app_settings_save(&cfg);
        app_logger_request_reload();
        return;
    }
    app_settings_save(&cfg);
    app_logger_request_reload();
}

static void on_tx_port(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= APP_PORT_COUNT) {
        return;
    }
    if (s_tx_on[index]) {
        app_logger_stop_periodic_tx_port(index);
        s_tx_on[index] = false;
        return;
    }
    static const uint32_t SEC_TABLE[] = {1000, 2000, 5000, 10000};
    int sec_index = s_tx_sec_dd ? (int)lv_dropdown_get_selected(s_tx_sec_dd) : 0;
    if (sec_index < 0 || sec_index > 3) {
        sec_index = 0;
    }
    ensure_port_open(index);
    app_logger_set_periodic_tx(index, SEC_TABLE[sec_index]);
    s_tx_on[index] = true;
}

static void on_toggle_port(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= APP_PORT_COUNT) {
        return;
    }
    app_view_t view;
    app_logger_get_view(&view);
    app_settings_t cfg;
    app_settings_get(&cfg);
    cfg.port[index].enabled = !view.port[index].open;
    app_settings_save(&cfg);
    app_logger_request_reload();
    s_edit_port = index;
}

static void on_apply(lv_event_t *event)
{
    (void)event;
    apply_current_port(false);
    lv_label_set_text(s_serial_msg, "已应用");
}

static void apply_current_port(bool force_open)
{
    app_settings_t cfg;
    app_settings_get(&cfg);
    int index = (int)lv_dropdown_get_selected(s_port_dd);
    s_edit_port = index;
    cfg.port[index].baud = BAUD_TABLE[lv_dropdown_get_selected(s_baud_dd)];
    cfg.port[index].data_bits = DATA_TABLE[lv_dropdown_get_selected(s_data_dd)];
    cfg.port[index].stop_bits = STOP_TABLE[lv_dropdown_get_selected(s_stop_dd)];
    cfg.port[index].parity = PARITY_TABLE[lv_dropdown_get_selected(s_par_dd)];
    if (force_open) {
        cfg.port[index].enabled = true;
    }
    app_settings_save(&cfg);
    app_logger_request_reload();
}

static const int CLK_MIN[6] = {2020, 1, 1, 0, 0, 0};
static const int CLK_MAX[6] = {2099, 12, 31, 23, 59, 59};
static const int CLK_WIDE[6] = {4, 2, 2, 2, 2, 2};
static const char *CLK_NAME[6] = {"年", "月", "日", "时", "分", "秒"};

typedef struct {
    int index;
    int delta;
} clk_step_t;

static clk_step_t s_clk_step[12];

static void show_clk(int index)
{
    if (s_clk_lbl[index]) {
        lv_label_set_text_fmt(s_clk_lbl[index], "%0*d", CLK_WIDE[index], s_clk[index]);
    }
}

static void load_clock_fields(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    s_clk[0] = tm.tm_year + 1900;
    s_clk[1] = tm.tm_mon + 1;
    s_clk[2] = tm.tm_mday;
    s_clk[3] = tm.tm_hour;
    s_clk[4] = tm.tm_min;
    s_clk[5] = tm.tm_sec;
    for (int i = 0; i < 6; i++) {
        show_clk(i);
    }
}

static void on_read_clock(lv_event_t *event)
{
    (void)event;
    load_clock_fields();
    lv_label_set_text(s_clock_msg, "已读取当前时间");
}

static void on_set_clock(lv_event_t *event)
{
    (void)event;
    int rc = app_time_set(s_clk[0], s_clk[1], s_clk[2], s_clk[3], s_clk[4], s_clk[5]);
    lv_label_set_text(s_clock_msg, rc == 0 ? "校时成功" : "日期无效");
}

static void on_clk_step(lv_event_t *event)
{
    clk_step_t *step = lv_event_get_user_data(event);
    if (!step) {
        return;
    }
    int value = s_clk[step->index] + step->delta;
    if (value < CLK_MIN[step->index]) {
        value = CLK_MIN[step->index];
    }
    if (value > CLK_MAX[step->index]) {
        value = CLK_MAX[step->index];
    }
    s_clk[step->index] = value;
    show_clk(step->index);
}

static void on_save_seg(lv_event_t *event)
{
    app_settings_t cfg;
    app_settings_get(&cfg);
    cfg.segment_min = SEG_TABLE[lv_dropdown_get_selected(s_seg_dd)];
    app_settings_save(&cfg);
    app_logger_request_reload();
    lv_label_set_text_fmt(s_log_msg, "分段已保存: %u分钟", cfg.segment_min);
}

static void on_remount(lv_event_t *event)
{
    app_logger_request_remount();
    lv_label_set_text(s_log_msg, "正在重新挂载");
}

static void on_inject(lv_event_t *event)
{
    const uint8_t frame[] = {0x55, 0xAA, 0x01, 0x02, 0x0D, 0x0A};
    app_logger_inject(s_edit_port, frame, sizeof(frame));
    lv_label_set_text_fmt(s_log_msg, "已写入测试帧 U%d", s_edit_port);
}

static void on_tx(lv_event_t *event)
{
    app_logger_request_tx_test(s_edit_port);
    lv_label_set_text_fmt(s_log_msg, "已发送测试 U%d", s_edit_port);
}

static lv_obj_t *make_small_btn(lv_obj_t *parent, const char *text, int width,
                                lv_event_cb_t cb, void *user, bool repeat)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, width, 32);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    use_cn_font(btn);
    use_cn_font(label);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
        if (repeat) {
            lv_obj_add_event_cb(btn, cb, LV_EVENT_LONG_PRESSED_REPEAT, user);
        }
    }
    return btn;
}

static lv_obj_t *make_row(lv_obj_t *parent, int height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, height);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static void add_clk_field(lv_obj_t *row, int index)
{
    lv_obj_t *box = lv_obj_create(row);
    lv_obj_set_size(box, 152, 34);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_column(box, 2, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name = lv_label_create(box);
    lv_label_set_text(name, CLK_NAME[index]);
    lv_obj_set_width(name, 18);
    use_cn_font(name);

    s_clk_lbl[index] = lv_label_create(box);
    lv_obj_set_width(s_clk_lbl[index], 46);
    lv_label_set_text(s_clk_lbl[index], "00");
    use_cn_font(s_clk_lbl[index]);

    s_clk_step[index * 2].index = index;
    s_clk_step[index * 2].delta = -1;
    s_clk_step[index * 2 + 1].index = index;
    s_clk_step[index * 2 + 1].delta = 1;
    make_small_btn(box, "-", 30, on_clk_step, &s_clk_step[index * 2], true);
    make_small_btn(box, "+", 30, on_clk_step, &s_clk_step[index * 2 + 1], true);
}

static void build_status(lv_obj_t *tab)
{
    style_tab(tab);
    s_sd = make_label(tab, "SD卡: --");
    s_wifi_line = make_label(tab, "WIFI");
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        s_port[i] = make_label(tab, "");
    }
    s_last = make_label(tab, "最新: -");
    make_label(tab, "U0 TX43/RX44 板载口");
    make_label(tab, "U1 TX5/RX6  U2 TX8/RX18");
    make_label(tab, "3.3V TTL, 必须共地");
}

static void build_serial(lv_obj_t *tab)
{
    style_tab(tab);
    lv_obj_t *open_row = make_row(tab, 36);
    s_open_btn[1] = make_small_btn(open_row, "J2打开", 100, on_toggle_port, (void *)(intptr_t)1, false);
    s_open_lbl[1] = lv_obj_get_child(s_open_btn[1], 0);
    s_open_btn[2] = make_small_btn(open_row, "J3打开", 100, on_toggle_port, (void *)(intptr_t)2, false);
    s_open_lbl[2] = lv_obj_get_child(s_open_btn[2], 0);
    s_open_btn[0] = make_small_btn(open_row, "U0打开", 100, on_toggle_port, (void *)(intptr_t)0, false);
    s_open_lbl[0] = lv_obj_get_child(s_open_btn[0], 0);
    make_label(tab, "发送时间");
    s_tx_sec_dd = make_dd(tab, "1秒\n2秒\n5秒\n10秒");
    lv_obj_t *tx_row = make_row(tab, 36);
    lv_obj_t *b1 = make_small_btn(tx_row, "J2发送", 148, on_tx_port, (void *)(intptr_t)1, false);
    s_tx_lbl[1] = lv_obj_get_child(b1, 0);
    lv_obj_t *b2 = make_small_btn(tx_row, "J3发送", 148, on_tx_port, (void *)(intptr_t)2, false);
    s_tx_lbl[2] = lv_obj_get_child(b2, 0);
    s_tx_msg = make_label(tab, "J2停 J3停");
    s_port_dd = make_dd(tab, "UART0 板载\nUART1 J2\nUART2 J3");
    lv_obj_add_event_cb(s_port_dd, on_port_changed, LV_EVENT_VALUE_CHANGED, NULL);
    make_label(tab, "J2=TX5/RX6  J3=TX8/RX18");
    make_label(tab, "波特率");
    s_baud_dd = make_dd(tab, "115200\n9600\n19200\n38400\n57600\n230400\n460800\n921600\n4800\n2400\n1200");
    make_label(tab, "数据位");
    s_data_dd = make_dd(tab, "8\n7\n6\n5");
    make_label(tab, "停止位");
    s_stop_dd = make_dd(tab, "1\n1.5\n2");
    make_label(tab, "校验");
    s_par_dd = make_dd(tab, "无\n偶\n奇");
    make_btn(tab, "应用", on_apply);
    s_serial_msg = make_label(tab, "未应用");
    load_port_fields(0);
}

static void build_clock(lv_obj_t *tab)
{
    style_tab(tab);
    lv_obj_set_style_pad_row(tab, 2, 0);
    lv_obj_t *row1 = make_row(tab, 36);
    add_clk_field(row1, 0);
    add_clk_field(row1, 3);
    lv_obj_t *row2 = make_row(tab, 36);
    add_clk_field(row2, 1);
    add_clk_field(row2, 4);
    lv_obj_t *row3 = make_row(tab, 36);
    add_clk_field(row3, 2);
    add_clk_field(row3, 5);
    make_btn(tab, "读取", on_read_clock);
    make_btn(tab, "校时", on_set_clock);
    s_clock_msg = make_label(tab, "掉电后时间不走,请重新校时");
    load_clock_fields();
}

static void build_log(lv_obj_t *tab)
{
    style_tab(tab);
    make_label(tab, "分段时长");
    s_seg_dd = make_dd(tab, "1分钟\n10分钟\n30分钟\n1小时\n2小时\n6小时\n12小时\n24小时");
    app_settings_t cfg;
    app_settings_get(&cfg);
    lv_dropdown_set_selected(s_seg_dd, index_of_u16(SEG_TABLE, 8, cfg.segment_min));
    make_btn(tab, "保存分段", on_save_seg);
    make_btn(tab, "重新挂载", on_remount);
    make_btn(tab, "写入测试帧", on_inject);
    make_btn(tab, "发送测试", on_tx);
    make_label(tab, "路径 /sdcard/UARTx/");
    make_label(tab, "格式 时间: AA BB CC");
    s_log_msg = make_label(tab, "十六进制存储");
}

static void refresh_cb(lv_timer_t *timer)
{
    char now[24];
    app_time_format(now, sizeof(now));
    lv_label_set_text(s_time, now + 11);
    lv_label_set_text(s_trust, app_time_is_trusted() ? "已校时" : "未校时");
    lv_obj_set_style_text_color(s_trust,
                                app_time_is_trusted() ? lv_color_hex(0x3DDC97) : lv_color_hex(0xFFB020),
                                0);

    app_view_t view;
    app_logger_get_view(&view);
    if (view.sd_mounted) {
        lv_label_set_text_fmt(s_sd, "SD卡正常  剩余 %lu MB", (unsigned long)view.sd_free_mb);
    } else {
        lv_label_set_text_fmt(s_sd, "SD %s", view.sd_msg[0] ? view.sd_msg : "无卡");
    }
    if (s_wifi_line) {
        app_wifi_info_t wifi;
        app_wifi_get(&wifi);
        lv_label_set_text_fmt(s_wifi_line, wifi.up ? "WIFI %s  IP %s" : "WIFI 未开",
                              wifi.ssid, wifi.ip);
    }
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        const char *tag = i == 1 ? "J2" : i == 2 ? "J3" : "U0";
        if (view.port[i].open) {
            lv_label_set_text_fmt(s_port[i], view.port[i].sending ? "%s 发 TX:%llu RX:%llu" : "%s 开 TX:%llu RX:%llu",
                                  tag,
                                  (unsigned long long)view.port[i].tx_bytes,
                                  (unsigned long long)view.port[i].rx_bytes);
        } else if (view.port[i].error) {
            lv_label_set_text_fmt(s_port[i], "%s 失败 %d", tag, view.port[i].error);
        } else {
            lv_label_set_text_fmt(s_port[i], "%s 关", tag);
        }
        lv_obj_set_style_text_color(s_port[i],
                                    view.port[i].open ? lv_color_hex(0x3DDC97) : lv_color_hex(0xD0D6DE),
                                    0);
        if (s_tx_lbl[i]) {
            lv_label_set_text_fmt(s_tx_lbl[i], s_tx_on[i] ? "%s停发" : "%s发送", tag);
            lv_obj_set_style_bg_color(lv_obj_get_parent(s_tx_lbl[i]),
                                      s_tx_on[i] ? lv_color_hex(0x1B7F4E) : lv_palette_main(LV_PALETTE_BLUE),
                                      0);
        }
        if (s_open_lbl[i]) {
            if (view.port[i].open) {
                lv_label_set_text_fmt(s_open_lbl[i], "%s关闭", tag);
                lv_obj_set_style_bg_color(s_open_btn[i], lv_color_hex(0x1B7F4E), 0);
            } else if (view.port[i].error) {
                lv_label_set_text_fmt(s_open_lbl[i], "%s失败", tag);
                lv_obj_set_style_bg_color(s_open_btn[i], lv_color_hex(0xB42318), 0);
            } else {
                lv_label_set_text_fmt(s_open_lbl[i], "%s打开", tag);
                lv_obj_set_style_bg_color(s_open_btn[i], lv_palette_main(LV_PALETTE_BLUE), 0);
            }
        }
    }
    lv_label_set_text_fmt(s_last, "最新 %s", view.last_hex[0] ? view.last_hex : "-");

    if (s_tx_msg) {
        lv_label_set_text_fmt(s_tx_msg, "J2%s  J3%s",
                              view.port[1].sending ? "发" : "停",
                              view.port[2].sending ? "发" : "停");
    }
    if (s_serial_msg && s_edit_port >= 0 && s_edit_port < APP_PORT_COUNT) {
        const char *tag = s_edit_port == 1 ? "J2" : s_edit_port == 2 ? "J3" : "U0";
        if (view.port[s_edit_port].open) {
            lv_label_set_text_fmt(s_serial_msg, "%s 已打开", tag);
        } else if (view.port[s_edit_port].error) {
            lv_label_set_text_fmt(s_serial_msg, "%s 打开失败 %d", tag, view.port[s_edit_port].error);
        } else {
            lv_label_set_text_fmt(s_serial_msg, "%s 已关闭", tag);
        }
    }
}

// Short-press handoff: the screen task (not LVGL-safe) only sets this flag;
// the tab switch itself runs in the UI task via the timer below.
static volatile int s_short_pending = -1;

static void short_press_cb(int key, void *arg)
{
    (void)arg;
    s_short_pending = key;
}

static void short_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    int key = s_short_pending;
    if (key < 0) {
        return;
    }
    s_short_pending = -1;
    uint16_t act = lv_tabview_get_tab_act(s_tv);
    if (key == APP_KEY_K1) {
        lv_tabview_set_act(s_tv, (act + 1) % 6, LV_ANIM_OFF);
    } else {
        lv_tabview_set_act(s_tv, (act + 5) % 6, LV_ANIM_OFF);
    }
}

static void on_tab_changed(lv_event_t *event)
{
    if (s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_tabview_get_tab_act(s_tv) == 4) {
        app_files_show();
    }
}


static const char *KB_LOW[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", ".", "Del", "\n",
    "OK", "_", ""
};
static const char *KB_UP[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "-", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", ".", "Del", "\n",
    "OK", "_", ""
};

static void show_kb(lv_obj_t *ta)
{
    s_kb_ta = ta;
    if (!s_kb) {
        return;
    }
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
}

static void on_ta_focus(lv_event_t *event)
{
    show_kb(lv_event_get_target(event));
}

static void on_kb(lv_event_t *event)
{
    lv_obj_t *kb = lv_event_get_target(event);
    uint16_t id = lv_btnmatrix_get_selected_btn(kb);
    if (id == LV_BTNMATRIX_BTN_NONE || !s_kb_ta) {
        return;
    }
    const char *txt = lv_btnmatrix_get_btn_text(kb, id);
    if (!txt) {
        return;
    }
    if (strcmp(txt, "Del") == 0) {
        lv_textarea_del_char(s_kb_ta);
    } else if (strcmp(txt, "ABC") == 0 || strcmp(txt, "abc") == 0) {
        s_kb_upper = !s_kb_upper;
        lv_btnmatrix_set_map(s_kb, s_kb_upper ? KB_UP : KB_LOW);
    } else if (strcmp(txt, "OK") == 0) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_textarea_add_text(s_kb_ta, txt);
    }
}

static lv_obj_t *make_ta(lv_obj_t *parent, const char *text, int max_len)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_height(ta, 36);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_text(ta, text ? text : "");
    use_cn_font(ta);
    lv_obj_add_event_cb(ta, on_ta_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, on_ta_focus, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void on_save_wifi(lv_event_t *event)
{
    (void)event;
    if (s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
    const char *ssid = lv_textarea_get_text(s_ssid);
    const char *key = lv_textarea_get_text(s_key);
    int rc = app_wifi_apply(ssid, key);
    if (s_wifi_msg) {
        lv_label_set_text(s_wifi_msg, rc == 0 ? "已保存" : "KEY < 8");
    }
}

static void build_wifi(lv_obj_t *tab)
{
    style_tab(tab);
    app_wifi_info_t wifi;
    app_wifi_get(&wifi);
    make_label(tab, "SSID");
    s_ssid = make_ta(tab, wifi.ssid, 32);
    make_label(tab, "KEY");
    s_key = make_ta(tab, wifi.pass, 63);
    make_btn(tab, "保存", on_save_wifi);
    s_wifi_msg = make_label(tab, "保存后重开");
    make_label(tab, "IP 192.168.4.1");
}

void app_ui_init(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(disp,
                                               lv_palette_main(LV_PALETTE_BLUE),
                                               lv_palette_main(LV_PALETTE_CYAN),
                                               true,
                                               &font_cn_16);
    lv_disp_set_theme(disp, theme);

    lv_obj_t *scr = lv_scr_act();
    use_cn_font(scr);
    s_trust = lv_label_create(scr);
    lv_label_set_text(s_trust, "未校时");
    use_cn_font(s_trust);
    lv_obj_align(s_trust, LV_ALIGN_TOP_LEFT, 4, 2);
    s_time = lv_label_create(scr);
    lv_label_set_text(s_time, "--:--:--");
    use_cn_font(s_time);
    lv_obj_align(s_time, LV_ALIGN_TOP_RIGHT, -4, 2);

    s_tv = lv_tabview_create(scr, LV_DIR_TOP, 32);
    use_cn_font(s_tv);
    use_cn_font(lv_tabview_get_tab_btns(s_tv));
    lv_obj_set_size(s_tv, 320, 214);
    lv_obj_align(s_tv, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t *tab_status = lv_tabview_add_tab(s_tv, "状态");
    lv_obj_t *tab_serial = lv_tabview_add_tab(s_tv, "串口");
    lv_obj_t *tab_clock = lv_tabview_add_tab(s_tv, "时钟");
    lv_obj_t *tab_log = lv_tabview_add_tab(s_tv, "日志");
    lv_obj_t *tab_file = lv_tabview_add_tab(s_tv, "文件");
    lv_obj_t *tab_wifi = lv_tabview_add_tab(s_tv, "WIFI");
    use_cn_font(tab_status);
    use_cn_font(tab_serial);
    use_cn_font(tab_clock);
    use_cn_font(tab_log);
    use_cn_font(tab_file);
    use_cn_font(tab_wifi);
    build_status(tab_status);
    build_serial(tab_serial);
    build_clock(tab_clock);
    build_log(tab_log);
    app_files_create(tab_file);
    build_wifi(tab_wifi);
    lv_obj_add_event_cb(s_tv, on_tab_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_kb = lv_btnmatrix_create(scr);
    lv_obj_set_size(s_kb, 320, 156);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_btnmatrix_set_map(s_kb, KB_LOW);
    use_cn_font(s_kb);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(0x1B2430), 0);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_kb, on_kb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(refresh_cb, 400, NULL);
    lv_timer_create(short_poll_cb, 50, NULL);
    app_screen_set_short_press_cb(short_press_cb, NULL);
}
