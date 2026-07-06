# Build the ignition source implementation selected by Kconfig.
set(ignition_srcs "")
if(CONFIG_IGNITION_SOURCE_ESPNOW)
    set(ignition_srcs "ignition_espnow.c")
elseif(CONFIG_IGNITION_SOURCE_GPIO)
    set(ignition_srcs "ignition_gpio.c")
else()
    # Kconfig default is ESPNOW, but if unset fall back to GPIO for safety.
    set(ignition_srcs "ignition_gpio.c")
endif()

idf_component_register(
    SRCS "main.c" "wifi.c" "ntp_sync.c" "wireguard.c" "mqtt_client.c"
         "bus_state.c"
         "i2c_bus.c" "sensor_sht4x.c" "sensor_scd4x.c"
         "sensor_ld2412.c" "sensor_gps.c"
         "co2_tracker.c" "ble_scanner.c" "beacon_config.c"
         "notifications.c" "ota_server.c"
         ${ignition_srcs}
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash esp_event esp_timer
             mqtt bt driver esp_http_client esp_wireguard
             json esp-tls esp_http_server app_update
)
