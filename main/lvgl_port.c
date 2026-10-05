#include "lvgl_port.h"

#include <stdlib.h>
#include "lcd.h"
#include "touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

static const char *TAG = "lvgl";

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
    lv_disp_flush_ready(drv);
}

static void tick_cb(void *arg)
{
    lv_tick_inc(1);
}

static void indev_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static lv_coord_t last_x;
    static lv_coord_t last_y;
    tp_dev.scan(0);
    if (tp_dev.sta & TP_PRES_DOWN) {
        last_x = (lv_coord_t)tp_dev.x[0];
        last_y = (lv_coord_t)tp_dev.y[0];
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

void lvgl_port_init(void)
{
    lv_init();

    lcd_cfg_t lcd_cfg = {0};
    lcd_init(lcd_cfg);
    ESP_LOGI(TAG, "lcd %ux%u", lcd_dev.width, lcd_dev.height);

    int lines = 16;
    size_t pixels = (size_t)lcd_dev.width * (size_t)lines;
    size_t bytes = pixels * sizeof(lv_color_t);
    void *buf1 = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *buf2 = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1) {
        lines = 8;
        pixels = (size_t)lcd_dev.width * (size_t)lines;
        bytes = pixels * sizeof(lv_color_t);
        buf1 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        buf2 = NULL;
    }
    if (!buf1) {
        ESP_LOGE(TAG, "draw buffer alloc failed, free internal %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        abort();
    }
    ESP_LOGI(TAG, "draw buf %d lines %s", lines, buf2 ? "psram" : "internal");

    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t disp_drv;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, pixels);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = lcd_dev.width;
    disp_drv.ver_res = lcd_dev.height;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_drv_register(&disp_drv);

    tp_init();
    static lv_indev_drv_t indev;
    lv_indev_drv_init(&indev);
    indev.type = LV_INDEV_TYPE_POINTER;
    indev.read_cb = indev_read;
    lv_indev_drv_register(&indev);

    const esp_timer_create_args_t timer_args = {
        .callback = tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 1000));
    ESP_LOGI(TAG, "port ready");
}
