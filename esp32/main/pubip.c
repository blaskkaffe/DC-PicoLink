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

/* the router's own outside address from UPnP (portfwd.c), when it is a public one: used when
   the OpenDNS lookup is blocked (DNS filters on the router answer it empty) */
static volatile uint32_t hint;
void pubip_hint(uint32_t a) { hint = a; }

static const char *why = "";
static volatile bool udp_faked_seen;
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
    if (udp_faked) udp_faked_seen = true;
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


/* ---- does the Dreamcast DNS answer the same over UDP and TCP? ----
   Some routers (OpenWrt "redirect all DNS", filters, some ISPs) answer every port-53 question
   themselves. Game servers such as Quake III's master are then looked up in the public DNS, which
   has other (or no) addresses than the Dreamcast DNS 46.101.91.123. TCP usually gets through. */
static size_t name_query(uint8_t *q, uint16_t id, const char *name) {
    size_t n = 0;
    q[n++] = (uint8_t)(id >> 8); q[n++] = (uint8_t)id; q[n++] = 1; q[n++] = 0; q[n++] = 0; q[n++] = 1;
    for (int i = 0; i < 6; i++) q[n++] = 0;
    while (*name) {
        const char *d = strchr(name, '.'); size_t l = d ? (size_t)(d - name) : strlen(name);
        q[n++] = (uint8_t)l; memcpy(q + n, name, l); n += l; name += l; if (d) name++;
    }
    q[n++] = 0; q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;
    return n;
}
static uint32_t first_a(const uint8_t *a, size_t n, uint16_t id) {            /* the first A record's address, or 0 */
    if (n < 12 || a[0] != (id >> 8) || a[1] != (uint8_t)id) return 0;
    size_t p = 12;
    for (int qd = a[4] << 8 | a[5]; qd > 0; qd--) {
        while (p < n && a[p]) { if ((a[p] & 0xC0) == 0xC0) { p++; break; } p += (size_t)a[p] + 1; }
        p += 5;
    }
    for (int an = a[6] << 8 | a[7]; an > 0 && p + 12 <= n; an--) {
        if ((a[p] & 0xC0) == 0xC0) p += 2; else { while (p < n && a[p]) p += (size_t)a[p] + 1; p++; }
        if (p + 10 > n) return 0;
        unsigned t = (unsigned)(a[p] << 8 | a[p + 1]), rl = (unsigned)(a[p + 8] << 8 | a[p + 9]);
        if (t == 1 && rl == 4 && p + 14 <= n) return (uint32_t)a[p + 10] << 24 | a[p + 11] << 16 | a[p + 12] << 8 | a[p + 13];
        p += 10 + rl;
    }
    return 0;
}
static void check_dns_path(void) {
    static const char *NAME = "master.id-q3a.games.sega.net";
    uint8_t q[80], a[512];
    uint32_t udp = 0, tcp = 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct timeval tv = { .tv_sec = 2 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(0x2e655b7bu) };   /* 46.101.91.123 */
        for (int t = 0; t < 2 && !udp; t++) {
            uint16_t id = (uint16_t)esp_random();
            size_t n = name_query(q, id, NAME);
            sendto(s, q, n, 0, (struct sockaddr *)&to, sizeof to);
            int r = recv(s, a, sizeof a, 0);
            if (r > 0) udp = first_a(a, (size_t)r, id);
        }
        close(s);
    }
    uint16_t id = (uint16_t)esp_random();
    size_t n = name_query(q, id, NAME);
    int r = dns_tcp_ask(0x2e655b7bu, q, n, a, sizeof a, 3000);
    if (r > 0) tcp = first_a(a, (size_t)r, id);
    char m[240]; int k = 0;
    #define IPS(x) (unsigned)((x) >> 24), (unsigned)((x) >> 16 & 255), (unsigned)((x) >> 8 & 255), (unsigned)((x) & 255)
    if (udp && tcp && udp != tcp) {
        dnsd_use_tcp(true);
        k = snprintf(m, sizeof m, "DNS: %s is %u.%u.%u.%u over UDP but %u.%u.%u.%u over TCP: something on your network answers port-53 questions itself (router DNS redirect or filter); the Dreamcast DNS is now asked over TCP",
                     NAME, IPS(udp), IPS(tcp));
    } else if (!udp && tcp) {
        dnsd_use_tcp(true);
        k = snprintf(m, sizeof m, "DNS: no UDP answer from the Dreamcast DNS, TCP works (%u.%u.%u.%u); asking over TCP", IPS(tcp));
    } else if (udp && !tcp && udp_faked_seen) {
        k = snprintf(m, sizeof m, "DNS: your network answers DNS itself (UDP) and TCP port 53 to the Dreamcast DNS gets no answer: game servers such as Quake III's master may resolve wrongly (%u.%u.%u.%u). Turn off the router's DNS redirect or let 46.101.91.123 through",
                     IPS(udp));
    } else if (udp || tcp) {
        k = snprintf(m, sizeof m, "DNS: no router interception seen (%s = %u.%u.%u.%u)", NAME, IPS(udp ? udp : tcp));
    }
    #undef IPS
    if (k > 0) log_add("esp32", m, (size_t)(k < (int)sizeof m ? k : (int)sizeof m - 1));
}

static void task(void *arg) {
    (void)arg;
    for (;;) {
        while (!inet_up()) vTaskDelay(pdMS_TO_TICKS(2000));
        uint32_t a = ask();
        { static int checks; if (checks < 3 && !dnsd_tcp()) { checks++; check_dns_path(); } }   /* the first few rounds (Wi-Fi may come up late) */
        if (a && a != pub) {
            char m[64];
            int k = snprintf(m, sizeof m, "public address %u.%u.%u.%u (for the game packet rules)",
                             (unsigned)(a >> 24), (unsigned)(a >> 16 & 255), (unsigned)(a >> 8 & 255), (unsigned)(a & 255));
            log_add("esp32", m, (size_t)k);
        }
        if (a) pub = a;
        else if (!pub && hint) {
            pub = hint;
            char m[160];
            int k = snprintf(m, sizeof m, "public address %u.%u.%u.%u (from the router: the OpenDNS lookup is blocked on this network)",
                             (unsigned)(hint >> 24), (unsigned)(hint >> 16 & 255), (unsigned)(hint >> 8 & 255), (unsigned)(hint & 255));
            log_add("esp32", m, (size_t)k);
        } else if (!pub) {
            static char said[200];                       /* once per reason, not every minute */
            if (strncmp(said, why, sizeof said - 1)) {
                snprintf(said, sizeof said, "%s", why);
                char m[256];
                int k = snprintf(m, sizeof m, "public address not found: %s; Planet Ring voice and Daytona need it", why);
                log_add("esp32", m, (size_t)(k < (int)sizeof m ? k : (int)sizeof m - 1));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(a ? 30 * 60 * 1000 : 60 * 1000));
    }
}

void pubip_start(void) { xTaskCreate(task, "pubip", 4096, NULL, 3, NULL); }
