/*
 * Internet for the Dreamcast over the serial cable (E-3).
 *
 * The cable passes the Dreamcast's serial bytes in channel SER. They first meet a Hayes modem
 * stub (hayes_model.h): AT -> OK, ATD -> CONNECT. After CONNECT the bytes are PPP. They go
 * through the frame filter (hdlc_model.h: TTL 64, PowerSmash double SYN) into lwIP's PPP server.
 * The Dreamcast gets 10.64.0.2, we are 10.64.0.1 (also its DNS: dnsd.c). lwIP's NAPT
 * (outgoing side = the Wi-Fi) is switched on for the PPP interface, as iptables MASQUERADE
 * is on a DreamPi. The call ends when the Dreamcast closes PPP, or when "AT" shows up again.
 *
 * Locking: `lk` guards the call state. The lwIP callbacks (output, status) never take it:
 * they run on lwIP's thread while we may be waiting for that thread under the lock.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/pbuf.h"
#include "sdkconfig.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "netif/ppp/ppp_impl.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "link_model.h"
#include "hayes_model.h"
#include "dcnet_model.h"
#include "hdlc_model.h"
#include "serq_model.h"
#include "app.h"

static const char *TAG = "ppp";


enum { ST_CMD, ST_PPP, ST_DCNET };
enum { C_UP, C_DOWN, C_DCNET_DONE };
typedef struct { uint8_t what; uint32_t session; int err; } ctl_t;

static SemaphoreHandle_t lk;
static QueueHandle_t ctl_q;
static hayes_t hay;
static hdlc_t flt;
static hdlc_t *dcw;                         /* DCNet: only watches for the Dreamcast talking AT again (during the call) */
static void dcw_drop(void *ctx, const uint8_t *p, size_t n) { (void)ctx; (void)p; (void)n; }
static ppp_pcb *pcb;
static struct netif pppif;
static uint32_t session;                    /* bumped for every call; late events of an old call are ignored */
static int st = ST_CMD;
static int carrier;                         /* who carries the call: 0 = the serial cable, 1 = the USB modem */
static volatile bool up;
static bool enabled = true;
static struct { uint32_t sessions, dials, stray, lwip_drop, q_drop; } cnt;

#define say say_line

/* bytes to the Dreamcast; waits a little when the link queue is full */
static size_t ser_out(const void *data, size_t len) {
    const uint8_t *d = data; size_t done = 0;
    if (carrier == 1) return usbmodem_write(data, len);
    while (done < len) {
        size_t c = len - done; if (c > SERQ_PIECE) c = SERQ_PIECE;   /* the cable holds ~20 ms + two pieces: small pieces keep that short */
        int tries = 0;
        while (!link_send(LINK_CH_SER, d + done, c)) {     /* rare: packets reach here only with room (serq_feed_ok / serq_admit) */
            if (++tries > 50) return done;
            link_wait_room(20);
        }
        link_note_ser_out(c);
        done += c;
    }
    return done;
}

static void sq_clear(void);                  /* the serial queue (below) */
static volatile bool call_lan;               /* this call: the Dreamcast has its home address, no NAT (dclan.c) */

static u32_t out_cb(ppp_pcb *p, const void *data, u32_t len, void *ctx) {
    (void)p; (void)ctx;
    return (u32_t)ser_out(data, len);
}

static void status_cb(ppp_pcb *p, int err, void *ctx) {
    (void)p;
    ctl_t c = { .what = err == PPPERR_NONE ? C_UP : C_DOWN, .session = (uint32_t)(uintptr_t)ctx, .err = err };
    if (err == PPPERR_NONE && !call_lan) ip_napt_enable_no(pppif.num, 1);   /* no home address for the Dreamcast: NAT as before */
    xQueueSend(ctl_q, &c, 0);
}

static void emit_to_lwip(void *ctx, const uint8_t *p, size_t n) {
    (void)ctx;
    if (pcb && pppos_input_tcpip(pcb, p, (int)n) != ERR_OK) cnt.lwip_drop++;
}

