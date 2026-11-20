/* host test: the ESP32 build's DHCP server settings (sdkconfig of the zero-stack build) */
#pragma once
#define CONFIG_LWIP_DHCPS 1
#define CONFIG_LWIP_DHCPS_LEASE_UNIT 60
#define CONFIG_LWIP_DHCPS_MAX_STATION_NUM 8
#define CONFIG_LWIP_DHCPS_STATIC_ENTRIES 1
#define CONFIG_LWIP_DHCPS_ADD_DNS 1
