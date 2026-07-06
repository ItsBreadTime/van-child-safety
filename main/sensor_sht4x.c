#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "common.h"
#include "sensor_sht4x.h"
#include "sensor_scd4x.h"
#include "util.h"

static const char *TAG = "SHT4X";
#define SHT4X_ADDR 0x44

static esp_err_t sht4x_read(float *temperature, float *humidity)
{
    uint8_t cmd = 0xFD;
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (SHT4X_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(handle, cmd, true);
    i2c_master_stop(handle);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, handle, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(handle);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t data[6];
    handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (SHT4X_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(handle, data, 6, I2C_MASTER_LAST_NACK);
    i2c_master_stop(handle);
    ret = i2c_master_cmd_begin(I2C_NUM_0, handle, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(handle);
    if (ret != ESP_OK) return ret;

    if (crc8_sensirion(data, 2) != data[2] || crc8_sensirion(data + 3, 2) != data[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t temp_raw = (data[0] << 8) | data[1];
    uint16_t hum_raw = (data[3] << 8) | data[4];

    *temperature = -45.0f + 175.0f * temp_raw / 65535.0f;
    *humidity = -6.0f + 125.0f * hum_raw / 65535.0f;
    if (*humidity > 100.0f) *humidity = 100.0f;
    if (*humidity < 0.0f) *humidity = 0.0f;

    return ESP_OK;
}

void sensor_sht4x_init(void)
{
    ESP_LOGI(TAG, "Init");
}

void sensor_sht4x_read_and_update(void)
{
    float temp, hum;
    if (sht4x_read(&temp, &hum) == ESP_OK) {
        if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_telemetry.temperature = temp;
            g_telemetry.humidity = hum;
            xSemaphoreGive(g_telemetry_mutex);
        }
        ESP_LOGD(TAG, "T=%.1f H=%.1f", temp, hum);
    }
}

void sensor_i2c_task(void *pvParameters)
{
    sensor_sht4x_init();
    sensor_scd4x_init();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        sensor_sht4x_read_and_update();
        sensor_scd4x_read_and_update();
    }
}
