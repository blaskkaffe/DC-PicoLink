/*
 * Serial end to end, bit by bit: the Dreamcast's SCIF at its real rate (111607 or 120192 for "115200"),
 * the cable's PIO receiver (uart.pio's exact sample points, 3-sample vote, stop bit), its low-pulse
 * meter and auto-baud (baud_model.h), the ESP32's Hayes stub (hayes_model.h) and PPP framing
 * (hdlc_model.h), and the way back: the cable's PIO transmitter at its rate into the SCIF's receiver.
 *
 * Scenarios from the field logs (Nov 2026):
 *   - openMenu "Serial": "AT" twice, 600 ms each, then ATZ, ATDT, PPP (DC at 111607 and at 120192);
 *   - the same against the old start rate 57600 (it failed: kept as the regression it was);
 *   - a loader at 120192, an SD session, then the game at 111607 (the cable forgets and starts again);
 *   - Quake III over serial: PPP frames both ways for a while, nothing may be lost.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#define DC_BAUD_MATCH 1
#include "../baud_model.h"
#include "../hayes_model.h"
#include "../hdlc_model.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---------- a line: edges in time (seconds); idle high ---------- */
typedef struct { double *t; uint8_t *lv; size_t n, cap; double end; } line_t;
static void line_add(line_t *l, double t, int lv) {
    if (l->n && l->lv[l->n - 1] == lv) return;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4096; l->t = realloc(l->t, l->cap * sizeof *l->t); l->lv = realloc(l->lv, l->cap); }
    l->t[l->n] = t; l->lv[l->n] = (uint8_t)lv; l->n++;
}
/* 8N1 at `rate` from time t; returns the time after the stop bit */
static double line_byte(line_t *l, double t, double rate, uint8_t b) {
    double T = 1.0 / rate;
    line_add(l, t, 0);
    for (int i = 0; i < 8; i++) line_add(l, t + (1 + i) * T, (b >> i) & 1);
    line_add(l, t + 9 * T, 1);
    l->end = t + 10 * T;
    return t + 10 * T;
}
static int line_level(const line_t *l, double t, size_t *hint) {
    size_t i = *hint;
    while (i < l->n && l->t[i] <= t) i++;
    while (i > 0 && l->t[i - 1] > t) i--;
    *hint = i;
    return i == 0 ? 1 : l->lv[i - 1];
}
/* next falling edge at or after t (the time of the edge), -1 when none */
static double line_next_fall(const line_t *l, double t, size_t *hint) {
    size_t i = *hint;
    while (i > 0 && l->t[i - 1] >= t) i--;
    while (i < l->n && (l->t[i] < t || l->lv[i] != 0)) i++;
    *hint = i;
    return i < l->n ? l->t[i] : -1;
}

/* ---------- the cable's receiver (uart.pio) at 16 SM clocks per bit of `wire` ----------
   wait 0 [7] / jmp pin / set x,7 [12] / samples at 22,24,26 (+16 per bit) / stop at 150 / wait 1 */
typedef struct {
    double wire;         /* current rate */
    double t;            /* SM time: waiting for a start bit from here */
    size_t h1, h2;
    uint32_t fe, chars;
} rx_t;
/* decode everything up to time `until`; out: bytes (ok ones), returns count */
static size_t rx_run(rx_t *r, const line_t *l, double until, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        double Tc = 1.0 / (16.0 * r->wire);
        double e = line_next_fall(l, r->t, &r->h1);
        if (e < 0) break;
        double t0 = ceil(e / Tc) * Tc;                    /* the SM notices on its next clock */
        if (t0 + 152 * Tc > until) break;
        if (line_level(l, t0 + 8 * Tc, &r->h2)) { r->t = t0 + 9 * Tc; continue; }   /* glitch */
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) {
            int s = line_level(l, t0 + (22 + 16 * k) * Tc, &r->h2) + line_level(l, t0 + (24 + 16 * k) * Tc, &r->h2) +
                    line_level(l, t0 + (26 + 16 * k) * Tc, &r->h2);
            if (s >= 2) b |= (uint8_t)(1u << k);
        }
        int stop = line_level(l, t0 + 150 * Tc, &r->h2);
        r->chars++;
        if (stop) { if (n < cap) out[n++] = b; } else r->fe++;
        /* wait 1 pin: the next clock with the line high */
        double t = t0 + 151 * Tc;
        while (!line_level(l, t, &r->h2)) t += Tc;
        r->t = t;
    }
    return n;
}

