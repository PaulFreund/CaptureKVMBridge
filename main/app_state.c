#include "app_state.h"
#include "input_activity_led.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "app_state";
static app_status_snapshot_t snapshot;
static int64_t keyboard_active_until = 0;
static int64_t mouse_active_until = 0;
static int64_t mic_active_until = 0;

#define FEATURE_ACTIVE_US   (500000LL)
#define MIC_ACTIVE_US       (500000LL)

static void refresh_feature_activity(int64_t now)
{
    snapshot.hs_keyboard_active = (keyboard_active_until > now);
    snapshot.hs_mouse_active = (mouse_active_until > now);
    snapshot.hs_microphone_active = (mic_active_until > now);
}

void app_state_init(void)
{
    snapshot.hs_state = APP_USB_STATE_DISCONNECTED;
    snapshot.hs_keyboard_active = false;
    snapshot.hs_mouse_active = false;
    snapshot.hs_microphone_active = false;
    snapshot.events_forwarded = 0;
    snapshot.last_source = APP_INPUT_SOURCE_NONE;
    snapshot.last_event_timestamp_us = esp_timer_get_time();
    keyboard_active_until = 0;
    mouse_active_until = 0;
    mic_active_until = 0;
    ESP_LOGI(TAG, "state initialized");
}

void app_state_update_usb_state(app_usb_state_t new_state)
{
    snapshot.hs_state = new_state;
}

void app_state_mark_feature_usage(bool keyboard, bool mouse, bool microphone)
{
    const int64_t now = esp_timer_get_time();

    if (keyboard) {
        keyboard_active_until = now + FEATURE_ACTIVE_US;
    }
    if (mouse) {
        mouse_active_until = now + FEATURE_ACTIVE_US;
    }
    if (microphone) {
        mic_active_until = now + MIC_ACTIVE_US;
    }

    refresh_feature_activity(now);
}

void app_state_note_event(app_input_source_t source)
{
    snapshot.events_forwarded++;
    snapshot.last_source = source;
    snapshot.last_event_timestamp_us = esp_timer_get_time();
    input_activity_led_pulse();
    refresh_feature_activity(snapshot.last_event_timestamp_us);
}

app_status_snapshot_t app_state_get_snapshot(void)
{
    refresh_feature_activity(esp_timer_get_time());
    return snapshot;
}
