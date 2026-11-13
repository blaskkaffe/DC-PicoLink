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
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "netif/ppp/ppp_impl.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "link_model.h"
#include "hayes_model.h"
#include "hdlc_model.h"
#include "app.h"

static const char *TAG = "ppp";


enum { ST_CMD, ST_PPP };
enum { C_UP, C_DOWN };
typedef struct { uint8_t what; uint32_t session; int err; } ctl_t;

static SemaphoreHandle_t lk;
static QueueHandle_t ctl_q;
static hayes_t hay;
static hdlc_t flt;
static ppp_pcb *pcb;
static struct netif pppif;
static uint32_t session;                    /* bumped for every call; late events of an old call are ignored */
static int st = ST_CMD;
static volatile bool up;
static bool enabled = true;
static struct { uint32_t sessions, dials, stray, lwip_drop; } cnt;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char t[128];
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (k > (int)sizeof t - 1) k = sizeof t - 1;
    log_add("esp32", t, (size_t)k);
}

/* bytes to the Dreamcast; waits a little when the link queue is full */
static size_t ser_out(const void *data, size_t len) {
    const uint8_t *d = data; size_t done = 0;
    while (done < len) {
        size_t c = len - done; if (c > 1024) c = 1024;
        int tries = 0;
        while (!link_send(LINK_CH_SER, d + done, c)) {
            if (++tries > 100) return done;
            vTaskDelay(1);
        }
        link_note_ser_out(c);
        done += c;
    }
    return done;
}

static u32_t out_cb(ppp_pcb *p, const void *data, u32_t len, void *ctx) {
    (void)p; (void)ctx;
    return (u32_t)ser_out(data, len);
}

static void status_cb(ppp_pcb *p, int err, void *ctx) {
    (void)p;
    ctl_t c = { .what = err == PPPERR_NONE ? C_UP : C_DOWN, .session = (uint32_t)(uintptr_t)ctx, .err = err };
    if (err == PPPERR_NONE) ip_napt_enable_no(pppif.num, 1);      /* NAT for what comes in from the Dreamcast */
    xQueueSend(ctl_q, &c, 0);
}

static void emit_to_lwip(void *ctx, const uint8_t *p, size_t n) {
    (void)ctx;
    if (pcb && pppos_input_tcpip(pcb, p, (int)n) != ERR_OK) cnt.lwip_drop++;
}

