#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t input_activity_led_init(void);
void input_activity_led_pulse(void);

#ifdef __cplusplus
}
#endif
