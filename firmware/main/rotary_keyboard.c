#include "rotary_keyboard.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "rotary_dial.h"

#define ITEM_COUNT ROTARY_DIAL_SLOT_COUNT
#define CAPS_INDEX 26
#define MODE_INDEX 27
#define BACKSPACE_INDEX 28
#define SLOT_DEGREES (360.0f / ITEM_COUNT)
#define KEYBOARD_FRAME_MS 20
#define KEYBOARD_INPUT_MS 10
#define DIAL_DRAG_GAIN 1.0f
#define LETTER_RING_RADIUS 196.0f
#define CENTER_X 233
#define CENTER_Y 233
#define CENTER_VIEW_WIDTH 220
#define CENTER_VIEW_HEIGHT 52
#define TOUCH_WIDTH 300
#define TOUCH_HEIGHT 240
#define TOUCH_SCROLL_THRESHOLD 6
#define TEXT_CAPACITY 128
#define GHOST_GAP 3
#define BEAM_SEGMENTS 7

static lv_obj_t *keyboard_root;
static lv_obj_t *letter_labels[ITEM_COUNT];
static lv_obj_t *selected_label;
static lv_obj_t *border_arc;
static lv_obj_t *beam_arcs[BEAM_SEGMENTS];
static lv_obj_t *ring_touch;
static lv_obj_t *center_view;
static lv_obj_t *committed_label;
static lv_obj_t *ghost_label;
static lv_obj_t *ellipsis_label;
static lv_obj_t *center_touch;
static lv_timer_t *keyboard_timer;
static lv_timer_t *input_timer;
static lv_timer_t *display_timer;
static rotary_dial_t dial;
static volatile bool keyboard_open;
static char committed_text[TEXT_CAPACITY];
static int rendered_selection = -1;
static bool items_dirty;
static bool numeric_mode;
static bool caps_enabled;
static int review_offset;
static int maximum_review;
static bool touch_scrolled;
static lv_point_t touch_start;
static int touch_start_review;
static float beam_offset;
static bool ring_dragging;
static bool ring_drag_moved;
static float ring_drag_distance;
static float last_touch_angle;
static int64_t last_touch_us;
static float spin_velocity;
static rotary_keyboard_text_callback_t text_changed_callback;
static void *text_changed_user_data;
static rotary_keyboard_text_callback_t submit_callback;
static void *submit_user_data;

static void update_keyboard(lv_timer_t *timer);

static const char *const number_items[CAPS_INDEX + 1] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    ".", ",", "?", "!", "@", "#", "$", "%", "&", "-", "+", "=",
    "/", ";", ":", "'", "\"",
};

static const char *item_text(int index, char letter[2])
{
    if (index == BACKSPACE_INDEX) {
        return LV_SYMBOL_BACKSPACE;
    }
    if (index == MODE_INDEX) {
        return numeric_mode ? "ABC" : "123";
    }
    if (!numeric_mode && index == CAPS_INDEX) {
        return "CAP";
    }
    if (numeric_mode) {
        return number_items[index];
    }
    letter[0] = (char)((caps_enabled ? 'A' : 'a') + index);
    letter[1] = '\0';
    return letter;
}

static void style_letter(int index)
{
    const bool active_caps =
        !numeric_mode && index == CAPS_INDEX && caps_enabled;
    lv_obj_set_style_text_font(
        letter_labels[index],
        &lv_font_montserrat_16,
        0);
    lv_obj_set_style_text_color(
        letter_labels[index],
        active_caps ? lv_color_white() : lv_color_make(205, 205, 205),
        0);
    lv_obj_set_style_text_opa(
        letter_labels[index],
        active_caps ? LV_OPA_COVER : LV_OPA_90,
        0);
}

static void refresh_item_labels(void)
{
    for (int index = 0; index < ITEM_COUNT; index++) {
        char letter[2];
        lv_label_set_text(letter_labels[index], item_text(index, letter));
        style_letter(index);
    }
    items_dirty = true;
}

static int glyph_width(const lv_font_t *font, uint32_t codepoint)
{
    lv_font_glyph_dsc_t descriptor;
    return lv_font_get_glyph_dsc(
        font,
        &descriptor,
        codepoint,
        0)
        ? descriptor.adv_w
        : 0;
}

static int text_width(const char *text, const lv_font_t *font)
{
    int width = 0;
    while (*text != '\0') {
        width += glyph_width(font, (uint32_t)(unsigned char)*text);
        text++;
    }
    return width;
}

