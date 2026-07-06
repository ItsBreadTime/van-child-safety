/* Ignition Remote
 *
 * Dumb heartbeat broadcaster for the VanChildSafety ignition-detection system.
 * This ESP32 is wired to the vehicle's ignition/accessory power rail: when
 * ignition is on, it boots and broadcasts an ESP-NOW heartbeat every
 * CONFIG_ESPNOW_HEARTBEAT_INTERVAL_MS. When ignition turns off, it loses
 * power and the heartbeats stop; the main firmware declares ignition off
 * after CONFIG_ESPNOW_HEARTBEAT_TIMEOUT_MS of silence.
 *
 * The remote never associates WiFi, has no MQTT, no OTA. It only brings up
 * the WiFi radio (unassociated, STA mode) so ESP-NOW operates, then broadcasts.
 *
 * The shared protocol header (../main/ignition_espnow_protocol.h) defines
 * the wire format; both sides must use the same CONFIG_ESPNOW_SECRET. */

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "ignition_espnow_protocol.h"

static const char *TAG = "IGN_REMOTE";

#define HEARTBEAT_INTERVAL_US (CONFIG_ESPNOW_HEARTBEAT_INTERVAL_MS * 1000LL)
#define ESPNOW_MIN_CHANNEL 1
#define ESPNOW_MAX_CHANNEL 13
#define CHANNEL_SWEEP_DELAY_MS 20

static uint32_t s_magic = 0;
static uint16_t s_seq = 0;
static EventGroupHandle_t s_events;
#define WIFI_READY_BIT BIT0

static uint32_t fnv1a32(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    while (s && *s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193u;
    }
    return h;
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* Don't connect to an AP; just start the radio so ESP-NOW works. */
        xEventGroupSetBits(s_events, WIFI_READY_BIT);
        ESP_LOGI(TAG, "WiFi started (unassociated) - ESP-NOW ready");
    }
}

static uint16_t read_battery_mv(void)
{
#if CONFIG_ESPNOW_BATTERY_ADC_CHANNEL >= 0
    /* TODO: implement with esp_adc/oneshot driver reading ADC1 channel
     * CONFIG_ESPNOW_BATTERY_ADC_CHANNEL and scaling by your divider ratio.
     * Returning 0 here keeps the telemetry contract valid (battery=0 means
     * "not measured this frame") without blocking the heartbeat path. */
    return 0;
#else
    /* Battery monitoring disabled; remote reports 0 in every heartbeat. The
     * main surfaces this as `remote_battery_mv: 0` in bus/state — HA shows 0,
     * which correctly means "no ADC configured" rather than a real 0 mV. */
    return 0;
#endif
}

static void send_heartbeat_on_channel(uint8_t channel,
                                      const uint8_t *peer_addr,
                                      const ignition_heartbeat_t *hb)
{
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_err_t ch = esp_wifi_set_channel(channel, sec);
    if (ch != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_channel(%u) failed: %s",
                 channel, esp_err_to_name(ch));
        return;
    }

    esp_err_t r = esp_now_send(peer_addr, (const uint8_t *)hb, sizeof(*hb));
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send(ch=%u) failed: %s",
                 channel, esp_err_to_name(r));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Ignition remote starting (interval=%dms)",
             CONFIG_ESPNOW_HEARTBEAT_INTERVAL_MS);

    /* NVS init (ESP-NOW / WiFi may use it). */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Minimal WiFi init: STA mode, started but never connected. ESP-NOW
     * rides on the WiFi radio without association. */
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* WIFI_EVENT_STA_START fires -> WIFI_READY_BIT set by event handler. */

    /* Wait for the radio to be up, then init ESP-NOW. */
    xEventGroupWaitBits(s_events, WIFI_READY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    s_magic = fnv1a32(CONFIG_ESPNOW_SECRET);

    esp_err_t en = esp_now_init();
    if (en != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(en));
        /* Nothing useful to do without ESP-NOW; reboot after a delay. */
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* Add the broadcast peer so our frames reach the main receiver. */
    esp_now_peer_info_t peer = {0};
    /* Channel 0 means "current WiFi channel"; send_heartbeat_on_channel()
     * moves the unassociated radio before each broadcast. */
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    memset(peer.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
    en = esp_now_add_peer(&peer);
    if (en != ESP_OK && en != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "esp_now_add_peer(broadcast) failed: %s", esp_err_to_name(en));
    }

    if (CONFIG_ESPNOW_CHANNEL == 0) {
        ESP_LOGI(TAG, "Broadcasting heartbeats on channels %d-%d (magic=0x%08lx)",
                 ESPNOW_MIN_CHANNEL, ESPNOW_MAX_CHANNEL, (unsigned long)s_magic);
    } else {
        ESP_LOGI(TAG, "Broadcasting heartbeats on fixed channel %d (magic=0x%08lx)",
                 CONFIG_ESPNOW_CHANNEL, (unsigned long)s_magic);
    }

    /* Heartbeat loop. While powered (ignition on), broadcast every interval.
     * Losing power (ignition off) stops the loop; the main firmware's timeout
     * handles the silent period. */
    while (1) {
        ignition_heartbeat_t hb = {
            .magic = s_magic,
            .seq = s_seq,
            .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
            .battery_mv = read_battery_mv(),
        };
        s_seq++;

        if (CONFIG_ESPNOW_CHANNEL == 0) {
            for (uint8_t channel = ESPNOW_MIN_CHANNEL;
                 channel <= ESPNOW_MAX_CHANNEL;
                 channel++) {
                send_heartbeat_on_channel(channel, peer.peer_addr, &hb);
                vTaskDelay(pdMS_TO_TICKS(CHANNEL_SWEEP_DELAY_MS));
            }
        } else {
            send_heartbeat_on_channel(CONFIG_ESPNOW_CHANNEL, peer.peer_addr, &hb);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_ESPNOW_HEARTBEAT_INTERVAL_MS));
    }
}
