#include "app_home.h"

#include <math.h>

#define APP_ICON_COUNT 12
#define HOME_FRAME_MS 40
#define HOME_SPHERE_RADIUS 165.0f

typedef struct {
    lv_obj_t *button;
    lv_obj_t *label;
    lv_obj_t *microphone_body;
    lv_obj_t *microphone_stem;
    lv_obj_t *microphone_base;
    app_home_launch_callback_t launch;
    void *user_data;
    float x;
    float y;
    float z;
} app_icon_t;

static lv_obj_t *home_root;
static lv_timer_t *rotation_timer;
static app_icon_t app_icons[APP_ICON_COUNT];
static float rotation;
static bool home_visible = true;
static bool rotation_started;

static void app_icon_clicked(lv_event_t *event)
{
    app_icon_t *icon = lv_event_get_user_data(event);
    if (icon != NULL && icon->launch != NULL) {
        icon->launch(icon->user_data);
    }
}

static void update_icon_positions(lv_timer_t *timer)
{
    (void)timer;
    rotation += 0.012f;
    const float sine = sinf(rotation);
    const float cosine = cosf(rotation);
    const float tilt_sine = sinf(0.24f);
    const float tilt_cosine = cosf(0.24f);

    for (int index = 0; index < APP_ICON_COUNT; index++) {
        const float rotated_x =
            app_icons[index].x * cosine + app_icons[index].z * sine;
        const float rotated_z =
            -app_icons[index].x * sine + app_icons[index].z * cosine;
        const float rotated_y =
            app_icons[index].y * tilt_cosine - rotated_z * tilt_sine;
        const float depth =
            fminf(fmaxf((rotated_z + 1.0f) * 0.5f, 0.0f), 1.0f);
        const int size = (int)lroundf(34.0f + depth * 48.0f);
        const lv_opa_t opacity =
            (lv_opa_t)lroundf(45.0f + depth * 210.0f);
        const lv_color_t shade =
            lv_color_make(opacity, opacity, opacity);

        lv_obj_set_size(app_icons[index].button, size, size);
        lv_obj_set_pos(
            app_icons[index].button,
            (int)lroundf(233.0f + rotated_x * HOME_SPHERE_RADIUS - size * 0.5f),
            (int)lroundf(233.0f + rotated_y * HOME_SPHERE_RADIUS - size * 0.5f));
        lv_obj_set_style_border_color(app_icons[index].button, shade, 0);
        lv_obj_set_style_border_opa(app_icons[index].button, opacity, 0);
        if (app_icons[index].label != NULL) {
            lv_obj_set_style_text_color(app_icons[index].label, shade, 0);
            lv_obj_set_style_text_opa(app_icons[index].label, opacity, 0);
        } else {
            lv_obj_set_style_border_color(
                app_icons[index].microphone_body,
                shade,
                0);
            lv_obj_set_style_border_opa(
                app_icons[index].microphone_body,
                opacity,
                0);
            lv_obj_set_style_bg_color(
                app_icons[index].microphone_stem,
                shade,
                0);
            lv_obj_set_style_bg_opa(
                app_icons[index].microphone_stem,
                opacity,
                0);
            lv_obj_set_style_bg_color(
                app_icons[index].microphone_base,
                shade,
                0);
            lv_obj_set_style_bg_opa(
                app_icons[index].microphone_base,
                opacity,
                0);
        }
    }
}

