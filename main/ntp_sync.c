#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "common.h"
#include "ntp_sync.h"

static const char *TAG = "NTP";

void ntp_sync_task(void *pvParameters)
{
    xEventGroupWaitBits(g_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    while (1) {
        int retry = 0;
        while (esp_netif_sntp_sync_wait(2000 / portTICK_PERIOD_MS) == ESP_ERR_TIMEOUT && retry < 30) {
            ESP_LOGI(TAG, "Waiting for NTP sync... (%d)", retry);
            retry++;
        }

        if (retry < 30) {
            time_t now = 0;
            struct tm timeinfo = {0};
            time(&now);
            localtime_r(&now, &timeinfo);
            ESP_LOGI(TAG, "Synced: %04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            xEventGroupSetBits(g_event_group, NTP_SYNCED_BIT);
            break;
        }

        ESP_LOGE(TAG, "NTP sync failed, retrying in 60s");
        vTaskDelay(pdMS_TO_TICKS(60000));
    }

    esp_netif_sntp_deinit();
    vTaskDelete(NULL);
}
