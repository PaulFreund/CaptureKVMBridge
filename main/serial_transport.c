#include "serial_transport.h"

#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol_tlv.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SERIAL_RX_BUF_SIZE    2048
#define SERIAL_TASK_STACK     4096
#define SERIAL_TASK_PRIO      (configMAX_PRIORITIES - 2)

static const char *TAG = "serial_tlv";

#if CONFIG_APP_ENABLE_USB_AUDIO && defined(CONFIG_UAC_SAMPLE_RATE) && defined(CONFIG_UAC_MIC_CHANNEL_NUM) && defined(CONFIG_UAC_MIC_INTERVAL_MS)
#define SERIAL_MIC_SAMPLES_PER_MS_RAW    ((CONFIG_UAC_SAMPLE_RATE + 999) / 1000)
#define SERIAL_MIC_BYTES_PER_MS_RAW      (SERIAL_MIC_SAMPLES_PER_MS_RAW * CONFIG_UAC_MIC_CHANNEL_NUM * 2)
#define SERIAL_MIC_BYTES_PER_FRAME_RAW   (SERIAL_MIC_BYTES_PER_MS_RAW * CONFIG_UAC_MIC_INTERVAL_MS)
#define SERIAL_MIC_MAX_BYTES             (SERIAL_MIC_BYTES_PER_FRAME_RAW * 8)
#else
#define SERIAL_MIC_MAX_BYTES 0
#endif

#ifndef SERIAL_MIC_MAX_BYTES
#define SERIAL_MIC_MAX_BYTES 0
#endif

#define TLV_WARN_EVERY             32U
#define TLV_WARN_EVERY_LEN         16U

typedef enum {
    TLV_STATE_SYNC0 = 0,
    TLV_STATE_SYNC1,
    TLV_STATE_TYPE,
    TLV_STATE_LEN_MSB,
    TLV_STATE_LEN_LSB,
    TLV_STATE_PAYLOAD,
} tlv_state_t;

static struct {
    tlv_state_t state;
    uint8_t type;
    uint16_t expected;
    uint16_t received;
    uint8_t frame[CONFIG_APP_TLV_MAX_PAYLOAD + 3];
} s_stream;

static serial_tlv_callback_t s_callback = NULL;
static TaskHandle_t s_task_handle = NULL;
static uint32_t s_invalid_type_counter = 0;
static uint32_t s_invalid_length_counter = 0;

static bool tlv_length_is_sane(uint8_t type, uint16_t length, const char **reason)
{
    if ((size_t)length > (size_t)CONFIG_APP_TLV_MAX_PAYLOAD) {
        if (reason) {
            *reason = "payload exceeds limit";
        }
        return false;
    }

    switch (type) {
    case PROTOCOL_EVENT_KEYBOARD:
        if (length != PROTOCOL_KEYBOARD_PAYLOAD_LEN) {
            if (reason) {
                *reason = "keyboard payload length mismatch";
            }
            return false;
        }
        return true;
    case PROTOCOL_EVENT_MOUSE:
        if (length != PROTOCOL_MOUSE_PAYLOAD_LEN) {
            if (reason) {
                *reason = "mouse payload length mismatch";
            }
            return false;
        }
        return true;
    case PROTOCOL_EVENT_MOUSE_ABSOLUTE:
        if (length != PROTOCOL_MOUSE_ABS_PAYLOAD_LEN) {
            if (reason) {
                *reason = "abs mouse payload length mismatch";
            }
            return false;
        }
        return true;
    case PROTOCOL_EVENT_MICROPHONE:
#if CONFIG_APP_ENABLE_USB_AUDIO
        if (length == 0 || (length & 0x01)) {
            if (reason) {
                *reason = "mic payload invalid size";
            }
            return false;
        }
#if SERIAL_MIC_MAX_BYTES > 0
        if (length > SERIAL_MIC_MAX_BYTES) {
            if (reason) {
                *reason = "mic payload exceeds frame capacity";
            }
            return false;
        }
#endif
        return true;
#else
        if (reason) {
            *reason = "mic payload received but audio disabled";
        }
        return false;
#endif
    default:
        if (reason) {
            *reason = "unknown TLV type";
        }
        return false;
    }
}

static void tlv_reset(void)
{
    s_stream.state = TLV_STATE_SYNC0;
    s_stream.expected = 0;
    s_stream.received = 0;
    memset(s_stream.frame, 0, sizeof(s_stream.frame));
}

static void tlv_deliver(void)
{
    if (!s_callback) {
        return;
    }
    size_t frame_len = 3 + s_stream.expected;
    s_callback(APP_INPUT_SOURCE_UART, s_stream.frame, frame_len);
}

static void tlv_feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        switch (s_stream.state) {
        case TLV_STATE_SYNC0:
            if (byte == PROTOCOL_TLV_SYNC0) {
                s_stream.state = TLV_STATE_SYNC1;
            }
            break;
        case TLV_STATE_SYNC1:
            if (byte == PROTOCOL_TLV_SYNC1) {
                s_stream.state = TLV_STATE_TYPE;
            } else if (byte == PROTOCOL_TLV_SYNC0) {
                s_stream.state = TLV_STATE_SYNC1;
            } else {
                s_stream.state = TLV_STATE_SYNC0;
            }
            break;
        case TLV_STATE_TYPE:
            if (!protocol_tlv_type_is_valid(byte)) {
                if ((++s_invalid_type_counter % TLV_WARN_EVERY) == 1U) {
                    ESP_LOGW(TAG, "discarding type 0x%02X while seeking TLV sync", byte);
                }
                if (byte == PROTOCOL_TLV_SYNC0) {
                    s_stream.state = TLV_STATE_SYNC1;
                } else {
                    s_stream.state = TLV_STATE_SYNC0;
                }
                break;
            }
            s_invalid_type_counter = 0;
            s_stream.type = byte;
            s_stream.frame[0] = byte;
            s_stream.state = TLV_STATE_LEN_MSB;
            break;
        case TLV_STATE_LEN_MSB:
            s_stream.expected = ((uint16_t)byte) << 8;
            s_stream.frame[1] = byte;
            s_stream.state = TLV_STATE_LEN_LSB;
            break;
        case TLV_STATE_LEN_LSB:
            s_stream.expected |= byte;
            s_stream.frame[2] = byte;
            {
                const char *reason = NULL;
                if (!tlv_length_is_sane(s_stream.type, s_stream.expected, &reason)) {
                    if ((++s_invalid_length_counter % TLV_WARN_EVERY_LEN) == 1U) {
                        ESP_LOGW(TAG, "%s (type=0x%02X len=%u)", reason ? reason : "invalid TLV length", s_stream.type, s_stream.expected);
                    }
                    tlv_reset();
                    break;
                }
                s_invalid_length_counter = 0;
            }

            if (s_stream.expected == 0) {
                tlv_deliver();
                tlv_reset();
            } else {
                s_stream.received = 0;
                s_stream.state = TLV_STATE_PAYLOAD;
            }
            break;
        case TLV_STATE_PAYLOAD:
            s_stream.frame[3 + s_stream.received] = byte;
            s_stream.received++;
            if (s_stream.received >= s_stream.expected) {
                tlv_deliver();
                tlv_reset();
            }
            break;
        default:
            tlv_reset();
            break;
        }
    }
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
    s_callback = cb;
    tlv_reset();

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
