#include "uart_transport.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tlv_stream.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>

#define UART_TLV_PORT       ((uart_port_t)CONFIG_APP_UART_PORT_NUM)
#define UART_TLV_RX_GPIO    CONFIG_APP_UART_RX_GPIO
#define UART_TLV_TX_GPIO    CONFIG_APP_UART_TX_GPIO

#define UART_RX_BUF_SIZE    32768
#define UART_EVENT_QUEUE_LEN 64
#define UART_TASK_STACK     4096
#define UART_TASK_PRIO      (configMAX_PRIORITIES - 2)

static const char *TAG = "uart_tlv";

static tlv_stream_t s_stream;
static uart_config_t s_uart_cfg;
static TaskHandle_t s_task_handle = NULL;
static QueueHandle_t s_uart_queue = NULL;

static void uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t chunk[512];
    uart_event_t event;

    while (1) {
        if (xQueueReceive(s_uart_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (event.type) {
        case UART_DATA: {
            size_t remaining = event.size;
            while (remaining > 0) {
                int to_read = remaining > sizeof(chunk) ? sizeof(chunk) : (int)remaining;
                int len = uart_read_bytes(UART_TLV_PORT, chunk, to_read, 0);
                if (len <= 0) {
                    break;
                }
                tlv_stream_feed(&s_stream, chunk, (size_t)len);
                remaining -= len;
                size_t buffered = 0;
                if (uart_get_buffered_data_len(UART_TLV_PORT, &buffered) == ESP_OK) {
                    if (buffered > 0) {
                        remaining += buffered;
                    }
                }
            }
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "UART overflow (%d)", event.type);
            uart_flush_input(UART_TLV_PORT);
            xQueueReset(s_uart_queue);
            tlv_stream_reset(&s_stream);
            break;
        case UART_PARITY_ERR:
            ESP_LOGW(TAG, "UART parity error");
            break;
        case UART_FRAME_ERR:
            ESP_LOGW(TAG, "UART frame error");
            break;
        case UART_BREAK:
        default:
            break;
        }
    }
}

esp_err_t uart_transport_start(serial_tlv_callback_t cb)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }

    if (UART_TLV_RX_GPIO < 0) {
        ESP_LOGW(TAG, "UART TLV bridge disabled (RX GPIO not configured)");
        return ESP_OK;
    }

    tlv_stream_init(&s_stream, cb, APP_INPUT_SOURCE_UART);

    if (uart_is_driver_installed(UART_TLV_PORT)) {
        ESP_RETURN_ON_ERROR(uart_driver_delete(UART_TLV_PORT), TAG, "uart driver delete");
    }

    uart_config_t cfg = {
        .baud_rate = CONFIG_APP_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    s_uart_cfg = cfg;
    ESP_RETURN_ON_ERROR(uart_param_config(UART_TLV_PORT, &s_uart_cfg), TAG, "uart param");

    ESP_RETURN_ON_ERROR(uart_driver_install(UART_TLV_PORT, UART_RX_BUF_SIZE, 0, UART_EVENT_QUEUE_LEN,
                                            &s_uart_queue, ESP_INTR_FLAG_IRAM),
                        TAG, "uart driver install");
    ESP_RETURN_ON_FALSE(s_uart_queue != NULL, ESP_FAIL, TAG, "uart queue");

    int tx_pin = (UART_TLV_TX_GPIO >= 0) ? UART_TLV_TX_GPIO : UART_PIN_NO_CHANGE;
    int rx_pin = (UART_TLV_RX_GPIO >= 0) ? UART_TLV_RX_GPIO : UART_PIN_NO_CHANGE;
    if (rx_pin >= 0) {
        gpio_set_pull_mode(rx_pin, GPIO_PULLUP_ONLY);
    }
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_TLV_PORT, tx_pin, rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");

    ESP_RETURN_ON_ERROR(uart_set_mode(UART_TLV_PORT, UART_MODE_UART), TAG, "uart mode");
    ESP_RETURN_ON_ERROR(uart_set_hw_flow_ctrl(UART_TLV_PORT, UART_HW_FLOWCTRL_DISABLE, 0), TAG, "uart flow");
    ESP_RETURN_ON_ERROR(uart_set_rx_full_threshold(UART_TLV_PORT, 16), TAG, "uart threshold");
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(UART_TLV_PORT, 1), TAG, "uart timeout");
    ESP_RETURN_ON_ERROR(uart_flush_input(UART_TLV_PORT), TAG, "uart flush");

    BaseType_t res = xTaskCreatePinnedToCore(uart_rx_task, "uart_tlv_rx", UART_TASK_STACK, NULL,
                                             UART_TASK_PRIO, &s_task_handle, tskNO_AFFINITY);
    ESP_RETURN_ON_FALSE(res == pdPASS, ESP_FAIL, TAG, "create task failed");

#if CONFIG_ESP_CONSOLE_UART
    if (UART_TLV_PORT == (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM) {
        ESP_LOGW(TAG, "UART TLV bridge shares console UART%d; switch APP_UART_PORT_NUM or console settings if needed",
                 (int)UART_TLV_PORT);
    }
#endif

    ESP_LOGI(TAG, "UART TLV bridge active on port %d (RX GPIO %d TX %d) %d,%d,%d,%d,%d",
             (int)UART_TLV_PORT,
             UART_TLV_RX_GPIO,
             UART_TLV_TX_GPIO,
             s_uart_cfg.baud_rate,
             s_uart_cfg.data_bits,
             s_uart_cfg.parity,
             s_uart_cfg.stop_bits,
             s_uart_cfg.flow_ctrl);
    return ESP_OK;
}
