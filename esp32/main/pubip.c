/*
 * Our public Internet address, as DreamPi gets it for dcvoip and dcdaytona
 * (myip.opendns.com from resolver1.opendns.com; ../../firmware/pubip_model.h).
 * Asked when Wi-Fi is up, again every 30 minutes, and every minute while unknown.
 * The packet rules (ipfilter_model.h) read it through pubip_get().
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "pubip_model.h"
#include "app.h"

static volatile uint32_t pub;

uint32_t pubip_get(void) { return pub; }

static const char *why = "";
static char whybuf[200];

static uint32_t ask(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct timeval tv = { .tv_sec = 3 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(PUBIP_SERVER) };
    uint8_t q[64], a[512];
    uint32_t found = 0;
    why = "no reply to the DNS question (is UDP port 53 to the Internet blocked?)";
    for (int tries = 0; tries < 3 && !found; tries++) {
        uint16_t id = (uint16_t)esp_random();
        size_t n = pubip_query(q, id);
        if (sendto(s, q, n, 0, (struct sockaddr *)&to, sizeof to) < 0) break;
        int r = recv(s, a, sizeof a, 0);
        if (r > 0) { found = pubip_answer(a, (size_t)r, id); if (!found) { snprintf(whybuf, sizeof whybuf, "a reply came (%d bytes, error code %d, %d answers) but without an address: a router or DNS filter may be answering for OpenDNS", r, r > 3 ? a[3] & 15 : -1, r > 7 ? (a[6] << 8 | a[7]) : -1); why = whybuf; } }
    }
    close(s);
    bool udp_faked = !found && why == whybuf;                 /* something answered, without the address */
    if (!found) {                                            /* the same question over TCP */
        uint16_t id = (uint16_t)esp_random();
        size_t n = pubip_query(q, id);
        int r = dns_tcp_ask(PUBIP_SERVER, q, n, a, sizeof a, 4000);
        if (r > 0) found = pubip_answer(a, (size_t)r, id);
        if (found && !dnsd_tcp()) {
            dnsd_use_tcp(true);
            const char *m = udp_faked
                ? "DNS: your router answers DNS questions itself (UDP port 53 is intercepted), so game servers would not be found; asking the Dreamcast DNS servers over TCP instead"
                : "DNS: no answers over UDP port 53; asking the Dreamcast DNS servers over TCP instead";
            log_add("esp32", m, strlen(m));
        }
        if (!found && why != whybuf) { strlcpy(whybuf, why, sizeof whybuf); why = whybuf; }
        if (!found) { snprintf(whybuf + strlen(whybuf), sizeof whybuf - strlen(whybuf), "; over TCP: no answer either"); why = whybuf; }
    }
    return found;
}

static void task(void *arg) {
    (void)arg;
    for (;;) {
        while (!inet_up()) vTaskDelay(pdMS_TO_TICKS(2000));
        uint32_t a = ask();
        if (a && a != pub) {
            char m[64];
            int k = snprintf(m, sizeof m, "public address %u.%u.%u.%u (for the game packet rules)",
                             (unsigned)(a >> 24), (unsigned)(a >> 16 & 255), (unsigned)(a >> 8 & 255), (unsigned)(a & 255));
            log_add("esp32", m, (size_t)k);
        }
        if (a) pub = a;
        else if (!pub) {
            char m[256];
            int k = snprintf(m, sizeof m, "public address not found: %s; Planet Ring voice and Daytona need it", why);
            log_add("esp32", m, (size_t)k);
        }
        vTaskDelay(pdMS_TO_TICKS(a ? 30 * 60 * 1000 : 60 * 1000));
    }
}

void pubip_start(void) { xTaskCreate(task, "pubip", 3072, NULL, 3, NULL); }
