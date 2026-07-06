#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "common.h"
#include "secrets.h"

static const char *TAG = "OTA";

#define OTA_PORT 3232

#ifndef CONFIG_OTA_TOKEN
#define CONFIG_OTA_TOKEN ""
#endif

static bool ota_auth_check(httpd_req_t *req)
{
    if (CONFIG_OTA_TOKEN[0] == '\0') return false;

    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        if (buf && httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char token[64] = {0};
            if (httpd_query_key_value(buf, "token", token, sizeof(token)) == ESP_OK) {
                free(buf);
                return strcmp(token, CONFIG_OTA_TOKEN) == 0;
            }
        }
        free(buf);
    }

    /* check header */
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "X-OTA-Token");
    if (hdr_len > 0 && hdr_len < 64) {
        char *hdr = malloc(hdr_len + 1);
        if (hdr && httpd_req_get_hdr_value_str(req, "X-OTA-Token", hdr, hdr_len + 1) == ESP_OK) {
            bool ok = strcmp(hdr, CONFIG_OTA_TOKEN) == 0;
            free(hdr);
            return ok;
        }
        free(hdr);
    }

    return false;
}

static void ota_reboot_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

static esp_err_t ota_update_handler(httpd_req_t *req)
{
    if (!ota_auth_check(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid token");
        return ESP_FAIL;
    }

    int content_len = req->content_len;
    if (content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing content length");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    int remaining = content_len;
    int total_written = 0;
    int64_t start = esp_timer_get_time();

    while (remaining > 0) {
        char buf[1024];
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Receive error: %d", received);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }
        total_written += received;
        remaining -= received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Boot partition failed");
        return ESP_FAIL;
    }

    int64_t elapsed = esp_timer_get_time() - start;
    ESP_LOGI(TAG, "OTA complete: %d bytes in %lld ms, rebooting...",
             total_written, (long long)(elapsed / 1000));

    httpd_resp_sendstr(req, "OK - rebooting");

    const esp_timer_create_args_t timer_args = {
        .callback = ota_reboot_timer_cb,
        .name = "ota_reboot",
    };
    esp_timer_handle_t reboot_timer;
    esp_timer_create(&timer_args, &reboot_timer);
    esp_timer_start_once(reboot_timer, 2000000);

    return ESP_OK;
}

void ota_server_task(void *pvParameters)
{
    xEventGroupWaitBits(g_event_group, WG_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    if (CONFIG_OTA_TOKEN[0] == '\0') {
        ESP_LOGE(TAG, "OTA token is empty - refusing to start OTA server. "
                 "Set CONFIG_OTA_TOKEN in menuconfig or secrets.h.");
        vTaskDelete(NULL);
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = OTA_PORT;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    /* httpd runs URI handlers (incl. the OTA write + esp_ota_end image
     * verification, which drives esp_sha_dma) on its own thread. The default
     * 4 KB stack overflows there and panics ("stack overflow in task httpd").
     * 12 KB is enough for the SHA + mbedtls OTA verify path. */
    config.stack_size = 12288;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", OTA_PORT);
        vTaskDelete(NULL);
        return;
    }

    httpd_uri_t update_uri = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = ota_update_handler,
    };
    httpd_register_uri_handler(server, &update_uri);

    ESP_LOGI(TAG,
             "OTA server ready on http://0.0.0.0:%d/update (WireGuard address %s)",
             OTA_PORT, WG_ADDRESS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
