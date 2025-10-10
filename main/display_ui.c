#include "display_ui.h"

#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_lcd_touch.h"
#include "esp_lvgl_port_touch.h"
#include "bsp/touch.h"
#include "esp_lcd_panel_ops.h"
#include <stdio.h>

static const char *TAG = "display";

#define DISPLAY_SLEEP_TIMEOUT_US   (30LL * 1000000LL)

static lv_display_t *s_display = NULL;
static lv_obj_t *s_state_label = NULL;
static lv_obj_t *s_source_label = NULL;
static lv_obj_t *s_keyboard_label = NULL;
static lv_obj_t *s_mouse_label = NULL;
static lv_obj_t *s_mic_label = NULL;

static lv_style_t s_header_style;
static lv_style_t s_body_style;
static bool s_styles_ready = false;

static bsp_lcd_handles_t s_lcd_handles;
static bool s_lcd_ready = false;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static bool s_display_awake = false;
static int64_t s_last_touch_timestamp_us = 0;

static void init_label_styles(lv_obj_t *reference_obj)
{
    if (s_styles_ready) {
        return;
    }

    lv_style_init(&s_header_style);
#if LV_FONT_MONTSERRAT_32
    lv_style_set_text_font(&s_header_style, &lv_font_montserrat_32);
#elif LV_FONT_MONTSERRAT_28
    lv_style_set_text_font(&s_header_style, &lv_font_montserrat_28);
#else
    lv_style_set_text_font(&s_header_style, lv_theme_get_font_large(reference_obj));
#endif
    lv_style_set_text_color(&s_header_style, lv_color_white());
    lv_style_set_text_align(&s_header_style, LV_TEXT_ALIGN_LEFT);

    lv_style_init(&s_body_style);
#if LV_FONT_MONTSERRAT_28
    lv_style_set_text_font(&s_body_style, &lv_font_montserrat_28);
#elif LV_FONT_MONTSERRAT_26
    lv_style_set_text_font(&s_body_style, &lv_font_montserrat_26);
#elif LV_FONT_MONTSERRAT_24
    lv_style_set_text_font(&s_body_style, &lv_font_montserrat_24);
#else
    lv_style_set_text_font(&s_body_style, lv_theme_get_font_medium(reference_obj));
#endif
    lv_style_set_text_color(&s_body_style, lv_color_white());
    lv_style_set_text_align(&s_body_style, LV_TEXT_ALIGN_LEFT);

    s_styles_ready = true;
}

static const char *state_to_string(app_usb_state_t state)
{
    switch (state) {
    case APP_USB_STATE_DISCONNECTED:
        return "Disconnected";
    case APP_USB_STATE_MOUNTED:
        return "Ready";
    case APP_USB_STATE_STREAMING:
        return "Streaming";
    default:
        return "Unknown";
    }
}

static const char *source_to_string(app_input_source_t source)
{
    switch (source) {
    case APP_INPUT_SOURCE_NONE:
        return "None";
    case APP_INPUT_SOURCE_NETWORK:
        return "Network";
    case APP_INPUT_SOURCE_USB_JTAG:
        return "USB-JTAG";
    case APP_INPUT_SOURCE_UART:
        return "UART";
    default:
        return "Other";
    }
}

