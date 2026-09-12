#include "thinking_orb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#define ORB_PARTICLE_COUNT 240
#define ORB_RADIUS 204.75f
#define ORB_SURFACE_SIZE 380
#define ORB_FRAME_MS 33
#define ORB_TRANSITION_SECONDS 0.72f

typedef struct {
    float x;
    float y;
    float z;
    float radius;
    float brightness;
} particle_t;

static lv_obj_t *surface;
static lv_timer_t *animation_timer;
static uint8_t *canvas_pixels;
static volatile thinking_orb_state_t requested_state = THINKING_ORB_SEARCHING;
static thinking_orb_state_t current_state = THINKING_ORB_SEARCHING;
static volatile float input_activity;
static volatile float output_activity;
static volatile bool live_session_active;
static bool animation_paused = true;
static bool speaking_active;
static uint32_t speaking_quiet_ms;
static float animation_time;
static float transition = 1.0f;
static uint32_t frame_period_ms = ORB_FRAME_MS;
static bool mapping_dirty;
static uint8_t particle_mapping[ORB_PARTICLE_COUNT];
static particle_t previous_particles[ORB_PARTICLE_COUNT];
static particle_t current_particles[ORB_PARTICLE_COUNT];
static particle_t painted_particles[ORB_PARTICLE_COUNT];
static const char *TAG = "thinking_orb";

static float clampf(float value, float minimum, float maximum)
{
    return fminf(fmaxf(value, minimum), maximum);
}

static float hashf(float a, float b)
{
    const float value = sinf(a * 12.9898f + b * 78.233f) * 43758.5453f;
    return value - floorf(value);
}

static void project(
    float x,
    float y,
    float z,
    float yaw,
    float tilt,
    particle_t *particle)
{
    const float sin_yaw = sinf(yaw);
    const float cos_yaw = cosf(yaw);
    const float sin_tilt = sinf(tilt);
    const float cos_tilt = cosf(tilt);
    const float rotated_x = x * cos_yaw + z * sin_yaw;
    const float rotated_z = -x * sin_yaw + z * cos_yaw;
    const float rotated_y = y * cos_tilt - rotated_z * sin_tilt;
    particle->x = rotated_x;
    particle->y = -rotated_y;
    particle->z = y * sin_tilt + rotated_z * cos_tilt;
}

static void sphere_point(int index, float *x, float *y, float *z)
{
    const float golden_angle = (float)M_PI * (3.0f - sqrtf(5.0f));
    *y = 1.0f - (2.0f * (index + 0.5f)) / ORB_PARTICLE_COUNT;
    const float radial = sqrtf(1.0f - *y * *y);
    const float angle = index * golden_angle;
    *x = radial * cosf(angle);
    *z = radial * sinf(angle);
}

static void generate_searching(float time, particle_t *particles)
{
    const float yaw = time * 0.5f;
    const float scan = time * 1.7f;
    for (int index = 0; index < ORB_PARTICLE_COUNT; index++) {
        float x;
        float y;
        float z;
        sphere_point(index, &x, &y, &z);
        project(
            x * ORB_RADIUS * 0.82f,
            y * ORB_RADIUS * 0.82f,
            z * ORB_RADIUS * 0.82f,
            yaw,
            0.4f + 0.06f * sinf(time * 0.35f),
            &particles[index]);
        const float depth = clampf(
            (particles[index].z / (ORB_RADIUS * 0.82f) + 1.0f) * 0.5f,
            0.0f,
            1.0f);
        const float longitude = atan2f(z, x);
        const float delta = atan2f(
            sinf(longitude + yaw - scan),
            cosf(longitude + yaw - scan));
        const float boost =
            expf(-(delta * delta) / 0.18f) * fmaxf(0.0f, particles[index].z);
        particles[index].radius = 1.1f + 2.5f * depth + boost * 0.02f;
        particles[index].brightness =
            clampf(0.34f + 0.56f * depth + boost * 0.01f, 0.0f, 1.0f);
    }
}

