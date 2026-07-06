#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "common.h"
#include "sensor_scd4x.h"
#include "util.h"

static const char *TAG = "SCD4X";
#define SCD4X_ADDR 0x62

static esp_err_t scd4x_send_command(uint16_t command)
{
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (SCD4X_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(handle, (command >> 8) & 0xFF, true);
    i2c_master_write_byte(handle, command & 0xFF, true);
    i2c_master_stop(handle);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, handle, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(handle);
    return ret;
}

static esp_err_t scd4x_read_measurement(uint16_t *co2, float *temperature, float *humidity)
{
    esp_err_t ret = scd4x_send_command(0xEC05);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send read-cmd failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t data[9];
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (SCD4X_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(handle, data, 9, I2C_MASTER_LAST_NACK);
    i2c_master_stop(handle);
    ret = i2c_master_cmd_begin(I2C_NUM_0, handle, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (crc8_sensirion(data, 2) != data[2] ||
        crc8_sensirion(data + 3, 2) != data[5] ||
        crc8_sensirion(data + 6, 2) != data[8]) {
        ESP_LOGW(TAG, "CRC fail");
        return ESP_ERR_INVALID_CRC;
    }

    *co2 = (data[0] << 8) | data[1];
    uint16_t temp_raw = (data[3] << 8) | data[4];
    uint16_t hum_raw = (data[6] << 8) | data[7];

    *temperature = -45.0f + 175.0f * temp_raw / 65536.0f;
    *humidity = 100.0f * hum_raw / 65536.0f;

    return ESP_OK;
}

void sensor_scd4x_init(void)
{
    ESP_LOGI(TAG, "Stopping any previous measurement");
    scd4x_send_command(0x3F86);
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Starting low power periodic measurement (cmd=0x21AC)");
    esp_err_t ret = scd4x_send_command(0x21AC);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "start cmd failed: %s — check SCD4x wired to I2C (SDA=5,SCL=6) and powered", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}

void sensor_scd4x_read_and_update(void)
{
    uint16_t co2;
    float temp, hum;
    esp_err_t ret = scd4x_read_measurement(&co2, &temp, &hum);
    if (ret == ESP_OK) {
        if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_telemetry.co2 = co2;
            g_telemetry.co2_last_update_us = esp_timer_get_time();
            g_telemetry.co2_status = SENSOR_STATUS_OK;
            xSemaphoreGive(g_telemetry_mutex);
        }

        xQueueOverwrite(g_co2_queue, &co2);
        ESP_LOGI(TAG, "CO2=%d ppm", co2);
    } else {
        ESP_LOGW(TAG, "read_measurement failed: %s", esp_err_to_name(ret));
        if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            /* Distinguish CRC errors / I2C failures from "stale" (no reading
             * in 90s). mqtt_publish_bus_state will still mark STALE when
             * co2_last_update_us is old; the ERROR status surfaces the
             * distinction so an operator knows the sensor is unhealthy vs
             * just idle. */
            g_telemetry.co2_status = SENSOR_STATUS_ERROR;
            xSemaphoreGive(g_telemetry_mutex);
        }
    }
}
