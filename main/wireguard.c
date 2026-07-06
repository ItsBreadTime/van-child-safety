#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wireguard.h"
#include "common.h"
#include "wireguard.h"
#include "secrets.h"

static const char *TAG = "WG";

static wireguard_config_t s_wg_config;
static wireguard_ctx_t s_wg_ctx;
static bool s_wg_initialized = false;

#define WG_NTP_WAIT_TIMEOUT_MS 60000

void wireguard_task(void *pvParameters)
{
    EventBits_t bits = xEventGroupWaitBits(
        g_event_group, NTP_SYNCED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(WG_NTP_WAIT_TIMEOUT_MS));
    if (!(bits & NTP_SYNCED_BIT)) {
        ESP_LOGW(TAG, "NTP not synced after %d ms; starting WireGuard anyway",
                 WG_NTP_WAIT_TIMEOUT_MS);
    }

    ESP_LOGI(TAG, "Initializing WireGuard (stack watermark=%d)", uxTaskGetStackHighWaterMark(NULL));

    s_wg_config = (wireguard_config_t)ESP_WIREGUARD_CONFIG_DEFAULT();
    s_wg_config.private_key = WG_PRIVATE_KEY;
    s_wg_config.public_key = WG_PUBLIC_KEY;
    s_wg_config.preshared_key = WG_PRESHARED_KEY;
    s_wg_config.allowed_ip = WG_ADDRESS;
    s_wg_config.allowed_ip_mask = WG_ADDRESS_MASK;
    s_wg_config.endpoint = WG_ENDPOINT;
    s_wg_config.port = WG_PORT;
    s_wg_config.listen_port = 0;
    s_wg_config.persistent_keepalive = CONFIG_WG_PERSISTENT_KEEPALIVE;

    esp_err_t err = esp_wireguard_init(&s_wg_config, &s_wg_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    s_wg_initialized = true;
    ESP_LOGI(TAG, "Init OK, connecting (stack watermark=%d)", uxTaskGetStackHighWaterMark(NULL));

    while (1) {
        err = esp_wireguard_connect(&s_wg_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Connect failed: %s (stack watermark=%d)", esp_err_to_name(err), uxTaskGetStackHighWaterMark(NULL));
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        ESP_LOGI(TAG, "Connect sent, waiting for peer");

        int retry = 0;
        while (esp_wireguardif_peer_is_up(&s_wg_ctx) != ESP_OK && retry < 60) {
            if (retry % 10 == 0) ESP_LOGI(TAG, "Waiting for peer... (%ds)", retry);
            vTaskDelay(pdMS_TO_TICKS(1000));
            retry++;
        }

        if (esp_wireguardif_peer_is_up(&s_wg_ctx) == ESP_OK) {
            ESP_LOGI(TAG, "Tunnel is up");
            esp_wireguard_set_default(&s_wg_ctx);
            xEventGroupSetBits(g_event_group, WG_CONNECTED_BIT);
        } else {
            ESP_LOGE(TAG, "Peer not reachable, retrying in 30s");
            xEventGroupClearBits(g_event_group, WG_CONNECTED_BIT);
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(30000));
            if (esp_wireguardif_peer_is_up(&s_wg_ctx) != ESP_OK) {
                ESP_LOGW(TAG, "Tunnel down, reconnecting");
                xEventGroupClearBits(g_event_group, WG_CONNECTED_BIT);
                break;
            }
        }
    }
}
