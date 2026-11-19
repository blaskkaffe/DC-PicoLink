/*
 * The SD card in the cable's slot, for the ESP32 (LINK_CH_SD): the web page reads and
 * writes the card's files (saves to and from a computer over Wi-Fi). The ESP32 runs the
 * FAT file system; the cable only moves 512-byte blocks.
 *
 * One owner at a time: while the ESP32 has the card, the Dreamcast sees no card (two
 * writers on one FAT would corrupt it). The cable hands it over only when the Dreamcast has
 * not touched the card for SDLK_DC_QUIET_US, and takes it back for the Dreamcast when the
 * ESP32 gives it back or goes silent for SDLK_IDLE_US.
 *
 * ESP32 -> cable                              cable -> ESP32
 *   'T' on:u8 ask:u8   take (1) / give back (0)  'S' state:u8 ask:u8 blocks:u32 why:text
 *   'R' tag:u16 lba:u32 n:u8   read n blocks      'D' tag:u16 lba:u32 data[512]  (one per block)
 *   'W' tag:u16 lba:u32 data[512]  write         'E' tag:u16 lba:u32  (read/write failed)
 *   '?' ask:u8         state, please               'A' tag:u16 lba:u32  (written)
 * Integers little-endian. ask: the last 'T'/'?' number the cable has seen, so an answer to
 * this question is not mistaken for an older state message still on its way. state: bit0 a card is in the slot, bit1 the ESP32 has it,
 * bit2 the Dreamcast used it in the last SDLK_DC_QUIET_US, bit3 the Pi's dcsd serves it.
 * Host-compilable: tests/test_sdlink.c runs it, with FatFs on top, against a card image.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define SDLK_DC_QUIET_US   3000000u       /* the Dreamcast left the card alone this long */
#define SDLK_IDLE_US      60000000u       /* ESP32 silent this long: the card goes back */
#define SDLK_MAX_RUN       16u            /* blocks per 'R' */

enum { SDLK_CARD = 1, SDLK_TAKEN = 2, SDLK_DC_BUSY = 4, SDLK_PI = 8 };

static inline void sdlk_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint32_t sdlk_get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- cable side ----
 * The includer defines:
 *   SDLK_SEND(p, n)       queue one packet to the ESP32, false = no room (tried again later)
 *   SDLK_READ(lba, run, dst514) one block (run: more follow in order), false = failed
 *   SDLK_WRITE(lba, src512)     one block, done when it returns, false = failed
 *   SDLK_HAVE_CARD()      a card is in the slot and set up
 *   SDLK_BLOCKS()         its size in 512-byte blocks
 *   SDLK_PI()             the Pi's dcsd serves the Dreamcast's card
 *   SDLK_DC_LAST_US()     when the Dreamcast last used its SD card (time_us_32 clock)
 *   SDLK_TAKE() / SDLK_GIVE()  hand the slot card from / back to the Dreamcast
 */
typedef struct {
    bool taken, say_state;
    uint8_t ask;                              /* the last question's number */
    uint32_t last_rx;                         /* ESP32's last message (taken only) */
    const char *why;                          /* why a take was refused */
    /* the one request in work */
    uint8_t op; uint16_t tag; uint32_t lba; uint8_t left;
    uint8_t wbuf[512];
    uint32_t reads, writes, errors;
} sdlk_cable_t;

#ifdef SDLK_SEND
static inline uint8_t sdlk_state(sdlk_cable_t *z, uint32_t now) {
    uint8_t s = 0;
    if (SDLK_HAVE_CARD()) s |= SDLK_CARD;
    if (z->taken) s |= SDLK_TAKEN;
    if ((uint32_t)(now - SDLK_DC_LAST_US()) < SDLK_DC_QUIET_US) s |= SDLK_DC_BUSY;
    if (SDLK_PI()) s |= SDLK_PI;
    return s;
}

static inline void sdlk_give(sdlk_cable_t *z) {
    if (!z->taken) return;
    z->taken = false; z->op = 0;
    SDLK_GIVE();
}

