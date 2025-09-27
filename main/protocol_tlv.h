#pragma once

#include "app_state.h"
#include "usb_hs_device.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROTOCOL_EVENT_KEYBOARD = 1,
    PROTOCOL_EVENT_MOUSE,
    PROTOCOL_EVENT_MICROPHONE,
    PROTOCOL_EVENT_MOUSE_ABSOLUTE = 4,
} protocol_event_type_t;

typedef struct {
    protocol_event_type_t type;
    app_input_source_t source;
    union {
        usb_keyboard_report_t keyboard;
        usb_mouse_report_t mouse;
        usb_mouse_absolute_report_t mouse_abs;
        usb_microphone_frame_t microphone;
    } payload;
} protocol_event_t;

// Expected payload sizes for TLV transport (bytes)
#define PROTOCOL_KEYBOARD_PAYLOAD_LEN        8U
#define PROTOCOL_MOUSE_PAYLOAD_LEN           5U
#define PROTOCOL_MOUSE_ABS_PAYLOAD_LEN       7U

static inline bool protocol_tlv_type_is_valid(uint8_t type)
{
    switch (type) {
    case PROTOCOL_EVENT_KEYBOARD:
    case PROTOCOL_EVENT_MOUSE:
    case PROTOCOL_EVENT_MICROPHONE:
    case PROTOCOL_EVENT_MOUSE_ABSOLUTE:
        return true;
    default:
        return false;
    }
}

typedef void (*protocol_event_handler_t)(const protocol_event_t *event);

esp_err_t protocol_tlv_init(protocol_event_handler_t handler);
void protocol_tlv_receive_frame(app_input_source_t source, const uint8_t *data, size_t length);
void protocol_tlv_generate_mock_events(void);

#ifdef __cplusplus
}
#endif
#define PROTOCOL_TLV_SYNC0 0xD5
#define PROTOCOL_TLV_SYNC1 0xAA
