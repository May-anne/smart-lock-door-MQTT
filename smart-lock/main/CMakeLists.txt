idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        mqtt
        driver
        esp_driver_gpio
        esp_driver_ledc
        log
        rc522
)