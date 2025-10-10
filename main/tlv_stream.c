#include "tlv_stream.h"

#include "esp_log.h"
#include "protocol_tlv.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "tlv_stream";

#define TLV_WARN_EVERY             32U
#define TLV_WARN_EVERY_LEN         16U

enum {
    TLV_STATE_SYNC0 = 0,
    TLV_STATE_SYNC1,
    TLV_STATE_TYPE,
    TLV_STATE_LEN_MSB,
    TLV_STATE_LEN_LSB,
    TLV_STATE_PAYLOAD,
};

#if CONFIG_APP_ENABLE_USB_AUDIO && defined(CONFIG_UAC_SAMPLE_RATE) && defined(CONFIG_UAC_MIC_CHANNEL_NUM) && defined(CONFIG_UAC_MIC_INTERVAL_MS)
#define TLV_MIC_SAMPLES_PER_MS_RAW    ((CONFIG_UAC_SAMPLE_RATE + 999) / 1000)
#define TLV_MIC_BYTES_PER_MS_RAW      (TLV_MIC_SAMPLES_PER_MS_RAW * CONFIG_UAC_MIC_CHANNEL_NUM * 2)
#define TLV_MIC_BYTES_PER_FRAME_RAW   (TLV_MIC_BYTES_PER_MS_RAW * CONFIG_UAC_MIC_INTERVAL_MS)
#define TLV_MIC_MAX_BYTES             (TLV_MIC_BYTES_PER_FRAME_RAW * 8)
#else
#define TLV_MIC_MAX_BYTES 0
#endif

#ifndef TLV_MIC_MAX_BYTES
#define TLV_MIC_MAX_BYTES 0
#endif

void tlv_stream_init(tlv_stream_t *stream, tlv_stream_callback_t cb, app_input_source_t source)
{
    if (!stream) {
        return;
    }
    memset(stream, 0, sizeof(*stream));
    stream->callback = cb;
    stream->source = source;
    tlv_stream_reset(stream);
}

void tlv_stream_reset(tlv_stream_t *stream)
{
    if (!stream) {
        return;
    }
    stream->state = TLV_STATE_SYNC0;
    stream->expected = 0;
    stream->received = 0;
    memset(stream->frame, 0, sizeof(stream->frame));
}

static bool tlv_length_is_sane(uint8_t type, uint16_t length, const char **reason)
{
    if ((size_t)length > (size_t)CONFIG_APP_TLV_MAX_PAYLOAD) {
        if (reason) {
            *reason = "payload exceeds limit";
        }
        return false;
    }

    switch (type) {
    case PROTOCOL_EVENT_KEYBOARD:
        if (length != PROTOCOL_KEYBOARD_PAYLOAD_LEN) {
            if (reason) {
                *reason = "keyboard payload length mismatch";
            }
            return false;
        }
        return true;
    case PROTOCOL_EVENT_MOUSE:
        if (length != PROTOCOL_MOUSE_PAYLOAD_LEN) {
            if (reason) {
                *reason = "mouse payload length mismatch";
            }
            return false;
        }
        return true;
    case PROTOCOL_EVENT_MOUSE_ABSOLUTE:
        if (length != PROTOCOL_MOUSE_ABS_PAYLOAD_LEN) {
            if (reason) {
                *reason = "abs mouse payload length mismatch";
            }
            return false;
        }
        return true;
    case PROTOCOL_EVENT_MICROPHONE:
#if CONFIG_APP_ENABLE_USB_AUDIO
        if (length == 0 || (length & 0x01)) {
            if (reason) {
                *reason = "mic payload invalid size";
            }
            return false;
        }
#if TLV_MIC_MAX_BYTES > 0
        if (length > TLV_MIC_MAX_BYTES) {
            if (reason) {
                *reason = "mic payload exceeds frame capacity";
            }
            return false;
        }
#endif
        return true;
#else
        if (reason) {
            *reason = "mic payload received but audio disabled";
        }
        return false;
#endif
    default:
        if (reason) {
            *reason = "unknown TLV type";
        }
        return false;
    }
}

void tlv_stream_feed(tlv_stream_t *stream, const uint8_t *data, size_t len)
{
    if (!stream || !data || len == 0) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        switch (stream->state) {
        case TLV_STATE_SYNC0:
            if (byte == PROTOCOL_TLV_SYNC0) {
                stream->state = TLV_STATE_SYNC1;
            }
            break;
        case TLV_STATE_SYNC1:
            if (byte == PROTOCOL_TLV_SYNC1) {
                stream->state = TLV_STATE_TYPE;
            } else if (byte == PROTOCOL_TLV_SYNC0) {
                stream->state = TLV_STATE_SYNC1;
            } else {
                stream->state = TLV_STATE_SYNC0;
            }
            break;
        case TLV_STATE_TYPE:
            if (!protocol_tlv_type_is_valid(byte)) {
                if ((++stream->invalid_type_counter % TLV_WARN_EVERY) == 1U) {
                    ESP_LOGW(TAG, "[%d] discard type 0x%02X while seeking TLV sync", stream->source, byte);
                }
                if (byte == PROTOCOL_TLV_SYNC0) {
                    stream->state = TLV_STATE_SYNC1;
                } else {
                    stream->state = TLV_STATE_SYNC0;
                }
                break;
            }
            stream->invalid_type_counter = 0;
            stream->type = byte;
            stream->frame[0] = byte;
            stream->state = TLV_STATE_LEN_MSB;
            break;
        case TLV_STATE_LEN_MSB:
            stream->expected = ((uint16_t)byte) << 8;
            stream->frame[1] = byte;
            stream->state = TLV_STATE_LEN_LSB;
            break;
        case TLV_STATE_LEN_LSB:
            stream->expected |= byte;
            stream->frame[2] = byte;
            {
                const char *reason = NULL;
                if (!tlv_length_is_sane(stream->type, stream->expected, &reason)) {
                    if ((++stream->invalid_length_counter % TLV_WARN_EVERY_LEN) == 1U) {
                        ESP_LOGW(TAG, "[%d] %s (type=0x%02X len=%u)", stream->source,
                                 reason ? reason : "invalid TLV length", stream->type, stream->expected);
                    }
                    tlv_stream_reset(stream);
                    break;
                }
                stream->invalid_length_counter = 0;
            }

            if (stream->expected == 0) {
                if (stream->callback) {
                    stream->callback(stream->source, stream->frame, 3);
                }
                tlv_stream_reset(stream);
            } else {
                stream->received = 0;
                stream->state = TLV_STATE_PAYLOAD;
            }
            break;
        case TLV_STATE_PAYLOAD:
            stream->frame[3 + stream->received] = byte;
            stream->received++;
            if (stream->received >= stream->expected) {
                if (stream->callback) {
                    stream->callback(stream->source, stream->frame, (size_t)(3 + stream->expected));
                }
                tlv_stream_reset(stream);
            }
            break;
        default:
            tlv_stream_reset(stream);
            break;
        }
    }
}
