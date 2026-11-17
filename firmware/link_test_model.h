/*
 * Link self-test: the cable sends numbered test packets on LINK_CH_TEST, the ESP32 sends
 * every one straight back, the cable checks each byte. It runs by itself whenever the link
 * comes up and ends in one status line, so the board-to-board wiring can be checked without
 * a Dreamcast. Host-compilable (tests/test_link_test.c).
 *
 * Packet i: 4 bytes i (little endian), then a pattern that depends on i; 8..1508 bytes.
 * At most LINK_TEST_WINDOW packets are on their way at a time, so the test never fills the
 * ESP32's queue and other traffic keeps flowing.
 */
#ifndef LINK_TEST_MODEL_H
#define LINK_TEST_MODEL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef LINK_TEST_N
#define LINK_TEST_N          2000u
#endif
#define LINK_TEST_WINDOW     6u
#define LINK_TEST_TIMEOUT_US 2000000u

enum { LINK_TEST_IDLE = 0, LINK_TEST_RUN, LINK_TEST_DONE };

typedef struct {
    uint8_t state;
    uint32_t sent, next_rx;          /* packets queued; the next number expected back */
    uint32_t good, bad, lost;
    uint32_t bytes;                  /* payload bytes that came back right */
    uint32_t t_start, t_last, t_end;
} link_test_t;

static inline size_t link_test_len(uint32_t i) { return 8u + (size_t)((i * 37u) % 1501u); }

static inline void link_test_fill(uint32_t i, uint8_t *p, size_t n) {
    uint32_t x = i * 2654435761u + 1u;
    p[0] = (uint8_t)i; p[1] = (uint8_t)(i >> 8); p[2] = (uint8_t)(i >> 16); p[3] = (uint8_t)(i >> 24);
    for (size_t k = 4; k < n; k++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; p[k] = (uint8_t)x; }
}

static inline void link_test_start(link_test_t *t, uint32_t now) {
    t->state = LINK_TEST_RUN;
    t->sent = t->next_rx = t->good = t->bad = t->lost = t->bytes = 0;
    t->t_start = t->t_last = now; t->t_end = now;
}

/* The next packet to queue: its length (0 = nothing to send now); payload in buf. Call
   link_test_queued() once it is really in the queue. */
static inline size_t link_test_next(const link_test_t *t, uint8_t *buf) {
    if (t->state != LINK_TEST_RUN || t->sent >= LINK_TEST_N || t->sent - t->next_rx >= LINK_TEST_WINDOW) return 0;
    size_t n = link_test_len(t->sent);
    link_test_fill(t->sent, buf, n);
    return n;
}
static inline void link_test_queued(link_test_t *t) { t->sent++; }

/* A packet came back. */
static inline void link_test_rx(link_test_t *t, const uint8_t *p, size_t n, uint32_t now) {
    if (t->state != LINK_TEST_RUN) return;
    t->t_last = now;
    if (n < 8) { t->bad++; return; }
    uint32_t i = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    if (i < t->next_rx || i >= t->sent) { t->bad++; return; }      /* twice, or never sent */
    t->lost += i - t->next_rx;
    t->next_rx = i + 1;
    bool ok = n == link_test_len(i);
    if (ok) {
        uint32_t x = i * 2654435761u + 1u;
        for (size_t k = 4; k < n; k++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; if (p[k] != (uint8_t)x) { ok = false; break; } }
    }
    if (ok) { t->good++; t->bytes += (uint32_t)n; } else t->bad++;
    if (t->next_rx >= LINK_TEST_N) { t->state = LINK_TEST_DONE; t->t_end = now; }
}

/* Call regularly. true once, when the test has just ended (all back, or nothing for 2 s).
   `now` may be a little older than the last answer's time (taken earlier in the same pass):
   the difference is signed, so that is not a 4295-second timeout. */
static inline bool link_test_poll(link_test_t *t, uint32_t now) {
    if (t->state != LINK_TEST_RUN) return false;
    if (t->next_rx >= LINK_TEST_N) { t->state = LINK_TEST_DONE; t->t_end = now; return true; }
    if ((int32_t)(now - t->t_last) > (int32_t)LINK_TEST_TIMEOUT_US) {
        t->lost += t->sent - t->next_rx;
        t->state = LINK_TEST_DONE; t->t_end = t->t_last;
        return true;
    }
    return false;
}

static inline bool link_test_passed(const link_test_t *t) {
    return t->state == LINK_TEST_DONE && t->good == LINK_TEST_N && !t->bad && !t->lost;
}
static inline uint32_t link_test_kb_per_s(const link_test_t *t) {
    uint32_t us = t->t_end - t->t_start;
    return us ? (uint32_t)((uint64_t)t->bytes * 1000000u / us / 1024u) : 0;
}
#endif
