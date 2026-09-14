#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "app_home.h"
#include "axp2101.h"
#include "battery_indicator.h"
#include "bsp/esp-bsp.h"
#include "device_config.h"
#include "esp_codec_dev.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "quick_settings.h"
#include "rotary_keyboard.h"
#include "thinking_orb.h"

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHANNELS 1
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_FRAME_MS 20
#define AUDIO_FRAME_BYTES \
    (AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_FRAME_MS / 1000)
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WEBSOCKET_CONNECTED_BIT BIT2
#define WIFI_MAX_RETRIES 10
#define PLAYBACK_QUEUE_DEPTH 12
#define PLAYBACK_PREBUFFER_FRAMES 3
#define PLAYBACK_GAP_TIMEOUT_MS 30
#define PLAYBACK_PREBUFFER_TIMEOUT_MS 60
#define INITIAL_SPEAKER_VOLUME 100
#define PLAYBACK_GAIN 2
#define POWER_BUTTON_POLL_MS 100
#define BATTERY_POLL_MS 5000

typedef struct {
    size_t length;
    uint8_t data[AUDIO_FRAME_BYTES];
} playback_frame_t;

typedef enum {
    ACTIVE_APP_HOME,
    ACTIVE_APP_VOICE,
    ACTIVE_APP_KEYBOARD,
} active_app_t;

static const char *TAG = "circle_voice";
static EventGroupHandle_t connection_events;
static QueueHandle_t playback_queue;
static esp_websocket_client_handle_t websocket;
static esp_codec_dev_handle_t microphone;
static esp_codec_dev_handle_t speaker;
static axp2101_t pmu;
static lv_obj_t *orb_surface;
static lv_obj_t *close_control;
static lv_obj_t *close_label;
static lv_timer_t *close_timeout_timer;
static int wifi_retry_count;
static volatile uint32_t websocket_generation;
static volatile float microphone_level;
static volatile float playback_level;
static volatile bool live_session_active;
static volatile bool live_toggle_requested;
static volatile bool voice_session_requested;
static active_app_t active_app = ACTIVE_APP_HOME;
static bool close_control_armed;
static playback_frame_t playback_assembly;
static size_t playback_expected_length;
static uint32_t dropped_playback_frames;

static void orb_click_event(lv_event_t *event);
static void open_voice_app(void *user_data);
static void open_keyboard_app(void *user_data);
static void open_quick_settings(void *user_data);
static void close_quick_settings(void *user_data);
static void close_control_clicked(lv_event_t *event);

static const app_home_entry_t home_apps[] = {
    {APP_HOME_ICON_MICROPHONE, NULL, open_voice_app, NULL},
    {APP_HOME_ICON_SYMBOL, LV_SYMBOL_KEYBOARD, open_keyboard_app, NULL},
    {APP_HOME_ICON_SYMBOL, LV_SYMBOL_WIFI, NULL, NULL},
    {APP_HOME_ICON_SYMBOL, LV_SYMBOL_SETTINGS, open_quick_settings, NULL},
    {APP_HOME_ICON_SYMBOL, LV_SYMBOL_HOME, NULL, NULL},
    {APP_HOME_ICON_SYMBOL, LV_SYMBOL_VIDEO, NULL, NULL},
    {APP_HOME_ICON_SYMBOL, LV_SYMBOL_BELL, NULL, NULL},
    {APP_HOME_ICON_SYMBOL, LV_SYMBOL_EDIT, NULL, NULL},
    {APP_HOME_ICON_SYMBOL, LV_SYMBOL_LIST, NULL, NULL},
    {APP_HOME_ICON_SYMBOL, LV_SYMBOL_EYE_OPEN, NULL, NULL},
    {APP_HOME_ICON_SYMBOL, LV_SYMBOL_REFRESH, NULL, NULL},
    {APP_HOME_ICON_SYMBOL, LV_SYMBOL_POWER, NULL, NULL},
};

