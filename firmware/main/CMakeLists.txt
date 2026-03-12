idf_component_register(
    SRCS
        "main.c"
        "net/wifi_manager.c"
        "ha/ha_client.c"
        "config/config_manager.c"
        "ui/ui_manager.c"
        "ui/screen_manager.c"
        "server/http_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp32_p4_function_ev_board
        nvs_flash esp_wifi esp_netif esp_event
        mqtt
        mbedtls
        spiffs
        esp_http_server
        app_update
    PRIV_REQUIRES json
)

