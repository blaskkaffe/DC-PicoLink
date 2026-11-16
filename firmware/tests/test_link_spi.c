/* SPI link (link_spi_model.h) on top of the link framing (link_model.h).
 *
 * A 1 us step simulation of the cable (SPI master: the firmware's lspi_task logic with the
 * real link_spi_master_start, link_chunk_fill, link_build, link_rx_byte) and the ESP32 (SPI
 * slave: a model of ESP-IDF's spi_slave queue with post_setup/post_trans driving HS, a task
 * that collects results with random latency and sometimes stalls, DR). Random traffic both
 * ways, random poll jitter, deliveries that sometimes have to wait (link_pending), and in
 * some runs resets of either side at random moments.
 *
 * Checked:
 *  - the cable never starts a transaction the ESP32 hasn't loaded ("unarmed"), ever
 *  - without resets: every packet arrives, in order, intact, no CRC errors, no probes
 *  - with resets: nothing corrupt or duplicated is ever delivered, order is kept, and
 *    traffic flows again shortly after both sides are back
 *  - the dc_pio_relocate() rules on synthetic instructions (the real programs are checked
 *    by test_pio_reloc.c against a build)
 *  - throughput with both directions saturated
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../link_model.h"
#include "../link_spi_model.h"
#include "../dc_pio_reloc.h"

#ifndef RP_BUDGET
#define RP_BUDGET 512
#endif
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static uint32_t rr(uint32_t lo, uint32_t hi) { return lo + rnd() % (hi - lo + 1); }

/* ---- packets with self-checking content ---------------------------------- */
static size_t make_payload(uint32_t id, uint8_t dir, uint8_t *p, size_t n) {
    if (n < 9) n = 9;
    p[0] = dir;
    memcpy(p + 1, &id, 4);
    uint32_t len32 = (uint32_t)n; memcpy(p + 5, &len32, 4);
    uint32_t x = id * 2654435761u + dir;
    for (size_t i = 9; i < n; i++) { x = x * 1103515245u + 12345u; p[i] = (x >> 16) % 5 ? (uint8_t)(x >> 8) : 0; }
    return n;
}
static bool check_payload(const uint8_t *p, size_t n, uint8_t dir, uint32_t *id_out) {
    if (n < 9 || p[0] != dir) return false;
    uint32_t id, len32; memcpy(&id, p + 1, 4); memcpy(&len32, p + 5, 4);
    if (len32 != n) return false;
    static uint8_t ref[LINK_MAX_PAYLOAD];
    make_payload(id, dir, ref, n);
    if (memcmp(ref, p, n)) return false;
    *id_out = id;
    return true;
}
static size_t pick_size(void) {
    uint32_t k = rnd() % 10;
    if (k < 4) return rr(9, 64);
    if (k < 8) return rr(60, 1514);
    if (k < 9) return 1514;
    return LINK_MAX_PAYLOAD;
}

/* ---- one side's sending queue and receiver ------------------------------- */
typedef struct {
    uint8_t *ring; uint32_t mask, wr, rd;
    uint8_t seq;
    uint32_t next_id;             /* id of the next packet generated */
    link_rx_t rx;
    uint32_t last_id_in; bool any_in;
    uint64_t got, got_bytes, corrupt, dup_or_reorder, gaps;
    uint64_t sent, sent_bytes;
} side_t;

static bool side_send(side_t *s, uint8_t dir) {
    static uint8_t pay[LINK_MAX_PAYLOAD], w[LINK_MAX_WIRE];
    if ((s->mask + 1) - (s->wr - s->rd) < LINK_MAX_WIRE) return false;
    size_t n = make_payload(s->next_id, dir, pay, pick_size());
    size_t k = link_build((uint8_t)(rnd() % LINK_CH_COUNT), s->seq, pay, n, w);
    CHECK(k);
    s->seq++; s->next_id++;
    for (size_t i = 0; i < k; i++) s->ring[(s->wr + (uint32_t)i) & s->mask] = w[i];
    s->wr += (uint32_t)k;
    s->sent++; s->sent_bytes += n;
    return true;
}
static void side_got(side_t *s, uint8_t from_dir) {
    uint32_t id;
    if (!check_payload(s->rx.payload, s->rx.n, from_dir, &id)) { s->corrupt++; return; }
    if (s->any_in && id <= s->last_id_in) s->dup_or_reorder++;
    if (s->any_in && id > s->last_id_in + 1) s->gaps += id - s->last_id_in - 1;
    s->last_id_in = id; s->any_in = true;
    s->got++; s->got_bytes += s->rx.n;
}
static void side_reset(side_t *s) {
    s->rd = s->wr;               /* queue lost */
    link_rx_init(&s->rx);
    s->seq = 0;                  /* a rebooted sender starts its sequence numbers again */
}

