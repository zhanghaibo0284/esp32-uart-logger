#include "app_screen.h"

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "xl9555.h"
#include "app_settings.h"

static const char *TAG = "screen";

#define POLL_PERIOD_MS   20
#define DEBOUNCE_US      40000      // level stable 40ms
#define LONG_PRESS_US    2000000    // 2 seconds
#define AUTO_OFF_US      (300LL * 1000000) // 5 minutes

typedef struct {
    uint16_t io_bit;
    int last_raw;
    int stable_level;
    int64_t last_change_us;
    bool pressed;
    int64_t press_start_us;
    bool long_consumed;
} key_t;

static SemaphoreHandle_t s_lock;
static key_t s_keys[2];
static bool s_screen_on = true;
static int64_t s_activity_us;

static app_screen_short_cb_t s_short_cb;
static void *s_short_arg;

// LEDR feedback schedule (non-blocking).
static int s_fb_toggles;
static int64_t s_fb_next_us;

void app_screen_note_activity(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_activity_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

void app_screen_set_short_press_cb(app_screen_short_cb_t cb, void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_short_cb = cb;
    s_short_arg = arg;
    xSemaphoreGive(s_lock);
}

bool app_screen_is_on(void)
{
    return s_screen_on;
}

static void start_feedback(int64_t now)
{
    // Three quick LEDR flashes: LED on now, then 5 timed toggles.
    xl9555_pin_write(LEDR_IO, 1);
    s_fb_toggles = 5;
    s_fb_next_us = now + 100000;
}

static void run_feedback(int64_t now)
{
    while (s_fb_toggles > 0 && now >= s_fb_next_us) {
        xl9555_pin_write(LEDR_IO, !xl9555_pin_read(LEDR_IO));
        s_fb_toggles--;
        s_fb_next_us += 100000;
    }
}

static void set_screen(bool on, int64_t now, const char *reason)
{
    if (s_screen_on == on) {
        return;
    }
    xl9555_pin_write(LCD_BL_IO, on ? 1 : 0);
    s_screen_on = on;
    if (on) {
        s_activity_us = now;
    }
    start_feedback(now);
    ESP_LOGI(TAG, "backlight %s (%s)", on ? "ON" : "OFF", reason);
}

static void do_long_press(int ki, int64_t now)
{
    if (ki == APP_KEY_K1) {
        set_screen(false, now, "K1 long press");
    } else {
        set_screen(true, now, "K2 long press");
    }
}

static void poll_key(int ki, uint8_t p0, int64_t now)
{
    key_t *k = &s_keys[ki];
    int raw = (p0 & k->io_bit) ? 1 : 0;

    if (raw != k->last_raw) {
        k->last_raw = raw;
        k->last_change_us = now;
        return;
    }
    if (now - k->last_change_us < DEBOUNCE_US) {
        return;
    }
    if (raw == k->stable_level) {
        // Level stable and already applied; check long-press hold time.
        if (k->pressed && !k->long_consumed && now - k->press_start_us >= LONG_PRESS_US) {
            k->long_consumed = true;
            do_long_press(ki, now);
        }
        return;
    }

    // Debounced level transition.
    k->stable_level = raw;
    if (raw == 0) {
        // Press confirmed.
        k->pressed = true;
        k->press_start_us = now;
        k->long_consumed = false;
        s_activity_us = now;
    } else {
        // Release confirmed: short press if not consumed and screen is on.
        if (k->pressed && !k->long_consumed && s_screen_on && s_short_cb) {
            s_short_cb(ki, s_short_arg);
        }
        k->pressed = false;
    }
}

static void screen_task(void *arg)
{
    s_keys[0].io_bit = KEY0_IO; // K1
    s_keys[1].io_bit = KEY1_IO; // K2
    for (int i = 0; i < 2; i++) {
        s_keys[i].last_raw = 1;
        s_keys[i].stable_level = 1;
        s_keys[i].last_change_us = esp_timer_get_time();
    }

    while (1) {
        int64_t now = esp_timer_get_time();
        uint8_t io[2] = {0xFF, 0xFF};
        if (xl9555_read_byte(io, 2) != ESP_OK) {
            // Treat as no change this cycle; debounce will swallow glitches.
        } else {
            poll_key(APP_KEY_K1, io[0], now);
            poll_key(APP_KEY_K2, io[0], now);
        }

        if (s_screen_on) {
            app_settings_t cfg;
            app_settings_get(&cfg);
            if (cfg.screen_auto_off && now - s_activity_us > AUTO_OFF_US) {
                set_screen(false, now, "5min inactivity");
            }
        }

        run_feedback(now);
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void app_screen_start(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_activity_us = esp_timer_get_time();
    xTaskCreate(screen_task, "screen", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "screen management ready");
}
