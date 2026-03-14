#include "app_state.h"
#include "board_rgb_led.h"
#include "display_ui.h"
#include "input_activity_led.h"
#include "protocol_tlv.h"
#include "sdkconfig.h"
#include "usb_hs_device.h"
#include "network_transport.h"
#include "serial_transport.h"
#include "uart_transport.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdbool.h>

static const char *TAG = "forwarder";

static void pulse_input_debug_led(protocol_event_type_t type)
{
    switch (type) {
    case PROTOCOL_EVENT_KEYBOARD:
        (void)board_rgb_led_pulse_color(0, 0, 96);
        break;
    case PROTOCOL_EVENT_MOUSE:
    case PROTOCOL_EVENT_MOUSE_ABSOLUTE:
        (void)board_rgb_led_pulse_color(0, 96, 0);
        break;
    default:
        break;
    }
}

static void handle_protocol_event(const protocol_event_t *event)
{
    if (!event) {
        return;
    }
    switch (event->type) {
    case PROTOCOL_EVENT_KEYBOARD:
        usb_hs_handle_keyboard(&event->payload.keyboard);
        app_state_mark_feature_usage(true, false, false);
        pulse_input_debug_led(event->type);
        break;
    case PROTOCOL_EVENT_MOUSE:
        usb_hs_handle_mouse(&event->payload.mouse);
        app_state_mark_feature_usage(false, true, false);
        pulse_input_debug_led(event->type);
        break;
    case PROTOCOL_EVENT_MOUSE_ABSOLUTE:
        usb_hs_handle_mouse_absolute(&event->payload.mouse_abs);
        app_state_mark_feature_usage(false, true, false);
        pulse_input_debug_led(event->type);
        break;
    case PROTOCOL_EVENT_MICROPHONE:
        usb_hs_handle_microphone_frame(&event->payload.microphone);
        app_state_mark_feature_usage(false, false, true);
        break;
    default:
        break;
    }
}

static void core_service_task(void *arg)
{
    (void)arg;
    while (1) {
        usb_hs_poll();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void display_task(void *arg)
{
    (void)arg;
    while (1) {
        app_status_snapshot_t snapshot = app_state_get_snapshot();
        display_ui_update(&snapshot);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting forwarder");
    esp_log_level_set("uart_tlv", ESP_LOG_DEBUG);
    esp_log_level_set("tlv_stream", ESP_LOG_DEBUG);
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    app_state_init();
    ESP_ERROR_CHECK(board_rgb_led_init());
    ESP_ERROR_CHECK(input_activity_led_init());
    esp_err_t display_err = display_ui_init();
    bool display_enabled = (display_err == ESP_OK);
    if (display_err == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "Display UI disabled (not enough memory)");
    } else if (display_err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Display UI disabled on target %s", CONFIG_IDF_TARGET);
    } else {
        ESP_ERROR_CHECK(display_err);
    }
    ESP_ERROR_CHECK(usb_hs_device_init());
    ESP_ERROR_CHECK(protocol_tlv_init(handle_protocol_event));
    ESP_ERROR_CHECK(network_transport_start(protocol_tlv_receive_frame));
#if CONFIG_APP_ENABLE_USB_SERIAL_JTAG_TLV
    ESP_ERROR_CHECK(serial_transport_start(protocol_tlv_receive_frame));
#endif
    ESP_ERROR_CHECK(uart_transport_start(protocol_tlv_receive_frame));

    xTaskCreatePinnedToCore(core_service_task, "core_service", 4096, NULL, 5, NULL, 1);
    if (display_enabled) {
        xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 1, NULL, tskNO_AFFINITY);
    }
}