static void render_center_text(int selected)
{
    const lv_font_t *font = &lv_font_montserrat_32;
    char letter[2];
    const char *ghost = item_text(selected, letter);
    const int committed_width = text_width(committed_text, font);
    const int ghost_width = selected == BACKSPACE_INDEX
        ? glyph_width(font, 0xF55A)
        : text_width(ghost, font);
    const int total_width =
        committed_width + (committed_text[0] == '\0' ? 0 : GHOST_GAP) +
        ghost_width;
    maximum_review =
        total_width > CENTER_VIEW_WIDTH ? total_width - CENTER_VIEW_WIDTH : 0;
    if (review_offset > maximum_review) {
        review_offset = maximum_review;
    }

    const int start_x = total_width <= CENTER_VIEW_WIDTH
        ? (CENTER_VIEW_WIDTH - total_width) / 2
        : CENTER_VIEW_WIDTH - total_width + review_offset;
    lv_label_set_text(committed_label, committed_text);
    lv_obj_set_size(
        committed_label,
        committed_width > 0 ? committed_width : 1,
        CENTER_VIEW_HEIGHT);
    const int text_y =
        (CENTER_VIEW_HEIGHT - lv_font_get_line_height(font)) / 2;
    lv_obj_set_pos(committed_label, start_x, text_y);

    lv_label_set_text(ghost_label, ghost);
    lv_obj_set_size(ghost_label, ghost_width, CENTER_VIEW_HEIGHT);
    lv_obj_set_pos(
        ghost_label,
        start_x + committed_width +
            (committed_text[0] == '\0' ? 0 : GHOST_GAP),
        text_y);

    if (total_width > CENTER_VIEW_WIDTH && review_offset < maximum_review - 1) {
        lv_obj_remove_flag(ellipsis_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ellipsis_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void activate_item(int selected)
{
    size_t length = strlen(committed_text);
    bool text_changed = false;
    if (selected == BACKSPACE_INDEX) {
        if (length > 0) {
            committed_text[length - 1] = '\0';
            text_changed = true;
        }
    } else if (!numeric_mode && selected == CAPS_INDEX) {
        caps_enabled = !caps_enabled;
        refresh_item_labels();
    } else if (selected == MODE_INDEX) {
        numeric_mode = !numeric_mode;
        refresh_item_labels();
    } else {
        if (length + 1 >= sizeof(committed_text)) {
            return;
        }
        char letter[2];
        committed_text[length] = item_text(selected, letter)[0];
        committed_text[length + 1] = '\0';
        text_changed = true;
    }
    review_offset = 0;
    render_center_text(rotary_dial_selected_slot(&dial));
    if (text_changed && text_changed_callback != NULL) {
        text_changed_callback(committed_text, text_changed_user_data);
    }
}

static void commit_selected_letter(void)
{
    activate_item(rotary_dial_selected_slot(&dial));
}

static void center_touch_event(lv_event_t *event)
{
    lv_indev_t *input = lv_indev_active();
    if (input == NULL) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(input, &point);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        touch_start = point;
        touch_start_review = review_offset;
        touch_scrolled = false;
    } else if (code == LV_EVENT_PRESSING) {
        const int delta_x = point.x - touch_start.x;
        const int delta_y = point.y - touch_start.y;
        if (
            abs(delta_x) > TOUCH_SCROLL_THRESHOLD ||
            abs(delta_y) > TOUCH_SCROLL_THRESHOLD) {
            touch_scrolled = true;
        }
        if (touch_scrolled) {
            review_offset = touch_start_review + delta_x;
            if (review_offset < 0) {
                review_offset = 0;
            } else if (review_offset > maximum_review) {
                review_offset = maximum_review;
            }
            const int selected = rotary_dial_selected_slot(&dial);
            render_center_text(selected);
        }
    } else if (code == LV_EVENT_CLICKED) {
        const bool inside =
            point.x >= CENTER_X - TOUCH_WIDTH / 2 &&
            point.x <= CENTER_X + TOUCH_WIDTH / 2 &&
            point.y >= CENTER_Y - TOUCH_HEIGHT / 2 &&
            point.y <= CENTER_Y + TOUCH_HEIGHT / 2;
        if (!touch_scrolled && inside) {
            commit_selected_letter();
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        touch_scrolled = true;
    }
}

static float point_angle(const lv_point_t *point)
{
    return atan2f(
        point->y - CENTER_Y,
        point->x - CENTER_X);
}

static void ring_touch_event(lv_event_t *event)
{
    lv_indev_t *input = lv_indev_active();
    if (input == NULL) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(input, &point);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        ring_dragging = true;
        ring_drag_moved = false;
        ring_drag_distance = 0.0f;
        spin_velocity = 0.0f;
        last_touch_angle = point_angle(&point);
        last_touch_us = esp_timer_get_time();
    } else if (code == LV_EVENT_PRESSING && ring_dragging) {
        const float angle = point_angle(&point);
        const float delta_radians = atan2f(
            sinf(angle - last_touch_angle),
            cosf(angle - last_touch_angle));
        const float delta_degrees =
            delta_radians * 180.0f / (float)M_PI;
        const float dial_delta_degrees =
            delta_degrees * DIAL_DRAG_GAIN;
        ring_drag_distance += fabsf(delta_degrees);
        if (ring_drag_distance > 3.0f) {
            ring_drag_moved = true;
        }
        const int64_t now = esp_timer_get_time();
        const float elapsed_seconds =
            (now - last_touch_us) / 1000000.0f;
        rotary_dial_rotate(&dial, dial_delta_degrees);
        review_offset = 0;
        if (elapsed_seconds > 0.002f && elapsed_seconds < 0.1f) {
            const float instantaneous_velocity =
                dial_delta_degrees / elapsed_seconds;
            spin_velocity =
                spin_velocity * 0.45f + instantaneous_velocity * 0.55f;
        }
        last_touch_angle = angle;
        last_touch_us = now;
    } else if (
        code == LV_EVENT_RELEASED ||
        code == LV_EVENT_PRESS_LOST) {
        ring_dragging = false;
    }
}

static void item_clicked_event(lv_event_t *event)
{
    if (ring_drag_moved) {
        return;
    }
    const intptr_t requested = (intptr_t)lv_event_get_user_data(event);
    const int selected = requested < 0
        ? rotary_dial_selected_slot(&dial)
        : (int)requested;
    activate_item(selected);
}

static void update_keyboard(lv_timer_t *timer)
{
    (void)timer;
    if (!ring_dragging) {
        if (fabsf(spin_velocity) > 8.0f) {
            rotary_dial_rotate(
                &dial,
                spin_velocity * KEYBOARD_FRAME_MS / 1000.0f);
            spin_velocity *= 0.90f;
        } else {
            const float rotation = rotary_dial_yaw_degrees(&dial);
            const float target =
                roundf(rotation / SLOT_DEGREES) * SLOT_DEGREES;
            const float correction = target - rotation;
            if (fabsf(correction) > 0.03f) {
                rotary_dial_rotate(&dial, correction * 0.28f);
            } else {
                rotary_dial_rotate(&dial, correction);
                spin_velocity = 0.0f;
            }
        }
    }

    const float yaw = rotary_dial_yaw_degrees(&dial);
    const int selected = rotary_dial_selected_slot(&dial);

    const float rotation_radians =
        yaw * (float)M_PI / 180.0f;
    for (int index = 0; index < ITEM_COUNT; index++) {
        const float angle =
            -(float)M_PI * 0.5f +
            index * (2.0f * (float)M_PI / ITEM_COUNT) +
            rotation_radians;
        lv_obj_set_pos(
            letter_labels[index],
            (int)lroundf(
                CENTER_X + cosf(angle) * LETTER_RING_RADIUS - 18.0f),
            (int)lroundf(
                CENTER_Y + sinf(angle) * LETTER_RING_RADIUS -
                lv_font_get_line_height(&lv_font_montserrat_16) * 0.5f));
    }

    const float beam_target =
        fminf(fmaxf(-spin_velocity * 0.008f, -3.5f), 3.5f);
    beam_offset += (beam_target - beam_offset) * 0.16f;
    const float ambient =
        sinf((float)esp_timer_get_time() / 1000000.0f * 1.3f);
    for (int segment = 0; segment < BEAM_SEGMENTS; segment++) {
        const int start =
            (int)lroundf(258.0f + segment * 3.5f + beam_offset + ambient);
        lv_arc_set_bg_angles(beam_arcs[segment], start, start + 5);
        lv_obj_set_style_arc_opa(
            beam_arcs[segment],
            (lv_opa_t)(65 + (3 - abs(3 - segment)) * 36),
            LV_PART_MAIN);
    }

    if (selected != rendered_selection || items_dirty) {
        if (rendered_selection >= 0) {
            lv_obj_remove_flag(
                letter_labels[rendered_selection],
                LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(letter_labels[selected], LV_OBJ_FLAG_HIDDEN);
        char letter[2];
        lv_label_set_text(selected_label, item_text(selected, letter));
        rendered_selection = selected;
        items_dirty = false;
        review_offset = 0;
        render_center_text(selected);
    }
}

lv_obj_t *rotary_keyboard_create(lv_obj_t *parent)
{
    rotary_dial_init(&dial);

    keyboard_root = lv_obj_create(parent);
    lv_obj_remove_style_all(keyboard_root);
    lv_obj_set_size(keyboard_root, 466, 466);
    lv_obj_center(keyboard_root);
    lv_obj_remove_flag(keyboard_root, LV_OBJ_FLAG_SCROLLABLE);

    border_arc = lv_arc_create(keyboard_root);
    lv_obj_set_size(border_arc, 454, 454);
    lv_obj_center(border_arc);
    lv_arc_set_bg_angles(border_arc, 0, 360);
    lv_obj_remove_style(border_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(border_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(border_arc, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_color(border_arc, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(border_arc, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(border_arc, LV_OPA_TRANSP, LV_PART_INDICATOR);

    for (int segment = 0; segment < BEAM_SEGMENTS; segment++) {
        beam_arcs[segment] = lv_arc_create(keyboard_root);
        lv_obj_set_size(beam_arcs[segment], 454, 454);
        lv_obj_center(beam_arcs[segment]);
        lv_obj_remove_style(beam_arcs[segment], NULL, LV_PART_KNOB);
        lv_obj_remove_flag(beam_arcs[segment], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(beam_arcs[segment], 3, LV_PART_MAIN);
        lv_obj_set_style_arc_color(
            beam_arcs[segment],
            lv_color_white(),
            LV_PART_MAIN);
        lv_obj_set_style_arc_opa(
            beam_arcs[segment],
            LV_OPA_TRANSP,
            LV_PART_INDICATOR);
    }

    ring_touch = lv_obj_create(keyboard_root);
    lv_obj_remove_style_all(ring_touch);
    lv_obj_set_size(ring_touch, 466, 466);
    lv_obj_center(ring_touch);
    lv_obj_remove_flag(ring_touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(
        ring_touch,
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(
        ring_touch,
        ring_touch_event,
        LV_EVENT_ALL,
        NULL);

    for (int index = 0; index < ITEM_COUNT; index++) {
        letter_labels[index] = lv_label_create(keyboard_root);
        lv_obj_set_size(letter_labels[index], 36, 36);
        lv_obj_set_style_text_align(
            letter_labels[index],
            LV_TEXT_ALIGN_CENTER,
            0);
        lv_obj_add_flag(
            letter_labels[index],
            LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_event_cb(
            letter_labels[index],
            ring_touch_event,
            LV_EVENT_ALL,
            NULL);
        lv_obj_add_event_cb(
            letter_labels[index],
            item_clicked_event,
            LV_EVENT_CLICKED,
            (void *)(intptr_t)index);
    }
    refresh_item_labels();

    selected_label = lv_label_create(keyboard_root);
    lv_label_set_text(selected_label, "A");
    lv_obj_set_size(selected_label, 40, 40);
    lv_obj_set_style_text_align(selected_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(
        selected_label,
        &lv_font_montserrat_24,
        0);
    lv_obj_set_style_text_color(selected_label, lv_color_white(), 0);
    lv_obj_set_style_text_outline_stroke_color(
        selected_label,
        lv_color_white(),
        0);
    lv_obj_set_style_text_outline_stroke_width(selected_label, 1, 0);
    lv_obj_set_style_text_outline_stroke_opa(
        selected_label,
        LV_OPA_50,
        0);
    lv_obj_align(selected_label, LV_ALIGN_TOP_MID, 0, 33);
    lv_obj_add_flag(
        selected_label,
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(
        selected_label,
        ring_touch_event,
        LV_EVENT_ALL,
        NULL);
    lv_obj_add_event_cb(
        selected_label,
        item_clicked_event,
        LV_EVENT_CLICKED,
        (void *)(intptr_t)-1);

    center_view = lv_obj_create(keyboard_root);
    lv_obj_remove_style_all(center_view);
    lv_obj_set_size(center_view, CENTER_VIEW_WIDTH, CENTER_VIEW_HEIGHT);
    lv_obj_center(center_view);
    lv_obj_remove_flag(center_view, LV_OBJ_FLAG_SCROLLABLE);

    committed_label = lv_label_create(center_view);
    lv_obj_set_style_text_font(
        committed_label,
        &lv_font_montserrat_32,
        0);
    lv_obj_set_style_text_color(committed_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(committed_label, LV_TEXT_ALIGN_LEFT, 0);

    ghost_label = lv_label_create(center_view);
    lv_obj_set_style_text_font(ghost_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(
        ghost_label,
        lv_color_make(58, 58, 58),
        0);

    ellipsis_label = lv_label_create(center_view);
    lv_label_set_text(ellipsis_label, "...");
    lv_obj_set_style_text_font(
        ellipsis_label,
        &lv_font_montserrat_24,
        0);
    lv_obj_set_style_text_color(ellipsis_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(ellipsis_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ellipsis_label, LV_OPA_COVER, 0);
    lv_obj_align(ellipsis_label, LV_ALIGN_LEFT_MID, 0, 0);

    center_touch = lv_obj_create(keyboard_root);
    lv_obj_remove_style_all(center_touch);
    lv_obj_set_size(center_touch, TOUCH_WIDTH, TOUCH_HEIGHT);
    lv_obj_center(center_touch);
    lv_obj_remove_flag(center_touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(
        center_touch,
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(
        center_touch,
        center_touch_event,
        LV_EVENT_ALL,
        NULL);

    update_keyboard(NULL);
    keyboard_timer = lv_timer_create(
        update_keyboard,
        KEYBOARD_FRAME_MS,
        NULL);
    lv_timer_pause(keyboard_timer);
    lv_indev_t *input = lv_indev_get_next(NULL);
    if (input != NULL) {
        input_timer = lv_indev_get_read_timer(input);
    }
    lv_display_t *display = lv_display_get_default();
    if (display != NULL) {
        display_timer = lv_display_get_refr_timer(display);
    }
    lv_obj_add_flag(keyboard_root, LV_OBJ_FLAG_HIDDEN);
    return keyboard_root;
}

void rotary_keyboard_open(void)
{
    if (keyboard_root == NULL) {
        return;
    }
    if (rendered_selection >= 0) {
        lv_obj_remove_flag(
            letter_labels[rendered_selection],
            LV_OBJ_FLAG_HIDDEN);
    }
    rotary_dial_open(&dial);
    numeric_mode = false;
    caps_enabled = false;
    refresh_item_labels();
    rendered_selection = -1;
    spin_velocity = 0.0f;
    ring_dragging = false;
    ring_drag_moved = false;
    review_offset = 0;
    keyboard_open = true;
    lv_obj_remove_flag(keyboard_root, LV_OBJ_FLAG_HIDDEN);
    if (input_timer != NULL) {
        lv_timer_set_period(input_timer, KEYBOARD_INPUT_MS);
    }
    if (display_timer != NULL) {
        lv_timer_set_period(display_timer, KEYBOARD_FRAME_MS);
    }
    lv_timer_resume(keyboard_timer);
}

void rotary_keyboard_close(void)
{
    keyboard_open = false;
    if (keyboard_root != NULL) {
        lv_timer_pause(keyboard_timer);
        if (input_timer != NULL) {
            lv_timer_set_period(input_timer, LV_DEF_REFR_PERIOD);
        }
        if (display_timer != NULL) {
            lv_timer_set_period(display_timer, LV_DEF_REFR_PERIOD);
        }
        lv_obj_add_flag(keyboard_root, LV_OBJ_FLAG_HIDDEN);
    }
}

bool rotary_keyboard_is_open(void)
{
    return keyboard_open;
}

const char *rotary_keyboard_get_text(void)
{
    return committed_text;
}

void rotary_keyboard_clear(void)
{
    committed_text[0] = '\0';
    review_offset = 0;
    if (keyboard_root != NULL) {
        const int selected = rotary_dial_selected_slot(&dial);
        render_center_text(selected);
    }
    if (text_changed_callback != NULL) {
        text_changed_callback(committed_text, text_changed_user_data);
    }
}

void rotary_keyboard_on_text_changed(
    rotary_keyboard_text_callback_t callback,
    void *user_data)
{
    text_changed_callback = callback;
    text_changed_user_data = user_data;
}

void rotary_keyboard_on_submit(
    rotary_keyboard_text_callback_t callback,
    void *user_data)
{
    submit_callback = callback;
    submit_user_data = user_data;
}
