#pragma once

#include <stdbool.h>

#include "axp2101.h"
#include "lvgl.h"

typedef void (*quick_settings_close_callback_t)(void *user_data);

lv_obj_t *quick_settings_create(
    lv_obj_t *parent,
    quick_settings_close_callback_t close_callback,
    void *user_data);
void quick_settings_open(void);
void quick_settings_close(void);
void quick_settings_update(
    const axp2101_battery_t *battery,
    bool wifi_connected,
    bool wifi_failed);
