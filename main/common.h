#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define WIFI_CONNECTED_BIT  BIT0
#define NTP_SYNCED_BIT      BIT1
#define WG_CONNECTED_BIT    BIT2
#define MQTT_CONNECTED_BIT  BIT3
#define WIFI_STARTED_BIT    BIT4

typedef enum {
    SENSOR_STATUS_OK = 0,
    SENSOR_STATUS_STALE,
    SENSOR_STATUS_ERROR,
    SENSOR_STATUS_NO_UART,
} sensor_status_t;

typedef struct {
    float temperature;
    float humidity;
    uint16_t co2;
    uint16_t co2_baseline;
    int16_t co2_delta_since_armed;
    int64_t co2_last_update_us;
    uint8_t co2_consecutive_rise_count;
    bool ld2412_target;
    bool ld2412_moving;
    bool ld2412_still;
    uint16_t moving_distance;
    uint16_t still_distance;
    uint8_t moving_energy;
    uint8_t still_energy;
    uint16_t detection_distance;
    uint8_t ld2412_light;
    int64_t ld2412_last_frame_us;
    int64_t ld2412_target_since_us;
    float latitude;
    float longitude;
    float course;
    float altitude;
    float speed;
    uint8_t satellites;
    bool gps_valid;
    int16_t co2_delta_3min;
    int8_t wifi_rssi;
    bool ignition_on;
    bool bus_armed;
    char bus_state[20];
    int64_t last_update;
    /* Sensor health: 0=ok, 1=stale, 2=error, 3=no_uart. The LD2412 task sets
     * NO_UART when zero bytes have arrived on its UART for a sustained window;
     * SCD4x sets ERROR when read_measurement returns ESP_ERR_INVALID_CRC or
     * an I2C failure. mqtt_publish_bus_state resolves the worst of (stale,
     * error, no_uart); bus_state_task emits a persistent sensor_fault advisory
     * while keeping the remaining detection paths active. */
    sensor_status_t co2_status;
    sensor_status_t ld2412_status;
    sensor_status_t ble_status;
    int64_t ble_last_scan_us;
    uint32_t ble_scan_drop_count;
} telemetry_data_t;

typedef enum {
    DISTRESS_BEACON_NEAR,
    DISTRESS_CO2_RISE,
    DISTRESS_PRESENCE_DETECTED,
} distress_type_t;

typedef struct {
    distress_type_t type;
    char beacon_name[32];
    uint16_t co2_delta;
    int64_t timestamp;
} distress_event_t;

extern EventGroupHandle_t g_event_group;
extern SemaphoreHandle_t g_telemetry_mutex;
extern QueueHandle_t g_distress_queue;
extern QueueHandle_t g_co2_queue;
extern telemetry_data_t g_telemetry;
extern volatile bool g_bus_empty;