/* ---------- the Dreamcast's SCIF receiver at `rate` (16x, sample in the middle of each bit) ---------- */
typedef struct { double rate, t; size_t h1, h2; uint32_t fe; } scif_rx_t;
static size_t scif_run(scif_rx_t *r, const line_t *l, double until, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        double Tc = 1.0 / (16.0 * r->rate);
        double e = line_next_fall(l, r->t, &r->h1);
        if (e < 0) break;
        double t0 = ceil(e / Tc) * Tc;
        if (t0 + 160 * Tc > until) break;
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) {   /* SH4 SCIF: majority of the 7th, 8th, 9th clock of each bit */
            int s = line_level(l, t0 + (16 + 16 * k + 6) * Tc, &r->h2) + line_level(l, t0 + (16 + 16 * k + 7) * Tc, &r->h2) +
                    line_level(l, t0 + (16 + 16 * k + 8) * Tc, &r->h2);
            if (s >= 2) b |= (uint8_t)(1u << k);
        }
        int stop = line_level(l, t0 + (16 * 9 + 7) * Tc, &r->h2);
        if (stop) { if (n < cap) out[n++] = b; } else r->fe++;
        double t = t0 + (16 * 9 + 8) * Tc;
        while (!line_level(l, t, &r->h2)) t += Tc;
        r->t = t;
    }
    return n;
}

/* ---------- the cable's meter + auto-baud (main.c autobaud_task, same models) ---------- */
#define SYS_HZ 250e6
typedef struct {
    uint32_t win[32], wn;
    abc_t abc;
    size_t hint, done;   /* edges consumed */
    int followed;
    double host;         /* host rate (cur_baud) */
    double holdoff_until;
} ab_t;
/* feed the low periods that ended before `until`; may change r->wire (between characters) */
static void ab_run(ab_t *a, rx_t *r, const line_t *l, double until, uint32_t fe_count) {
    for (; a->done + 1 < l->n && l->t[a->done + 1] <= until; a->done++) {
        if (l->lv[a->done] != 0) continue;
        double w = l->t[a->done + 1] - l->t[a->done];
        uint32_t cyc = 2u * (uint32_t)(w * SYS_HZ / 2.0) + 4u;
        if (cyc < 40u) continue;
        a->win[a->wn++] = cyc;
        if (a->wn < 32) continue;
        a->wn = 0;
        float rate; bool clean;
        if (!abw_measure(a->win, (float)SYS_HZ, &rate, &clean)) continue;
        if (rate > 400000.0f || r->wire > 400000.0) { a->abc.cand = 0; a->abc.n = 0; continue; }
        if (l->t[a->done] < a->holdoff_until) continue;
        if (!abc_step(&a->abc, rate, (float)r->wire, clean, fe_count)) continue;
        r->wire = snap_dc_rate(rate);
        a->followed++;
    }
}
/* a new program after an SD session: forget the learned rate (main.c) */
static void ab_forget(ab_t *a, rx_t *r) { a->wn = 0; a->abc.cand = 0; a->abc.n = 0; r->wire = dc_effective_baud((uint32_t)a->host); }

/* ---------- one simulated session ---------- */
typedef struct {
    double dc_rate;      /* the SCIF's real rate */
    double t;            /* now */
    line_t up, down;     /* DC TXD (to the cable), cable TXD (to the DC) */
    rx_t rx; scif_rx_t srx; ab_t ab;
    hayes_t hay; bool ppp;
    hdlc_t esp_hdlc, dc_hdlc;
    uint32_t esp_good, dc_good;
    char dc_text[4096]; size_t dc_tn;      /* what the DC received while not in PPP */
    double down_free;                      /* the cable's TX is busy until */
} sim_t;

static void esp_frame(void *ctx, const uint8_t *p, size_t n) { (void)p; (void)n; ((sim_t *)ctx)->esp_good++; }
static void dc_frame(void *ctx, const uint8_t *p, size_t n) { (void)p; (void)n; ((sim_t *)ctx)->dc_good++; }

