#include "serial_transport.h"

#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol_tlv.h"
#include "tlv_stream.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SERIAL_RX_BUF_SIZE    2048
#define SERIAL_TASK_STACK     4096
#define SERIAL_TASK_PRIO      (configMAX_PRIORITIES - 2)

static const char *TAG = "serial_tlv";

static TaskHandle_t s_task_handle = NULL;
static tlv_stream_t s_stream;

static void tlv_feed(const uint8_t *data, size_t len)
{
    tlv_stream_feed(&s_stream, data, len);
}

static void serial_rx_task(void *arg)
{
    (void)arg;
    uint8_t *rx_buffer = malloc(SERIAL_RX_BUF_SIZE);
    if (!rx_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RX buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int len = usb_serial_jtag_read_bytes(rx_buffer, SERIAL_RX_BUF_SIZE, portMAX_DELAY);
        if (len > 0) {
            tlv_feed(rx_buffer, (size_t)len);

            // Drain any additional bytes that may already be buffered to minimise latency.
            while ((len = usb_serial_jtag_read_bytes(rx_buffer, SERIAL_RX_BUF_SIZE, 0)) > 0) {
                tlv_feed(rx_buffer, (size_t)len);
            }
        }
    }
}

esp_err_t serial_transport_start(serial_tlv_callback_t cb)
{
    tlv_stream_init(&s_stream, cb, APP_INPUT_SOURCE_USB_JTAG);

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        usj_cfg.rx_buffer_size = SERIAL_RX_BUF_SIZE;
        ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usj_cfg), TAG, "install usb-serial-jtag");
    }

    BaseType_t res = xTaskCreatePinnedToCore(serial_rx_task, "serial_tlv_rx", SERIAL_TASK_STACK, NULL,
                                             SERIAL_TASK_PRIO, &s_task_handle, tskNO_AFFINITY);
    ESP_RETURN_ON_FALSE(res == pdPASS, ESP_FAIL, TAG, "create task failed");

    ESP_LOGI(TAG, "USB Serial/JTAG TLV bridge active");
    return ESP_OK;
}

#ifdef CEIL_DIV
#undef CEIL_DIV
#endif