/* ---- the ESP32 (slave) model --------------------------------------------- */
enum { B_FREE, B_QUEUED, B_LOADED, B_DONE };
#define NBUF_MAX 4
typedef struct {
    int nbuf;
    int st[NBUF_MAX];
    uint64_t qorder[NBUF_MAX];             /* FIFO order of queued buffers */
    uint64_t qcount;
    uint8_t tx[NBUF_MAX][LINK_SPI_CHUNK], rxb[NBUF_MAX][LINK_SPI_CHUNK];
    uint32_t txn[NBUF_MAX], rxlen[NBUF_MAX];
    int hw;                                 /* loaded buffer or -1 */
    bool hs, dr;
    bool alive; uint64_t boot_done;
    uint64_t isr_at; bool isr_pending;      /* post_trans (+ load next) */
    uint64_t load_at; bool load_pending;    /* queue_trans into an idle driver */
    uint64_t task_at; bool task_waiting;
    bool slow_phase; uint64_t slow_until;
    side_t s;
} esp_t;

/* ---- the cable (master) model --------------------------------------------- */
typedef struct {
    link_spi_master_t m;
    bool alive; uint64_t boot_done;
    uint64_t next_poll, xfer_end;
    bool cs_low, xfer_valid;
    int xfer_buf;                           /* ESP buffer this transaction talks to */
    uint8_t tx[LINK_SPI_CHUNK], rx[LINK_SPI_CHUNK];
    uint32_t rx_pos;
    bool pending;
    bool hs_fell;                           /* GPIO falling-edge latch on HS */
    bool link_up; uint64_t last_rx;
    side_t s;
} rp_t;

static esp_t E; static rp_t R;
static uint64_t now;
static uint64_t unarmed, short_xfers, xfers, cs_low_load;

static void set_hs(bool v) {
    if (E.hs && !v) R.hs_fell = true;       /* the cable's GPIO latches the edge */
    E.hs = v;
}
static void esp_update_dr(void) {
    uint32_t loaded = 0;
    for (int i = 0; i < E.nbuf; i++) if (E.st[i] == B_QUEUED || E.st[i] == B_LOADED) loaded += E.txn[i];
    E.dr = E.alive && (E.s.wr != E.s.rd || loaded);
}
static void esp_load_next(void) {             /* driver: load the oldest queued buffer */
    int best = -1;
    for (int i = 0; i < E.nbuf; i++)
        if (E.st[i] == B_QUEUED && (best < 0 || E.qorder[i] < E.qorder[best])) best = i;
    if (best < 0) return;
    if (R.cs_low) cs_low_load++;              /* would start mid-transaction: must never happen */
    E.st[best] = B_LOADED; E.hw = best;
    set_hs(true);                             /* post_setup */
}
static void esp_queue(int i) {                /* lspi_load(): refill from the ring, queue */
    E.txn[i] = (uint32_t)link_chunk_fill(E.s.ring, E.s.mask, &E.s.rd, E.s.wr, E.tx[i], LINK_SPI_CHUNK);
    E.st[i] = B_QUEUED; E.qorder[i] = E.qcount++;
    esp_update_dr();
    if (E.hw < 0 && !E.load_pending) { E.load_pending = true; E.load_at = now + rr(1, 3); }
}
static void esp_reset(void) {
    if (E.hs) set_hs(false);
    E.alive = false; E.hw = -1; E.isr_pending = E.load_pending = E.task_waiting = false;
    for (int i = 0; i < E.nbuf; i++) { E.st[i] = B_FREE; E.txn[i] = 0; }
    side_reset(&E.s);
    E.dr = false;
    E.boot_done = now + rr(200000, 400000);
}
static void esp_step(void) {
    if (!E.alive) {
        if (now >= E.boot_done) { E.alive = true; for (int i = 0; i < E.nbuf; i++) esp_queue(i); }
        return;
    }
    if (E.load_pending && now >= E.load_at) { E.load_pending = false; if (E.hw < 0) esp_load_next(); }
    if (E.isr_pending && now >= E.isr_at) {  /* post_trans, then the next queued one */
        E.isr_pending = false;
        set_hs(false);
        if (E.hw >= 0) { E.st[E.hw] = B_DONE; E.hw = -1; }
        esp_load_next();
    }
    bool any_done = false;
    for (int i = 0; i < E.nbuf; i++) any_done |= E.st[i] == B_DONE;
    if (any_done && !E.task_waiting) {
        E.task_waiting = true;
        uint32_t lat = rr(5, 120);
        if (rnd() % 200 == 0) lat = rr(2000, 8000);            /* lwIP / Wi-Fi busy */
        if (E.slow_phase && now < E.slow_until) lat += 3000;
        E.task_at = now + lat;
    }
    if (E.task_waiting && now >= E.task_at) {  /* rx_task: one result */
        E.task_waiting = false;
        int i = -1;
        for (int k = 0; k < E.nbuf; k++) if (E.st[k] == B_DONE && (i < 0 || E.qorder[k] < E.qorder[i])) i = k;
        if (i >= 0) {
            for (uint32_t k = 0; k < E.rxlen[i]; k++)
                if (link_rx_byte(&E.s.rx, E.rxb[i][k])) side_got(&E.s, 'R');
            esp_queue(i);
        }
    }
}

