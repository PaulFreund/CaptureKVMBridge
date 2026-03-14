#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t board_rgb_led_init(void);
esp_err_t board_rgb_led_set_color(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t board_rgb_led_pulse_color(uint8_t red, uint8_t green, uint8_t blue);

#ifdef __cplusplus
}
#endif