static void generate_idle(float time, particle_t *particles)
{
    generate_searching(time * 0.65f, particles);
    for (int index = 0; index < ORB_PARTICLE_COUNT; index++) {
        const float depth = clampf(
            (particles[index].z / (ORB_RADIUS * 0.82f) + 1.0f) * 0.5f,
            0.0f,
            1.0f);
        particles[index].radius = 1.0f + 2.2f * depth;
        particles[index].brightness = 0.22f + 0.46f * depth;
    }
}

static void generate_listening(float time, float activity, particle_t *particles)
{
    const float base_radius = ORB_RADIUS * 0.82f;
    const float response = activity * 0.14f;
    for (int index = 0; index < ORB_PARTICLE_COUNT; index++) {
        float x;
        float y;
        float z;
        sphere_point(index, &x, &y, &z);
        const float wave =
            sinf(y * 8.0f - time * 4.2f) *
            sinf(atan2f(z, x) * 3.0f + time * 2.1f);
        const float radius = base_radius * (1.0f + response * wave);
        project(
            x * radius,
            y * radius,
            z * radius,
            time * 0.45f,
            0.34f + 0.04f * sinf(time * 0.7f),
            &particles[index]);
        const float depth = clampf(
            (particles[index].z / (base_radius * 1.14f) + 1.0f) * 0.5f,
            0.0f,
            1.0f);
        const float reaction = activity * (0.35f + 0.65f * fabsf(wave));
        particles[index].radius = 1.1f + 2.5f * depth + reaction * 0.8f;
        particles[index].brightness =
            clampf(0.34f + 0.56f * depth + reaction * 0.18f, 0.0f, 1.0f);
    }
}

static void generate_working(float time, float activity, particle_t *particles)
{
    const int orbits = 8;
    const int points_per_orbit = ORB_PARTICLE_COUNT / orbits;
    for (int orbit = 0; orbit < orbits; orbit++) {
        const float h1 = hashf(orbit, 1.7f);
        const float h2 = hashf(orbit, 5.2f);
        const float h3 = hashf(orbit, 8.9f);
        const float orbit_radius = ORB_RADIUS * (0.42f + 0.48f * h1);
        const float theta = h1 * 2.0f * (float)M_PI;
        const float phi = acosf(2.0f * h2 - 1.0f);
        const float normal_x = sinf(phi) * cosf(theta);
        const float normal_y = cosf(phi);
        const float normal_z = sinf(phi) * sinf(theta);
        float basis_x = -normal_y;
        float basis_y = normal_x;
        const float basis_length =
            fmaxf(0.0001f, sqrtf(basis_x * basis_x + basis_y * basis_y));
        basis_x /= basis_length;
        basis_y /= basis_length;
        const float second_x = -normal_z * basis_y;
        const float second_y = normal_z * basis_x;
        const float second_z = normal_x * basis_y - normal_y * basis_x;
        const float speed = (0.25f + 0.55f * h3) * (h3 > 0.5f ? 1.0f : -1.0f);
        const float particle_angle = time * speed + h2 * 6.0f;

        for (int point = 0; point < points_per_orbit; point++) {
            const int index = orbit * points_per_orbit + point;
            const float angle =
                ((float)point / points_per_orbit) * 2.0f * (float)M_PI;
            const float cosine = cosf(angle);
            const float sine = sinf(angle);
            const float x =
                (basis_x * cosine + second_x * sine) * orbit_radius;
            const float y =
                (basis_y * cosine + second_y * sine) * orbit_radius;
            const float z = second_z * sine * orbit_radius;
            project(x, y, z, time * 0.12f, 0.3f, &particles[index]);
            const float depth = clampf(
                (particles[index].z / orbit_radius + 1.0f) * 0.5f,
                0.0f,
                1.0f);
            const float delta = atan2f(
                sinf(angle - particle_angle),
                cosf(angle - particle_angle));
            const float runner = expf(-(delta * delta) / 0.08f);
            particles[index].radius =
                1.2f + runner * (2.8f + activity * 2.0f) + depth;
            particles[index].brightness =
                clampf(0.22f + depth * 0.35f + runner * 0.55f, 0.0f, 1.0f);
        }
    }
}