static void set_close_control_armed(bool armed)
{
    close_control_armed = armed;
    if (armed) {
        lv_obj_set_size(close_control, 48, 48);
        lv_obj_align(close_control, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_style_radius(close_control, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(close_control, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(close_control, LV_OPA_70, 0);
        lv_obj_set_style_border_color(close_control, lv_color_white(), 0);
        lv_obj_set_style_border_opa(close_control, LV_OPA_70, 0);
        lv_obj_set_style_border_width(close_control, 1, 0);
        lv_obj_set_style_text_opa(close_label, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_size(close_control, 466, 56);
        lv_obj_align(close_control, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(close_control, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(close_control, 0, 0);
        lv_obj_set_style_text_opa(close_label, LV_OPA_TRANSP, 0);
    }
}

static void close_timeout(lv_timer_t *timer)
{
    set_close_control_armed(false);
    lv_timer_pause(timer);
}

static void open_voice_app(void *user_data)
{
    (void)user_data;
    active_app = ACTIVE_APP_VOICE;
    app_home_set_visible(false);
    lv_obj_remove_flag(orb_surface, LV_OBJ_FLAG_HIDDEN);
    thinking_orb_set_paused(false);
    set_close_control_armed(false);
    lv_obj_remove_flag(close_control, LV_OBJ_FLAG_HIDDEN);
}

static void open_keyboard_app(void *user_data)
{
    (void)user_data;
    active_app = ACTIVE_APP_KEYBOARD;
    app_home_set_visible(false);
    rotary_keyboard_open();
    set_close_control_armed(false);
    lv_obj_remove_flag(close_control, LV_OBJ_FLAG_HIDDEN);
}

static void open_quick_settings(void *user_data)
{
    (void)user_data;
    app_home_set_visible(false);
    lv_obj_add_flag(close_control, LV_OBJ_FLAG_HIDDEN);
    quick_settings_open();
}

static void close_quick_settings(void *user_data)
{
    (void)user_data;
    quick_settings_close();
    app_home_set_visible(true);
}

static void return_to_home(void)
{
    if (
        active_app == ACTIVE_APP_VOICE &&
        voice_session_requested) {
        voice_session_requested = false;
        live_toggle_requested = true;
    }
    if (active_app == ACTIVE_APP_KEYBOARD) {
        rotary_keyboard_close();
    }
    active_app = ACTIVE_APP_HOME;
    set_close_control_armed(false);
    lv_timer_pause(close_timeout_timer);
    lv_obj_add_flag(close_control, LV_OBJ_FLAG_HIDDEN);
    thinking_orb_set_paused(true);
    lv_obj_add_flag(orb_surface, LV_OBJ_FLAG_HIDDEN);
    app_home_set_visible(true);
}

static void power_off_device(void)
{
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGE(TAG, "Unable to lock display for shutdown");
        return;
    }

    return_to_home();
    quick_settings_close();
    app_home_set_visible(false);
    battery_indicator_set_visible(false);
    xQueueReset(playback_queue);
    playback_level = 0.0f;
    const int volume_result = esp_codec_dev_set_out_vol(speaker, 0);
    if (volume_result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Unable to mute speaker for shutdown: %d", volume_result);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_display_brightness_set(0));
    bsp_display_unlock();

    ESP_LOGI(TAG, "Powering off; press PWR to cold boot");
    vTaskDelay(pdMS_TO_TICKS(50));
    const esp_err_t result = axp2101_power_off(&pmu);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Hardware power-off failed: %s", esp_err_to_name(result));
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGE(TAG, "Hardware power-off did not complete; restoring display");
    if (bsp_display_lock(1000) == ESP_OK) {
        app_home_set_visible(true);
        battery_indicator_set_visible(true);
        ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_display_brightness_set(80));
        bsp_display_unlock();
    }
    const int restore_result =
        esp_codec_dev_set_out_vol(speaker, INITIAL_SPEAKER_VOLUME);
    if (restore_result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Unable to restore speaker: %d", restore_result);
    }
}

static void close_control_clicked(lv_event_t *event)
{
    (void)event;
    if (close_control_armed) {
        return_to_home();
        return;
    }

    set_close_control_armed(true);
    lv_timer_reset(close_timeout_timer);
    lv_timer_resume(close_timeout_timer);
}

static void set_orb_state(thinking_orb_state_t state)
{
    thinking_orb_set_state(state);
}

static esp_err_t start_display(void)
{
    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "Display initialization failed");
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(
        bsp_display_lock(1000),
        TAG,
        "Unable to lock display");

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    orb_surface = thinking_orb_create(screen);
    if (orb_surface == NULL) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "Unable to allocate Thinking Orb canvas");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_event_cb(orb_surface, orb_click_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(orb_surface, LV_OBJ_FLAG_HIDDEN);

    if (rotary_keyboard_create(screen) == NULL) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "Unable to create rotary keyboard");
        return ESP_ERR_NO_MEM;
    }

    if (app_home_create(
            screen,
            home_apps,
            sizeof(home_apps) / sizeof(home_apps[0])) == NULL) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "Unable to create app home");
        return ESP_ERR_NO_MEM;
    }

    close_control = lv_obj_create(screen);
    lv_obj_remove_flag(close_control, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(
        close_control,
        close_control_clicked,
        LV_EVENT_CLICKED,
        NULL);
    close_label = lv_label_create(close_control);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_center(close_label);
    set_close_control_armed(false);
    lv_obj_add_flag(close_control, LV_OBJ_FLAG_HIDDEN);
    close_timeout_timer = lv_timer_create(close_timeout, 3000, NULL);
    lv_timer_pause(close_timeout_timer);

    if (battery_indicator_create(screen) == NULL) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "Unable to create battery indicator");
        return ESP_ERR_NO_MEM;
    }

    if (quick_settings_create(
            screen,
            close_quick_settings,
            NULL) == NULL) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "Unable to create quick settings");
        return ESP_ERR_NO_MEM;
    }

    bsp_display_unlock();
    return bsp_display_brightness_set(80);
}

