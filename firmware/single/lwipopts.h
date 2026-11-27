/* lwIP for the single-chip build: sockets under FreeRTOS, PPP server, DNS/DHCP clients. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H
#define NO_SYS                      0
#define LWIP_SOCKET                 1
#define LWIP_NETCONN                1
#define LWIP_COMPAT_SOCKETS         1
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (40 * 1024)
#define MEMP_NUM_TCP_PCB            16
#define MEMP_NUM_UDP_PCB            12
#define MEMP_NUM_TCP_SEG            48
#define PBUF_POOL_SIZE              24
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_ICMP                   1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_SO_RCVTIMEO            1
#define LWIP_TIMEVAL_PRIVATE        0
#define LWIP_TCPIP_CORE_LOCKING     1
#define TCPIP_THREAD_STACKSIZE      2048
#define TCPIP_THREAD_PRIO           5
#define TCPIP_MBOX_SIZE             16
#define DEFAULT_TCP_RECVMBOX_SIZE   8
#define DEFAULT_UDP_RECVMBOX_SIZE   8
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define DEFAULT_ACCEPTMBOX_SIZE     8
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_STATS                  0
/* PPP: the Dreamcast's call (ppp_srv) */
#define PPP_SUPPORT                 1
#define PPPOS_SUPPORT               1
#define PPP_SERVER                  1
#define LWIP_PPP_API                1
#define PAP_SUPPORT                 1
#define CHAP_SUPPORT                0
#define MSCHAP_SUPPORT              0
#define CCP_SUPPORT                 0
#define VJ_SUPPORT                  0
#define PRINTPKT_SUPPORT            0
#endif
