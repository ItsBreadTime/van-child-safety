#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "common.h"
#include "wifi.h"
#include "secrets.h"

static const char *TAG = "WIFI";

static int s_current_network = 0;
static volatile bool s_initial_scan_done = false;

static const char *ssids[] = {WIFI_SSID1, WIFI_SSID2, WIFI_SSID3, WIFI_SSID4};
static const char *passwords[] = {WIFI_PASS1, WIFI_PASS2, WIFI_PASS3, WIFI_PASS4};
#define WIFI_NETWORK_COUNT ((int)(sizeof(ssids) / sizeof(ssids[0])))

static bool network_is_configured(int index)
{
    return index >= 0 && index < WIFI_NETWORK_COUNT && ssids[index][0] != '\0';
}

static int next_configured_network(int start)
{
    for (int offset = 0; offset < WIFI_NETWORK_COUNT; offset++) {
        int candidate = (start + offset) % WIFI_NETWORK_COUNT;
        if (network_is_configured(candidate)) {
            return candidate;
        }
    }
    return -1;
}

static int network_index_for_ssid(const uint8_t *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
        if (network_is_configured(i) &&
            strncmp((const char *)ssid, ssids[i], sizeof(((wifi_ap_record_t *)0)->ssid)) == 0) {
            return i;
        }
    }
    return -1;
}

static void try_connect_network(int index)
{
    if (!network_is_configured(index)) {
        int next = next_configured_network(index + 1);
        if (next < 0) {
            ESP_LOGE(TAG, "No WiFi SSIDs configured");
            return;
        }
        s_current_network = next;
        index = next;
    }
    if (!network_is_configured(index)) {
        return;
    }
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssids[index], sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, passwords[index], sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(g_event_group, WIFI_STARTED_BIT);
        if (s_initial_scan_done) {
            try_connect_network(s_current_network);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = event_data;
        xEventGroupClearBits(g_event_group, WIFI_CONNECTED_BIT);
        int next = next_configured_network(s_current_network + 1);
        if (next >= 0) {
            s_current_network = next;
        }
        ESP_LOGI(TAG, "Disconnected (reason=%d), reconnecting to: %s", disc->reason, ssids[s_current_network]);
        try_connect_network(s_current_network);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(g_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (!netif) {
        ESP_LOGE(TAG, "Failed to create netif");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));

    s_current_network = next_configured_network(0);
    if (s_current_network < 0) {
        ESP_LOGE(TAG, "No WiFi SSIDs configured");
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "Connecting to %s", ssids[s_current_network]);

    {
        wifi_scan_config_t scan_cfg = { .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
        for (int attempt = 0; attempt < 3; attempt++) {
            esp_err_t sret = esp_wifi_scan_start(&scan_cfg, true);
            ESP_LOGI(TAG, "Scan attempt %d -> %s", attempt + 1, esp_err_to_name(sret));
            if (sret == ESP_OK) {
                uint16_t n = 0;
                esp_wifi_scan_get_ap_num(&n);
                ESP_LOGI(TAG, "Scan complete: %u AP records", n);
                if (n > 32) n = 32;
                wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
                int best_network = -1;
                int best_rssi = -128;
                if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                    for (int i = 0; i < n; i++) {
                        ESP_LOGI(TAG, "  ch=%2d rssi=%3d auth=%d ssid=%s",
                                 recs[i].primary, recs[i].rssi, recs[i].authmode,
                                 (char *)recs[i].ssid);
                        int network = network_index_for_ssid(recs[i].ssid);
                        if (network >= 0 && recs[i].rssi > best_rssi) {
                            best_network = network;
                            best_rssi = recs[i].rssi;
                        }
                    }
                }
                free(recs);
                if (best_network >= 0) {
                    s_current_network = best_network;
                    ESP_LOGI(TAG, "Selected visible configured SSID: %s (rssi=%d)",
                             ssids[s_current_network], best_rssi);
                }
                if (n > 0) break;
            }
            if (attempt < 2) {
                ESP_LOGW(TAG, "Scan found 0 APs, retrying in 2s...");
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
    }
    s_initial_scan_done = true;
    try_connect_network(s_current_network);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_telemetry.wifi_rssi = ap_info.rssi;
                xSemaphoreGive(g_telemetry_mutex);
            }
        } else {
            ESP_LOGW(TAG, "Not connected, reconnecting...");
            try_connect_network(s_current_network);
        }
    }
}
