#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef void (*rotary_keyboard_text_callback_t)(const char *text, void *user_data);

lv_obj_t *rotary_keyboard_create(lv_obj_t *parent);
void rotary_keyboard_open(void);
void rotary_keyboard_close(void);
bool rotary_keyboard_is_open(void);
const char *rotary_keyboard_get_text(void);
void rotary_keyboard_clear(void);
void rotary_keyboard_on_text_changed(
    rotary_keyboard_text_callback_t callback,
    void *user_data);
void rotary_keyboard_on_submit(
    rotary_keyboard_text_callback_t callback,
    void *user_data);
