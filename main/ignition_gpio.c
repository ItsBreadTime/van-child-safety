#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ignition.h"
#include "common.h"

static const char *TAG = "IGN_GPIO";

#define IGNITION_GPIO        CONFIG_IGNITION_GPIO
#define IGNITION_ACTIVE      CONFIG_IGNITION_ACTIVE_LEVEL
#define IGNITION_DEBOUNCE_MS CONFIG_IGNITION_DEBOUNCE_MS

/* One-shot debounce state. Synchronously advanced by ignition_gpio_poll()
 * each time bus_state_task calls ignition_get(). This preserves the exact
 * timing behavior of the original inline GPIO loop (250 ms poll + 500 ms
 * debounce), with no extra FreeRTOS task. */
static bool s_raw = false;
static bool s_stable = false;
static int64_t s_changed_at = 0;
static bool s_initialized = false;

static bool read_ignition_gpio(void)
{
    int level = gpio_get_level((gpio_num_t)IGNITION_GPIO);
    return IGNITION_ACTIVE ? (level != 0) : (level == 0);
}

void ignition_init(void)
{
    /* GPIO source does not need WiFi; return immediately. */
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << IGNITION_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = IGNITION_ACTIVE ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = IGNITION_ACTIVE ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    s_raw = read_ignition_gpio();
    s_stable = s_raw;
    s_changed_at = esp_timer_get_time();
    s_initialized = true;
    ESP_LOGI(TAG, "Init: GPIO=%d active_level=%d stable=%d",
             IGNITION_GPIO, IGNITION_ACTIVE, s_stable);
}

static void ignition_gpio_poll(void)
{
    if (!s_initialized) return;
    s_raw = read_ignition_gpio();
    int64_t now = esp_timer_get_time();
    if (s_raw != s_stable) {
        if (now - s_changed_at >= IGNITION_DEBOUNCE_MS * 1000LL) {
            s_stable = s_raw;
        }
    } else {
        s_changed_at = now;
    }
}

bool ignition_get(void)
{
    ignition_gpio_poll();
    return s_stable;
}

ignition_source_status_t ignition_source_status(void)
{
    return s_initialized ? IGNITION_SOURCE_OK : IGNITION_SOURCE_ERROR;
}

const char *ignition_source_name(void)
{
    return "gpio";
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
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return false;  /* not an ESPNOW source */
}