static void rotate_slice(float *x, float *y, float *z, int axis, float angle)
{
    const float cosine = cosf(angle);
    const float sine = sinf(angle);
    if (axis == 0) {
        const float next_y = *y * cosine - *z * sine;
        *z = *y * sine + *z * cosine;
        *y = next_y;
    } else if (axis == 1) {
        const float next_x = *x * cosine + *z * sine;
        *z = -*x * sine + *z * cosine;
        *x = next_x;
    } else {
        const float next_x = *x * cosine - *y * sine;
        *y = *x * sine + *y * cosine;
        *x = next_x;
    }
}

static void generate_solving(float time, float activity, particle_t *particles)
{
    const int active_axis = ((int)floorf(time * 1.4f)) % 3;
    const int active_band = ((int)floorf(time * 0.7f)) % 4;
    const float band_low = -1.0f + active_band * 0.5f;
    const float turn =
        sinf(time * 2.2f) * (float)M_PI * (0.30f + activity * 0.20f);
    const float response_scale =
        1.0f + activity * 0.08f * (0.5f + 0.5f * sinf(time * 5.0f));
    for (int index = 0; index < ORB_PARTICLE_COUNT; index++) {
        float x;
        float y;
        float z;
        sphere_point(index, &x, &y, &z);
        const float coordinate = active_axis == 0 ? x : active_axis == 1 ? y : z;
        const bool active = coordinate >= band_low && coordinate < band_low + 0.5f;
        if (active) {
            rotate_slice(&x, &y, &z, active_axis, turn);
        }
        project(
            x * ORB_RADIUS * 0.82f * response_scale,
            y * ORB_RADIUS * 0.82f * response_scale,
            z * ORB_RADIUS * 0.82f * response_scale,
            time * 0.55f,
            0.35f + 0.1f * sinf(time * 0.9f),
            &particles[index]);
        const float depth = clampf(
            (particles[index].z / (ORB_RADIUS * 0.82f) + 1.0f) * 0.5f,
            0.0f,
            1.0f);
        particles[index].radius =
            1.1f + 2.6f * depth + (active ? 0.5f : 0.0f) + activity * 0.6f;
        particles[index].brightness =
            clampf(
                0.34f + 0.56f * depth + (active ? 0.1f : 0.0f) +
                    activity * 0.12f,
                0.0f,
                1.0f);
    }
}

static void generate_breathing(float time, particle_t *particles)
{
    const int lanes = 6;
    const int segments = ORB_PARTICLE_COUNT / lanes;
    const float pulse = 1.0f + 0.035f * sinf(time * 1.3f);
    for (int lane = 0; lane < lanes; lane++) {
        const float lane_center = (lanes - 1) * 0.5f;
        const float lane_offset = (lane - lane_center) * 0.045f;
        const float edge = fabsf(lane - lane_center) / lane_center;
        for (int segment = 0; segment < segments; segment++) {
            const int index = lane * segments + segment;
            const float angle =
                ((float)segment / segments) * 2.0f * (float)M_PI;
            const float wobble =
                0.035f * sinf(angle * 3.0f - time * 0.9f + lane * 0.25f);
            const float radial =
                ORB_RADIUS * 0.72f * pulse * (1.0f + lane_offset + wobble);
            particles[index].x = cosf(angle) * radial;
            particles[index].y = sinf(angle) * radial;
            particles[index].z = lane;
            particles[index].radius = (1.3f + edge * 0.2f) * pulse;
            particles[index].brightness = 0.28f + (1.0f - edge) * 0.18f;
        }
    }
}

static void generate_particles(
    thinking_orb_state_t state,
    float time,
    float activity,
    particle_t *particles)
{
    switch (state) {
    case THINKING_ORB_IDLE:
        generate_idle(time, particles);
        break;
    case THINKING_ORB_SEARCHING:
        generate_searching(time, particles);
        break;
    case THINKING_ORB_COMPOSING:
        generate_listening(time, activity, particles);
        break;
    case THINKING_ORB_WORKING:
        generate_working(time, activity, particles);
        break;
    case THINKING_ORB_SOLVING:
        generate_solving(time, activity, particles);
        break;
    case THINKING_ORB_BREATHING:
        generate_breathing(time, particles);
        break;
    }
}