/* a packet from the ESP32; false = busy with the last request (the link holds it) */
static inline bool sdlk_cable_rx(sdlk_cable_t *z, const uint8_t *p, size_t n, uint32_t now) {
    if (!n) return true;
    if (z->op) return false;
    if (z->taken) z->last_rx = now;
    switch (p[0]) {
    case 'T':
        if (n < 2) return true;
        if (n > 2) z->ask = p[2];
        if (p[1]) {
            uint8_t s = sdlk_state(z, now);
            z->why = NULL;
            if (z->taken) {}
            else if (s & SDLK_PI) z->why = "the Pi's dcsd serves the SD card";
            else if (!(s & SDLK_CARD)) z->why = "no card in the slot";
            else if (s & SDLK_DC_BUSY) z->why = "the Dreamcast is using the card";
            else { z->taken = true; z->last_rx = now; SDLK_TAKE(); }
        } else sdlk_give(z);
        z->say_state = true;
        return true;
    case '?': if (n > 1) z->ask = p[1]; z->say_state = true; return true;
    case 'R': case 'W':
        if (n < 7 || (p[0] == 'W' && n < 7 + 512)) return true;
        z->tag = (uint16_t)(p[1] | p[2] << 8);
        z->lba = sdlk_get32(p + 3);
        if (!z->taken) { z->op = 'E'; z->left = 1; return true; }   /* answered as failed */
        z->op = p[0];
        if (p[0] == 'R') { z->left = n > 7 ? p[7] : 1; if (!z->left || z->left > SDLK_MAX_RUN) z->left = 1; }
        else { memcpy(z->wbuf, p + 7, 512); z->left = 1; }
        return true;
    default:
        return true;
    }
}

/* call often: answers, one block at a time; gives the card back if the ESP32 went quiet */
static inline void sdlk_cable_poll(sdlk_cable_t *z, uint32_t now) {
    if (z->taken && !z->op && (uint32_t)(now - z->last_rx) > SDLK_IDLE_US) { sdlk_give(z); z->say_state = true; }
    if (z->taken && !SDLK_HAVE_CARD()) { sdlk_give(z); z->say_state = true; }
    if (z->say_state) {
        uint8_t m[80] = { 'S', sdlk_state(z, now), z->ask };
        sdlk_put32(m + 3, SDLK_HAVE_CARD() ? SDLK_BLOCKS() : 0);
        size_t k = 7;
        if (z->why) { size_t l = strlen(z->why); if (l > sizeof m - 7) l = sizeof m - 7; memcpy(m + 7, z->why, l); k += l; }
        if (SDLK_SEND(m, k)) z->say_state = false;
        return;
    }
    if (!z->op) return;
    static uint8_t m[7 + 514];
    m[1] = (uint8_t)z->tag; m[2] = (uint8_t)(z->tag >> 8);
    sdlk_put32(m + 3, z->lba);
    if (z->op == 'E') {
        m[0] = 'E';
        if (SDLK_SEND(m, 7)) z->op = 0;
        return;
    }
    if (z->op == 'R') {
        bool ok = z->taken && SDLK_READ(z->lba, z->left > 1, m + 7);
        m[0] = ok ? 'D' : 'E';
        if (!SDLK_SEND(m, ok ? 7 + 512 : 7)) return;   /* read again next time: no room */
        if (ok) z->reads++; else z->errors++;
        if (!ok || --z->left == 0) { z->op = 0; return; }
        z->lba++;
        return;
    }
    /* 'W': write once, then answer (kept until the answer is queued) */
    if (z->op == 'W') {
        bool ok = z->taken && SDLK_WRITE(z->lba, z->wbuf);
        if (ok) z->writes++; else z->errors++;
        z->op = ok ? 'a' : 'E';
    }
    if (z->op == 'a') { m[0] = 'A'; if (SDLK_SEND(m, 7)) z->op = 0; }
}
#endif

/* ---- ESP32 side: answers into the request it waits for ---- */
typedef struct {
    uint8_t state, ask; uint32_t blocks; char why[72]; uint32_t state_seq;
    /* the request waited for */
    uint16_t tag; uint32_t lba0; uint8_t n, got; uint8_t *dst;
    bool failed, written;
} sdlk_esp_t;

/* one packet from the cable; true = the waited-for request moved on (wake the waiter) */
static inline bool sdlk_esp_rx(sdlk_esp_t *e, const uint8_t *p, size_t n) {
    if (!n) return false;
    if (p[0] == 'S' && n >= 7) {
        e->state = p[1]; e->ask = p[2]; e->blocks = sdlk_get32(p + 3);
        size_t l = n - 7; if (l > sizeof e->why - 1) l = sizeof e->why - 1;
        memcpy(e->why, p + 7, l); e->why[l] = 0;
        e->state_seq++;
        return true;
    }
    if (n < 7) return false;
    uint16_t tag = (uint16_t)(p[1] | p[2] << 8);
    uint32_t lba = sdlk_get32(p + 3);
    if (tag != e->tag) return false;                 /* an old answer (timed out) */
    if (p[0] == 'E') { e->failed = true; return true; }
    if (p[0] == 'A') { e->written = true; return true; }
    if (p[0] == 'D' && n >= 7 + 512 && e->dst && e->got < e->n && lba == e->lba0 + e->got) {
        memcpy(e->dst + 512u * e->got, p + 7, 512);
        e->got++;
        return true;
    }
    return false;
}
