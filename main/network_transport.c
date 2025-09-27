#include "network_transport.h"
#include "protocol_tlv.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "esp_netif.h"
#if CONFIG_APP_WIFI_ENABLE && SOC_WIFI_SUPPORTED
#include "esp_wifi.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "sdkconfig.h"
#include <errno.h>

#define UDP_TASK_STACK   4096
#define UDP_TASK_PRIO    7

static const char *TAG = "net_tlv";
static network_tlv_callback_t s_callback = NULL;
#if CONFIG_APP_WIFI_ENABLE && SOC_WIFI_SUPPORTED
static TaskHandle_t s_udp_task = NULL;
static bool s_wifi_started = false;
static void udp_listener_task(void *arg)
{
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_APP_TLV_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Listening for TLV packets on UDP port %u", CONFIG_APP_TLV_UDP_PORT);

    uint8_t buffer[CONFIG_APP_TLV_MAX_PAYLOAD + 3];

    while (1) {
        ssize_t len = recvfrom(sock, buffer, sizeof(buffer), 0, NULL, NULL);
        if (len < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (len < 5) {
            ESP_LOGW(TAG, "discarding short packet (%d bytes)", (int)len);
            continue;
        }

        if (buffer[0] != PROTOCOL_TLV_SYNC0 || buffer[1] != PROTOCOL_TLV_SYNC1) {
            ESP_LOGW(TAG, "missing TLV sync word (0x%02X 0x%02X)", buffer[0], buffer[1]);
            continue;
        }

        uint8_t type = buffer[2];
        if (!protocol_tlv_type_is_valid(type)) {
            ESP_LOGW(TAG, "unknown TLV type 0x%02X (udp)", type);
            continue;
        }

        uint16_t payload_len = ((uint16_t)buffer[3] << 8) | buffer[4];
        if ((size_t)payload_len > (size_t)CONFIG_APP_TLV_MAX_PAYLOAD) {
            ESP_LOGW(TAG, "payload exceeds limit (%u)", payload_len);
            continue;
        }
        if ((size_t)len != (size_t)(payload_len + 5)) {
            ESP_LOGW(TAG, "length mismatch: header=%u, packet=%d", payload_len, (int)len);
            continue;
        }

        if (!s_callback) {
            continue;
        }

        s_callback(APP_INPUT_SOURCE_NETWORK, &buffer[2], (size_t)(payload_len + 3));
    }
}

static esp_err_t start_wifi_softap(void)
{
    if (s_wifi_started) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "netif init");
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "event loop");
    }

    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_FAIL, TAG, "create default wifi ap");

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "wifi init");

    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, CONFIG_APP_WIFI_AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(CONFIG_APP_WIFI_AP_SSID);
    ap_cfg.ap.channel = CONFIG_APP_WIFI_AP_CHANNEL;
    ap_cfg.ap.max_connection = 4;

    size_t password_len = strlen(CONFIG_APP_WIFI_AP_PASSWORD);
    if (password_len >= 8) {
        strlcpy((char *)ap_cfg.ap.password, CONFIG_APP_WIFI_AP_PASSWORD, sizeof(ap_cfg.ap.password));
        ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    s_wifi_started = true;
    ESP_LOGI(TAG, "SoftAP started: SSID='%s' channel=%d", CONFIG_APP_WIFI_AP_SSID, CONFIG_APP_WIFI_AP_CHANNEL);
    return ESP_OK;
}
#endif

esp_err_t network_transport_start(network_tlv_callback_t cb)
{
    s_callback = cb;

#if !CONFIG_APP_WIFI_ENABLE
    ESP_LOGW(TAG, "Wi-Fi transport disabled in menuconfig");
    return ESP_OK;
#elif !SOC_WIFI_SUPPORTED
    ESP_LOGW(TAG, "Wi-Fi not supported on this target; network transport disabled");
    return ESP_OK;
#else
    ESP_RETURN_ON_ERROR(start_wifi_softap(), TAG, "start wifi");

    if (!s_udp_task) {
        BaseType_t res = xTaskCreatePinnedToCore(udp_listener_task, "tlv_udp", UDP_TASK_STACK, NULL,
                                                 UDP_TASK_PRIO, &s_udp_task, tskNO_AFFINITY);
        ESP_RETURN_ON_FALSE(res == pdPASS, ESP_FAIL, TAG, "create udp task failed");
    }

    return ESP_OK;
#endif
}
