idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "../../main"
    REQUIRES esp_wifi esp_netif nvs_flash esp_event esp_timer
)
