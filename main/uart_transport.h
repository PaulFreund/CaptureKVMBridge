#pragma once

#include "serial_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uart_transport_start(serial_tlv_callback_t cb);

#ifdef __cplusplus
}
#endif