lv_obj_t *app_home_create(
    lv_obj_t *parent,
    const app_home_entry_t *entries,
    size_t entry_count)
{
    if (entries == NULL || entry_count == 0 || entry_count > APP_ICON_COUNT) {
        return NULL;
    }

    home_root = lv_obj_create(parent);
    lv_obj_remove_style_all(home_root);
    lv_obj_set_size(home_root, 466, 466);
    lv_obj_center(home_root);
    lv_obj_remove_flag(home_root, LV_OBJ_FLAG_SCROLLABLE);

    const float golden_angle = (float)M_PI * (3.0f - sqrtf(5.0f));
    for (int index = 0; index < APP_ICON_COUNT; index++) {
        if (index == 0) {
            app_icons[index].x = 0.0f;
            app_icons[index].y = 0.0f;
            app_icons[index].z = 1.0f;
        } else {
            const float point = (float)(index - 1) + 0.5f;
            const float count = APP_ICON_COUNT - 1;
            app_icons[index].y = 1.0f - 2.0f * point / count;
            const float radial =
                sqrtf(1.0f - app_icons[index].y * app_icons[index].y);
            const float angle = (index - 1) * golden_angle + 0.8f;
            app_icons[index].x = radial * cosf(angle);
            app_icons[index].z = radial * sinf(angle);
        }

        app_icons[index].launch =
            index < entry_count ? entries[index].launch : NULL;
        app_icons[index].user_data =
            index < entry_count ? entries[index].user_data : NULL;
        app_icons[index].button = lv_obj_create(home_root);
        lv_obj_remove_flag(app_icons[index].button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(
            app_icons[index].button,
            LV_RADIUS_CIRCLE,
            0);
        lv_obj_set_style_bg_color(
            app_icons[index].button,
            lv_color_black(),
            0);
        lv_obj_set_style_bg_opa(
            app_icons[index].button,
            LV_OPA_COVER,
            0);
        lv_obj_set_style_border_width(app_icons[index].button, 2, 0);
        lv_obj_set_style_pad_all(app_icons[index].button, 0, 0);
        lv_obj_set_style_shadow_width(
            app_icons[index].button,
            app_icons[index].launch != NULL ? 18 : 0,
            0);
        lv_obj_set_style_shadow_color(
            app_icons[index].button,
            lv_color_white(),
            0);
        lv_obj_set_style_shadow_opa(
            app_icons[index].button,
            app_icons[index].launch != NULL ? LV_OPA_50 : LV_OPA_TRANSP,
            0);

        if (
            index < entry_count &&
            entries[index].icon_type == APP_HOME_ICON_MICROPHONE) {
            app_icons[index].microphone_body =
                lv_obj_create(app_icons[index].button);
            lv_obj_remove_style_all(app_icons[index].microphone_body);
            lv_obj_remove_flag(
                app_icons[index].microphone_body,
                LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(app_icons[index].microphone_body, 12, 22);
            lv_obj_set_style_radius(
                app_icons[index].microphone_body,
                LV_RADIUS_CIRCLE,
                0);
            lv_obj_set_style_border_width(
                app_icons[index].microphone_body,
                2,
                0);
            lv_obj_align(
                app_icons[index].microphone_body,
                LV_ALIGN_CENTER,
                0,
                -5);

            app_icons[index].microphone_stem =
                lv_obj_create(app_icons[index].button);
            lv_obj_remove_style_all(app_icons[index].microphone_stem);
            lv_obj_remove_flag(
                app_icons[index].microphone_stem,
                LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(app_icons[index].microphone_stem, 2, 9);
            lv_obj_align(
                app_icons[index].microphone_stem,
                LV_ALIGN_CENTER,
                0,
                10);

            app_icons[index].microphone_base =
                lv_obj_create(app_icons[index].button);
            lv_obj_remove_style_all(app_icons[index].microphone_base);
            lv_obj_remove_flag(
                app_icons[index].microphone_base,
                LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(app_icons[index].microphone_base, 15, 2);
            lv_obj_align(
                app_icons[index].microphone_base,
                LV_ALIGN_CENTER,
                0,
                14);
        } else {
            app_icons[index].label = lv_label_create(app_icons[index].button);
            lv_label_set_text(
                app_icons[index].label,
                index < entry_count && entries[index].symbol != NULL
                    ? entries[index].symbol
                    : LV_SYMBOL_PLUS);
            lv_obj_set_style_text_font(
                app_icons[index].label,
                &lv_font_montserrat_24,
                0);
            lv_obj_center(app_icons[index].label);
        }

        if (app_icons[index].launch != NULL) {
            lv_obj_add_event_cb(
                app_icons[index].button,
                app_icon_clicked,
                LV_EVENT_CLICKED,
                &app_icons[index]);
        } else {
            lv_obj_remove_flag(
                app_icons[index].button,
                LV_OBJ_FLAG_CLICKABLE);
        }
    }

    update_icon_positions(NULL);
    rotation_timer = lv_timer_create(
        update_icon_positions,
        HOME_FRAME_MS,
        NULL);
    lv_timer_pause(rotation_timer);
    return home_root;
}

void app_home_start(void)
{
    rotation_started = true;
    if (rotation_timer != NULL && home_visible) {
        lv_timer_resume(rotation_timer);
    }
}

void app_home_set_visible(bool visible)
{
    if (home_root == NULL || rotation_timer == NULL) {
        return;
    }
    home_visible = visible;
    if (visible) {
        lv_obj_remove_flag(home_root, LV_OBJ_FLAG_HIDDEN);
        if (rotation_started) {
            lv_timer_resume(rotation_timer);
        }
    } else {
        lv_obj_add_flag(home_root, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(rotation_timer);
    }
}
