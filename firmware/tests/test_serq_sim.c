/*
 * The serial line to the Dreamcast, simulated with the real rules (serq_model.h):
 * a game gets a 200-byte UDP update every 50 ms while a TCP download keeps an 8 KB window
 * full (the worst case: the server sends as fast as the window allows).
 *   old: one line of bytes (ESP32 ring + 4 KB on the cable, 1 KB pieces), nothing first
 *   new: UDP-first queues on the ESP32, ~20 ms on the cable, 256-byte pieces
 * The SPI link is >100x faster than the line, so bytes cross it instantly here.
 * Checks: the line never idles while data waits (the download keeps its speed); the game
 * update waits about one TCP segment's time with the new rules, much less than before.
 */
#include <stdio.h>
#include <string.h>
#include "serq_model.h"

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

typedef struct { int udp; double born; uint32_t len; } pk_t;
#define FRAME(l) ((double)(l) + 8.0)                 /* PPP/HDLC framing */
#define NP 8192
static pk_t pool[NP];
static int pool_i;
static uint32_t drops;
static void drop(void *p) { (void)p; drops++; }

/* the stream after the queue: packets in line order and the byte where each ends */
static pk_t *strm[1 << 17];
static double strm_end[1 << 17];

typedef struct { double avg_ms, max_ms, line_use, kbps; uint32_t udp_done, drops; } res_t;

static res_t run(int newq, float baud, int bulk, double secs) {
    const double dt = 0.0001, Bps = baud / 10.0;
    serq_t s; memset(&s, 0, sizeof s);
    int head = 0, tail = 0;
    double put = 0, drained = 0, zero = 0, tcp_bytes = 0;
    double inflight = 0, next_udp = 0.0137, busy = 0, waiting = 0, sum = 0, mx = 0;
    uint32_t udp_done = 0;
    uint32_t hold = newq ? serq_hold(baud) : 4096, piece = newq ? SERQ_PIECE : 1024;
    drops = 0; pool_i = 0;
    for (double t = 0; t < secs; t += dt) {
        /* arrivals: the game's update, and TCP segments while the window has room */
        pk_t *in[8]; int nin = 0;
        if (t >= next_udp) { pk_t *p = &pool[pool_i++ % NP]; *p = (pk_t){ 1, t, 200 }; in[nin++] = p; next_udp += 0.05; }
        while (bulk && inflight + 1500 <= 8192 && nin < 8) { pk_t *p = &pool[pool_i++ % NP]; *p = (pk_t){ 0, t, 1500 }; in[nin++] = p; inflight += 1500; }
        for (int i = 0; i < nin; i++) {
            if (newq) serq_put(&s, in[i], in[i]->len, in[i]->udp, drop);
            else { strm[tail & ((1 << 17) - 1)] = in[i]; put += FRAME(in[i]->len); strm_end[tail++ & ((1 << 17) - 1)] = put; }
        }
        /* new: feed the stream one packet at a time while little waits on the ESP32 */
        if (newq) {
            pk_t *p;
            while (serq_feed_ok((uint32_t)(put - drained - zero)) && (p = serq_next(&s, NULL))) {
                strm[tail & ((1 << 17) - 1)] = p; put += FRAME(p->len); strm_end[tail++ & ((1 << 17) - 1)] = put;
            }
        }
        /* the cable takes pieces while it holds less than its limit */
        for (;;) {
            double esp = put - drained - zero;
            if (esp <= 0) break;
            double c = esp < piece ? esp : piece;
            if (newq ? zero >= hold : 4096 - zero < c) break;
            zero += c;
        }
        /* the line */
        double before = put - drained;
        if (before > 0) waiting += dt;
        double d = Bps * dt; if (d > zero) d = zero;
        if (d > 0) busy += dt;
        zero -= d; drained += d;
        /* packets that are now fully on the line */
        while (head < tail && strm_end[head & ((1 << 17) - 1)] <= drained + 1e-9) {
            pk_t *p = strm[head++ & ((1 << 17) - 1)];
            if (p->udp) { double ms = (t + dt - p->born) * 1000; sum += ms; if (ms > mx) mx = ms; udp_done++; }
            else { inflight -= 1500; tcp_bytes += 1500; }   /* the Dreamcast ACKs at once */
        }
    }
    /* a dropped TCP segment frees its window too (the server's retry is not modelled) */
    res_t r = { udp_done ? sum / udp_done : 0, mx, waiting > 0 ? busy / waiting : 1, tcp_bytes / secs / 1000.0, udp_done, drops };
    return r;
}

static void show(const char *what, res_t r) {
    printf("  %-28s game update %6.1f ms average, %6.1f ms worst; download %5.2f KB/s; line busy %5.1f %% of the time data waited; dropped %u\n",
           what, r.avg_ms, r.max_ms, r.kbps, r.line_use * 100, r.drops);
}

int main(void) {
    const float bauds[] = { 115200, 57600 };
    for (int b = 0; b < 2; b++) {
        float baud = bauds[b];
        double seg_ms = FRAME(1500) / (baud / 10.0) * 1000;     /* one TCP segment on the line */
        printf("%.0f baud (one 1500-byte segment = %.0f ms):\n", baud, seg_ms);
        res_t o = run(0, baud, 1, 20), n = run(1, baud, 1, 20);
        res_t og = run(0, baud, 0, 5), ng = run(1, baud, 0, 5);
        show("old, game + download", o);
        show("new, game + download", n);
        show("old, game only", og);
        show("new, game only", ng);
        CHECK(n.line_use > 0.995);                            /* the line never idles while data waits */
        CHECK(n.udp_done >= o.udp_done);                       /* no game update lost (old: many still queued at the end) */
        CHECK(n.drops == 0);                                  /* an 8 KB TCP window fits the bulk queue */
        /* worst: a whole segment just started + what the cable holds + the update itself */
        double bound = seg_ms + (serq_hold(baud) + SERQ_PIECE + FRAME(200)) / (baud / 10.0) * 1000 + 2;
        printf("  bound for the new worst case: %.0f ms\n", bound);
        CHECK(n.max_ms < bound);
        CHECK(n.max_ms < o.max_ms / 3);
        CHECK(ng.max_ms <= og.max_ms + 0.5);                   /* a game alone: as fast as before */
    }
    printf("%s: %d failures\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