static void wifi_event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        set_orb_state(THINKING_ORB_SEARCHING);
        ESP_ERROR_CHECK(esp_wifi_connect());
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(
            connection_events,
            WIFI_CONNECTED_BIT | WEBSOCKET_CONNECTED_BIT);
        set_orb_state(THINKING_ORB_SEARCHING);
        if (wifi_retry_count < WIFI_MAX_RETRIES) {
            wifi_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi disconnected; retry %d/%d", wifi_retry_count, WIFI_MAX_RETRIES);
            ESP_ERROR_CHECK(esp_wifi_connect());
        } else {
            xEventGroupSetBits(connection_events, WIFI_FAILED_BIT);
            set_orb_state(THINKING_ORB_BREATHING);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = event_data;
        wifi_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi connected with IP " IPSTR, IP2STR(&got_ip->ip_info.ip));
        set_orb_state(THINKING_ORB_SEARCHING);
        xEventGroupSetBits(connection_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t connect_wifi(void)
{
    if (strlen(CIRCLE_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "Set CIRCLE_WIFI_SSID and CIRCLE_WIFI_PASSWORD in device_config.h");
        return ESP_ERR_INVALID_STATE;
    }

    connection_events = xEventGroupCreate();
    if (connection_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Network interface initialization failed");
    ESP_RETURN_ON_ERROR(
        esp_event_loop_create_default(),
        TAG,
        "Default event loop initialization failed");
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&initialization), TAG, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
        TAG,
        "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
        TAG,
        "IP event registration failed");

    wifi_config_t configuration = {0};
    strlcpy(
        (char *)configuration.sta.ssid,
        CIRCLE_WIFI_SSID,
        sizeof(configuration.sta.ssid));
    strlcpy(
        (char *)configuration.sta.password,
        CIRCLE_WIFI_PASSWORD,
        sizeof(configuration.sta.password));
    configuration.sta.threshold.authmode = WIFI_AUTH_OPEN;
    configuration.sta.pmf_cfg.capable = true;
    configuration.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode setup failed");
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_config(WIFI_IF_STA, &configuration),
        TAG,
        "Wi-Fi station configuration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");

    const EventBits_t bits = xEventGroupWaitBits(
        connection_events,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "Unable to connect to Wi-Fi after %d attempts", WIFI_MAX_RETRIES);
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_ps(WIFI_PS_NONE),
        TAG,
        "Unable to disable Wi-Fi power saving");
    return ESP_OK;
}

