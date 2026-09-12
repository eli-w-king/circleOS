#pragma once

#include <stdint.h>

#define ROTARY_DIAL_SLOT_COUNT 29

typedef struct {
    float yaw_degrees;
    float selection_degrees;
    int32_t selected_slot;
} rotary_dial_t;

void rotary_dial_init(rotary_dial_t *dial);
void rotary_dial_open(rotary_dial_t *dial);
void rotary_dial_rotate(rotary_dial_t *dial, float delta_degrees);
float rotary_dial_yaw_degrees(const rotary_dial_t *dial);
int rotary_dial_selected_slot(const rotary_dial_t *dial);
