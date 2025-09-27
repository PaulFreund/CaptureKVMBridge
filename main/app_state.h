#pragma once

#include "esp_err.h"
#include "esp_timer.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_USB_STATE_DISCONNECTED = 0,
    APP_USB_STATE_MOUNTED,
    APP_USB_STATE_STREAMING,
} app_usb_state_t;

typedef enum {
    APP_INPUT_SOURCE_NONE = 0,
    APP_INPUT_SOURCE_NETWORK,
    APP_INPUT_SOURCE_UART,
} app_input_source_t;

typedef struct {
    app_usb_state_t hs_state;
    bool hs_keyboard_active;
    bool hs_mouse_active;
    bool hs_microphone_active;
    uint32_t events_forwarded;
    app_input_source_t last_source;
    int64_t last_event_timestamp_us;
} app_status_snapshot_t;

void app_state_init(void);
void app_state_update_usb_state(app_usb_state_t new_state);
void app_state_mark_feature_usage(bool keyboard, bool mouse, bool microphone);
void app_state_note_event(app_input_source_t source);
app_status_snapshot_t app_state_get_snapshot(void);

#ifdef __cplusplus
}
#endif
