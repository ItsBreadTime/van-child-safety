#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "ignition.h"
#include "ignition_espnow_protocol.h"
#include "common.h"

static const char *TAG = "IGN_ESPNOW";

#define HEARTBEAT_TIMEOUT_US  (CONFIG_ESPNOW_HEARTBEAT_TIMEOUT_MS * 1000LL)
#define BOOT_GRACE_US         (CONFIG_ESPNOW_BOOT_GRACE_S * 1000000LL)

#ifndef CONFIG_ESPNOW_SECRET
/* Kconfig always provides CONFIG_ESPNOW_SECRET for ESPNOW builds (it has a
 * string default and `depends on IGNITION_SOURCE_ESPNOW`). This fallback is a
 * string so fnv1a32() receives a valid C string if the symbol is ever unset. */
#define CONFIG_ESPNOW_SECRET "change-me-ignition-secret"
#endif

/* Compile-time magic. CONFIG_ESPNOW_SECRET is a string in Kconfig; we hash it
 * at build time to a 32-bit value so operators can use a memorable secret
 * without worrying about byte layout on the wire. Both sides use the same
 * hash, so they agree. */
static uint32_t s_magic = 0;

/* Last valid heartbeat state. Read by ignition_get() under the lock. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_rx_us = 0;            /* 0 = never received */
static ignition_heartbeat_t s_last_heartbeat = {0};
static uint16_t s_missed_seq = 0;
static bool s_synchronized = false;         /* have we seen at least one hb */
static int64_t s_boot_at = 0;
static bool s_initialized = false;
static ignition_source_status_t s_status = IGNITION_SOURCE_BOOT;

/* FNV-1a 32-bit hash of a string. Used to derive the on-wire magic from the
 * Kconfig secret string. Both main and remote include this header so they
 * produce the same hash for the same CONFIG_ESPNOW_SECRET. */
static uint32_t fnv1a32(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    while (s && *s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193u;
    }
    return h;
}

/* ESP-NOW receive callback. Runs on the esp_now task; keep it short. */
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int data_len)
{
    if (!data || data_len < (int)sizeof(ignition_heartbeat_t)) {
        return;
    }
    ignition_heartbeat_t hb;
    memcpy(&hb, data, sizeof(hb));
    if (hb.magic != s_magic) {
        return;  /* not our remote */
    }

    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    /* Gap detection: count frames between last seq and this one. Handles wrap. */
    if (s_synchronized) {
        uint16_t delta = hb.seq - s_last_heartbeat.seq;
        if (delta > 1 && delta < 0x8000u) {
            s_missed_seq += (delta - 1);
        }
    }
    s_last_heartbeat = hb;
    s_last_rx_us = now;
    s_synchronized = true;
    s_status = IGNITION_SOURCE_OK;
    portEXIT_CRITICAL(&s_lock);
}

void ignition_init(void)
{
    s_boot_at = esp_timer_get_time();
    s_magic = fnv1a32(CONFIG_ESPNOW_SECRET);

    /* Wait for WiFi to be started so esp_now_init can attach. The WiFi task
     * sets WIFI_STARTED_BIT on WIFI_EVENT_STA_START. */
    ESP_LOGI(TAG, "Waiting for WIFI_STARTED_BIT...");
    xEventGroupWaitBits(g_event_group, WIFI_STARTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(ret));
        s_status = IGNITION_SOURCE_ERROR;
        s_initialized = false;
        return;
    }

    /* Register for all broadcasts; we filter on magic in the callback. */
    ret = esp_now_register_recv_cb(espnow_recv_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_recv_cb failed: %s", esp_err_to_name(ret));
        s_status = IGNITION_SOURCE_ERROR;
        return;
    }

    /* Add a broadcast peer so we receive broadcast frames. */
    esp_now_peer_info_t peer = {0};
    peer.channel = 0;  /* current WiFi channel */
    peer.ifidx = WIFI_IF_STA;
    memset(peer.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);  /* broadcast */
    ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "esp_now_add_peer(broadcast) failed: %s", esp_err_to_name(ret));
        /* not fatal: we still receive broadcasts from registered peers */
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Init ok (magic=0x%08lx timeout=%lldms boot_grace=%lds)",
             (unsigned long)s_magic,
             (long long)HEARTBEAT_TIMEOUT_US / 1000,
             (long)BOOT_GRACE_US / 1000000);
}

bool ignition_get(void)
{
    if (!s_initialized) {
        return false;  /* init failed: fail-closed = ignition off */
    }

    int64_t now = esp_timer_get_time();
    int64_t last_rx;

    portENTER_CRITICAL(&s_lock);
    last_rx = s_last_rx_us;
    portEXIT_CRITICAL(&s_lock);

    int64_t boot_elapsed = now - s_boot_at;

    /* Boot grace: assume ignition ON during the first BOOT_GRACE_US to give
     * the remote time to boot and connect. If the heartbeat never arrives
     * and grace expires, fall through to the timeout path (fail-closed). */
    if (boot_elapsed < BOOT_GRACE_US && last_rx == 0) {
        /* Still in boot grace, no heartbeat yet: assume on. Status stays BOOT
         * so telemetry reflects "we're guessing". */
        return true;
    }

    /* No heartbeat ever received after grace: fail-closed (ignition off). */
    if (last_rx == 0) {
        portENTER_CRITICAL(&s_lock);
        s_status = IGNITION_SOURCE_STALE;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }

    /* Have at least one heartbeat: check staleness. */
    int64_t age = now - last_rx;
    if (age >= HEARTBEAT_TIMEOUT_US) {
        /* Heartbeats went silent for longer than the timeout: ignition off. */
        portENTER_CRITICAL(&s_lock);
        s_status = IGNITION_SOURCE_STALE;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }

    /* Fresh heartbeats: ignition on. */
    portENTER_CRITICAL(&s_lock);
    s_status = IGNITION_SOURCE_OK;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

ignition_source_status_t ignition_source_status(void)
{
    ignition_source_status_t status;
    portENTER_CRITICAL(&s_lock);
    status = s_status;
    portEXIT_CRITICAL(&s_lock);
    return status;
}

const char *ignition_source_name(void)
{
    return "espnow";
}

const char *ignition_source_status_name(ignition_source_status_t status)
{
    switch (status) {
    case IGNITION_SOURCE_OK:         return "ok";
    case IGNITION_SOURCE_STALE:      return "stale";
    case IGNITION_SOURCE_ERROR:      return "error";
    case IGNITION_SOURCE_BOOT:       return "boot";
    case IGNITION_SOURCE_OVERRIDDEN: return "overridden";
    default:                          return "unknown";
    }
}

bool ignition_espnow_diag(ignition_espnow_diag_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_lock);
    out->remote_uptime_s = s_last_heartbeat.uptime_s;
    out->remote_battery_mv = s_last_heartbeat.battery_mv;
    out->seq = s_last_heartbeat.seq;
    out->missed_seq = s_missed_seq;
    int64_t last = s_last_rx_us;
    portEXIT_CRITICAL(&s_lock);
    if (last > 0) {
        out->last_heartbeat_age_us = (int64_t)esp_timer_get_time() - last;
    } else {
        out->last_heartbeat_age_us = -1;
    }
    return true;
}
