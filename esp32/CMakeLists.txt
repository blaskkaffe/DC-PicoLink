# DC-PicoLink ESP32 side (ESP32-S3-Zero): idf.py set-target esp32s3 && idf.py build
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# lwIP NAT port maps: DreamPi's game port list is 215 entries (portfwd.c); the default is 32
idf_build_set_property(COMPILE_DEFINITIONS "IP_PORTMAP_MAX=232" APPEND)
project(dc_picolink_esp32)
