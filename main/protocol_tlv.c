#include "protocol_tlv.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "tlv";
static protocol_event_handler_t s_handler = NULL;

#define TLV_WARN_EVERY 16U

static inline bool warn_should_log(uint32_t *counter)
{
    (*counter)++;
    return (*counter == 1U) || ((*counter % TLV_WARN_EVERY) == 0U);
}

#if CONFIG_APP_ENABLE_USB_AUDIO && defined(CONFIG_UAC_SAMPLE_RATE) && defined(CONFIG_UAC_MIC_CHANNEL_NUM) && defined(CONFIG_UAC_MIC_INTERVAL_MS)
#define TLV_MIC_SAMPLES_PER_MS   ((CONFIG_UAC_SAMPLE_RATE + 999) / 1000)
#define TLV_MIC_BYTES_PER_MS     (TLV_MIC_SAMPLES_PER_MS * CONFIG_UAC_MIC_CHANNEL_NUM * 2)
#define TLV_MIC_BYTES_PER_FRAME  (TLV_MIC_BYTES_PER_MS * CONFIG_UAC_MIC_INTERVAL_MS)
#define TLV_MIC_MAX_BYTES        (TLV_MIC_BYTES_PER_FRAME * 8)
#else
#define TLV_MIC_MAX_BYTES 0
#endif

static uint32_t s_unknown_type_warn = 0;
static uint32_t s_length_mismatch_warn = 0;
static uint32_t s_keyboard_len_warn = 0;
static uint32_t s_mouse_len_warn = 0;
static uint32_t s_mouse_abs_len_warn = 0;
static uint32_t s_mic_size_warn = 0;

static void dispatch_event(protocol_event_t *event)
{
    if (!s_handler || !event) {
        return;
    }
    s_handler(event);
}

esp_err_t protocol_tlv_init(protocol_event_handler_t handler)
{
    s_handler = handler;
    return ESP_OK;
}

void protocol_tlv_receive_frame(app_input_source_t source, const uint8_t *data, size_t length)
{
    if (!data || length < 3) {
        return;
    }

    uint8_t type = data[0];
    if (!protocol_tlv_type_is_valid(type)) {
        if (warn_should_log(&s_unknown_type_warn)) {
            ESP_LOGW(TAG, "unknown TLV type 0x%02X", type);
        }
        return;
    }
    uint16_t payload_len = ((uint16_t)data[1] << 8) | data[2];
    if ((size_t)payload_len != (length - 3)) {
        if (warn_should_log(&s_length_mismatch_warn)) {
            ESP_LOGW(TAG, "TLV length mismatch type=0x%02X header=%u frame=%u", type, payload_len, (unsigned)(length - 3));
        }
        return;
    }

    const uint8_t *payload = &data[3];

    protocol_event_t event = {
        .source = source,
    };

    switch (type) {
    case PROTOCOL_EVENT_KEYBOARD:
        if (payload_len != PROTOCOL_KEYBOARD_PAYLOAD_LEN) {
            if (warn_should_log(&s_keyboard_len_warn)) {
                ESP_LOGW(TAG, "keyboard payload length mismatch: %u", (unsigned)payload_len);
            }
            return;
        }
        s_keyboard_len_warn = 0;
        event.type = PROTOCOL_EVENT_KEYBOARD;
        memcpy(&event.payload.keyboard, payload, sizeof(event.payload.keyboard));
        break;

    case PROTOCOL_EVENT_MOUSE:
        if (payload_len != PROTOCOL_MOUSE_PAYLOAD_LEN) {
            if (warn_should_log(&s_mouse_len_warn)) {
                ESP_LOGW(TAG, "mouse payload length mismatch: %u", (unsigned)payload_len);
            }
            return;
        }
        s_mouse_len_warn = 0;
        event.type = PROTOCOL_EVENT_MOUSE;
        event.payload.mouse.buttons = payload[0];
        event.payload.mouse.x = (int8_t)payload[1];
        event.payload.mouse.y = (int8_t)payload[2];
        event.payload.mouse.wheel = (int8_t)payload[3];
        event.payload.mouse.pan = (int8_t)payload[4];
        break;

    case PROTOCOL_EVENT_MOUSE_ABSOLUTE:
        if (payload_len != PROTOCOL_MOUSE_ABS_PAYLOAD_LEN) {
            if (warn_should_log(&s_mouse_abs_len_warn)) {
                ESP_LOGW(TAG, "abs mouse payload length mismatch: %u", (unsigned)payload_len);
            }
            return;
        }
        s_mouse_abs_len_warn = 0;
        event.type = PROTOCOL_EVENT_MOUSE_ABSOLUTE;
        event.payload.mouse_abs.buttons = payload[0];
        event.payload.mouse_abs.x = ((uint16_t)payload[1] << 8) | payload[2];
        event.payload.mouse_abs.y = ((uint16_t)payload[3] << 8) | payload[4];
        event.payload.mouse_abs.wheel = (int8_t)payload[5];
        event.payload.mouse_abs.pan = (int8_t)payload[6];
        break;

    case PROTOCOL_EVENT_MICROPHONE:
        if (payload_len == 0 || (payload_len & 0x01)) {
            if (warn_should_log(&s_mic_size_warn)) {
                ESP_LOGW(TAG, "mic payload invalid size: %u", (unsigned)payload_len);
            }
            return;
        }
#if TLV_MIC_MAX_BYTES > 0
        if (payload_len > TLV_MIC_MAX_BYTES) {
            if (warn_should_log(&s_mic_size_warn)) {
                ESP_LOGW(TAG, "mic payload exceeds frame capacity: %u", (unsigned)payload_len);
            }
            return;
        }
#endif
        s_mic_size_warn = 0;
        event.type = PROTOCOL_EVENT_MICROPHONE;
        event.payload.microphone.samples = (const uint16_t *)payload;
        event.payload.microphone.sample_count = payload_len / sizeof(uint16_t);
        break;

    default:
        return;
    }

    app_state_note_event(source);
    dispatch_event(&event);
}

void protocol_tlv_generate_mock_events(void)
{
    static TickType_t last_tick = 0;
    TickType_t now = xTaskGetTickCount();
    if ((now - last_tick) < pdMS_TO_TICKS(2000)) {
        return;
    }
    last_tick = now;

    uint16_t kbd_len = PROTOCOL_KEYBOARD_PAYLOAD_LEN;
    uint8_t keyboard_frame[3 + PROTOCOL_KEYBOARD_PAYLOAD_LEN] = {
        PROTOCOL_EVENT_KEYBOARD,
        (uint8_t)(kbd_len >> 8),
        (uint8_t)(kbd_len & 0xFF),
    };
    usb_keyboard_report_t *kbd = (usb_keyboard_report_t *)&keyboard_frame[3];
    memset(kbd, 0, sizeof(*kbd));
    kbd->keycodes[0] = 0x04; // 'A'
    protocol_tlv_receive_frame(APP_INPUT_SOURCE_NONE, keyboard_frame, sizeof(keyboard_frame));

    uint8_t mouse_frame[3 + PROTOCOL_MOUSE_PAYLOAD_LEN] = {
        PROTOCOL_EVENT_MOUSE,
        0x00,
        PROTOCOL_MOUSE_PAYLOAD_LEN,
        0x01, // buttons
        5,
        0,
        0,
        0,
    };
    protocol_tlv_receive_frame(APP_INPUT_SOURCE_NONE, mouse_frame, sizeof(mouse_frame));
}