static void rp_reset(void) {
    if (R.cs_low) {                           /* CS floats high (ESP32 pull-up): the slave ends early */
        R.cs_low = false;
        if (R.xfer_valid && E.alive && R.xfer_buf == E.hw) {
            uint32_t done = (uint32_t)((now - (R.xfer_end - (uint64_t)LINK_SPI_CHUNK * 8 * 1000000 / LINK_SPI_HZ)) *
                                       LINK_SPI_HZ / 8 / 1000000);
            if (done > LINK_SPI_CHUNK) done = LINK_SPI_CHUNK;
            memcpy(E.rxb[E.hw], R.tx, done);
            E.rxlen[E.hw] = done;
            E.isr_pending = true; E.isr_at = now + rr(1, 5);
            short_xfers++;
        }
    }
    R.alive = false;
    link_spi_master_init(&R.m);
    R.rx_pos = LINK_SPI_CHUNK; R.pending = false; R.link_up = false;
    side_reset(&R.s);
    R.boot_done = now + rr(20000, 60000);
}
static void rp_on_packet(void) {
    R.last_rx = now;
    if (!R.link_up) { R.link_up = true; }
    side_got(&R.s, 'E');
}
static void rp_step(void) {
    if (!R.alive) {
        if (now >= R.boot_done) { R.alive = true; R.next_poll = now; R.hs_fell = false; }
        return;
    }
    if (R.link_up && now - R.last_rx > 3000000) R.link_up = false;
    if (R.cs_low && now >= R.xfer_end) {      /* last clock done: exchange, CS high */
        R.cs_low = false;
        xfers++;
        if (R.xfer_valid && E.alive && E.hw == R.xfer_buf) {
            memcpy(R.rx, E.tx[R.xfer_buf], LINK_SPI_CHUNK);
            memcpy(E.rxb[R.xfer_buf], R.tx, LINK_SPI_CHUNK);
            E.rxlen[R.xfer_buf] = LINK_SPI_CHUNK;
            E.isr_pending = true; E.isr_at = now + rr(1, 6);
        } else {
            memset(R.rx, 0xFF, LINK_SPI_CHUNK);   /* nobody answered (ESP32 reset meanwhile) */
        }
    }
    if (now < R.next_poll) return;
    uint32_t jit = rr(2, 25);
    if (rnd() % 100 == 0) jit = rr(100, 600);   /* USB / other main-loop work */
    R.next_poll = now + jit;
    /* lspi_task */
    if (R.m.busy) {
        if (R.cs_low) return;
        link_spi_master_done(&R.m, (uint32_t)now);
        R.rx_pos = 0;
    }
    if (R.pending) {                           /* link_deliver() said "not now" before */
        if (rnd() % 3) return;
        R.pending = false;
        rp_on_packet();
    }
    for (int budget = RP_BUDGET; budget-- && !R.pending && R.rx_pos < LINK_SPI_CHUNK;)
        if (link_rx_byte(&R.s.rx, R.rx[R.rx_pos++])) {
            if (rnd() % 50 == 0) R.pending = true;   /* the W5500 queue / serial ring is full */
            else rp_on_packet();
        }
    bool want = R.rx_pos >= LINK_SPI_CHUNK && !R.pending && (R.s.wr != R.s.rd || E.dr);
    if (!link_spi_master_start(&R.m, (uint32_t)now, E.hs, R.hs_fell, want, R.link_up)) return;
    link_chunk_fill(R.s.ring, R.s.mask, &R.s.rd, R.s.wr, R.tx, LINK_SPI_CHUNK);
    R.hs_fell = false;                         /* gpio_acknowledge_irq(HS, EDGE_FALL) */
    R.cs_low = true;
    R.xfer_valid = E.alive && E.hw >= 0;
    if (!R.xfer_valid) unarmed++;
    R.xfer_buf = E.hw;
    R.xfer_end = now + LINK_SPI_CS_US + (uint64_t)LINK_SPI_CHUNK * 8 * 1000000 / LINK_SPI_HZ;
}

