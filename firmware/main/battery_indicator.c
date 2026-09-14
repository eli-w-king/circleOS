#include "battery_indicator.h"

#include <stdio.h>

#define BATTERY_TOP_OFFSET 26
#define BATTERY_WIDTH 88
#define BATTERY_HEIGHT 26
#define BATTERY_PAD 10
#define BATTERY_LOW_PERCENTAGE 20
#define BATTERY_CRITICAL_PERCENTAGE 10
#define BATTERY_CHARGING_COLOR 0x30D158
#define BATTERY_LOW_COLOR 0xFF9F0A
#define BATTERY_CRITICAL_COLOR 0xFF453A

static lv_obj_t *indicator_root;
static lv_obj_t *icon_label;
static lv_obj_t *text_label;

static const char *battery_symbol(uint8_t percentage)
{
    if (percentage >= 90) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (percentage >= 70) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (percentage >= 40) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (percentage >= 15) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

lv_obj_t *battery_indicator_create(lv_obj_t *parent)
{
    if (parent == NULL) {
        return NULL;
    }

    indicator_root = lv_obj_create(parent);
    lv_obj_remove_style_all(indicator_root);
    lv_obj_remove_flag(indicator_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(indicator_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(indicator_root, BATTERY_WIDTH, BATTERY_HEIGHT);
    lv_obj_align(indicator_root, LV_ALIGN_TOP_MID, 0, BATTERY_TOP_OFFSET);
    lv_obj_set_style_pad_all(indicator_root, 0, 0);
    lv_obj_set_style_radius(indicator_root, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(indicator_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(indicator_root, LV_OPA_60, 0);

    icon_label = lv_label_create(indicator_root);
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(icon_label, lv_color_white(), 0);
    lv_label_set_text(icon_label, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, BATTERY_PAD, 0);

    text_label = lv_label_create(indicator_root);
    lv_obj_set_style_text_font(text_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(text_label, lv_color_white(), 0);
    lv_label_set_text(text_label, "--");
    lv_obj_align(text_label, LV_ALIGN_RIGHT_MID, -BATTERY_PAD, 0);

    return indicator_root;
}

void battery_indicator_update(const axp2101_battery_t *battery)
{
    if (indicator_root == NULL || battery == NULL) {
        return;
    }

    lv_color_t color = lv_color_white();
    if (battery->charging) {
        lv_label_set_text(icon_label, LV_SYMBOL_CHARGE);
        color = lv_color_hex(BATTERY_CHARGING_COLOR);
    } else if (!battery->battery_present) {
        lv_label_set_text(icon_label, LV_SYMBOL_USB);
    } else {
        lv_label_set_text(icon_label, battery_symbol(battery->percentage));
        if (battery->percentage_valid &&
            battery->percentage <= BATTERY_CRITICAL_PERCENTAGE) {
            color = lv_color_hex(BATTERY_CRITICAL_COLOR);
        } else if (
            battery->percentage_valid &&
            battery->percentage <= BATTERY_LOW_PERCENTAGE) {
            color = lv_color_hex(BATTERY_LOW_COLOR);
        }
    }

    if (battery->percentage_valid) {
        char text[8];
        snprintf(text, sizeof(text), "%u%%", (unsigned)battery->percentage);
        lv_label_set_text(text_label, text);
    } else {
        lv_label_set_text(text_label, battery->external_power ? "USB" : "--");
    }

    lv_obj_set_style_text_color(icon_label, color, 0);
    lv_obj_set_style_text_color(text_label, color, 0);
}

void battery_indicator_set_visible(bool visible)
{
    if (indicator_root == NULL) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(indicator_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(indicator_root, LV_OBJ_FLAG_HIDDEN);
    }
}
