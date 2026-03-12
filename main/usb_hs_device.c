#include "usb_hs_device.h"

#include "app_state.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tusb.h"
#include "class/hid/hid_device.h"
#if CFG_TUD_AUDIO
#include "usb_device_uac.h"
#include "tusb_uac/uac_descriptors.h"
#include "class/audio/audio_device.h"
#endif

#if CFG_TUD_HID
void tud_suspend_cb(bool remote_wakeup_en)
{
    usb_hs_on_suspend(remote_wakeup_en);
}

void tud_resume_cb(void)
{
    usb_hs_on_resume();
}
#endif
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "usb_hs";

#if CFG_TUD_HID
#define HID_EVENT_QUEUE_LEN 64
#endif

#if CONFIG_APP_ENABLE_USB_AUDIO
#define MIC_SAMPLES_PER_MS       ((CONFIG_UAC_SAMPLE_RATE + 999) / 1000)
#define MIC_RING_BUFFER_FRAMES   24
#define MIC_BYTES_PER_MS         (MIC_SAMPLES_PER_MS * CONFIG_UAC_MIC_CHANNEL_NUM * 2)
#define MIC_BYTES_PER_FRAME_RAW  (MIC_BYTES_PER_MS * CONFIG_UAC_MIC_INTERVAL_MS)
#define MIC_RING_BUFFER_BYTES_RAW (MIC_BYTES_PER_FRAME_RAW * MIC_RING_BUFFER_FRAMES)
#if MIC_RING_BUFFER_BYTES_RAW < 4096
#define MIC_RING_BUFFER_BYTES 4096
#else
#define MIC_RING_BUFFER_BYTES MIC_RING_BUFFER_BYTES_RAW
#endif
#else
#define MIC_RING_BUFFER_BYTES   (16 * 1024)
#endif

#define TUD_HID_REPORT_DESC_ABSMOUSE_16BIT(...) \
  HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP      )                   ,\
  HID_USAGE      ( HID_USAGE_DESKTOP_MOUSE     )                   ,\
  HID_COLLECTION ( HID_COLLECTION_APPLICATION  )                   ,\
    /* Report ID if any */\
    __VA_ARGS__ \
    HID_USAGE      ( HID_USAGE_DESKTOP_POINTER )                   ,\
    HID_COLLECTION ( HID_COLLECTION_PHYSICAL   )                   ,\
      HID_USAGE_PAGE  ( HID_USAGE_PAGE_BUTTON  )                   ,\
        HID_USAGE_MIN   ( 1                                      ) ,\
        HID_USAGE_MAX   ( 5                                      ) ,\
        HID_LOGICAL_MIN ( 0                                      ) ,\
        HID_LOGICAL_MAX ( 1                                      ) ,\
        HID_REPORT_COUNT( 5                                      ) ,\
        HID_REPORT_SIZE ( 1                                      ) ,\
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ) ,\
        HID_REPORT_COUNT( 1                                      ) ,\
        HID_REPORT_SIZE ( 3                                      ) ,\
        HID_INPUT       ( HID_CONSTANT                           ) ,\
      HID_USAGE_PAGE  ( HID_USAGE_PAGE_DESKTOP )                   ,\
        HID_USAGE       ( HID_USAGE_DESKTOP_X                    ) ,\
        HID_USAGE       ( HID_USAGE_DESKTOP_Y                    ) ,\
        HID_LOGICAL_MIN  ( 0x00                                ) ,\
        HID_LOGICAL_MAX_N( 0x7FFF, 2                           ) ,\
        HID_REPORT_SIZE  ( 16                                  ) ,\
        HID_REPORT_COUNT ( 2                                   ) ,\
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ) ,\
        HID_USAGE       ( HID_USAGE_DESKTOP_WHEEL                )  ,\
        HID_LOGICAL_MIN ( 0x81                                   )  ,\
        HID_LOGICAL_MAX ( 0x7f                                   )  ,\
        HID_REPORT_COUNT( 1                                      )  ,\
        HID_REPORT_SIZE ( 8                                      )  ,\
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE )  ,\
      HID_USAGE_PAGE  ( HID_USAGE_PAGE_CONSUMER ), \
        HID_USAGE_N     ( HID_USAGE_CONSUMER_AC_PAN, 2           ), \
        HID_LOGICAL_MIN ( 0x81                                   ), \
        HID_LOGICAL_MAX ( 0x7f                                   ), \
        HID_REPORT_COUNT( 1                                      ), \
        HID_REPORT_SIZE ( 8                                      ), \
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE ), \
    HID_COLLECTION_END                                            , \
  HID_COLLECTION_END \

