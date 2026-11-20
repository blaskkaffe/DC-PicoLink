/* lwIP options for the Dreamcast-network host test: ESP-IDF's lwIP copy, no OS, Ethernet + ARP,
   NAPT and ESP-IDF's DHCP server, as the ESP32 build has them (sdkconfig: NAPT on, DHCP server). */
#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0
#define LWIP_TIMERS             1
#define LWIP_NETCONN            0
#define LWIP_SOCKET             0
#define LWIP_TCP                1
#define LWIP_UDP                1
#define LWIP_RAW                1
#define LWIP_ICMP               1
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_DNS                0
#define LWIP_DHCP               0
#define LWIP_AUTOIP             0
#define LWIP_IGMP               0
#define LWIP_ARP                1
#define LWIP_ETHERNET           1
#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define IP_FORWARD              1
#define MEM_ALIGNMENT           8
#define MEM_SIZE                (256 * 1024)
#define PBUF_POOL_SIZE          64
#define PBUF_POOL_BUFSIZE       1600
#define MEMP_NUM_PBUF           32
#define MEMP_NUM_UDP_PCB        8
#define LWIP_NETIF_HOSTNAME     0
#define LWIP_DEBUG              0
#define LWIP_NUM_NETIF_CLIENT_DATA 1
#define ESP_LWIP                1
#define ESP_DHCPS               1
#define IP_NAPT                 1
#define IP_NAPT_MAX             512
#define IP_PORTMAP_MAX          32
#define IP_NAPT_PORTMAP         1
#define LWIP_HOOK_IP4_ROUTE_SRC(src, dest) sim_route_src((src), (dest))
struct netif;
struct ip4_addr;
struct netif *sim_route_src(const struct ip4_addr *src, const struct ip4_addr *dest);
/* names ESP-IDF's copy of lwIP expects */
#define ESP_LWIP_DHCP_FINE_TIMERS_ONDEMAND  0
#define ESP_LWIP_IGMP_TIMERS_ONDEMAND       0
#define ESP_LWIP_DNS_TIMERS_ONDEMAND        0
#define ESP_LWIP_MLD6_TIMERS_ONDEMAND       0
#define DHCP_DOES_ARP_CHECK                 0
#define LWIP_ACD                            0
#define ESP_IPV6                            0
#define ESP_PING                            0
#define ESP_GRATUITOUS_ARP                  0
#define ESP_DHCP_DISABLE_VENDOR_CLASS_IDENTIFIER 1
#define DHCP_COARSE_TIMER_MSECS 60000   /* lwip/dhcp.h has it only with the DHCP client on */
#define MEMP_NUM_SYS_TIMEOUT 32
/* the ESP32 build has the DHCP client on (Wi-Fi), which makes lwIP take source 0.0.0.0 (DHCP DISCOVER) */
#define LWIP_IP_ACCEPT_UDP_PORT(port) 0
#define MEM_LIBC_MALLOC 1   /* as on the ESP32: the DHCP server frees with free() */