static void set_feature_style(lv_obj_t *label, bool active)
{
    if (!label) {
        return;
    }
    lv_color_t color = active ? lv_palette_main(LV_PALETTE_GREEN) : lv_color_white();
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void display_wake(void)
{
    if (s_display_awake) {
        return;
    }

    esp_err_t backlight_err = bsp_display_backlight_on();
    if (backlight_err != ESP_OK) {
        ESP_LOGW(TAG, "backlight enable failed: 0x%x", (int)backlight_err);
    }

    s_display_awake = true;
    ESP_LOGI(TAG, "display awake");
}

static void display_sleep(void)
{
    if (!s_display_awake) {
        return;
    }

    esp_err_t backlight_err = bsp_display_backlight_off();
    if (backlight_err != ESP_OK) {
        ESP_LOGW(TAG, "backlight disable failed: 0x%x", (int)backlight_err);
    }

    s_display_awake = false;
    ESP_LOGI(TAG, "display sleeping");
}

static void poll_touch_activity(int64_t now_us)
{
    if (!s_touch_handle) {
        return;
    }

    if (esp_lcd_touch_read_data(s_touch_handle) != ESP_OK) {
        return;
    }

    uint16_t touch_x[1];
    uint16_t touch_y[1];
    uint16_t touch_strength[1];
    uint8_t touch_points = 0;

    bool touched = esp_lcd_touch_get_coordinates(s_touch_handle, touch_x, touch_y, touch_strength, &touch_points, 1);
    if (touched && touch_points > 0) {
        s_last_touch_timestamp_us = now_us;
        if (!s_display_awake) {
            display_wake();
        }
    }
}

static void update_display_power_state(int64_t now_us, int64_t latest_activity_us)
{
    if (latest_activity_us <= 0) {
        display_wake();
        return;
    }

    int64_t inactive_us = now_us - latest_activity_us;
    if (inactive_us < 0) {
        inactive_us = 0;
    }

    if (inactive_us >= DISPLAY_SLEEP_TIMEOUT_US) {
        display_sleep();
    } else {
        display_wake();
    }
}

esp_err_t display_ui_init(void)
{
    if (s_display) {
        return ESP_OK;
    }

    static bool s_lvgl_initialized = false;
    if (!s_lvgl_initialized) {
        lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl init failed");
        ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "backlight init failed");
        s_lvgl_initialized = true;
    }

    if (!s_lcd_ready) {
        ESP_RETURN_ON_ERROR(bsp_display_new_with_handles(NULL, &s_lcd_handles), TAG, "display new failed");
        s_panel_handle = s_lcd_handles.panel;
        s_lcd_ready = true;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_handles.io,
        .panel_handle = s_lcd_handles.panel,
        .control_handle = s_lcd_handles.control,
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
#if LVGL_VERSION_MAJOR >= 9
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
        .color_format = LV_COLOR_FORMAT_RGB888,
#else
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
#endif
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = (BSP_LCD_BIGENDIAN ? true : false),
#endif
#if CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR
            .sw_rotate = false,
#else
            .sw_rotate = true,
#endif
#if CONFIG_BSP_DISPLAY_LVGL_FULL_REFRESH
            .full_refresh = true,
#elif CONFIG_BSP_DISPLAY_LVGL_DIRECT_MODE
            .direct_mode = true,
#endif
        }
    };

    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {
#if CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR
            .avoid_tearing = true,
#else
            .avoid_tearing = false,
#endif
        }
    };

    s_display = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    ESP_RETURN_ON_FALSE(s_display != NULL, ESP_FAIL, TAG, "lvgl disp add failed");

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(s_panel_handle, true));
    lv_disp_set_default(s_display);

    if (!s_touch_handle) {
        esp_err_t touch_err = bsp_touch_new(NULL, &s_touch_handle);
        if (touch_err != ESP_OK) {
            ESP_LOGW(TAG, "touch init failed: 0x%x", (int)touch_err);
        } else {
            const lvgl_port_touch_cfg_t touch_cfg = {
                .disp = s_display,
                .handle = s_touch_handle,
                .scale = {
                    .x = 1.0f,
                    .y = 1.0f,
                },
            };
            lv_indev_t *touch_indev = lvgl_port_add_touch(&touch_cfg);
            if (!touch_indev) {
                ESP_LOGW(TAG, "lvgl touch registration failed");
            }
        }
    }

    bsp_display_backlight_on();
    s_display_awake = true;
    s_last_touch_timestamp_us = esp_timer_get_time();

    ESP_RETURN_ON_FALSE(bsp_display_lock(portMAX_DELAY), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    lv_obj_t *scr = lv_disp_get_scr_act(s_display);
    init_label_styles(scr);

    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, lv_color_white(), LV_PART_MAIN);

    s_state_label = lv_label_create(scr);
    lv_obj_add_style(s_state_label, &s_header_style, LV_PART_MAIN);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_LEFT, 32, 36);
    lv_label_set_text(s_state_label, "USB: -");

    s_source_label = lv_label_create(scr);
    lv_obj_add_style(s_source_label, &s_body_style, LV_PART_MAIN);
    lv_obj_align_to(s_source_label, s_state_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 40);
    lv_label_set_text(s_source_label, "Last input: -");

    s_keyboard_label = lv_label_create(scr);
    lv_obj_add_style(s_keyboard_label, &s_body_style, LV_PART_MAIN);
    lv_label_set_text(s_keyboard_label, LV_SYMBOL_KEYBOARD " Keyboard");
    lv_obj_align_to(s_keyboard_label, s_source_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 56);

    s_mouse_label = lv_label_create(scr);
    lv_obj_add_style(s_mouse_label, &s_body_style, LV_PART_MAIN);
    lv_label_set_text(s_mouse_label, LV_SYMBOL_EDIT " Mouse");
    lv_obj_align_to(s_mouse_label, s_keyboard_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 32);

    s_mic_label = lv_label_create(scr);
    lv_obj_add_style(s_mic_label, &s_body_style, LV_PART_MAIN);
    lv_label_set_text(s_mic_label, LV_SYMBOL_AUDIO " Microphone");
    lv_obj_align_to(s_mic_label, s_mouse_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 32);

    ESP_LOGI(TAG, "LVGL resolution: %d x %d",
             (int)lv_disp_get_hor_res(s_display), (int)lv_disp_get_ver_res(s_display));

    bsp_display_unlock();

    ESP_LOGI(TAG, "display initialized");
    return ESP_OK;
}

void display_ui_update(const app_status_snapshot_t *snapshot)
{
    if (!snapshot || !s_display) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    poll_touch_activity(now_us);

    int64_t latest_activity_us = snapshot->last_event_timestamp_us;
    if (latest_activity_us <= 0 || s_last_touch_timestamp_us > latest_activity_us) {
        latest_activity_us = s_last_touch_timestamp_us;
    }

    update_display_power_state(now_us, latest_activity_us);

    char buffer[64];

    if (!bsp_display_lock(portMAX_DELAY)) {
        return;
    }

    if (s_state_label) {
        snprintf(buffer, sizeof(buffer), "USB: %s", state_to_string(snapshot->hs_state));
        lv_label_set_text(s_state_label, buffer);
    }

    if (s_source_label) {
        snprintf(buffer, sizeof(buffer), "Last input: %s", source_to_string(snapshot->last_source));
        lv_label_set_text(s_source_label, buffer);
    }

    set_feature_style(s_keyboard_label, snapshot->hs_keyboard_active);
    set_feature_style(s_mouse_label, snapshot->hs_mouse_active);
    set_feature_style(s_mic_label, snapshot->hs_microphone_active);

    bsp_display_unlock();
}
