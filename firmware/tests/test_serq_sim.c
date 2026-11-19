/*
 * The serial line to the Dreamcast, simulated with the real rules (serq_model.h).
 * Down: a game update (200-byte UDP) every 50 ms, with or without a TCP download keeping an
 * 8 KB window full. Up: the Dreamcast's own game packet every 33 ms, as Quake III's client
 * sends; it reaches the ESP32 only at a link transaction, and the cable runs none while it
 * holds a piece from the ESP32 it could not take (a refused piece).
 *   old    one line of bytes: ESP32 ring + 4 KB on the cable, 1 KB pieces, nothing first
 *   a539   UDP first, the cable refused pieces above ~20 ms (19 Nov: the Dreamcast's own
 *          packets waited behind refused pieces)
 *   new    UDP first, the ESP32 paces by time (serq_pace_t), the cable takes what fits in 4 KB
 * The SPI link is >100x faster than the line, so bytes cross it instantly here.
 * Checks: the Dreamcast's own packets never wait (a539 made them); the line never idles while
 * data waits; game updates wait far less than "old" during a download, and as little alone.
 */
#include <stdio.h>
#include <string.h>
#include "serq_model.h"

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

typedef struct { int udp; double born; uint32_t len; } pk_t;
#define FRAME(l) ((double)(l) + 8.0)                 /* PPP/HDLC framing */
#define NP 8192
#define SM ((1 << 17) - 1)
static pk_t pool[NP];
static int pool_i;
static uint32_t drops;
static void drop(void *p) { (void)p; drops++; }
static pk_t *strm[SM + 1];
static double strm_end[SM + 1];

enum { OLD, A539, NEW };
typedef struct { double avg_ms, max_ms, line_use, kbps, up_avg, up_max; uint32_t udp_done, drops; } res_t;

static res_t run(int mode, float baud, int bulk, double secs) {
    const double dt = 0.0001, Bps = baud / 10.0;
    serq_t s; memset(&s, 0, sizeof s);
    serq_pace_t pace = { 0, 0 };
    int head = 0, tail = 0;
    double put = 0, drained = 0, zero = 0, tcp_bytes = 0;
    double inflight = 0, next_udp = 0.0137, busy = 0, waiting = 0, sum = 0, mx = 0;
    double next_up = 0.005, up_born = -1, up_sum = 0, up_mx = 0; uint32_t up_n = 0;
    uint32_t udp_done = 0;
    uint32_t piece = mode == OLD ? 1024 : mode == A539 ? 256 : SERQ_PIECE;
    drops = 0; pool_i = 0;
    for (double t = 0; t < secs; t += dt) {
        pk_t *in[8]; int nin = 0;
        if (t >= next_udp) { pk_t *p = &pool[pool_i++ % NP]; *p = (pk_t){ 1, t, 200 }; in[nin++] = p; next_udp += 0.05; }
        while (bulk && inflight + 1500 <= 8192 && nin < 8) { pk_t *p = &pool[pool_i++ % NP]; *p = (pk_t){ 0, t, 1500 }; in[nin++] = p; inflight += 1500; }
        for (int i = 0; i < nin; i++) {
            if (mode != OLD) serq_put(&s, in[i], in[i]->len, in[i]->udp, drop);
            else { strm[tail & SM] = in[i]; put += FRAME(in[i]->len); strm_end[tail++ & SM] = put; }
        }
        if (mode == A539) {
            pk_t *p;
            while (put - drained - zero < 1 && (p = serq_next(&s, NULL))) {
                strm[tail & SM] = p; put += FRAME(p->len); strm_end[tail++ & SM] = put;
            }
        } else if (mode == NEW) {
            pk_t *p; uint32_t w;
            while (!serq_empty(&s) && serq_feed_ok((uint32_t)(put - drained - zero)) &&
                   serq_pace_ok(&pace, (uint64_t)(t * 1e6), baud, &w) && (p = serq_next(&s, NULL))) {
                strm[tail & SM] = p; put += FRAME(p->len); strm_end[tail++ & SM] = put;
                serq_pace_sent(&pace, (uint32_t)FRAME(p->len));
            }
        }
        int pending = 0;
        for (;;) {
            double esp = put - drained - zero;
            if (esp <= 0) break;
            double c = esp < piece ? esp : piece;
            if (mode == A539 ? zero >= serq_hold(baud) : 4096 - zero < c) { pending = 1; break; }
            zero += c;
        }
        if (t >= next_up) { if (up_born < 0) up_born = t; next_up += 0.033; }
        if (up_born >= 0 && !pending) {
            double ms = (t - up_born) * 1000; up_sum += ms; if (ms > up_mx) up_mx = ms; up_n++; up_born = -1;
        }
        if (put - drained > 0) waiting += dt;
        double d = Bps * dt; if (d > zero) d = zero;
        if (d > 0) busy += dt;
        zero -= d; drained += d;
        while (head < tail && strm_end[head & SM] <= drained + 1e-9) {
            pk_t *p = strm[head++ & SM];
            if (p->udp) { double ms = (t + dt - p->born) * 1000; sum += ms; if (ms > mx) mx = ms; udp_done++; }
            else { inflight -= 1500; tcp_bytes += 1500; }
        }
    }
    res_t r = { udp_done ? sum / udp_done : 0, mx, waiting > 0 ? busy / waiting : 1, tcp_bytes / secs / 1000.0,
                up_n ? up_sum / up_n : 0, up_mx, udp_done, drops };
    return r;
}

static void show(const char *what, res_t r) {
    printf("  %-22s down: game %6.1f avg %6.1f worst ms, download %5.2f KB/s, line busy %5.1f %%; up: %5.1f avg %5.1f worst ms\n",
           what, r.avg_ms, r.max_ms, r.kbps, r.line_use * 100, r.up_avg, r.up_max);
}

int main(void) {
    const float bauds[] = { 115200, 57600 };
    for (int b = 0; b < 2; b++) {
        float baud = bauds[b];
        double seg_ms = FRAME(1500) / (baud / 10.0) * 1000;
        printf("%.0f baud (one 1500-byte segment = %.0f ms):\n", baud, seg_ms);
        res_t o = run(OLD, baud, 1, 20), a = run(A539, baud, 1, 20), n = run(NEW, baud, 1, 20);
        res_t og = run(OLD, baud, 0, 10), ag = run(A539, baud, 0, 10), ng = run(NEW, baud, 0, 10);
        show("old, game + download", o); show("a539, game + download", a); show("new, game + download", n);
        show("old, game only", og); show("a539, game only", ag); show("new, game only", ng);
        CHECK(ng.up_max <= og.up_max + 0.2);                  /* the Dreamcast's own packets: never held up */
        CHECK(n.up_max <= o.up_max + 0.2);
        CHECK(a.up_max > 5.0);                                 /* the model shows the a539 fault */
        CHECK(n.line_use > 0.995);
        CHECK(n.udp_done >= o.udp_done);
        CHECK(n.drops == 0);
        double bound = seg_ms + (serq_hold(baud) + FRAME(200)) / (baud / 10.0) * 1000 + 5;
        printf("  bound for the new worst case: %.0f ms\n", bound);
        CHECK(n.max_ms < bound);
        CHECK(n.max_ms < o.max_ms / 3);
        CHECK(ng.max_ms <= og.max_ms + 0.5);
    }
    printf("%s: %d failures\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