typedef struct {
    int nbuf; uint64_t dur_us; uint32_t rp_rate, esp_rate;   /* packets per second offered (0 = saturate) */
    bool resets; bool esp_first; const char *name;
} run_t;

static void run(const run_t *c, bool expect_lossless) {
    static uint8_t rring[8192], ering[32768];
    memset(&E, 0, sizeof E); memset(&R, 0, sizeof R);
    E.nbuf = c->nbuf; E.hw = -1; E.s.ring = ering; E.s.mask = sizeof ering - 1;
    R.s.ring = rring; R.s.mask = sizeof rring - 1;
    link_rx_init(&E.s.rx); link_rx_init(&R.s.rx);
    link_spi_master_init(&R.m); R.rx_pos = LINK_SPI_CHUNK;
    unarmed = short_xfers = xfers = cs_low_load = 0;
    now = 0;
    E.boot_done = c->esp_first ? 0 : 150000;  /* who comes up first */
    R.boot_done = c->esp_first ? 250000 : 0;
    uint64_t next_rp_pkt = 0, next_esp_pkt = 0, next_hello = 0, next_reset = 2000000;
    uint64_t last_reset = 0, worst_gap_after_reset = 0, last_progress = 0;
    uint64_t prev_got = 0;
    for (now = 0; now < c->dur_us; now++) {
        if (c->resets && now >= next_reset) {
            if (rnd() & 1) esp_reset(); else rp_reset();
            last_reset = now;
            next_reset = now + rr(800000, 2500000);
        }
        bool gen = now + 600000 < c->dur_us;      /* the last 0.6 s: let everything drain */
        if (gen && R.alive && now >= next_rp_pkt) {
            if (c->rp_rate == 0) { while (side_send(&R.s, 'R')) {} next_rp_pkt = now + 50; }
            else { side_send(&R.s, 'R'); next_rp_pkt = now + rr(1, 2000000 / c->rp_rate); }
        }
        if (gen && E.alive && now >= next_esp_pkt) {
            if (c->esp_rate == 0) { while (side_send(&E.s, 'E')) {} next_esp_pkt = now + 50; }
            else { side_send(&E.s, 'E'); next_esp_pkt = now + rr(1, 2000000 / c->esp_rate); }
            esp_update_dr();
        }
        if (gen && E.alive && now >= next_hello) { side_send(&E.s, 'E'); esp_update_dr(); next_hello = now + 1000000; }
        if (rnd() % 400000 == 0) { E.slow_phase = true; E.slow_until = now + 50000; }
        esp_step();
        rp_step();
        uint64_t got = E.s.got + R.s.got;
        if (got != prev_got) {
            if (last_reset && now - last_progress > worst_gap_after_reset && last_progress >= last_reset)
                worst_gap_after_reset = now - last_progress;
            prev_got = got; last_progress = now;
        }
    }
    /* rates over the time traffic was offered (minus the boot of the later side) */
    double secs = (double)(c->dur_us - 600000 - 250000) / 1e6;
    printf("  %-34s nbuf %d: %6llu xfers; cable->esp %7.1f KB/s (%llu pkts), esp->cable %7.1f KB/s (%llu pkts); "
           "crc %u/%u, gaps %llu/%llu, probes %u, short %llu\n",
           c->name, c->nbuf, (unsigned long long)xfers,
           E.s.got_bytes / secs / 1000, (unsigned long long)E.s.got, R.s.got_bytes / secs / 1000, (unsigned long long)R.s.got,
           E.s.rx.crc_bad, R.s.rx.crc_bad, (unsigned long long)E.s.gaps, (unsigned long long)R.s.gaps,
           R.m.probes, (unsigned long long)short_xfers);
    CHECK(unarmed == 0);                  /* never clock into an ESP32 that isn't ready */
    CHECK(cs_low_load == 0);
    CHECK(E.s.corrupt == 0 && R.s.corrupt == 0);
    CHECK(E.s.dup_or_reorder == 0 && R.s.dup_or_reorder == 0);
    CHECK(E.s.got > 0 && R.s.got > 0);
    if (expect_lossless) {
        CHECK(E.s.rx.crc_bad == 0 && R.s.rx.crc_bad == 0);
        CHECK(E.s.rx.cobs_bad + E.s.rx.too_long + E.s.rx.bad_hdr == 0);
        CHECK(R.s.rx.cobs_bad + R.s.rx.too_long + R.s.rx.bad_hdr == 0);
        CHECK(E.s.gaps == 0 && R.s.gaps == 0);
        CHECK(R.m.probes == 0);
        /* everything sent has arrived */
        CHECK(E.s.got == R.s.sent && R.s.got == E.s.sent);
    } else {
        CHECK(worst_gap_after_reset < 1500000);   /* traffic resumes after a reset */
    }
}

