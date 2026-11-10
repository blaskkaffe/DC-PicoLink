/*
 * Host test: ESP-IDF's lwIP as the PPP server, fed through our frame filter, like ppp_srv.c does.
 * stdin = bytes from the "Dreamcast", stdout = bytes to it, stderr = what happened.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/ip4_addr.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp_impl.h"
#include "hdlc_model.h"

u32_t sys_now(void) { struct timeval tv; gettimeofday(&tv, NULL); return (u32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000); }
u32_t sys_jiffies(void) { return sys_now(); }

static ppp_pcb *pcb;
static struct netif pppif;
static hdlc_t flt;

static u32_t out_cb(ppp_pcb *p, const void *data, u32_t len, void *ctx) {
    (void)p; (void)ctx;
    size_t done = 0;
    while (done < len) { ssize_t k = write(1, (const char *)data + done, len - done); if (k <= 0) break; done += (size_t)k; }
    return len;
}
static void status_cb(ppp_pcb *p, int err, void *ctx) {
    (void)p; (void)ctx;
    fprintf(stderr, "STATUS %d\n", err);
    if (err == PPPERR_NONE) {
        fprintf(stderr, "UP our=%s", ip4addr_ntoa(netif_ip4_addr(&pppif)));
        fprintf(stderr, " his=%s\n", ip4addr_ntoa(netif_ip4_gw(&pppif)));
    }
}
static void emit(void *ctx, const uint8_t *p, size_t n) { (void)ctx; pppos_input(pcb, p, (int)n); }

int main(void) {
    lwip_init();
    hdlc_init(&flt, emit, NULL);
    pcb = pppos_create(&pppif, out_cb, status_cb, NULL);
    ip4_addr_t our, his;
    IP4_ADDR(&our, 10, 64, 0, 1);
    IP4_ADDR(&his, 10, 64, 0, 2);
    ppp_set_ipcp_ouraddr(pcb, &our);
    ppp_set_ipcp_hisaddr(pcb, &his);
    ppp_set_ipcp_dnsaddr(pcb, 0, &our);
    if (ppp_listen(pcb) != ERR_OK) { fprintf(stderr, "listen failed\n"); return 1; }
    fprintf(stderr, "READY\n");
    for (;;) {
        fd_set rs; FD_ZERO(&rs); FD_SET(0, &rs);
        struct timeval tv = { 0, 10000 };
        int r = select(1, &rs, NULL, NULL, &tv);
        if (r > 0) {
            uint8_t b[2048];
            ssize_t n = read(0, b, sizeof b);
            if (n <= 0) break;
            hdlc_feed(&flt, b, (size_t)n, sys_now());
            if (flt.saw_at) { fprintf(stderr, "SAW_AT\n"); flt.saw_at = false; }
        }
        sys_check_timeouts();
    }
    fprintf(stderr, "STATS frames=%u ttl=%u syn=%u fcs_bad=%u\n", flt.frames, flt.ipf.ttl_fixed, flt.ipf.syn_dropped, flt.fcs_bad);
    return 0;
}
