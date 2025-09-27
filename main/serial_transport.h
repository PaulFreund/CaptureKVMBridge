#pragma once

#include "app_state.h"
#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*serial_tlv_callback_t)(app_input_source_t source, const uint8_t *frame, size_t length);

esp_err_t serial_transport_start(serial_tlv_callback_t cb);

#ifdef __cplusplus
}
#endif
