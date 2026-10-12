idf_component_register(SRCS "main.c" "ble_link.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash esp_driver_uart esp_driver_gpio)