// -----------------------------------------------------------------------------
// Interface and endpoint layout
// -----------------------------------------------------------------------------

#if CFG_TUD_AUDIO
#define ITF_NUM_AUDIO_CONTROL        0
#define ITF_NUM_AUDIO_STREAMING_MIC  (ITF_NUM_AUDIO_CONTROL + 1)
#define _ITF_AFTER_AUDIO             (ITF_NUM_AUDIO_STREAMING_MIC + 1)
#else
#define _ITF_AFTER_AUDIO             0
#endif

#if CFG_TUD_HID
#define ITF_NUM_HID                  _ITF_AFTER_AUDIO
#define _ITF_AFTER_HID               (ITF_NUM_HID + 1)
#else
#define _ITF_AFTER_HID               _ITF_AFTER_AUDIO
#endif

#define ITF_NUM_TOTAL                _ITF_AFTER_HID

#if CFG_TUD_AUDIO
#define EP_AUDIO_MIC_IN   0x81
#define EP_HID_IN         0x82
#else
#define EP_HID_IN 0x81
#endif

// -----------------------------------------------------------------------------
// USB descriptors
// -----------------------------------------------------------------------------

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x5092,
    .bcdDevice = 0x0101,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static const tusb_desc_device_qualifier_t device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0
};

// HID report descriptor (keyboard + mouse, distinct report IDs)
#if CFG_TUD_HID
#define HID_REPORT_ID_KEYBOARD     1
#define HID_REPORT_ID_MOUSE_REL     2
#define HID_REPORT_ID_MOUSE_ABS     3

static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD( HID_REPORT_ID(HID_REPORT_ID_KEYBOARD) ),
    TUD_HID_REPORT_DESC_MOUSE   ( HID_REPORT_ID(HID_REPORT_ID_MOUSE_REL) ),
    TUD_HID_REPORT_DESC_ABSMOUSE_16BIT( HID_REPORT_ID(HID_REPORT_ID_MOUSE_ABS) )
};
#endif

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
#if CFG_TUD_AUDIO
    STRID_AUDIO_CTRL,
    STRID_AUDIO_MIC,
#endif
#if CFG_TUD_HID
    STRID_HID,
#endif
    STRID_COUNT
};

#if CFG_TUD_AUDIO && CFG_TUD_HID
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t configuration_descriptor_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, STRID_AUDIO_CTRL, 0, EP_AUDIO_MIC_IN, 0),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, false, sizeof(hid_report_descriptor), EP_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

static const uint8_t configuration_descriptor_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, STRID_AUDIO_CTRL, 0, EP_AUDIO_MIC_IN, 0),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, false, sizeof(hid_report_descriptor), EP_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

_Static_assert(sizeof(configuration_descriptor_fs) == CONFIG_TOTAL_LEN, "FS descriptor length mismatch");
_Static_assert(sizeof(configuration_descriptor_hs) == CONFIG_TOTAL_LEN, "HS descriptor length mismatch");

#elif CFG_TUD_AUDIO
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN)

static const uint8_t configuration_descriptor_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, STRID_AUDIO_CTRL, 0, EP_AUDIO_MIC_IN, 0),
};

