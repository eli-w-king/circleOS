#pragma once

#include "lvgl.h"

typedef enum {
    THINKING_ORB_IDLE,
    THINKING_ORB_SEARCHING,
    THINKING_ORB_COMPOSING,
    THINKING_ORB_WORKING,
    THINKING_ORB_SOLVING,
    THINKING_ORB_BREATHING,
} thinking_orb_state_t;

lv_obj_t *thinking_orb_create(lv_obj_t *parent);
void thinking_orb_start(void);
void thinking_orb_set_paused(bool paused);
void thinking_orb_set_state(thinking_orb_state_t state);
thinking_orb_state_t thinking_orb_get_state(void);
void thinking_orb_set_audio_levels(float input_level, float output_level, bool session_active);
