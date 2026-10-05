#pragma once

#include <stdbool.h>
#include <stddef.h>

void app_time_init(void);
void app_time_restore(void);
bool app_time_is_trusted(void);
int app_time_set(int year, int month, int day, int hour, int minute, int second);
void app_time_persist_now(void);
void app_time_format(char *out, size_t out_len);