static const uint8_t configuration_descriptor_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, STRID_AUDIO_CTRL, 0, EP_AUDIO_MIC_IN, 0),
};

_Static_assert(sizeof(configuration_descriptor_fs) == CONFIG_TOTAL_LEN, "FS descriptor length mismatch");
_Static_assert(sizeof(configuration_descriptor_hs) == CONFIG_TOTAL_LEN, "HS descriptor length mismatch");

#elif CFG_TUD_HID
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t configuration_descriptor_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, false, sizeof(hid_report_descriptor), EP_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

static const uint8_t configuration_descriptor_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, false, sizeof(hid_report_descriptor), EP_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

_Static_assert(sizeof(configuration_descriptor_fs) == CONFIG_TOTAL_LEN, "FS descriptor length mismatch");
_Static_assert(sizeof(configuration_descriptor_hs) == CONFIG_TOTAL_LEN, "HS descriptor length mismatch");

#else
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN)

static const uint8_t configuration_descriptor_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
};

static const uint8_t configuration_descriptor_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
};

_Static_assert(sizeof(configuration_descriptor_fs) == CONFIG_TOTAL_LEN, "FS descriptor length mismatch");
_Static_assert(sizeof(configuration_descriptor_hs) == CONFIG_TOTAL_LEN, "HS descriptor length mismatch");

#endif

// String descriptors
static const char string_langid[] = { 0x09, 0x04 };
static char serial_string[13];

static const char *string_desc_table[STRID_COUNT] = {
    string_langid,
    "CaptureKVM",
    "CaptureKVMBridge",
    serial_string,
#if CFG_TUD_AUDIO
    "Audio Control",
    "Microphone Stream",
#endif
#if CFG_TUD_HID
    "Keyboard/Mouse",
#endif
};


// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

static bool s_usb_ready = false;
#if CFG_TUD_HID
static bool s_usb_suspended = false;
static bool s_remote_wakeup_enabled = false;
static int64_t s_last_remote_wakeup_us = 0;
typedef enum {
    HID_EVENT_KIND_KEYBOARD = 1,
    HID_EVENT_KIND_MOUSE,
    HID_EVENT_KIND_MOUSE_ABSOLUTE,
} hid_event_kind_t;

typedef struct {
    hid_event_kind_t kind;
    union {
        usb_keyboard_report_t keyboard;
        usb_mouse_report_t mouse;
        usb_mouse_absolute_report_t mouse_abs;
    } report;
} hid_event_t;

static QueueHandle_t s_hid_event_queue = NULL;
static uint32_t s_hid_queue_drop_count = 0;
#endif
#if CFG_TUD_AUDIO
static RingbufHandle_t s_mic_ring = NULL;
static int64_t s_last_mic_drop_log_us = 0;
#endif

#if CFG_TUD_HID
static int16_t scale_abs_axis(uint32_t value, uint32_t max_cfg)
{
    if (max_cfg <= 1) {
        return value ? INT16_MAX : 0;
    }

    if (value >= max_cfg) {
        value = max_cfg - 1;
    }

    int32_t scaled = (int32_t)value * INT16_MAX / (int32_t)(max_cfg - 1);
    return (int16_t)scaled;
}
#endif

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static void fill_serial_string(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(serial_string, sizeof(serial_string), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static tinyusb_port_t get_usb_device_port(void)
{
#if (SOC_USB_OTG_PERIPH_NUM > 1)
    return TINYUSB_PORT_HIGH_SPEED_0;
#else
    return TINYUSB_PORT_FULL_SPEED_0;
#endif
}

static void tinyusb_event_handler(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_usb_ready = true;
        app_state_update_usb_state(APP_USB_STATE_MOUNTED);
        break;
    case TINYUSB_EVENT_DETACHED:
        s_usb_ready = false;
        app_state_update_usb_state(APP_USB_STATE_DISCONNECTED);
        break;
    default:
        break;
    }
}

#if CFG_TUD_AUDIO
static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx)
{
    (void)ctx;
    size_t total = 0;

    while (total < len) {
        size_t fetched = 0;
        uint8_t *chunk = (uint8_t *) xRingbufferReceiveUpTo(s_mic_ring, &fetched, 0, len - total);
        if (!chunk) {
            break;
        }
        memcpy(buf + total, chunk, fetched);
        vRingbufferReturnItem(s_mic_ring, chunk);
        total += fetched;
    }

    if (total < len) {
        memset(buf + total, 0, len - total);
    } else {
        app_state_update_usb_state(APP_USB_STATE_STREAMING);
    }

    *bytes_read = len;
    return ESP_OK;
}
#endif