int main(void) {
    /* chunk filling: whole ring, part, wrap-around, padding */
    {
        uint8_t ring[16], out[8]; uint32_t rd = 14, wr = 14 + 5;
        for (int i = 0; i < 16; i++) ring[i] = (uint8_t)(i + 1);
        size_t n = link_chunk_fill(ring, 15, &rd, wr, out, sizeof out);
        CHECK(n == 5 && rd == wr);
        CHECK(out[0] == 15 && out[1] == 16 && out[2] == 1 && out[3] == 2 && out[4] == 3);
        CHECK(out[5] == 0 && out[6] == 0 && out[7] == 0);
        wr = rd + 12;
        n = link_chunk_fill(ring, 15, &rd, wr, out, sizeof out);
        CHECK(n == 8 && wr - rd == 4);
    }
    /* padding between packets is invisible to the receiver */
    {
        static uint8_t w[LINK_MAX_WIRE]; link_rx_t r; link_rx_init(&r);
        int got = 0;
        for (int z = 0; z < 100; z++) CHECK(!link_rx_byte(&r, 0));
        size_t k = link_build(LINK_CH_SER, 7, "abc", 3, w);
        for (size_t i = 0; i < k; i++) got += link_rx_byte(&r, w[i]);
        for (int z = 0; z < 100; z++) CHECK(!link_rx_byte(&r, 0));
        CHECK(got == 1 && r.good == 1 && r.crc_bad == 0 && r.bad_hdr == 0);
    }
    /* master decision rules */
    {
        link_spi_master_t m; link_spi_master_init(&m);
        CHECK(!link_spi_master_start(&m, 0, false, false, true, false));   /* HS low */
        CHECK(!link_spi_master_start(&m, 0, true, false, false, false));   /* nothing wanted */
        CHECK(link_spi_master_start(&m, 0, true, false, true, false));     /* first: HS is enough */
        CHECK(!link_spi_master_start(&m, 1, true, true, true, true));      /* busy */
        link_spi_master_done(&m, 1);
        CHECK(!link_spi_master_start(&m, 2, true, false, true, true));     /* no fall seen yet */
        CHECK(!link_spi_master_start(&m, 2, true, false, true, false));    /* link down but just finished: no probe */
        CHECK(link_spi_master_start(&m, 3, true, true, true, true));       /* fall seen, HS high */
        link_spi_master_done(&m, 4);
        CHECK(!link_spi_master_start(&m, 4, true, false, true, false));    /* link down, too early to probe */
        CHECK(!link_spi_master_start(&m, 3 + LINK_SPI_PROBE_US, true, false, true, false));
        CHECK(link_spi_master_start(&m, 4 + LINK_SPI_PROBE_US, true, false, true, false) && m.probes == 1);
        link_spi_master_done(&m, 5 + LINK_SPI_PROBE_US);
        CHECK(!link_spi_master_start(&m, 6 + LINK_SPI_PROBE_US, true, false, true, false));  /* one per period */
        CHECK(link_spi_master_start(&m, 5 + 2 * LINK_SPI_PROBE_US, true, false, true, false) && m.probes == 2);
    }
    /* PIO relocation rules (synthetic) */
    {
        const uint16_t src[] = { 0x2085 /* wait 1 gpio 5 */, 0x2002 /* wait 0 gpio 2 */, 0x20a0 /* wait 1 pin 0 */,
                                 0x2020 /* wait 0 pin 0 */, 0x20c0 /* wait 1 irq 0 */, 0x4001 /* in pins 1 */,
                                 0x2382 /* wait 1 gpio 2 [3] */, 0x209f /* wait 1 gpio 31 */ };
        uint16_t dst[8];
        CHECK(dc_pio_relocate(src, dst, 8, 7) == 4);
        CHECK(dst[0] == 0x208c && dst[1] == 0x2009 && dst[2] == 0x20a0 && dst[3] == 0x2020);
        CHECK(dst[4] == 0x20c0 && dst[5] == 0x4001 && dst[6] == 0x2389 && dst[7] == 0x2086);
        CHECK(dc_pio_relocate(src, dst, 8, 0) == 0 && memcmp(src, dst, sizeof src) == 0);
    }

    printf("timing simulation at %u Hz, %u-byte chunks:\n", (unsigned)LINK_SPI_HZ, (unsigned)LINK_SPI_CHUNK);
    const run_t runs[] = {
        { 3, 3000000,  200,  200, false, true,  "light traffic, ESP32 first" },
        { 3, 3000000,  200,  200, false, false, "light traffic, cable first" },
        { 1, 3000000, 2000, 2000, false, true,  "moderate, 1 buffer" },
        { 2, 3000000, 2000, 2000, false, false, "moderate, 2 buffers" },
        { 3, 3000000,    0,    0, false, true,  "both directions saturated" },
        { 3, 3000000,    0,   50, false, true,  "cable->ESP32 saturated" },
        { 3, 3000000,   50,    0, false, true,  "ESP32->cable saturated" },
        { 2, 3000000,    0,    0, false, false, "saturated, 2 buffers" },
    };
    for (size_t i = 0; i < sizeof runs / sizeof runs[0]; i++) {
        for (int seed = 0; seed < 3; seed++) { rs = 0x9E3779B97F4A7C15ull * (i * 7 + (size_t)seed + 1); run(&runs[i], true); }
    }
    printf("with resets of either side at random moments:\n");
    const run_t rruns[] = {
        { 3, 20000000, 2000, 2000, true, true,  "resets, moderate traffic" },
        { 3, 20000000,    0,    0, true, false, "resets, saturated" },
        { 2, 20000000,  300,  300, true, true,  "resets, light, 2 buffers" },
    };
    for (size_t i = 0; i < sizeof rruns / sizeof rruns[0]; i++)
        for (int seed = 0; seed < 2; seed++) { rs = 0xD1B54A32D192ED03ull * (i * 5 + (size_t)seed + 1); run(&rruns[i], false); }

    printf("OK: chunking, padding, master rules, PIO relocation rules; %d timing runs: never clocked an unready "
           "ESP32, lossless without resets, nothing corrupt/duplicated with resets\n", 24 + 6);
    return 0;
}
