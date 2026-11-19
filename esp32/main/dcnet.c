/*
 * DCNet: the call goes to one of flyinghead's DCNet access points instead of our own PPP
 * server, as DreamPi's netlink.py + dcnet.rpi do it (../../firmware/dcnet_model.h):
 * find the closest access point, then pass the modem bytes unchanged over TCP. The access
 * point runs PPP itself (Dreamcast ISP settings: any user name, password "password").
 *
 * Used by the USB modem (usbmodem.c) and by the serial cable's modem emulation (ppp_srv.c).
 * One task does the connecting and the TCP -> Dreamcast direction; bytes from the
 * Dreamcast are sent from the caller's task (dcnet_from_dc).
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "dcnet_model.h"
#include "app.h"

static SemaphoreHandle_t lk;
static TaskHandle_t task_h;
static int mode = DCNET_PREFIX;
static volatile int sock = -1;
static volatile bool want, hang;              /* a call is asked for / the Dreamcast side ended it */
static volatile bool active;
static char number[32];
static dcnet_out_t out_fn;
static dcnet_done_t done_fn;
static uint8_t early[1024]; static size_t early_n;    /* from the Dreamcast before the connection is up */
static bool tilde;
static char ap_name[32] = "";
static struct { uint32_t calls, connected, failed, to_ap, to_dc; } cnt;

#define say say_line

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

int dcnet_mode(void) { return mode; }
void dcnet_set_mode(int m) {
    if (m < DCNET_OFF || m > DCNET_ALWAYS) return;
    mode = m;
    nvs_handle_t h;
    if (nvs_open("dcl", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "dcnet", (uint8_t)m); nvs_commit(h); nvs_close(h); }
}
bool dcnet_active(void) { return active; }

static bool resolve(const char *host, uint32_t *ip) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    *ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

/* the closest access point (lowest of up to 3 pings each within 1 s); 0 when none answered */
static uint32_t best_ap(void) {
    uint32_t disco;
    if (!resolve(DCNET_HOST, &disco)) return 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(DCNET_DISCO), .sin_addr.s_addr = disco };
    uint8_t b[300];
    sendto(s, b, dcnet_disco_query(b), 0, (struct sockaddr *)&to, sizeof to);
    uint32_t ip[DCNET_MAX_AP]; char name[DCNET_MAX_AP][32]; int n = 0;
    int sum[DCNET_MAX_AP] = { 0 }, got[DCNET_MAX_AP] = { 0 };
    uint64_t t0 = now_ms();
    while (now_ms() - t0 < 1000) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int r = recvfrom(s, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) continue;
        if (!dcnet_magic_ok(b, (size_t)r)) continue;
        if (b[4] == 3 && !n) {
            n = dcnet_disco_answer(b, (size_t)r, ip, name, DCNET_MAX_AP);
            if (n == 1) break;
            for (int i = 0; i < n; i++) {
                struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(DCNET_DISCO), .sin_addr.s_addr = ip[i] };
                sendto(s, b, dcnet_ping(b, now_ms()), 0, (struct sockaddr *)&a, sizeof a);
            }
        } else if (b[4] == 2 && r == 13) {
            uint64_t sent; memcpy(&sent, b + 5, 8);
            bool done = false;
            for (int i = 0; i < n; i++) if (ip[i] == from.sin_addr.s_addr) {
                sum[i] += (int)(now_ms() - sent);
                if (++got[i] == 3) done = true;
                else { struct sockaddr_in a = from; sendto(s, b, dcnet_ping(b, now_ms()), 0, (struct sockaddr *)&a, sizeof a); }
            }
            if (done) break;
        }
    }
    close(s);
    int best = -1, bp = 1 << 30;
    for (int i = 0; i < n; i++) if (got[i] && sum[i] / got[i] < bp) { bp = sum[i] / got[i]; best = i; }
    if (best < 0 && n == 1) best = 0;
    if (best < 0) return 0;
    strlcpy(ap_name, name[best], sizeof ap_name);
    if (got[best]) say("DCNet: access point %s (%d ms)", ap_name, bp);
    else say("DCNet: access point %s", ap_name);
    return ip[best];
}

