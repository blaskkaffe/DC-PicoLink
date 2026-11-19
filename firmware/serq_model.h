/*
 * The way to the Dreamcast over the serial line (ESP32: ppp_srv.c; cable: main.c).
 *
 * The line is the slowest part (115200 baud = 11.5 KB/s). Whatever waits in front of it
 * waits in order, so a game update that arrives behind a 4 KB download waits ~0.35 s. So:
 *  - the cable keeps only ~20 ms of bytes (serq_hold), the ESP32 keeps the rest as whole
 *    IP packets in two queues and gives the line one packet at a time (serq_feed_ok);
 *  - "fast" (UDP, ICMP: game updates, DNS, ping) goes before "bulk" (TCP). TCP stays in one
 *    queue so its segments never overtake each other (old Dreamcast stacks dislike that);
 *  - full queues drop: fast drops its oldest (a game wants the newest update), bulk its
 *    newest (TCP sends it again), as a modem's full buffer would.
 * The Dreamcast connects exactly as before; only the order and the waiting change.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "link_model.h"

#define SERQ_FEED_BELOW  1u       /* ESP32: next packet only when nothing of ours waits for the cable (the cable still has ~20 ms) */
#define SERQ_PIECE       256u     /* ESP32 -> cable link packets for the line: the cable holds at most hold + 2 pieces */
#define SERQ_FAST_MAX    3072u    /* bytes in the fast queue (~0.27 s at 115200) */
#define SERQ_BULK_MAX    8192u    /* bytes in the bulk queue (TCP windows of Dreamcast stacks fit) */
#define SERQ_PKTS        32u      /* packets per queue */

/* cable: bytes it may hold for the line, ~20 ms at the wire rate (64 .. 2048) */
static inline uint32_t serq_hold(float wire_baud) {
    uint32_t b = (uint32_t)(wire_baud / 500.0f);          /* baud/10 bytes per s, 20 ms */
    if (b < 64u) b = 64u;
    if (b > 2048u) b = 2048u;
    return b;
}

static inline bool serq_feed_ok(uint32_t backlog) { return backlog < SERQ_FEED_BELOW; }

/* an IPv4 packet: UDP / ICMP go first; TCP and anything unreadable is bulk */
static inline bool serq_is_fast(const uint8_t *ip, size_t len) {
    if (len < 20 || (ip[0] >> 4) != 4) return false;
    return ip[9] == 17 || ip[9] == 1;
}

/* ESP32 builds without the SPI link (UART cable): no queue, take a packet only if the link
   has room for all of it (never wait inside lwIP's thread). PPP/HDLC adds a few bytes and
   escapes some (1/8 allowed); each 1 KB piece needs LINK_MAX_WIRE of room when queued. */
static inline bool serq_admit(uint32_t room, uint32_t ip_len) {
    return room >= ip_len + ip_len / 8u + 16u + LINK_MAX_WIRE;
}

/* the queues: a ring of packet handles each (void *: pbufs on the ESP32, anything in tests) */
typedef struct { void *p[SERQ_PKTS]; uint32_t len[SERQ_PKTS]; uint8_t rd, wr; uint32_t bytes; } serq_q_t;
typedef struct { serq_q_t fast, bulk; uint32_t dropped; } serq_t;

static inline bool serq_q_empty(const serq_q_t *q) { return q->rd == q->wr; }
static inline void *serq_q_pop(serq_q_t *q, uint32_t *len) {
    if (serq_q_empty(q)) return NULL;
    uint8_t i = (uint8_t)(q->rd++ % SERQ_PKTS);
    q->bytes -= q->len[i];
    if (len) *len = q->len[i];
    return q->p[i];
}
static inline void serq_q_push(serq_q_t *q, void *p, uint32_t len) {
    uint8_t i = (uint8_t)(q->wr++ % SERQ_PKTS);
    q->p[i] = p; q->len[i] = len; q->bytes += len;
}

/* put a packet in; whatever is dropped (the new packet, or the oldest fast ones) goes to drop() */
static inline void serq_put(serq_t *s, void *p, uint32_t len, bool fast, void (*drop)(void *)) {
    serq_q_t *q = fast ? &s->fast : &s->bulk;
    uint32_t max = fast ? SERQ_FAST_MAX : SERQ_BULK_MAX;
    if (len > max) { s->dropped++; drop(p); return; }
    if (fast)
        while ((uint8_t)(q->wr - q->rd) >= SERQ_PKTS || q->bytes + len > max) { drop(serq_q_pop(q, NULL)); s->dropped++; }
    else if ((uint8_t)(q->wr - q->rd) >= SERQ_PKTS || q->bytes + len > max) { s->dropped++; drop(p); return; }
    serq_q_push(q, p, len);
}
/* everything out (end of a call) */
static inline void serq_clear(serq_t *s, void (*drop)(void *)) {
    void *p;
    while ((p = serq_q_pop(&s->fast, NULL))) drop(p);
    while ((p = serq_q_pop(&s->bulk, NULL))) drop(p);
}
/* fast first */
static inline void *serq_next(serq_t *s, uint32_t *len) {
    void *p = serq_q_pop(&s->fast, len);
    return p ? p : serq_q_pop(&s->bulk, len);
}
static inline bool serq_empty(const serq_t *s) { return serq_q_empty(&s->fast) && serq_q_empty(&s->bulk); }
