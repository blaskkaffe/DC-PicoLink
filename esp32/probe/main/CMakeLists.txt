idf_component_register(SRCS "probe.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_spi esp_wifi nvs_flash esp_timer hal soc)