#if CFG_TUD_HID
static void update_hid_activity(bool keyboard, bool mouse)
{
    app_state_mark_feature_usage(keyboard, mouse, false);
}

static bool hid_queue_has_pending_reports(void)
{
    return s_hid_event_queue && uxQueueMessagesWaiting(s_hid_event_queue) > 0;
}

static void request_remote_wakeup_if_needed(void)
{
    if (!s_usb_suspended || !s_remote_wakeup_enabled) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if ((now - s_last_remote_wakeup_us) < 20000) {
        return;
    }

    if (tud_remote_wakeup()) {
        s_last_remote_wakeup_us = now;
        s_remote_wakeup_enabled = false;
        ESP_LOGI(TAG, "Remote wakeup signalled");
    } else {
        ESP_LOGW(TAG, "Remote wakeup request failed");
    }
}

static void enqueue_hid_event(const hid_event_t *event)
{
    if (!event || !s_hid_event_queue) {
        return;
    }

    if (xQueueSend(s_hid_event_queue, event, 0) == pdTRUE) {
        return;
    }

    s_hid_queue_drop_count++;
    if (s_hid_queue_drop_count == 1U || (s_hid_queue_drop_count % 16U) == 0U) {
        ESP_LOGW(TAG, "HID event queue full, dropping event kind=%d (drops=%u)",
                 (int)event->kind, (unsigned)s_hid_queue_drop_count);
    }
}

static bool send_keyboard_report_now(const usb_keyboard_report_t *report)
{
    if (!tud_hid_ready()) {
        return false;
    }

    bool ok = tud_hid_report(HID_REPORT_ID_KEYBOARD, report, sizeof(*report));
    if (ok) {
        update_hid_activity(true, false);
    }
    return ok;
}

static bool send_mouse_report_now(const usb_mouse_report_t *report)
{
    if (!tud_hid_ready()) {
        return false;
    }

    hid_mouse_report_t hid_report = {
        .buttons = report->buttons & 0x1F,
        .x = report->x,
        .y = report->y,
        .wheel = report->wheel,
        .pan = report->pan,
    };

    bool ok = tud_hid_report(HID_REPORT_ID_MOUSE_REL, &hid_report, sizeof(hid_report));
    if (ok) {
        update_hid_activity(false, true);
    }
    return ok;
}

static bool send_mouse_abs_report_now(const usb_mouse_absolute_report_t *report)
{
    if (!tud_hid_ready()) {
        return false;
    }

    int16_t scaled_x = scale_abs_axis(report->x, (uint32_t)CONFIG_APP_ABS_MOUSE_MAX_X);
    int16_t scaled_y = scale_abs_axis(report->y, (uint32_t)CONFIG_APP_ABS_MOUSE_MAX_Y);

    hid_abs_mouse_report_t hid_report = {
        .buttons = report->buttons & 0x1F,
        .x = scaled_x,
        .y = scaled_y,
        .wheel = report->wheel,
        .pan = report->pan,
    };

    bool ok = tud_hid_report(HID_REPORT_ID_MOUSE_ABS, &hid_report, sizeof(hid_report));
    if (ok) {
        update_hid_activity(false, true);
    }
    return ok;
}
#else
static inline void update_hid_activity(bool keyboard, bool mouse)
{
    (void)keyboard;
    (void)mouse;
}
#endif

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

