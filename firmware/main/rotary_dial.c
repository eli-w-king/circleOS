#include "rotary_dial.h"

#include <math.h>
#include <stddef.h>

#define SLOT_DEGREES (360.0f / ROTARY_DIAL_SLOT_COUNT)
#define SLOT_HYSTERESIS 0.58f
void rotary_dial_init(rotary_dial_t *dial)
{
    if (dial == NULL) {
        return;
    }
    *dial = (rotary_dial_t) {0};
}

void rotary_dial_open(rotary_dial_t *dial)
{
    if (dial == NULL) {
        return;
    }
    dial->yaw_degrees = 0.0f;
    dial->selection_degrees = 0.0f;
    dial->selected_slot = 0;
}

void rotary_dial_rotate(rotary_dial_t *dial, float delta_degrees)
{
    if (dial == NULL) {
        return;
    }
    dial->yaw_degrees += delta_degrees;
    dial->selection_degrees = -dial->yaw_degrees;

    const float boundary = SLOT_DEGREES * SLOT_HYSTERESIS;
    while (
        dial->selection_degrees -
            dial->selected_slot * SLOT_DEGREES >
        boundary) {
        dial->selected_slot++;
    }
    while (
        dial->selection_degrees -
            dial->selected_slot * SLOT_DEGREES <
        -boundary) {
        dial->selected_slot--;
    }
}

float rotary_dial_yaw_degrees(const rotary_dial_t *dial)
{
    return dial == NULL ? 0.0f : dial->yaw_degrees;
}

int rotary_dial_selected_slot(const rotary_dial_t *dial)
{
    if (dial == NULL) {
        return 0;
    }
    int selected = dial->selected_slot % ROTARY_DIAL_SLOT_COUNT;
    return selected < 0 ? selected + ROTARY_DIAL_SLOT_COUNT : selected;
}
