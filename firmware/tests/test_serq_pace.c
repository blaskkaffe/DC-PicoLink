/* Host test of the pacing part of serq_model.h (USB modem: a slow line with a deep hidden buffer).
   A Quake 3 style load on a 33.6k line: 20 updates/s of 100 B (UDP) beside a TCP download that
   offers more than the rest of the line. Without pacing everything goes into the modem (an 8 KB
   buffer is assumed) and an update waits behind it for seconds; with pacing the modem never
   holds more than ~hold + one packet and the update goes first. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "serq_model.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { int fast; uint32_t len; int64_t t_in; } pk_t;
static pk_t pool[4096]; static int npool;
static void drop_cb(void *p) { (void)p; }

int main(void) {
    serq_pace_t p; serq_pace_init(&p, 33600, 0);
    CHECK(p.bps == 3360, "33600 bit/s is 3360 B/s, got %u", p.bps);
    CHECK(serq_pace_hold(&p) == 403, "hold %u", serq_pace_hold(&p));
    serq_pace_init(&p, 100, 0); CHECK(p.bps == 480, "clamped low: %u", p.bps);
    serq_pace_init(&p, 115200, 0); CHECK(p.bps == 5600, "clamped high: %u", p.bps);

    /* the line drains at its rate, and not below zero */
    serq_pace_init(&p, 33600, 1000000);
    CHECK(serq_pace_ok(&p, 1000000), "empty line takes a packet");
    serq_pace_add(&p, 1000000, 1000);
    CHECK(p.pending == 1000 + 125 + 8, "framing added: %u", p.pending);
    CHECK(!serq_pace_ok(&p, 1000000), "1.1 KB pending is above the hold");
    uint32_t w = serq_pace_wait_ms(&p, 1000000);
    CHECK(w >= 200 && w <= 230, "wait for (1133-403)/3360 s = ~217 ms, got %u", w);
    CHECK(serq_pace_ok(&p, 1000000 + 230000), "ok after the wait");
    serq_pace_age(&p, 1000000 + 5000000);
    CHECK(p.pending == 0, "drained fully: %u", p.pending);
    serq_pace_age(&p, 10);   /* clock going back never wraps */
    CHECK(p.pending == 0, "backwards clock: %u", p.pending);

    /* drain with remainders: 100 steps of 1 ms must equal one step of 100 ms */
    serq_pace_t a, b; serq_pace_init(&a, 33600, 0); serq_pace_init(&b, 33600, 0);
    a.pending = b.pending = 2000;
    for (int i = 1; i <= 100; i++) serq_pace_age(&a, i * 1000);
    serq_pace_age(&b, 100000);
    CHECK(abs((int)a.pending - (int)b.pending) <= 1, "stepwise %u vs at once %u", a.pending, b.pending);

    /* queue sizes follow the line */
    serq_t s; memset(&s, 0, sizeof s);
    serq_pace_init(&p, 33600, 0); serq_limits(&s, &p);
    CHECK(s.fast_max == 1600 && s.bulk_max == 5040, "limits %u %u", s.fast_max, s.bulk_max);
    serq_pace_init(&p, 4800, 0); serq_limits(&s, &p);
    CHECK(s.fast_max == 1600 && s.bulk_max == 3000, "minimum limits %u %u", s.fast_max, s.bulk_max);
    memset(&s, 0, sizeof s); serq_limits(&s, &p);
    static int dummy[64]; int n_acc = 0;
    for (int i = 0; i < 30; i++) { uint32_t d0 = s.dropped; serq_put(&s, &dummy[i % 64], 1000, false, drop_cb); n_acc += s.dropped == d0; }
    CHECK(n_acc == 3, "bulk holds 3 x 1000 B at 3000 B, took %d", n_acc);
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 30; i++) serq_put(&s, &dummy[i % 64], 1000, false, drop_cb);
    CHECK(s.bulk.bytes == 8000, "default bulk limit unchanged (8192): %u", s.bulk.bytes);

    /* the simulation: 60 s on a 33.6k line */
    for (int paced = 0; paced <= 1; paced++) {
        serq_pace_t pc; serq_pace_init(&pc, 33600, 0);
        serq_t q; memset(&q, 0, sizeof q); if (paced) serq_limits(&q, &pc);
        int64_t modem_free_at = 0;          /* when the modem's buffer is empty (what it holds / 3360 B/s) */
        uint32_t worst = 0, sum = 0, nfast = 0, tcp_bytes = 0;
        npool = 0;
        for (int64_t t = 0; t < 60000000; t += 1000) {
            /* arrivals: a game update every 50 ms, TCP segments to keep ~8 KB per RTT (1 KB per 100 ms, in bursts of 8) */
            if (t % 50000 == 0) { pk_t *k = &pool[npool++ % 4096]; k->fast = 1; k->len = 100; k->t_in = t; serq_put(&q, k, 100, true, drop_cb); }
            if (t % 4000000 == 0) for (int i = 0; i < 8; i++) { pk_t *k = &pool[npool++ % 4096]; k->fast = 0; k->len = 1000; k->t_in = t; serq_put(&q, k, 1000, false, drop_cb); }
            /* feeding: paced = only while the modem holds little; unpaced = everything at once */
            for (;;) {
                if (paced && !serq_pace_ok(&pc, t)) break;
                uint32_t len; pk_t *k = serq_next(&q, &len);
                if (!k) break;
                int64_t start = modem_free_at > t ? modem_free_at : t;
                uint32_t wire = k->len + k->len / 8 + 8;
                if (!paced && (start - t) * 3360 / 1000000 + wire > 8192) continue;   /* the modem's own buffer is full: lost */
                modem_free_at = start + (int64_t)wire * 1000000 / 3360;
                if (paced) serq_pace_add(&pc, t, k->len);
                if (k->fast) { uint32_t d = (uint32_t)((modem_free_at - k->t_in) / 1000); if (d > worst) worst = d; sum += d; nfast++; }
                else tcp_bytes += k->len;
            }
        }
        printf("%s: updates %u, mean delay %u ms, worst %u ms, TCP %u B/s, dropped %u\n",
               paced ? "paced  " : "unpaced", nfast, nfast ? sum / nfast : 0, worst, tcp_bytes / 60, q.dropped);
        if (paced) {
            CHECK(worst < 800, "paced: worst update delay %u ms", worst);
            CHECK(tcp_bytes / 60 > 800, "paced: the download still gets the rest of the line, %u B/s", tcp_bytes / 60);
        } else {
            CHECK(worst > 1500, "unpaced: expected the old behaviour to show seconds, got %u ms", worst);
        }
    }
    if (fails) { printf("FAILED: %d\n", fails); return 1; }
    printf("OK: pace arithmetic, limits, Quake-3-style load on a 33.6k line\n");
    return 0;
}
