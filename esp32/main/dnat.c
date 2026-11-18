/*
 * DreamPi's dcnatrules service: the address swaps of https://shumania.ddns.net/dnat.txt
 * ("old-address new-address [port [proto]]", one per line; iptables -t nat PREROUTING ... DNAT).
 * Alien Front Online asks its old Sega address; this is what sends it to the revived server.
 * DreamPi downloads the file whenever it starts and falls back to its earlier copy: here it is
 * fetched when Wi-Fi is up (renewed every six hours) and the last good copy is kept in flash.
 * The rules themselves are applied by the packet filters (../../firmware/ipfilter_model.h).
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "ipfilter_model.h"
#include "app.h"

#define DNAT_URL "https://shumania.ddns.net/dnat.txt"
#define KEEP_MAX 1400                                   /* the stored copy */

static ipf_dnat_t sets[2];
static volatile int cur;                                /* the set in use; the other is filled, then swapped in */
static char fl[160]; static size_t fln;
static char keep[KEEP_MAX + 1]; static size_t keep_n;
static int took;

const void *dnat_rules(void) { return sets[cur].n ? &sets[cur] : NULL; }

static void line(ipf_dnat_t *d) {
    if (fln < sizeof fl && fln && fl[0] != '#' && ipf_dnat_line(d, fl, fln)) took++;
    fln = 0;
}
static void feed(ipf_dnat_t *d, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n' || p[i] == '\r') line(d);
        else if (fln < sizeof fl) fl[fln++] = p[i];
        else fln = sizeof fl;                           /* too long: skipped */
    }
}
static ipf_dnat_t *fill;
static esp_err_t http_ev(esp_http_client_event_t *e) {
    if (e->event_id != HTTP_EVENT_ON_DATA || !fill) return ESP_OK;
    if (keep_n + (size_t)e->data_len <= KEEP_MAX) { memcpy(keep + keep_n, e->data, (size_t)e->data_len); keep_n += (size_t)e->data_len; }
    else keep_n = KEEP_MAX + 1;                         /* too big to keep: used, not stored */
    feed(fill, e->data, (size_t)e->data_len);
    return ESP_OK;
}

static void swap_in(ipf_dnat_t *d) { cur = (int)(d - sets); }

/* the earlier copy from flash (before the first download, or without Internet) */
static void dnat_load(void) {
    nvs_handle_t h; size_t n = sizeof keep - 1;
    if (nvs_open("dcl", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "dnat", keep, &n) == ESP_OK && n) {
        ipf_dnat_t *d = &sets[1 - cur]; memset(d, 0, sizeof *d); fln = 0; took = 0;
        feed(d, keep, n); if (fln) line(d);
        if (d->n) { swap_in(d); char m[96]; int k = snprintf(m, sizeof m, "DNAT: %d address swaps from the last download (as DreamPi's dcnatrules)", d->n); log_add("esp32", m, (size_t)k); }
    }
    nvs_close(h);
}

static bool fetch(char *err, size_t en) {
    ipf_dnat_t *d = &sets[1 - cur]; memset(d, 0, sizeof *d); fln = 0; took = 0; keep_n = 0; fill = d;
    esp_http_client_config_t c = { .url = DNAT_URL, .event_handler = http_ev, .timeout_ms = 10000,
                                   .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 1024 };
    tls_lock();
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_err_t er = h ? esp_http_client_perform(h) : ESP_ERR_NO_MEM;
    int code = h && er == ESP_OK ? esp_http_client_get_status_code(h) : 0;
    if (h) esp_http_client_cleanup(h);
    tls_unlock();
    fill = NULL;
    if (er != ESP_OK) { snprintf(err, en, "%s", esp_err_to_name(er)); return false; }
    if (code != 200) { snprintf(err, en, "HTTP %d", code); return false; }
    if (fln) line(d);
    if (!d->n) { snprintf(err, en, "no rules in the file"); return false; }
    bool changed = d->n != sets[cur].n || memcmp(d->r, sets[cur].r, sizeof d->r[0] * (size_t)d->n);
    swap_in(d);
    if (changed) {
        char m[96]; int k = snprintf(m, sizeof m, "DNAT: %d address swaps from shumania.ddns.net/dnat.txt (as DreamPi's dcnatrules)", d->n);
        log_add("esp32", m, (size_t)k);
        if (keep_n <= KEEP_MAX) {
            nvs_handle_t h2;
            if (nvs_open("dcl", NVS_READWRITE, &h2) == ESP_OK) { nvs_set_blob(h2, "dnat", keep, keep_n); nvs_commit(h2); nvs_close(h2); }
        }
    }
    return true;
}

/* called often from DCNow!'s task (dcnow.c), like dnsd_fetch_poll */
void dnat_fetch_poll(void) {
    static bool loaded, warned;
    static uint32_t due_ms = 12000;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!loaded) { loaded = true; dnat_load(); }
    if (now - due_ms > 0x80000000u) return;
    if (!inet_up()) { due_ms = now + 2000; return; }
    char err[48];
    if (fetch(err, sizeof err)) { warned = false; due_ms = now + 6u * 3600 * 1000; return; }
    if (!warned) {
        char m[120]; int k = snprintf(m, sizeof m, "DNAT: could not fetch dnat.txt (%s); %s", err,
                                      sets[cur].n ? "using the last copy, will retry" : "no address swaps yet (Alien Front Online needs them), will retry");
        log_add("esp32", m, (size_t)k); warned = true;
    }
    due_ms = now + 30000;
}
