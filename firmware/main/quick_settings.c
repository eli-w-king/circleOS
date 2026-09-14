#include "quick_settings.h"

#include <stdio.h>

#include "bsp/esp-bsp.h"

#define SETTINGS_WIDTH 386
#define SETTINGS_HEIGHT 386
#define SETTINGS_BRIGHTNESS_HIGH 80
#define SETTINGS_BRIGHTNESS_LOW 25

static lv_obj_t *settings_root;
static lv_obj_t *wifi_value;
static lv_obj_t *battery_value;
static lv_obj_t *brightness_value;
static bool brightness_high = true;
static quick_settings_close_callback_t close_callback;
static void *close_user_data;

static void close_clicked(lv_event_t *event)
{
    (void)event;
    quick_settings_close();
    if (close_callback != NULL) {
        close_callback(close_user_data);
    }
}

static void brightness_clicked(lv_event_t *event)
{
    (void)event;
    brightness_high = !brightness_high;
    const uint8_t brightness =
        brightness_high ? SETTINGS_BRIGHTNESS_HIGH : SETTINGS_BRIGHTNESS_LOW;
    if (bsp_display_brightness_set(brightness) != ESP_OK) {
        brightness_high = !brightness_high;
        return;
    }
    lv_label_set_text(
        brightness_value,
        brightness_high ? "Bright" : "Dim");
}

static lv_obj_t *create_row(
    const char *title,
    lv_obj_t **value,
    int y,
    bool clickable)
{
    lv_obj_t *row = lv_obj_create(settings_root);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (!clickable) {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_size(row, 326, 54);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x171717), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, clickable ? 1 : 0, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x444444), 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *title_label = lv_label_create(row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 16, 0);

    *value = lv_label_create(row);
    lv_obj_set_style_text_font(*value, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(*value, lv_color_hex(0xAFAFAF), 0);
    lv_obj_align(*value, LV_ALIGN_RIGHT_MID, -16, 0);
    return row;
}

lv_obj_t *quick_settings_create(
    lv_obj_t *parent,
    quick_settings_close_callback_t close_handler,
    void *user_data)
{
    if (parent == NULL) {
        return NULL;
    }

    close_callback = close_handler;
    close_user_data = user_data;
    settings_root = lv_obj_create(parent);
    lv_obj_remove_style_all(settings_root);
    lv_obj_remove_flag(settings_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(settings_root, 466, 466);
    lv_obj_center(settings_root);
    lv_obj_set_style_bg_color(settings_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(settings_root, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(settings_root);
    lv_label_set_text(title, "Quick Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *close = lv_obj_create(settings_root);
    lv_obj_remove_style_all(close);
    lv_obj_remove_flag(close, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(close, 48, 48);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -28, 24);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(close, 1, 0);
    lv_obj_set_style_border_color(close, lv_color_hex(0x666666), 0);
    lv_obj_add_event_cb(close, close_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
    lv_obj_center(close_label);

    create_row("Wi-Fi", &wifi_value, 112, false);
    create_row("Battery", &battery_value, 178, false);
    lv_obj_t *brightness_row =
        create_row("Display", &brightness_value, 244, true);
    lv_obj_add_event_cb(
        brightness_row,
        brightness_clicked,
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *hint = lv_label_create(settings_root);
    lv_label_set_text(hint, "Tap Display to toggle brightness");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x777777), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -42);

    lv_label_set_text(brightness_value, "Bright");
    lv_label_set_text(wifi_value, "Checking...");
    lv_label_set_text(battery_value, "Checking...");
    lv_obj_add_flag(settings_root, LV_OBJ_FLAG_HIDDEN);
    return settings_root;
}

void quick_settings_open(void)
{
    if (settings_root == NULL) {
        return;
    }
    lv_obj_remove_flag(settings_root, LV_OBJ_FLAG_HIDDEN);
}

void quick_settings_close(void)
{
    if (settings_root == NULL) {
        return;
    }
    lv_obj_add_flag(settings_root, LV_OBJ_FLAG_HIDDEN);
}

void quick_settings_update(
    const axp2101_battery_t *battery,
    bool wifi_connected,
    bool wifi_failed)
{
    if (settings_root == NULL || battery == NULL) {
        return;
    }

    if (wifi_connected) {
        lv_label_set_text(wifi_value, "Connected");
    } else if (wifi_failed) {
        lv_label_set_text(wifi_value, "Offline");
    } else {
        lv_label_set_text(wifi_value, "Connecting...");
    }

    if (battery->charging) {
        lv_label_set_text(battery_value, "Charging");
    } else if (battery->percentage_valid) {
        char text[16];
        snprintf(
            text,
            sizeof(text),
            "%u%%",
            (unsigned)battery->percentage);
        lv_label_set_text(battery_value, text);
    } else if (battery->external_power) {
        lv_label_set_text(battery_value, "USB");
    } else {
        lv_label_set_text(battery_value, "Unknown");
    }
}
