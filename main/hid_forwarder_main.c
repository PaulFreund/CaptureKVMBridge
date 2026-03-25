#include "app_state.h"
#include "display_ui.h"
#include "protocol_tlv.h"
#include "usb_hs_device.h"
#include "network_transport.h"
#include "serial_transport.h"
#include "uart_transport.h"

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdbool.h>

static const char *TAG = "forwarder";

static void handle_protocol_event(const protocol_event_t *event)
{
    if (!event) {
        return;
    }
    switch (event->type) {
    case PROTOCOL_EVENT_KEYBOARD:
        usb_hs_handle_keyboard(&event->payload.keyboard);
        app_state_mark_feature_usage(true, false, false);
        break;
    case PROTOCOL_EVENT_MOUSE:
        usb_hs_handle_mouse(&event->payload.mouse);
        app_state_mark_feature_usage(false, true, false);
        break;
    case PROTOCOL_EVENT_MOUSE_ABSOLUTE:
        usb_hs_handle_mouse_absolute(&event->payload.mouse_abs);
        app_state_mark_feature_usage(false, true, false);
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
        vTaskDelay(1);
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
    esp_err_t spiffs_err = bsp_spiffs_mount();
    if (spiffs_err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s), display unavailable", esp_err_to_name(spiffs_err));
    }
    app_state_init();
    // Skip display init — Waveshare MIPI DSI display not present on this board.
    // Calling display_ui_init() would hang the watchdog waiting for ST7703.
    bool display_enabled = false;
    ESP_LOGI(TAG, "Display UI skipped (no Waveshare panel)");
    ESP_ERROR_CHECK(usb_hs_device_init());
    ESP_ERROR_CHECK(protocol_tlv_init(handle_protocol_event));
    ESP_ERROR_CHECK(network_transport_start(protocol_tlv_receive_frame));
    ESP_ERROR_CHECK(serial_transport_start(protocol_tlv_receive_frame));
    ESP_ERROR_CHECK(uart_transport_start(protocol_tlv_receive_frame));

    xTaskCreatePinnedToCore(core_service_task, "core_service", 4096, NULL, 5, NULL, 1);
    if (display_enabled) {
        xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 1, NULL, tskNO_AFFINITY);
    }
}