static void sim_init(sim_t *s, double dc_rate, double start_wire) {
    memset(s, 0, sizeof *s);
    s->dc_rate = dc_rate;
    s->rx.wire = start_wire; s->srx.rate = dc_rate;
    s->ab.host = start_wire;
    hayes_init(&s->hay);
    hdlc_init(&s->esp_hdlc, esp_frame, s);
    hdlc_init(&s->dc_hdlc, dc_frame, s);
}
/* the cable sends bytes to the DC (from the ESP32), at the cable's current rate */
static void cable_send(sim_t *s, const uint8_t *p, size_t n) {
    double t = s->down_free > s->t ? s->down_free : s->t;
    t += 20e-6;                                        /* link latency */
    for (size_t i = 0; i < n; i++) t = line_byte(&s->down, t, s->rx.wire, p[i]);
    s->down_free = t;
}
/* run the world until time `until` */
static void sim_run(sim_t *s, double until) {
    uint8_t b[8192];
    for (double t = s->t; t < until; ) {
        double step = 0.002;
        t = t + step > until ? until : t + step;
        ab_run(&s->ab, &s->rx, &s->up, t - 0.0002, s->rx.fe);
        size_t n = rx_run(&s->rx, &s->up, t, b, sizeof b);
        s->t = t;
        for (size_t i = 0; i < n; i++) {                /* the ESP32 */
            if (s->ppp) { hdlc_feed(&s->esp_hdlc, &b[i], 1, (uint32_t)(t * 1000)); continue; }
            char o[128]; size_t ol;
            hayes_ev_t ev = hayes_byte(&s->hay, b[i], o, sizeof o, &ol);
            if (ol) cable_send(s, (const uint8_t *)o, ol);
            if (ev == HAYES_DIAL) s->ppp = true;
            if (ev == HAYES_PPP_FLAG) { s->ppp = true; hdlc_feed(&s->esp_hdlc, &b[i], 1, (uint32_t)(t * 1000)); }
        }
        size_t m = scif_run(&s->srx, &s->down, t, b, sizeof b);   /* the Dreamcast */
        for (size_t i = 0; i < m; i++) {
            if (s->dc_tn + 1 < sizeof s->dc_text && (b[i] >= 0x20 || b[i] == '\r' || b[i] == '\n')) s->dc_text[s->dc_tn++] = (char)b[i];
            s->dc_text[s->dc_tn] = 0;
            hdlc_feed(&s->dc_hdlc, &b[i], 1, (uint32_t)(t * 1000));
        }
    }
}
/* the DC sends bytes at its rate, starting now (after what it is still sending) */
static double dc_t;
static void dc_send(sim_t *s, const char *p, size_t n) {
    double t = dc_t > s->t ? dc_t : s->t;
    for (size_t i = 0; i < n; i++) t = line_byte(&s->up, t, s->dc_rate, (uint8_t)p[i]);
    dc_t = t;
}
/* the DC waits up to ms for a line starting with `want` (after what it had seen before) */
static bool dc_wait(sim_t *s, const char *want, double ms) {
    size_t from = s->dc_tn;
    double end = s->t + ms / 1000.0;
    while (s->t < end) {
        sim_run(s, s->t + 0.005);
        for (size_t i = from; i < s->dc_tn; i++)
            if ((i == 0 || s->dc_text[i - 1] == '\n' || s->dc_text[i - 1] == '\r') && strncmp(&s->dc_text[i], want, strlen(want)) == 0) return true;
    }
    return false;
}

/* openMenu's serial way (dcnow_net.c serial_probe + run_serial) */
static int openmenu(sim_t *s, double *probe_ms) {
    double t0 = s->t;
    bool ok = false;
    for (int i = 0; i < 2 && !ok; i++) { dc_send(s, "AT\r", 3); ok = dc_wait(s, "OK", 600); }
    *probe_ms = (s->t - t0) * 1000;
    if (!ok) return -1;
    dc_send(s, "ATZ\r", 4);
    if (!dc_wait(s, "OK", 1000)) return -2;
    dc_send(s, "ATDT111111111\r", 14);
    if (!dc_wait(s, "CONNECT", 10000)) return -3;
    return 0;
}

