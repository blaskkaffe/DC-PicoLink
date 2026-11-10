# VPN profiles (E-7): esp32/vpn/*.conf and server.list are built in when present (git-ignored files)
file(GLOB VPN_CONFS "${CMAKE_CURRENT_LIST_DIR}/../vpn/*.conf")
list(SORT VPN_CONFS)
set(VPN_LIST "${CMAKE_CURRENT_LIST_DIR}/../vpn/server.list")
set(VPN_OUT "${CMAKE_CURRENT_BINARY_DIR}/vpn_profiles.txt")
set(VPN_TXT "@@SERVERS@@\n")
if(EXISTS "${VPN_LIST}")
    file(READ "${VPN_LIST}" _l)
    string(APPEND VPN_TXT "${_l}\n")
endif()
foreach(f ${VPN_CONFS})
    file(READ "${f}" _c)
    string(APPEND VPN_TXT "@@PROFILE@@\n${_c}\n")
endforeach()
file(WRITE "${VPN_OUT}" "${VPN_TXT}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${VPN_CONFS} ${VPN_LIST})
list(LENGTH VPN_CONFS _n)
message(STATUS "VPN profiles built in: ${_n}")

idf_component_register(SRCS "main.c" "link.c" "wifi.c" "web.c" "ppp_srv.c" "dnsd.c" "dcnow.c" "dc_eth.c" "vpn.c" "ovpn.c"
                       INCLUDE_DIRS "." "../../firmware"
                       EMBED_TXTFILES "${VPN_OUT}"
                       REQUIRES esp_driver_uart esp_driver_gpio esp_timer esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_http_client esp-tls mbedtls lwip)