/* end the call (lk held): close, wait until lwIP lets go of it, free */
static void teardown_locked(void) {
    if (!pcb) return;
    sq_clear();                                    /* nothing queued may reach a PPP that is going away */
    pppapi_close(pcb, 1);
    bool freed = false;
    for (int i = 0; i < 80 && !freed; i++) {
        if (pppapi_free(pcb) == ERR_OK) freed = true; else vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (!freed) { ESP_LOGW(TAG, "call still closing (LCP); freed when lwIP reports it down"); return; }   /* normal after a lost carrier: the C_DOWN path frees it */
    pcb = NULL;
    session++;
    if (up) { up = false; dnsd_set_listen(0, false, 0); dcnow_set_online(0, false); }
    st = ST_CMD;
    carrier = 0;
}

/* packets to the Dreamcast pass the DreamPi helper rules (dc2k2, dcvoip: ipfilter_model.h) */
static ipf_t ipf_in;
static netif_output_fn ppp_output_orig;
static err_t ppp_output_to_dc(struct netif *nif, struct pbuf *p, const ip4_addr_t *dst);

/* ---- the serial line's queue (serq_model.h): whole packets wait here, UDP before TCP, and
   the cable gets one at a time. Only touched in lwIP's context (its thread or the core lock). */
#if CONFIG_DCL_LINK_SPI
static serq_t sq;
static volatile bool feed_posted;
static void sq_drop(void *p) { pbuf_free((struct pbuf *)p); }
static void sq_feed(void) {
    struct pbuf *q;
    while (serq_feed_ok((uint32_t)link_backlog()) && (q = serq_next(&sq, NULL))) {
        if (netif_is_up(&pppif)) ppp_output_orig(&pppif, q, NULL);
        pbuf_free(q);
    }
}
static void sq_feed_cb(void *a) { (void)a; feed_posted = false; sq_feed(); }
/* the link task: the cable took bytes; go on in lwIP's thread if something waits */
void ppp_serial_room(void) {
    if (feed_posted || serq_empty(&sq) || !serq_feed_ok((uint32_t)link_backlog())) return;
    feed_posted = true;
    if (tcpip_try_callback(sq_feed_cb, NULL) != ERR_OK) feed_posted = false;
}
static void sq_clear(void) { LOCK_TCPIP_CORE(); serq_clear(&sq, sq_drop); UNLOCK_TCPIP_CORE(); }
#else
void ppp_serial_room(void) {}
static void sq_clear(void) {}
#endif

static err_t ppp_output_to_dc(struct netif *nif, struct pbuf *p, const ip4_addr_t *dst) {
    ipf_in.public_ip = pubip_get(); ipf_in.dnat = dnat_rules();
    if (carrier != 0) {                                        /* the USB modem has its own buffer */
        if (p->len == p->tot_len) ipf_to_dc(&ipf_in, (uint8_t *)p->payload, p->len);
        return ppp_output_orig(nif, p, dst);
    }
#if CONFIG_DCL_LINK_SPI
    struct pbuf *c = pbuf_clone(PBUF_RAW, PBUF_RAM, p);       /* own copy: a Wi-Fi buffer must not wait here */
    if (!c) { cnt.q_drop++; return ERR_OK; }
    ipf_to_dc(&ipf_in, (uint8_t *)c->payload, c->len);
    uint32_t d0 = sq.dropped;
    serq_put(&sq, c, c->tot_len, serq_is_fast(c->payload, c->len), sq_drop);
    cnt.q_drop += sq.dropped - d0;
    sq_feed();
    return ERR_OK;
#else
    if (!serq_admit((uint32_t)link_room(), p->tot_len)) { cnt.q_drop++; return ERR_OK; }   /* full: drop, never wait in lwIP's thread */
    if (p->len == p->tot_len) ipf_to_dc(&ipf_in, (uint8_t *)p->payload, p->len);
    return ppp_output_orig(nif, p, dst);
#endif
}

/* DCNet ended (dcnet.c's task): back to the modem's command mode, tell the Dreamcast */
static void dcnet_done(void) { ctl_t c = { .what = C_DCNET_DONE, .session = session }; xQueueSend(ctl_q, &c, 0); }

static uint32_t dc_addr, our_addr;           /* this call's addresses (network order) */
uint32_t ppp_dc_ip(void) { return up ? dc_addr : 0; }
struct netif *ppp_dc_netif(void) { return &pppif; }
bool ppp_dc_lanmode(void) { return call_lan; }
static bool fast_echo;                      /* Capcom matching: LCP echo every second, 4 missed = gone (netlink.py capcom) */

static bool start_ppp(int who) {
    teardown_locked();
    if (pcb) { say("PPP: the last call has not ended yet; restart the box if this repeats"); return false; }
    carrier = who;
    sq_clear();
    memset(&pppif, 0, sizeof pppif);
    hdlc_init(&flt, emit_to_lwip, NULL);
    pcb = pppapi_pppos_create(&pppif, out_cb, status_cb, (void *)(uintptr_t)session);
    if (!pcb) { say("PPP: out of memory"); carrier = 0; return false; }
    ppp_output_orig = pppif.output;
    pppif.output = ppp_output_to_dc;
    ip4_addr_t our, his;
    IP4_ADDR(&our, 10, 64, 0, 1);
    IP4_ADDR(&his, 10, 64, 0, 2);
    /* as DreamPi's autoconfigure_ppp: the VPN's tun+1/tun+2, else the home network's .98/.99 */
    uint32_t vip = vpn_dc_addr(), lthis, ldc;
    call_lan = dclan_addrs(&lthis, &ldc);
    if (vip) { his.addr = lwip_htonl(vip); our.addr = lwip_htonl(vip + 1); }
    else if (call_lan) { his.addr = lwip_htonl(ldc); our.addr = lwip_htonl(lthis); }
    dc_addr = his.addr; our_addr = our.addr;
    ppp_set_ipcp_ouraddr(pcb, &our);
    ppp_set_ipcp_hisaddr(pcb, &his);
    ppp_set_ipcp_dnsaddr(pcb, 0, &our);
    if (fast_echo) { pcb->settings.lcp_echo_interval = 1; pcb->settings.lcp_echo_fails = 4; fast_echo = false; }
    if (pppapi_listen(pcb) != ERR_OK) {
        say("PPP: could not start the server");
        pppapi_free(pcb); pcb = NULL;
        carrier = 0;
        return false;
    }
    st = ST_PPP;
    cnt.sessions++;
    return true;
}

void ser_from_dc(const uint8_t *p, size_t n) {
    xSemaphoreTake(lk, portMAX_DELAY);
    if (!enabled || carrier == 1) {             /* off, or a USB modem call is up: only the monitor */
        ser_mon_add(true, p, n);
        xSemaphoreGive(lk);
        return;
    }
    size_t i = 0;
    while (i < n) {
        if (st == ST_CMD) {
            char o[160]; size_t ol;
            hayes_ev_t ev = hayes_byte(&hay, p[i], o, sizeof o, &ol);
            ser_mon_add(true, &p[i], 1);
            if (ol) { ser_out(o, ol); ser_mon_add(false, (const uint8_t *)o, ol); }
            char num[32];
            if (ev == HAYES_DIAL && dcnet_wanted(dcnet_mode(), hay.number, false, num, sizeof num)) {
                cnt.dials++;
                say("Dreamcast dialled %s; passing the call to DCNet", hay.number);
                teardown_locked();
                if (dcnet_call(num, ser_out, dcnet_done)) { st = ST_DCNET; carrier = 0; if (!dcw) dcw = malloc(sizeof *dcw); if (dcw) hdlc_init(dcw, dcw_drop, NULL); }
                else { size_t k = hayes_no_carrier(&hay, o, sizeof o); ser_out(o, k); }
                i++;
            } else if (ev == HAYES_DIAL) {
                cnt.dials++;
                say("Dreamcast dialled%s%s; starting PPP", hay.number[0] ? " " : "", hay.number);
                if (!start_ppp(0)) {
                    size_t k = hayes_no_carrier(&hay, o, sizeof o);
                    ser_out(o, k);
                }
                i++;
            } else if (ev == HAYES_PPP_FLAG) {
                say("PPP started without a dial");
                if (!start_ppp(0)) i++;          /* on success the same byte goes again, now as PPP */
            } else {
                i++;
            }
        } else if (st == ST_DCNET) {                 /* the bytes go to DCNet as they are */
            dcnet_from_dc(p + i, n - i);
            if (dcw) hdlc_feed(dcw, p + i, n - i, (uint32_t)(esp_timer_get_time() / 1000));
            i = n;
            if (dcw && dcw->saw_at) { dcw->saw_at = false; say("Dreamcast sent AT during the DCNet call; ending it"); dcnet_hangup(); }
        } else {
            flt.ipf.public_ip = pubip_get(); flt.ipf.dnat = dnat_rules();
            hdlc_feed(&flt, &p[i], 1, (uint32_t)(esp_timer_get_time() / 1000));
            i++;
            if (flt.saw_at) {                   /* the Dreamcast is at its modem again: the call is over */
                cnt.stray++;
                say("Dreamcast sent AT during PPP; ending the call");
                teardown_locked();
                hayes_init(&hay);
                char o[160]; size_t ol;
                hayes_byte(&hay, 'A', o, sizeof o, &ol);
                hayes_byte(&hay, 'T', o, sizeof o, &ol);
                if (ol) ser_out(o, ol);
            }
        }
    }
    xSemaphoreGive(lk);
}

/* ---- a call that came in through the USB modem (usbmodem.c) ---- */
bool ppp_modem_call_start(bool capcom) {
    xSemaphoreTake(lk, portMAX_DELAY);
    fast_echo = capcom;
    bool ok = enabled && start_ppp(1);
    xSemaphoreGive(lk);
    return ok;
}

void ppp_modem_bytes(const uint8_t *p, size_t n) {
    xSemaphoreTake(lk, portMAX_DELAY);
    if (carrier == 1 && pcb && st == ST_PPP) {
        flt.ipf.public_ip = pubip_get(); flt.ipf.dnat = dnat_rules();
        hdlc_feed(&flt, p, n, (uint32_t)(esp_timer_get_time() / 1000));
    }
    xSemaphoreGive(lk);
}

void ppp_modem_call_end(void) {
    xSemaphoreTake(lk, portMAX_DELAY);
    if (carrier == 1) teardown_locked();
    xSemaphoreGive(lk);
}

bool ppp_srv_up(void) { return up; }

void ppp_srv_set_enabled(bool on) {
    xSemaphoreTake(lk, portMAX_DELAY);
    if (!on) { teardown_locked(); if (st == ST_DCNET) { dcnet_hangup(); st = ST_CMD; } hayes_init(&hay); }
    enabled = on;
    xSemaphoreGive(lk);
    say("Internet for the Dreamcast over serial: %s", on ? "on" : "off (serial bytes go to the monitor)");
}

static void ctl_task(void *arg) {
    (void)arg;
    ctl_t c;
    for (;;) {
        if (xQueueReceive(ctl_q, &c, portMAX_DELAY) != pdTRUE) continue;
        if (c.session != session) continue;                       /* an old call */
        if (c.what == C_DCNET_DONE) {
            xSemaphoreTake(lk, portMAX_DELAY);
            if (st == ST_DCNET) {
                st = ST_CMD;
                free(dcw); dcw = NULL;
                hayes_init(&hay);
                char o[32]; size_t k = hayes_no_carrier(&hay, o, sizeof o);
                if (k) ser_out(o, k);
            }
            xSemaphoreGive(lk);
        } else if (c.what == C_UP) {
            up = true;
            uint32_t d = lwip_ntohl(dc_addr), o = lwip_ntohl(our_addr);
            say("PPP is up: Dreamcast %u.%u.%u.%u, DNS and gateway %u.%u.%u.%u%s", (unsigned)(d >> 24), (unsigned)(d >> 16 & 255), (unsigned)(d >> 8 & 255), (unsigned)(d & 255),
                (unsigned)(o >> 24), (unsigned)(o >> 16 & 255), (unsigned)(o >> 8 & 255), (unsigned)(o & 255), vpn_dc_addr() ? " (VPN addresses, as on DreamPi: only the VPN's game networks go through the tunnel, everything else goes out as the home address)" : "");
            dnsd_set_listen(0, true, o);
            dcnow_set_online(0, true);
        } else {
            static const char *const why[] = { "ended", "bad parameter", "could not open", "no device", "out of memory",
                "closed", "connection lost", "login refused", "protocol failed", "no answer from the Dreamcast",
                "idle", "time limit", "loopback" };
            say("PPP %s", c.err >= 0 && c.err < 13 ? why[c.err] : "ended");
            xSemaphoreTake(lk, portMAX_DELAY);
            if (c.session == session) {
                bool modem = carrier == 1;
                teardown_locked();
                if (modem) usbmodem_call_over();
                else {
                    char o[32]; size_t k = hayes_no_carrier(&hay, o, sizeof o);
                    if (k) ser_out(o, k);
                }
            }
            xSemaphoreGive(lk);
        }
    }
}

size_t net_info_json(char *out, size_t cap) {
    uint32_t q, fwd, ovr, nx, ok, fail; int n_ovr, n_up; char last[128];
    dnsd_info(&q, &fwd, &ovr, &nx, &n_ovr, &n_up, last, sizeof last);
    dcnow_info(&ok, &fail);
    char idj[220]; dcnow_id_json(idj, sizeof idj);
    char le[260]; size_t ln = json_escape(le, sizeof le - 1, last, strlen(last)); le[ln] = 0;
    return (size_t)snprintf(out, cap,
        "{\"on\":%s,\"mode\":\"%s\",\"via\":\"%s\",\"up\":%s,\"calls\":%lu,\"dials\":%lu,\"stray_at\":%lu,"
        "\"frames\":%lu,\"ttl_fixed\":%lu,\"syn_dropped\":%lu,\"fcs_bad\":%lu,\"lwip_drop\":%lu,\"queue_full\":%lu,"
        "\"dns\":{\"tcp\":%s,\"queries\":%lu,\"forwarded\":%lu,\"overridden\":%lu,\"refused\":%lu,\"overrides\":%d,\"servers\":%d,\"last\":\"%s\"},"
        "\"dcnow\":{\"on\":%s,\"ok\":%lu,\"failed\":%lu,\"id\":%s}}",
        enabled ? "true" : "false", st == ST_PPP ? "ppp" : st == ST_DCNET ? "dcnet" : "command", carrier == 1 ? "USB modem" : "serial cable", up ? "true" : "false",
        (unsigned long)cnt.sessions, (unsigned long)cnt.dials, (unsigned long)cnt.stray,
        (unsigned long)flt.frames, (unsigned long)flt.ipf.ttl_fixed, (unsigned long)flt.ipf.syn_dropped,
        (unsigned long)flt.fcs_bad, (unsigned long)cnt.lwip_drop, (unsigned long)cnt.q_drop,
        dnsd_tcp() ? "true" : "false", (unsigned long)q, (unsigned long)fwd, (unsigned long)ovr, (unsigned long)nx, n_ovr, n_up, le,
        dcnow_enabled() ? "true" : "false", (unsigned long)ok, (unsigned long)fail, idj);
}

void ppp_srv_start(void) {
    lk = xSemaphoreCreateMutex();
    ctl_q = xQueueCreate(8, sizeof(ctl_t));
    hayes_init(&hay);
    hdlc_init(&flt, emit_to_lwip, NULL);
    xTaskCreate(ctl_task, "ppp_ctl", 4096, NULL, 6, NULL);
    dnsd_start();
    dcnow_start();
    dcnet_start();
}