static void build_nearest_mapping(void)
{
    bool used[ORB_PARTICLE_COUNT] = {false};
    for (int target = 0; target < ORB_PARTICLE_COUNT; target++) {
        float nearest_distance = INFINITY;
        int nearest_source = 0;
        for (int source = 0; source < ORB_PARTICLE_COUNT; source++) {
            if (used[source]) {
                continue;
            }
            const float dx = previous_particles[source].x - current_particles[target].x;
            const float dy = previous_particles[source].y - current_particles[target].y;
            const float distance = dx * dx + dy * dy;
            if (distance < nearest_distance) {
                nearest_distance = distance;
                nearest_source = source;
            }
        }
        particle_mapping[target] = nearest_source;
        used[nearest_source] = true;
    }
}

static void draw_particle(const particle_t *particle)
{
    const float radius = clampf(particle->radius * 0.82f, 0.8f, 4.2f);
    const int extent = (int)ceilf(radius + 0.5f);
    const int center_x =
        (int)lroundf(ORB_SURFACE_SIZE * 0.5f + particle->x);
    const int center_y =
        (int)lroundf(ORB_SURFACE_SIZE * 0.5f + particle->y);
    const uint8_t gray =
        (uint8_t)lroundf(
            clampf(particle->brightness * 1.25f, 0.0f, 1.0f) * 255.0f);

    const float inner_radius_squared =
        (radius - 0.5f) * (radius - 0.5f);
    const float outer_radius_squared =
        (radius + 0.5f) * (radius + 0.5f);
    const float edge_span =
        fmaxf(0.001f, outer_radius_squared - inner_radius_squared);

    for (int offset_y = -extent; offset_y <= extent; offset_y++) {
        const int y = center_y + offset_y;
        if (y < 0 || y >= ORB_SURFACE_SIZE) {
            continue;
        }
        for (int offset_x = -extent; offset_x <= extent; offset_x++) {
            const float distance_squared =
                offset_x * offset_x + offset_y * offset_y;
            const float coverage = clampf(
                (outer_radius_squared - distance_squared) / edge_span,
                0.0f,
                1.0f);
            if (coverage <= 0.0f) {
                continue;
            }
            const int x = center_x + offset_x;
            if (x < 0 || x >= ORB_SURFACE_SIZE) {
                continue;
            }
            uint8_t *pixel =
                &canvas_pixels[y * ORB_SURFACE_SIZE + x];
            const uint8_t antialiased_gray =
                (uint8_t)lroundf(gray * coverage);
            if (antialiased_gray > *pixel) {
                *pixel = antialiased_gray;
            }
        }
    }
}

static void render_canvas(void)
{
    const float activity =
        current_state == THINKING_ORB_SOLVING ? output_activity : input_activity;

    generate_particles(current_state, animation_time, activity, current_particles);
    if (mapping_dirty) {
        build_nearest_mapping();
        mapping_dirty = false;
    }

    const float eased = transition * transition * (3.0f - 2.0f * transition);
    for (int index = 0; index < ORB_PARTICLE_COUNT; index++) {
        const particle_t *to = &current_particles[index];
        if (transition < 1.0f) {
            const particle_t *from =
                &previous_particles[particle_mapping[index]];
            painted_particles[index] = (particle_t) {
                .x = from->x + (to->x - from->x) * eased,
                .y = from->y + (to->y - from->y) * eased,
                .z = from->z + (to->z - from->z) * eased,
                .radius = from->radius + (to->radius - from->radius) * eased,
                .brightness =
                    from->brightness +
                    (to->brightness - from->brightness) * eased,
            };
        } else {
            painted_particles[index] = *to;
        }
    }
    memset(
        canvas_pixels,
        0,
        ORB_SURFACE_SIZE * ORB_SURFACE_SIZE);
    for (int index = 0; index < ORB_PARTICLE_COUNT; index++) {
        draw_particle(&painted_particles[index]);
    }
    lv_draw_buf_flush_cache(lv_canvas_get_draw_buf(surface), NULL);
    lv_obj_invalidate(surface);
}