static bool payload_contains(
    const char *payload,
    size_t payload_length,
    const char *needle)
{
    const size_t needle_length = strlen(needle);
    if (needle_length == 0 || needle_length > payload_length) {
        return false;
    }

    for (size_t offset = 0; offset <= payload_length - needle_length; offset++) {
        if (memcmp(payload + offset, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static void queue_playback_frame(const esp_websocket_event_data_t *data)
{
    if (playback_queue == NULL) {
        ESP_LOGE(TAG, "Received audio before playback queue initialization");
        return;
    }
    if (
        data->data_len <= 0 ||
        data->payload_len <= 0 ||
        data->payload_len > AUDIO_FRAME_BYTES ||
        data->payload_len % 2 != 0) {
        ESP_LOGE(TAG, "Invalid playback message length: %d", data->payload_len);
        playback_expected_length = 0;
        return;
    }

    if (data->payload_offset == 0) {
        playback_assembly.length = 0;
        playback_expected_length = (size_t)data->payload_len;
    }
    if (
        playback_expected_length == 0 ||
        (size_t)data->payload_offset != playback_assembly.length ||
        playback_assembly.length + (size_t)data->data_len >
            playback_expected_length) {
        ESP_LOGE(
            TAG,
            "Invalid playback fragment offset=%d length=%d total=%d",
            data->payload_offset,
            data->data_len,
            data->payload_len);
        playback_expected_length = 0;
        return;
    }

    memcpy(
        playback_assembly.data + playback_assembly.length,
        data->data_ptr,
        (size_t)data->data_len);
    playback_assembly.length += (size_t)data->data_len;
    if (playback_assembly.length < playback_expected_length) {
        return;
    }

    playback_frame_t frame = playback_assembly;
    playback_expected_length = 0;

    if (xQueueSend(playback_queue, &frame, 0) == pdPASS) {
        return;
    }

    playback_frame_t discarded;
    xQueueReceive(playback_queue, &discarded, 0);
    if (xQueueSend(playback_queue, &frame, 0) != pdPASS) {
        ESP_LOGE(TAG, "Unable to enqueue playback frame");
        return;
    }

    dropped_playback_frames++;
    if (dropped_playback_frames == 1 || dropped_playback_frames % 50 == 0) {
        ESP_LOGW(
            TAG,
            "Playback queue full; dropped %lu old frames",
            (unsigned long)dropped_playback_frames);
    }
}

static void websocket_event_handler(
    void *handler_argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        websocket_generation++;
        xEventGroupSetBits(connection_events, WEBSOCKET_CONNECTED_BIT);
        set_orb_state(THINKING_ORB_IDLE);
        ESP_LOGI(TAG, "Connected to voice gateway");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        xEventGroupClearBits(connection_events, WEBSOCKET_CONNECTED_BIT);
        set_orb_state(THINKING_ORB_SEARCHING);
        ESP_LOGW(TAG, "Disconnected from voice gateway");
        break;
    case WEBSOCKET_EVENT_DATA: {
        const esp_websocket_event_data_t *data = event_data;
        if (data->op_code == 0x1 && data->data_len > 0) {
            ESP_LOGI(TAG, "Gateway: %.*s", data->data_len, (const char *)data->data_ptr);
            if (payload_contains(
                    data->data_ptr,
                    data->data_len,
                    "\"state\": \"connecting\"")) {
                set_orb_state(THINKING_ORB_SEARCHING);
            } else if (payload_contains(
                           data->data_ptr,
                           data->data_len,
                           "\"state\": \"active\"")) {
                live_session_active = true;
                set_orb_state(THINKING_ORB_COMPOSING);
            } else if (payload_contains(
                           data->data_ptr,
                           data->data_len,
                           "\"state\": \"processing\"")) {
                set_orb_state(THINKING_ORB_SEARCHING);
            } else if (payload_contains(
                           data->data_ptr,
                           data->data_len,
                           "\"state\": \"failed\"")) {
                live_session_active = false;
                voice_session_requested = false;
                set_orb_state(THINKING_ORB_BREATHING);
            } else if (payload_contains(
                           data->data_ptr,
                           data->data_len,
                           "\"state\": \"ready\"")) {
                live_session_active = false;
                voice_session_requested = false;
                set_orb_state(THINKING_ORB_IDLE);
            }
        } else if (data->op_code == 0x2) {
            queue_playback_frame(data);
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "Voice gateway connection error");
        break;
    default:
        break;
    }
}

static esp_err_t start_websocket(void)
{
    const esp_websocket_client_config_t configuration = {
        .uri = CIRCLE_GATEWAY_URI,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 2000,
        .buffer_size = 2048,
    };
    websocket = esp_websocket_client_init(&configuration);
    if (websocket == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(
        esp_websocket_register_events(
            websocket,
            WEBSOCKET_EVENT_ANY,
            websocket_event_handler,
            NULL),
        TAG,
        "WebSocket event registration failed");
    return esp_websocket_client_start(websocket);
}

static esp_err_t start_microphone(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C initialization failed");
    const i2s_std_config_t audio_configuration = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(
        bsp_audio_init(&audio_configuration),
        TAG,
        "I2S audio initialization failed");

    microphone = bsp_audio_codec_microphone_init();
    if (microphone == NULL) {
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
    };
    ESP_RETURN_ON_ERROR(
        esp_codec_dev_open(microphone, &format),
        TAG,
        "Microphone codec open failed");
    ESP_RETURN_ON_ERROR(
        esp_codec_dev_set_in_gain(microphone, 24.0f),
        TAG,
        "Microphone gain setup failed");
    return ESP_OK;
}

static esp_err_t start_speaker(void)
{
    speaker = bsp_audio_codec_speaker_init();
    if (speaker == NULL) {
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
    };
    ESP_RETURN_ON_ERROR(
        esp_codec_dev_open(speaker, &format),
        TAG,
        "Speaker codec open failed");
    ESP_RETURN_ON_ERROR(
        esp_codec_dev_set_out_vol(speaker, INITIAL_SPEAKER_VOLUME),
        TAG,
        "Speaker volume setup failed");

    playback_queue = xQueueCreate(PLAYBACK_QUEUE_DEPTH, sizeof(playback_frame_t));
    if (playback_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void orb_click_event(lv_event_t *event)
{
    (void)event;
    if (active_app != ACTIVE_APP_VOICE) {
        return;
    }
    voice_session_requested = !voice_session_requested;
    live_toggle_requested = true;
    set_orb_state(
        voice_session_requested
            ? THINKING_ORB_SEARCHING
            : THINKING_ORB_IDLE);
}

static void pmu_task(void *argument)
{
    (void)argument;
    uint32_t since_battery_poll_ms = BATTERY_POLL_MS;
    while (true) {
        bool short_press = false;
        const esp_err_t result = axp2101_poll_button(&pmu, &short_press);
        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Power-button poll failed: %s",
                esp_err_to_name(result));
        } else if (short_press) {
            power_off_device();
        }

        if (since_battery_poll_ms >= BATTERY_POLL_MS) {
            since_battery_poll_ms = 0;
            axp2101_battery_t battery;
            const esp_err_t battery_result =
                axp2101_read_battery(&pmu, &battery);
            if (battery_result != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "Battery read failed: %s",
                    esp_err_to_name(battery_result));
            } else if (bsp_display_lock(100) == ESP_OK) {
                battery_indicator_update(&battery);
                const EventBits_t connection_bits =
                    xEventGroupGetBits(connection_events);
                quick_settings_update(
                    &battery,
                    (connection_bits & WIFI_CONNECTED_BIT) != 0,
                    (connection_bits & WIFI_FAILED_BIT) != 0);
                bsp_display_unlock();
            }
        }
        since_battery_poll_ms += POWER_BUTTON_POLL_MS;

        vTaskDelay(pdMS_TO_TICKS(POWER_BUTTON_POLL_MS));
    }
}

static void speaker_playback_task(void *argument)
{
    playback_frame_t frame;
    bool playback_active = false;
    while (true) {
        const TickType_t receive_timeout = playback_active
            ? pdMS_TO_TICKS(PLAYBACK_GAP_TIMEOUT_MS)
            : portMAX_DELAY;
        if (
            xQueueReceive(
                playback_queue,
                &frame,
                receive_timeout) != pdPASS) {
            playback_active = false;
            playback_level = 0.0f;
            continue;
        }

        if (!playback_active) {
            uint32_t waited_ms = 0;
            while (
                uxQueueMessagesWaiting(playback_queue) <
                    PLAYBACK_PREBUFFER_FRAMES - 1 &&
                waited_ms < PLAYBACK_PREBUFFER_TIMEOUT_MS) {
                vTaskDelay(pdMS_TO_TICKS(2));
                waited_ms += 2;
            }
            playback_active = true;
        }

        int16_t *samples = (int16_t *)frame.data;
        const size_t sample_count = frame.length / sizeof(*samples);
        for (size_t index = 0; index < sample_count; index++) {
            int32_t amplified = (int32_t)samples[index] * PLAYBACK_GAIN;
            if (amplified > INT16_MAX) {
                amplified = INT16_MAX;
            } else if (amplified < INT16_MIN) {
                amplified = INT16_MIN;
            }
            samples[index] = (int16_t)amplified;
        }

        int32_t peak = 0;
        for (size_t index = 0; index < sample_count; index++) {
            int32_t magnitude = samples[index];
            if (magnitude < 0) {
                magnitude = -magnitude;
            }
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        playback_level = peak / 32768.0f;

        const int result = esp_codec_dev_write(speaker, frame.data, frame.length);
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Speaker write failed: %d", result);
        }
    }
}

static void microphone_stream_task(void *argument)
{
    uint8_t audio_frame[AUDIO_FRAME_BYTES];
    uint32_t announced_generation = 0;
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

    char hello[192];
    const int hello_length = snprintf(
        hello,
        sizeof(hello),
        "{\"type\":\"hello\",\"device_id\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"format\":\"pcm_s16le\",\"sample_rate\":%d,\"channels\":%d}",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5],
        AUDIO_SAMPLE_RATE,
        AUDIO_CHANNELS);
    if (hello_length <= 0 || (size_t)hello_length >= sizeof(hello)) {
        ESP_LOGE(TAG, "Unable to create gateway hello");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        const esp_err_t read_result =
            esp_codec_dev_read(microphone, audio_frame, sizeof(audio_frame));
        if (read_result != ESP_OK) {
            ESP_LOGE(TAG, "Microphone read failed: %s", esp_err_to_name(read_result));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const int16_t *samples = (const int16_t *)audio_frame;
        int32_t peak = 0;
        for (size_t index = 0;
             index < AUDIO_FRAME_BYTES / sizeof(*samples);
             index++) {
            int32_t magnitude = samples[index];
            if (magnitude < 0) {
                magnitude = -magnitude;
            }
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        const float current_level = peak / 32768.0f;
        microphone_level = microphone_level * 0.75f + current_level * 0.25f;
        thinking_orb_set_audio_levels(
            microphone_level,
            playback_level,
            live_session_active);

        const EventBits_t bits = xEventGroupGetBits(connection_events);
        if ((bits & WEBSOCKET_CONNECTED_BIT) == 0) {
            continue;
        }

        const uint32_t current_generation = websocket_generation;
        if (announced_generation != current_generation) {
            const int sent = esp_websocket_client_send_text(
                websocket,
                hello,
                hello_length,
                pdMS_TO_TICKS(1000));
            if (sent != hello_length) {
                ESP_LOGW(TAG, "Unable to announce audio format");
                continue;
            }
            announced_generation = current_generation;
        }

        if (live_toggle_requested) {
            static const char toggle_message[] = "{\"type\":\"toggle_live\"}";
            const int toggle_sent = esp_websocket_client_send_text(
                websocket,
                toggle_message,
                sizeof(toggle_message) - 1,
                pdMS_TO_TICKS(1000));
            if (toggle_sent == (int)sizeof(toggle_message) - 1) {
                live_toggle_requested = false;
            } else {
                ESP_LOGW(TAG, "Unable to send voice-session toggle");
            }
        }

        const int sent = esp_websocket_client_send_bin(
            websocket,
            (const char *)audio_frame,
            sizeof(audio_frame),
            pdMS_TO_TICKS(1000));
        if (sent != (int)sizeof(audio_frame)) {
            ESP_LOGW(TAG, "Audio frame send failed: %d", sent);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting circleOS microphone bridge");

    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    ESP_ERROR_CHECK(start_display());
    ESP_ERROR_CHECK(start_microphone());
    ESP_ERROR_CHECK(
        axp2101_init(&pmu, bsp_i2c_get_handle()));
    ESP_ERROR_CHECK(start_speaker());

    BaseType_t task_created = xTaskCreate(
        pmu_task,
        "pmu",
        4096,
        NULL,
        5,
        NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Unable to create PMU task");
        abort();
    }

    const esp_err_t wifi_result = connect_wifi();
    if (wifi_result == ESP_OK) {
        ESP_ERROR_CHECK(start_websocket());
    } else {
        ESP_LOGW(
            TAG,
            "Continuing offline because Wi-Fi setup failed: %s",
            esp_err_to_name(wifi_result));
    }

    task_created = xTaskCreate(
        speaker_playback_task,
        "speaker_playback",
        4096,
        NULL,
        7,
        NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Unable to create speaker playback task");
        abort();
    }

    task_created = xTaskCreate(
        microphone_stream_task,
        "microphone_stream",
        8192,
        NULL,
        6,
        NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Unable to create microphone stream task");
        abort();
    }

    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGE(TAG, "Unable to start Thinking Orb animation");
        abort();
    }
    thinking_orb_start();
    app_home_start();
    bsp_display_unlock();
}
