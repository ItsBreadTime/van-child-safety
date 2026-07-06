#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "common.h"
#include "sensor_gps.h"

static const char *TAG = "GPS";

static int s_rmc_count = 0;
static int s_gga_count = 0;
static int s_byte_count = 0;
static int s_dollar_count = 0;
static int64_t s_last_log = 0;
static int64_t s_last_valid_fix_us = 0;
static bool s_gps_valid_reported = false;

#define GPS_FIX_STALE_S 90
static uint8_t s_last_raw[40];
static int s_last_raw_len = 0;

static float nmea_to_decimal(const char *nmea, char dir)
{
    if (!nmea || nmea[0] == '\0') return 0.0f;
    float val = atof(nmea);
    int degrees = (int)(val / 100);
    float minutes = val - degrees * 100.0f;
    float decimal = degrees + minutes / 60.0f;
    if (dir == 'S' || dir == 'W') decimal = -decimal;
    return decimal;
}

static bool nmea_checksum_valid(const char *line)
{
    const char *start = strchr(line, '$');
    if (!start) return false;
    start++;

    const char *end = strchr(start, '*');
    if (!end || end - start < 1) return false;

    uint8_t checksum = 0;
    for (const char *p = start; p < end; p++) {
        checksum ^= (uint8_t)*p;
    }

    int received = 0;
    if (end[1] && end[2]) {
        char hex[3] = {end[1], end[2], '\0'};
        received = (int)strtol(hex, NULL, 16);
    } else {
        return false;
    }

    return checksum == (uint8_t)received;
}

static void mark_gps_invalid(void)
{
    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_telemetry.gps_valid = false;
        xSemaphoreGive(g_telemetry_mutex);
    }
    s_gps_valid_reported = false;
}

static void parse_gprmc(char *line)
{
    char *fields[12];
    int idx = 0;
    char *tok = strtok(line, ",");
    while (tok && idx < 12) {
        fields[idx++] = tok;
        tok = strtok(NULL, ",");
    }
    if (idx < 10) return;

    if (fields[2][0] != 'A') {
        mark_gps_invalid();
        return;
    }

    float lat = nmea_to_decimal(fields[3], fields[4][0]);
    float lon = nmea_to_decimal(fields[5], fields[6][0]);
    float speed = fields[7][0] ? atof(fields[7]) * 1.852f : 0.0f;
    float course = fields[8][0] ? atof(fields[8]) : 0.0f;

    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_telemetry.latitude = lat;
        g_telemetry.longitude = lon;
        g_telemetry.speed = speed;
        g_telemetry.course = course;
        g_telemetry.gps_valid = true;
        xSemaphoreGive(g_telemetry_mutex);
    }
    s_last_valid_fix_us = esp_timer_get_time();
    s_gps_valid_reported = true;
}

static void parse_gpgga(char *line)
{
    char *fields[15];
    int idx = 0;
    char *tok = strtok(line, ",");
    while (tok && idx < 15) {
        fields[idx++] = tok;
        tok = strtok(NULL, ",");
    }
    if (idx < 10) return;

    int fix = fields[6][0] ? atoi(fields[6]) : 0;
    if (fix == 0) {
        mark_gps_invalid();
        return;
    }

    int sats = fields[7][0] ? atoi(fields[7]) : 0;
    float alt = fields[9][0] ? atof(fields[9]) : 0.0f;

    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_telemetry.satellites = sats;
        g_telemetry.altitude = alt;
        xSemaphoreGive(g_telemetry_mutex);
    }
}

void gps_task(void *pvParameters)
{
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, -1, 9, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, 1024, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "Init (RX=9, 9600)");

    uint8_t buf[128];
    char line[128];
    int line_pos = 0;
    s_last_log = esp_timer_get_time();

    while (1) {
        int len = uart_read_bytes(UART_NUM_2, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0) {
            s_byte_count += len;
            if (s_last_raw_len < 40) {
                int copy = 40 - s_last_raw_len;
                if (copy > len) copy = len;
                memcpy(s_last_raw + s_last_raw_len, buf, copy);
                s_last_raw_len += copy;
            }
        }
        for (int i = 0; i < len; i++) {
            uint8_t b = buf[i];
            if (b == '$') {
                line_pos = 0;
                s_dollar_count++;
            }
            if (line_pos < 127) {
                line[line_pos++] = (char)b;
            }
            if (b == '\n' || line_pos >= 127) {
                line[line_pos] = '\0';
                if (line_pos > 6 && nmea_checksum_valid(line)) {
                    char tmp[128];
                    strncpy(tmp, line, sizeof(tmp));
                    tmp[sizeof(tmp) - 1] = '\0';
                    if (strstr(tmp, "RMC") == tmp + 3) {
                        parse_gprmc(tmp);
                        s_rmc_count++;
                    } else if (strstr(tmp, "GGA") == tmp + 3) {
                        parse_gpgga(tmp);
                        s_gga_count++;
                    }
                }
                line_pos = 0;
            }
        }
        int64_t now = esp_timer_get_time();
        if (s_gps_valid_reported && s_last_valid_fix_us > 0 &&
            now - s_last_valid_fix_us > (int64_t)GPS_FIX_STALE_S * 1000000LL) {
            ESP_LOGW(TAG, "GPS fix stale >%ds; marking invalid", GPS_FIX_STALE_S);
            mark_gps_invalid();
        }
        if (now - s_last_log > 15000000) {
            char raw_hex[128] = "";
            for (int i = 0; i < s_last_raw_len && i < 16; i++) {
                char tmp[4];
                snprintf(tmp, sizeof(tmp), "%02X ", s_last_raw[i]);
                strcat(raw_hex, tmp);
            }
            ESP_LOGI(TAG, "stats: %d RMC, %d GGA, %d '$' chars, %d bytes in 15s. raw[0..15]=%s",
                     s_rmc_count, s_gga_count, s_dollar_count, s_byte_count, raw_hex);
            if (s_byte_count == 0) {
                ESP_LOGW(TAG, "No UART data — check wiring RX=9 GPS-TX, 9600 baud");
            } else if (s_dollar_count == 0) {
                ESP_LOGW(TAG, "Bytes received but no '$' found — likely wrong baud rate (try 4800 or 38400)");
            }
            s_rmc_count = 0;
            s_gga_count = 0;
            s_byte_count = 0;
            s_dollar_count = 0;
            s_last_raw_len = 0;
            s_last_log = now;
        }
    }
}
