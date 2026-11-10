idf_component_register(SRCS "main.c" "link.c" "wifi.c" "web.c" "ppp_srv.c" "dnsd.c" "dcnow.c" "dc_eth.c"
                       INCLUDE_DIRS "." "../../firmware"
                       REQUIRES esp_driver_uart esp_driver_gpio esp_timer esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_http_client esp-tls mbedtls lwip)
