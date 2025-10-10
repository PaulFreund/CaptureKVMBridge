#pragma once

#include "app_state.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"

typedef void (*tlv_stream_callback_t)(app_input_source_t source, const uint8_t *frame, size_t length);

typedef struct {
    uint8_t state;
    uint8_t type;
    uint16_t expected;
    uint16_t received;
    uint8_t frame[CONFIG_APP_TLV_MAX_PAYLOAD + 3];
    tlv_stream_callback_t callback;
    app_input_source_t source;
    uint32_t invalid_type_counter;
    uint32_t invalid_length_counter;
} tlv_stream_t;

void tlv_stream_init(tlv_stream_t *stream, tlv_stream_callback_t cb, app_input_source_t source);
void tlv_stream_feed(tlv_stream_t *stream, const uint8_t *data, size_t len);
void tlv_stream_reset(tlv_stream_t *stream);

#ifdef __cplusplus
}
#endif
