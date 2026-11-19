/*
 * Packets to the Dreamcast over the serial line (ESP32 side, ppp_srv.c): take one only if
 * the link to the cable has room for all of it. Without this, a full queue made the PPP output
 * wait inside lwIP's own thread (every network on the box stalled at the serial line's pace)
 * and after a second gave up halfway through a frame. Dropping a whole IP packet instead is
 * what a modem's full buffer does; TCP sends it again, a game's next UDP packet replaces it.
 * Small packets (game updates, ACKs) get in while big ones are already refused, so they wait
 * behind less.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "link_model.h"

/* room: free bytes in the link's queue to the cable. ip_len: the packet. PPP/HDLC adds a few
   bytes and escapes some (~1 % on real traffic; 1/8 allowed), and the link sends it in 1 KB
   pieces, each needing LINK_MAX_WIRE of room when it is queued. */
static inline bool serq_admit(uint32_t room, uint32_t ip_len) {
    uint32_t need = ip_len + ip_len / 8u + 16u + LINK_MAX_WIRE;
    return room >= need;
}