esp_err_t usb_hs_device_init(void)
{
    fill_serial_string();

#if CFG_TUD_HID
    if (!s_hid_event_queue) {
        s_hid_event_queue = xQueueCreate(HID_EVENT_QUEUE_LEN, sizeof(hid_event_t));
        ESP_RETURN_ON_FALSE(s_hid_event_queue != NULL, ESP_ERR_NO_MEM, TAG, "hid event queue alloc failed");
    }
#endif

#if CFG_TUD_AUDIO
    if (!s_mic_ring) {
        s_mic_ring = xRingbufferCreate(MIC_RING_BUFFER_BYTES, RINGBUF_TYPE_BYTEBUF);
        ESP_RETURN_ON_FALSE(s_mic_ring != NULL, ESP_ERR_NO_MEM, TAG, "mic ring buffer alloc failed");
    }
    ESP_LOGI(TAG, "Audio desc len=%d, total=%d, AS interfaces=%d", (int)CFG_TUD_AUDIO_FUNC_1_DESC_LEN, (int)CONFIG_TOTAL_LEN, (int)CFG_TUD_AUDIO_FUNC_1_N_AS_INT);
#endif

    tinyusb_desc_config_t desc = {
        .device = &device_descriptor,
        .qualifier = TUD_OPT_HIGH_SPEED ? &device_qualifier : NULL,
        .string = string_desc_table,
        .string_count = STRID_COUNT,
        .full_speed_config = configuration_descriptor_fs,
        .high_speed_config = TUD_OPT_HIGH_SPEED ? configuration_descriptor_hs : NULL,
    };

    tinyusb_config_t tusb_cfg = {
        .port = get_usb_device_port(),
        .phy = {
            .skip_setup = false,
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        .task = {
            .size = 4096,
            .priority = 5,
            .xCoreID = 0,
        },
        .descriptor = desc,
        .event_cb = tinyusb_event_handler,
        .event_arg = NULL,
    };

    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb install failed");

    #if CFG_TUD_AUDIO
    uac_device_config_t uac_cfg = {
        .skip_tinyusb_init = true,
        .output_cb = NULL,
        .input_cb = uac_input_cb,
        .set_mute_cb = NULL,
        .set_volume_cb = NULL,
        .cb_ctx = NULL,
#if CONFIG_USB_DEVICE_UAC_AS_PART
        .spk_itf_num = -1,
        .mic_itf_num = ITF_NUM_AUDIO_STREAMING_MIC,
#endif
    };
    ESP_RETURN_ON_ERROR(uac_device_init(&uac_cfg), TAG, "uac init failed");
    #endif

    app_state_update_usb_state(APP_USB_STATE_DISCONNECTED);
    return ESP_OK;
}

#if CFG_TUD_HID
void usb_hs_handle_keyboard(const usb_keyboard_report_t *report)
{
    if (!s_usb_ready || !report) {
        return;
    }

    hid_event_t event = {
        .kind = HID_EVENT_KIND_KEYBOARD,
        .report.keyboard = *report,
    };
    enqueue_hid_event(&event);
}

void usb_hs_handle_mouse(const usb_mouse_report_t *report)
{
    if (!s_usb_ready || !report) {
        return;
    }

    hid_event_t event = {
        .kind = HID_EVENT_KIND_MOUSE,
        .report.mouse = *report,
    };
    enqueue_hid_event(&event);
}

void usb_hs_handle_mouse_absolute(const usb_mouse_absolute_report_t *report)
{
    if (!s_usb_ready || !report) {
        return;
    }

    hid_event_t event = {
        .kind = HID_EVENT_KIND_MOUSE_ABSOLUTE,
        .report.mouse_abs = *report,
    };
    enqueue_hid_event(&event);
}
#else
void usb_hs_handle_keyboard(const usb_keyboard_report_t *report)
{
    (void)report;
}

void usb_hs_handle_mouse(const usb_mouse_report_t *report)
{
    (void)report;
}

void usb_hs_handle_mouse_absolute(const usb_mouse_absolute_report_t *report)
{
    (void)report;
}
#endif

void usb_hs_handle_microphone_frame(const usb_microphone_frame_t *frame)
{
#if !CFG_TUD_AUDIO
    (void)frame;
    return;
#else
    if (!frame || frame->sample_count == 0 || frame->samples == NULL) {
        return;
    }

    const uint16_t *samples = frame->samples;
    size_t sample_count = frame->sample_count;
    size_t bytes = sample_count * sizeof(uint16_t);

    BaseType_t sent = xRingbufferSend(s_mic_ring, samples, bytes, 0);
    if (sent != pdTRUE) {
        size_t dropped = 0;
        uint8_t *oldest = (uint8_t *) xRingbufferReceiveUpTo(s_mic_ring, &dropped, 0, bytes);
        if (oldest) {
            vRingbufferReturnItem(s_mic_ring, oldest);
            sent = xRingbufferSend(s_mic_ring, samples, bytes, 0);
        }
    }

    if (sent != pdTRUE) {
        int64_t now = esp_timer_get_time();
        if (now - s_last_mic_drop_log_us > 500000) {
            size_t free_bytes = s_mic_ring ? xRingbufferGetCurFreeSize(s_mic_ring) : 0;
            ESP_LOGW(TAG, "mic ring buffer full, dropping audio (free=%u)", (unsigned)free_bytes);
            s_last_mic_drop_log_us = now;
        }
        return;
    }

    app_state_mark_feature_usage(false, false, true);
#endif
}

void usb_hs_poll(void)
{
#if CFG_TUD_HID
    if (!s_usb_ready) {
        return;
    }

    if (hid_queue_has_pending_reports()) {
        request_remote_wakeup_if_needed();
    }

    hid_event_t event;
    while (tud_hid_ready() && s_hid_event_queue && xQueueReceive(s_hid_event_queue, &event, 0) == pdTRUE) {
        bool ok = false;

        switch (event.kind) {
        case HID_EVENT_KIND_KEYBOARD:
            ok = send_keyboard_report_now(&event.report.keyboard);
            break;
        case HID_EVENT_KIND_MOUSE:
            ok = send_mouse_report_now(&event.report.mouse);
            break;
        case HID_EVENT_KIND_MOUSE_ABSOLUTE:
            ok = send_mouse_abs_report_now(&event.report.mouse_abs);
            break;
        default:
            break;
        }

        if (!ok) {
            if (xQueueSendToFront(s_hid_event_queue, &event, 0) != pdTRUE) {
                ESP_LOGW(TAG, "failed to requeue HID event kind=%d", (int)event.kind);
            }
            break;
        }
    }
#else
    (void)s_usb_ready;
#endif
}

void usb_hs_on_suspend(bool remote_wakeup_enabled)
{
#if CFG_TUD_HID
    s_usb_suspended = true;
    s_remote_wakeup_enabled = remote_wakeup_enabled;
    s_last_remote_wakeup_us = 0;
    ESP_LOGI(TAG, "USB suspended (remote wake %s)", remote_wakeup_enabled ? "enabled" : "disabled");
#else
    (void)remote_wakeup_enabled;
#endif
}

void usb_hs_on_resume(void)
{
#if CFG_TUD_HID
    s_usb_suspended = false;
    s_remote_wakeup_enabled = false;
    s_last_remote_wakeup_us = 0;
    ESP_LOGI(TAG, "USB resumed");
#endif
}

// -----------------------------------------------------------------------------
// TinyUSB callbacks
// -----------------------------------------------------------------------------

#if CFG_TUD_HID
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}
#else
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return NULL;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}
#endif
