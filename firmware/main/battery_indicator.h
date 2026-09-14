#pragma once

#include <stdbool.h>

#include "axp2101.h"
#include "lvgl.h"

lv_obj_t *battery_indicator_create(lv_obj_t *parent);
void battery_indicator_update(const axp2101_battery_t *battery);
void battery_indicator_set_visible(bool visible);
