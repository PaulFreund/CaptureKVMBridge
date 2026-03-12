#include "input_activity_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <stdbool.h>

#ifndef CONFIG_APP_TLV_ACTIVITY_LED_ACTIVE_LOW
#define CONFIG_APP_TLV_ACTIVITY_LED_ACTIVE_LOW 0
#endif

#ifndef CONFIG_APP_TLV_ACTIVITY_LED_PULSE_MS
#define CONFIG_APP_TLV_ACTIVITY_LED_PULSE_MS 120
#endif

static const char *TAG = "tlv_led";

static bool s_led_enabled = false;
static esp_timer_handle_t s_led_off_timer = NULL;

static int get_led_active_level(void)
{
#if CONFIG_APP_TLV_ACTIVITY_LED_ACTIVE_LOW
    return 0;
#else
    return 1;
#endif
}

static int get_led_inactive_level(void)
{
    return get_led_active_level() ? 0 : 1;
}

static void led_off_timer_cb(void *arg)
{
    (void)arg;

    if (!s_led_enabled) {
        return;
    }

    gpio_set_level((gpio_num_t)CONFIG_APP_TLV_ACTIVITY_LED_GPIO, get_led_inactive_level());
}

esp_err_t input_activity_led_init(void)
{
    if (s_led_off_timer) {
        return ESP_OK;
    }

    const int led_gpio = CONFIG_APP_TLV_ACTIVITY_LED_GPIO;

    if (led_gpio < 0) {
        ESP_LOGI(TAG, "TLV activity LED disabled");
        return ESP_OK;
    }

    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << led_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_level((gpio_num_t)led_gpio, get_led_inactive_level());
    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = led_off_timer_cb,
        .arg = NULL,
        .name = "tlv_led_off",
    };

    err = esp_timer_create(&timer_args, &s_led_off_timer);
    if (err != ESP_OK) {
        return err;
    }

    s_led_enabled = true;
    ESP_LOGI(TAG, "TLV activity LED on GPIO%d (%s)", led_gpio,
             get_led_active_level() ? "active-high" : "active-low");
    return ESP_OK;
}

void input_activity_led_pulse(void)
{
    if (!s_led_enabled) {
        return;
    }

    gpio_set_level((gpio_num_t)CONFIG_APP_TLV_ACTIVITY_LED_GPIO, get_led_active_level());
    esp_err_t stop_err = esp_timer_stop(s_led_off_timer);
    if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "failed to stop TLV LED timer: %s", esp_err_to_name(stop_err));
    }

    esp_err_t start_err = esp_timer_start_once(s_led_off_timer,
                                               (uint64_t)CONFIG_APP_TLV_ACTIVITY_LED_PULSE_MS * 1000ULL);
    if (start_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to start TLV LED timer: %s", esp_err_to_name(start_err));
    }
}
