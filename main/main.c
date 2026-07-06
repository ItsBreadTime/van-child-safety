#include <string.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "common.h"
#include "wifi.h"
#include "ntp_sync.h"
#include "wireguard.h"
#include "bus_mqtt.h"
#include "bus_state.h"
#include "i2c_bus.h"
#include "sensor_sht4x.h"
#include "sensor_scd4x.h"
#include "sensor_ld2412.h"
#include "sensor_gps.h"
#include "co2_tracker.h"
#include "ble_scanner.h"
#include "beacon_config.h"
#include "notifications.h"
#include "ota_server.h"

static const char *TAG = "MAIN";

static void status_watchdog_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        EventBits_t bits = xEventGroupGetBits(g_event_group);
        ESP_LOGI(TAG, "STATUS uptime=%lus wifi=%d ntp=%d wg=%d mqtt=%d heap=%lu",
                 (unsigned long)(esp_timer_get_time() / 1000000),
                 !!(bits & WIFI_CONNECTED_BIT),
                 !!(bits & NTP_SYNCED_BIT),
                 !!(bits & WG_CONNECTED_BIT),
                 !!(bits & MQTT_CONNECTED_BIT),
                 (unsigned long)esp_get_free_heap_size());
    }
}

EventGroupHandle_t g_event_group;
SemaphoreHandle_t g_telemetry_mutex;
QueueHandle_t g_distress_queue;
QueueHandle_t g_co2_queue;
telemetry_data_t g_telemetry;
volatile bool g_bus_empty;

void app_main(void)
{
    ESP_LOGI(TAG, "VanChildSafety starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    g_event_group = xEventGroupCreate();
    g_telemetry_mutex = xSemaphoreCreateMutex();
    g_distress_queue = xQueueCreate(20, sizeof(distress_event_t));
    g_co2_queue = xQueueCreate(1, sizeof(uint16_t));
    if (!g_event_group || !g_telemetry_mutex || !g_distress_queue || !g_co2_queue) {
        ESP_LOGE(TAG, "Failed to create FreeRTOS objects");
        esp_restart();
    }
    memset(&g_telemetry, 0, sizeof(g_telemetry));
    g_bus_empty = false;

    i2c_bus_init();
    beacon_config_init();

    xTaskCreate(wifi_task, "wifi_task", 4096, NULL, 5, NULL);
    xTaskCreate(ntp_sync_task, "ntp_sync_task", 4096, NULL, 4, NULL);
    xTaskCreate(wireguard_task, "wireguard_task", 10240, NULL, 4, NULL);
    xTaskCreate(mqtt_task, "mqtt_task", 6144, NULL, 4, NULL);
    xTaskCreate(bus_state_task, "bus_state_task", 4096, NULL, 4, NULL);
    xTaskCreate(sensor_i2c_task, "sensor_i2c_task", 4096, NULL, 3, NULL);
    xTaskCreate(ld2412_task, "ld2412_task", 3072, NULL, 3, NULL);
    xTaskCreate(gps_task, "gps_task", 3072, NULL, 3, NULL);
    xTaskCreate(co2_tracker_task, "co2_tracker_task", 4096, NULL, 3, NULL);
    xTaskCreate(ble_scan_task, "ble_scan_task", 4096, NULL, 4, NULL);
    xTaskCreate(notification_task, "notification_task", 8192, NULL, 3, NULL);
    xTaskCreate(status_watchdog_task, "status_watchdog", 3072, NULL, 1, NULL);
    xTaskCreate(ota_server_task, "ota_server_task", 12288, NULL, 3, NULL);

    ESP_LOGI(TAG, "All tasks created.");
    vTaskDelete(NULL);
}
