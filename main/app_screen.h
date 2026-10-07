#pragma once

#include <stdbool.h>

// Screen control key identifiers.
#define APP_KEY_K1 0
#define APP_KEY_K2 1

// Reset the auto-off timer (touch, any user activity). Safe from any task.
void app_screen_note_activity(void);

// Short-press callback (press released before the long-press threshold).
typedef void (*app_screen_short_cb_t)(int key, void *arg);
void app_screen_set_short_press_cb(app_screen_short_cb_t cb, void *arg);

bool app_screen_is_on(void);

// Start the key/screen management task (after XL9555 init).
void app_screen_start(void);