/* PPP both ways: n frames of each size, interleaved as a game does */
static void ppp_traffic(sim_t *s, int n, uint32_t *sent_up, uint32_t *sent_down) {
    uint8_t p[1600], e[3300];
    for (int i = 0; i < n; i++) {
        size_t len = (size_t)(20 + (i * 37) % 600);
        for (size_t k = 0; k < len; k++) p[k] = (uint8_t)(k * 7 + i);
        p[0] = 0xFF; p[1] = 0x03; p[2] = 0x00; p[3] = 0x21;
        size_t el = hdlc_encode(p, len, e);
        dc_send(s, (const char *)e, el); (*sent_up)++;
        cable_send(s, e, el); (*sent_down)++;
        sim_run(s, s->t + 0.01);
    }
    sim_run(s, (dc_t > s->down_free ? dc_t : s->down_free) + 0.05);
}

int main(void) {
    const double rates[] = { 50e6 / (32 * 14), 50e6 / (32 * 13) };   /* 111607 (KOS now), 120192 (dcload, older KOS) */
    for (int r = 0; r < 2; r++) {
        sim_t *s = calloc(1, sizeof *s); dc_t = 0;
        sim_init(s, rates[r], 115200);
        s->t = 0.1;
        double pm;
        int rc = openmenu(s, &pm);
        printf("%s openMenu serial, DC at %.0f, cable from 115200: %s (probe answered after %.0f ms; wire now %.0f, %u framing errors)\n",
               rc == 0 ? "PASS" : "FAIL", rates[r], rc == 0 ? "CONNECT" : rc == -1 ? "probe unanswered" : rc == -2 ? "ATZ unanswered" : "no CONNECT",
               pm, s->rx.wire, s->rx.fe);
        fails += rc != 0;
        uint32_t up = 0, down = 0;
        ppp_traffic(s, 200, &up, &down);
        bool ok = s->esp_good == up && s->dc_good == down && fabs(s->rx.wire - rates[r]) < 1;
        printf("%s Quake III-like PPP at %.0f: %u/%u frames to the ESP32, %u/%u to the Dreamcast; the cable followed to %.0f (%d changes, %u framing errors)\n",
               ok ? "PASS" : "FAIL", rates[r], s->esp_good, up, s->dc_good, down, s->rx.wire, s->ab.followed, s->rx.fe);
        fails += !ok;
        free(s);
    }
    {   /* the regression: the cable at 57600 (before 28f4e08) and openMenu's two short probes */
        sim_t *s = calloc(1, sizeof *s); dc_t = 0;
        sim_init(s, rates[0], 57600);
        s->t = 0.1;
        double pm;
        int rc = openmenu(s, &pm);
        printf("%s old start rate 57600: openMenu's probe %s (as on the console)\n", rc == -1 ? "PASS" : "FAIL",
               rc == -1 ? "goes unanswered" : "was answered");
        fails += rc != -1;
        free(s);
    }
    {   /* the field log: loader at 120192, an SD session, the game at 111607 */
        sim_t *s = calloc(1, sizeof *s); dc_t = 0;
        sim_init(s, rates[1], 115200);
        s->t = 0.1;
        const char *junk = "DreamShell loader v4 starting..\r\n";
        for (int i = 0; i < 6; i++) { dc_send(s, junk, strlen(junk)); sim_run(s, s->t + 0.05); }
        sim_run(s, s->t + 0.3);
        double loader_wire = s->rx.wire;
        CHECK(fabs(loader_wire - rates[1]) < 1, "the loader's 120192 was not followed (wire %.0f)", loader_wire);
        ab_forget(&s->ab, &s->rx);                      /* > 1 s of SPI (SD) in between */
        s->dc_rate = rates[0]; s->srx.rate = rates[0];
        s->t += 2; dc_t = s->t;
        s->rx.t = s->t; s->srx.t = s->t;
        double pm;
        int rc = openmenu(s, &pm);
        printf("%s loader 120192 -> SD -> game 111607: %s (wire %.0f)\n", rc == 0 ? "PASS" : "FAIL",
               rc == 0 ? "CONNECT" : "failed", s->rx.wire);
        fails += rc != 0;
        free(s);
    }
    printf(fails ? "serial_sim: %d failures\n" : "serial_sim: all ok\n", fails);
    return fails != 0;
}
