#pragma once

#include "esp_err.h"
#include "app_state.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keycodes[6];
} usb_keyboard_report_t;

typedef struct {
    int8_t x;
    int8_t y;
    int8_t wheel;
    int8_t pan;
    uint8_t buttons;
} usb_mouse_report_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    int8_t wheel;
    int8_t pan;
    uint8_t buttons;
} usb_mouse_absolute_report_t;

typedef struct {
    const uint16_t *samples;
    size_t sample_count;
} usb_microphone_frame_t;

esp_err_t usb_hs_device_init(void);
void usb_hs_handle_keyboard(const usb_keyboard_report_t *report);
void usb_hs_handle_mouse(const usb_mouse_report_t *report);
void usb_hs_handle_mouse_absolute(const usb_mouse_absolute_report_t *report);
void usb_hs_handle_microphone_frame(const usb_microphone_frame_t *frame);
void usb_hs_poll(void);

#ifdef __cplusplus
}
#endif