/* end the call (lk held): close, wait until lwIP lets go of it, free */
static void teardown_locked(void) {
    if (!pcb) return;
    pppapi_close(pcb, 1);
    bool freed = false;
    for (int i = 0; i < 80 && !freed; i++) {
        if (pppapi_free(pcb) == ERR_OK) freed = true; else vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (!freed) { ESP_LOGE(TAG, "call did not end"); say("PPP: the call would not end, restart the box if it hangs"); return; }
    pcb = NULL;
    session++;
    if (up) { up = false; dnsd_set_listen(0, false, 0); dcnow_set_online(0, false); }
    st = ST_CMD;
}

/* packets to the Dreamcast pass the DreamPi helper rules (dc2k2, dcvoip: ipfilter_model.h) */
static ipf_t ipf_in;
static netif_output_fn ppp_output_orig;
static err_t ppp_output_to_dc(struct netif *nif, struct pbuf *p, const ip4_addr_t *dst) {
    if (p->len == p->tot_len) ipf_to_dc(&ipf_in, (uint8_t *)p->payload, p->len);
    return ppp_output_orig(nif, p, dst);
}

static bool start_ppp(void) {
    teardown_locked();
    if (pcb) return false;
    memset(&pppif, 0, sizeof pppif);
    hdlc_init(&flt, emit_to_lwip, NULL);
    pcb = pppapi_pppos_create(&pppif, out_cb, status_cb, (void *)(uintptr_t)session);
    if (!pcb) { say("PPP: out of memory"); return false; }
    ppp_output_orig = pppif.output;
    pppif.output = ppp_output_to_dc;
    ip4_addr_t our, his;
    IP4_ADDR(&our, 10, 64, 0, 1);
    IP4_ADDR(&his, 10, 64, 0, 2);
    ppp_set_ipcp_ouraddr(pcb, &our);
    ppp_set_ipcp_hisaddr(pcb, &his);
    ppp_set_ipcp_dnsaddr(pcb, 0, &our);
    if (pppapi_listen(pcb) != ERR_OK) {
        say("PPP: could not start the server");
        pppapi_free(pcb); pcb = NULL;
        return false;
    }
    st = ST_PPP;
    cnt.sessions++;
    return true;
}

void ser_from_dc(const uint8_t *p, size_t n) {
    xSemaphoreTake(lk, portMAX_DELAY);
    if (!enabled) {
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
            if (ev == HAYES_DIAL) {
                cnt.dials++;
                say("Dreamcast dialled; starting PPP");
                if (!start_ppp()) {
                    size_t k = hayes_no_carrier(&hay, o, sizeof o);
                    ser_out(o, k);
                }
                i++;
            } else if (ev == HAYES_PPP_FLAG) {
                say("PPP started without a dial");
                if (!start_ppp()) i++;          /* on success the same byte goes again, now as PPP */
            } else {
                i++;
            }
        } else {
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

void ppp_srv_set_enabled(bool on) {
    xSemaphoreTake(lk, portMAX_DELAY);
    if (!on) { teardown_locked(); hayes_init(&hay); }
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
        if (c.what == C_UP) {
            up = true;
            say("PPP is up: Dreamcast 10.64.0.2, DNS and gateway 10.64.0.1");
            dnsd_set_listen(0, true, 0x0A400001);
            dcnow_set_online(0, true);
        } else {
            static const char *const why[] = { "ended", "bad parameter", "could not open", "no device", "out of memory",
                "closed", "connection lost", "login refused", "protocol failed", "no answer from the Dreamcast",
                "idle", "time limit", "loopback" };
            say("PPP %s", c.err >= 0 && c.err < 13 ? why[c.err] : "ended");
            xSemaphoreTake(lk, portMAX_DELAY);
            if (c.session == session) {
                teardown_locked();
                char o[32]; size_t k = hayes_no_carrier(&hay, o, sizeof o);
                if (k) ser_out(o, k);
            }
            xSemaphoreGive(lk);
        }
    }
}

size_t net_info_json(char *out, size_t cap) {
    uint32_t q, fwd, ovr, nx, ok, fail; int n_ovr, n_up; char last[128];
    dnsd_info(&q, &fwd, &ovr, &nx, &n_ovr, &n_up, last, sizeof last);
    dcnow_info(&ok, &fail);
    char le[260]; size_t ln = json_escape(le, sizeof le - 1, last, strlen(last)); le[ln] = 0;
    return (size_t)snprintf(out, cap,
        "{\"on\":%s,\"mode\":\"%s\",\"up\":%s,\"calls\":%lu,\"dials\":%lu,\"stray_at\":%lu,"
        "\"frames\":%lu,\"ttl_fixed\":%lu,\"syn_dropped\":%lu,\"fcs_bad\":%lu,\"lwip_drop\":%lu,"
        "\"dns\":{\"queries\":%lu,\"forwarded\":%lu,\"overridden\":%lu,\"refused\":%lu,\"overrides\":%d,\"servers\":%d,\"last\":\"%s\"},"
        "\"dcnow\":{\"on\":%s,\"ok\":%lu,\"failed\":%lu}}",
        enabled ? "true" : "false", st == ST_PPP ? "ppp" : "command", up ? "true" : "false",
        (unsigned long)cnt.sessions, (unsigned long)cnt.dials, (unsigned long)cnt.stray,
        (unsigned long)flt.frames, (unsigned long)flt.ipf.ttl_fixed, (unsigned long)flt.ipf.syn_dropped,
        (unsigned long)flt.fcs_bad, (unsigned long)cnt.lwip_drop,
        (unsigned long)q, (unsigned long)fwd, (unsigned long)ovr, (unsigned long)nx, n_ovr, n_up, le,
        dcnow_enabled() ? "true" : "false", (unsigned long)ok, (unsigned long)fail);
}

void ppp_srv_start(void) {
    lk = xSemaphoreCreateMutex();
    ctl_q = xQueueCreate(8, sizeof(ctl_t));
    hayes_init(&hay);
    hdlc_init(&flt, emit_to_lwip, NULL);
    xTaskCreate(ctl_task, "ppp_ctl", 4096, NULL, 6, NULL);
    dnsd_start();
    dcnow_start();
}