static int connect_to(uint32_t ip, uint16_t port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct timeval tv = { .tv_sec = 8 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip };
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return -1; }
    tv.tv_sec = 1;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

static void end_call(const char *why) {
    xSemaphoreTake(lk, portMAX_DELAY);
    if (sock >= 0) { close(sock); sock = -1; }
    bool was = active;
    active = false;
    dcnet_done_t d = done_fn;
    xSemaphoreGive(lk);
    if (was) { say("DCNet: %s", why); if (d) d(); }
}

static void task(void *arg) {
    (void)arg;
    static uint8_t in[512], fixed[1025];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!want) continue;
        want = false;
        uint16_t port = dcnet_port_for(number);
        uint32_t ip = best_ap();
        if (!ip && !resolve(DCNET_HOST, &ip)) { cnt.failed++; end_call("no access point found (no internet?)"); continue; }
        if (!ip) strlcpy(ap_name, DCNET_HOST, sizeof ap_name);
        int s = connect_to(ip, port);
        if (s < 0) { cnt.failed++; end_call("could not connect to the access point"); continue; }
        xSemaphoreTake(lk, portMAX_DELAY);
        if (!active || hang) { xSemaphoreGive(lk); close(s); end_call("the call ended before DCNet answered"); continue; }
        sock = s;
        if (early_n) { send(s, early, early_n, 0); cnt.to_ap += (uint32_t)early_n; early_n = 0; }
        xSemaphoreGive(lk);
        cnt.connected++;
        say("DCNet: connected to %s port %u", ap_name, (unsigned)port);
        tilde = false;
        for (;;) {
            if (hang) { end_call("the Dreamcast hung up"); break; }
            fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
            int r = select(s + 1, &rs, NULL, NULL, &tv);
            if (r < 0) { end_call("connection error"); break; }
            if (r == 0) continue;
            int k = recv(s, in, sizeof in, 0);
            if (k <= 0) { end_call("the access point ended the call"); break; }
            size_t m = dcnet_fix_to_dc(&tilde, in, (size_t)k, fixed);
            cnt.to_dc += (uint32_t)m;
            if (out_fn) out_fn(fixed, m);
        }
    }
}

bool dcnet_call(const char *num, dcnet_out_t out, dcnet_done_t done) {
    xSemaphoreTake(lk, portMAX_DELAY);
    if (active) { xSemaphoreGive(lk); return false; }
    strlcpy(number, num, sizeof number);
    out_fn = out; done_fn = done;
    early_n = 0; hang = false; active = true; want = true;
    cnt.calls++;
    xSemaphoreGive(lk);
    say("DCNet: calling (number %s)", num[0] ? num : "-");
    xTaskNotifyGive(task_h);
    return true;
}

void dcnet_from_dc(const uint8_t *p, size_t n) {
    xSemaphoreTake(lk, portMAX_DELAY);
    if (active) {
        if (sock >= 0) { int k = send(sock, p, n, 0); if (k > 0) cnt.to_ap += (uint32_t)k; }
        else { size_t c = n < sizeof early - early_n ? n : sizeof early - early_n; memcpy(early + early_n, p, c); early_n += c; }
    }
    xSemaphoreGive(lk);
}

void dcnet_hangup(void) { if (active) hang = true; }

size_t dcnet_info_json(char *o, size_t cap) {
    static const char *const m[] = { "off", "prefix", "always" };
    return (size_t)snprintf(o, cap, "{\"mode\":\"%s\",\"active\":%s,\"ap\":\"%s\",\"calls\":%lu,\"connected\":%lu,\"failed\":%lu,\"to_ap\":%lu,\"to_dc\":%lu}",
        m[mode], active ? "true" : "false", ap_name, (unsigned long)cnt.calls, (unsigned long)cnt.connected,
        (unsigned long)cnt.failed, (unsigned long)cnt.to_ap, (unsigned long)cnt.to_dc);
}

void dcnet_start(void) {
    lk = xSemaphoreCreateMutex();
    nvs_handle_t h; uint8_t v = DCNET_PREFIX;
    if (nvs_open("dcl", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "dcnet", &v); nvs_close(h); }
    if (v <= DCNET_ALWAYS) mode = v;
    xTaskCreate(task, "dcnet", 3584, NULL, 6, &task_h);
}
