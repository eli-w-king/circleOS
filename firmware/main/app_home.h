#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

typedef void (*app_home_launch_callback_t)(void *user_data);

typedef enum {
    APP_HOME_ICON_SYMBOL,
    APP_HOME_ICON_MICROPHONE,
} app_home_icon_type_t;

typedef struct {
    app_home_icon_type_t icon_type;
    const char *symbol;
    app_home_launch_callback_t launch;
    void *user_data;
} app_home_entry_t;

lv_obj_t *app_home_create(
    lv_obj_t *parent,
    const app_home_entry_t *entries,
    size_t entry_count);
void app_home_start(void);
void app_home_set_visible(bool visible);