static void timer_callback(lv_timer_t *timer)
{
    const int64_t frame_started = esp_timer_get_time();
    thinking_orb_state_t target = requested_state;
    if (live_session_active && target == THINKING_ORB_COMPOSING) {
        if (output_activity > 0.045f) {
            speaking_active = true;
            speaking_quiet_ms = 0;
        } else if (speaking_active && output_activity < 0.015f) {
            speaking_quiet_ms += frame_period_ms;
            if (speaking_quiet_ms >= 240) {
                speaking_active = false;
                speaking_quiet_ms = 0;
            }
        }
        if (speaking_active) {
            target = THINKING_ORB_SOLVING;
        }
    } else {
        speaking_active = false;
        speaking_quiet_ms = 0;
    }
    if (target != current_state) {
        memcpy(
            previous_particles,
            painted_particles,
            sizeof(previous_particles));
        current_state = target;
        transition = 0.0f;
        mapping_dirty = true;
    }
    if (transition < 1.0f) {
        transition = fminf(
            1.0f,
            transition +
                ((float)frame_period_ms / 1000.0f) /
                    ORB_TRANSITION_SECONDS);
    }

    float speed = 1.0f;
    if (current_state == THINKING_ORB_IDLE) {
        speed = 1.2f;
    } else if (current_state == THINKING_ORB_SEARCHING) {
        speed = 2.015f;
    } else if (current_state == THINKING_ORB_COMPOSING) {
        speed = 2.34f;
    } else if (current_state == THINKING_ORB_WORKING) {
        speed = 1.885f;
    } else if (current_state == THINKING_ORB_SOLVING) {
        speed = 1.82f;
    } else if (current_state == THINKING_ORB_BREATHING) {
        speed = 1.0f;
    }
    animation_time += ((float)frame_period_ms / 1000.0f) * speed;
    render_canvas();

    const uint32_t render_ms =
        (uint32_t)((esp_timer_get_time() - frame_started + 999) / 1000);
    if (
        transition >= 1.0f &&
        render_ms + 3 > frame_period_ms) {
        frame_period_ms = render_ms + 3;
        lv_timer_set_period(timer, frame_period_ms);
        ESP_LOGI(
            TAG,
            "Adjusted orb frame interval to %lu ms",
            (unsigned long)frame_period_ms);
    }
}

lv_obj_t *thinking_orb_create(lv_obj_t *parent)
{
    canvas_pixels = heap_caps_malloc(
        ORB_SURFACE_SIZE * ORB_SURFACE_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (canvas_pixels == NULL) {
        return NULL;
    }
    memset(canvas_pixels, 0, ORB_SURFACE_SIZE * ORB_SURFACE_SIZE);

    surface = lv_canvas_create(parent);
    lv_canvas_set_buffer(
        surface,
        canvas_pixels,
        ORB_SURFACE_SIZE,
        ORB_SURFACE_SIZE,
        LV_COLOR_FORMAT_L8);
    lv_obj_center(surface);
    lv_obj_remove_flag(surface, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(surface, LV_OBJ_FLAG_CLICKABLE);
    for (int index = 0; index < ORB_PARTICLE_COUNT; index++) {
        particle_mapping[index] = index;
    }
    return surface;
}

void thinking_orb_start(void)
{
    if (animation_timer == NULL) {
        animation_timer = lv_timer_create(timer_callback, ORB_FRAME_MS, NULL);
        if (animation_paused) {
            lv_timer_pause(animation_timer);
        }
    }
}

void thinking_orb_set_paused(bool paused)
{
    animation_paused = paused;
    if (animation_timer == NULL) {
        return;
    }
    if (paused) {
        lv_timer_pause(animation_timer);
    } else {
        lv_timer_resume(animation_timer);
    }
}

void thinking_orb_set_state(thinking_orb_state_t state)
{
    requested_state = state;
}

thinking_orb_state_t thinking_orb_get_state(void)
{
    return requested_state;
}

void thinking_orb_set_audio_levels(
    float input_level,
    float output_level,
    bool session_active)
{
    input_activity = clampf((input_level - 0.003f) * 8.0f, 0.0f, 1.0f);
    output_activity = clampf((output_level - 0.008f) * 3.0f, 0.0f, 1.0f);
    live_session_active = session_active;
}
