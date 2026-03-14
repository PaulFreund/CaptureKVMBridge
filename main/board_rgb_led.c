#include "board_rgb_led.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "sdkconfig.h"

#ifndef CONFIG_APP_BOARD_RGB_LED_PULSE_MS
#define CONFIG_APP_BOARD_RGB_LED_PULSE_MS 120
#endif

static const char *TAG = "board_rgb_led";
static led_strip_handle_t s_led_strip = NULL;
static esp_timer_handle_t s_led_off_timer = NULL;
static SemaphoreHandle_t s_led_lock = NULL;

static esp_err_t write_color_locked(uint8_t red, uint8_t green, uint8_t blue)
{
    esp_err_t err = led_strip_set_pixel(s_led_strip, 0, red, green, blue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to set RGB LED color: %s", esp_err_to_name(err));
        return err;
    }

    err = led_strip_refresh(s_led_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to refresh RGB LED color: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static void stop_led_timer(void)
{
    if (!s_led_off_timer) {
        return;
    }

    esp_err_t err = esp_timer_stop(s_led_off_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "failed to stop RGB LED timer: %s", esp_err_to_name(err));
    }
}

static void led_off_timer_cb(void *arg)
{
    (void)arg;

    if (!s_led_strip || !s_led_lock) {
        return;
    }

    if (xSemaphoreTake(s_led_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }

    (void)write_color_locked(0, 0, 0);
    xSemaphoreGive(s_led_lock);
}

esp_err_t board_rgb_led_init(void)
{
    if (s_led_strip) {
        return ESP_OK;
    }

    if (CONFIG_APP_BOARD_RGB_LED_GPIO < 0) {
        ESP_LOGI(TAG, "board RGB LED disabled");
        return ESP_OK;
    }

    if (!s_led_lock) {
        s_led_lock = xSemaphoreCreateMutex();
        if (!s_led_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    const led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_APP_BOARD_RGB_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create RGB LED driver on GPIO%d: %s",
                 CONFIG_APP_BOARD_RGB_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = led_off_timer_cb,
        .arg = NULL,
        .name = "rgb_led_off",
    };
    err = esp_timer_create(&timer_args, &s_led_off_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create RGB LED timer: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "board RGB LED ready on GPIO%d", CONFIG_APP_BOARD_RGB_LED_GPIO);
    return board_rgb_led_set_color(0, 0, 0);
}

esp_err_t board_rgb_led_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (CONFIG_APP_BOARD_RGB_LED_GPIO < 0) {
        return ESP_OK;
    }

    if (!s_led_strip) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_led_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    stop_led_timer();

    if (xSemaphoreTake(s_led_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = write_color_locked(red, green, blue);
    xSemaphoreGive(s_led_lock);
    return err;
}

esp_err_t board_rgb_led_pulse_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (CONFIG_APP_BOARD_RGB_LED_GPIO < 0) {
        return ESP_OK;
    }

    esp_err_t err = board_rgb_led_set_color(red, green, blue);
    if (err != ESP_OK) {
        return err;
    }

    if (!s_led_off_timer) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_timer_start_once(s_led_off_timer,
                               (uint64_t)CONFIG_APP_BOARD_RGB_LED_PULSE_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to start RGB LED timer: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
