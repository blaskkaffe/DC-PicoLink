/*
 * The cable's answers to dc1 messages on the ESP32 link (host-compilable).
 * Language: 04-control-channel.md (docs repo). Keys this cable knows:
 *
 *   get hello                 -> hello.kind cable, .name, .version, .api, .caps
 *   get link.mode             -> serial | spi | offline       (what the DC is using now)
 *   get link.serial.baud      -> the host-side rate last set (0 = none yet)
 *   get link.serial.wire      -> the rate on the wire (may follow the DC: auto-baud)
 *   set link.serial.baud N    -> like a USB host changing the line coding
 *   get link.force / set link.force auto|serial|spi|w5500|sd
 *   get link.route            -> where serial / network traffic goes: "usb" or "esp32"
 *   get link.*                -> all of the above
 *
 * Unknown keys answer "err <key> unknown"; a message without the "dc1" first line
 * gets no answer (rule of the language).
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include "link_model.h"

typedef struct {
    /* read by the handler */
    const char *version;
    int mode;              /* 0 serial, 1 spi, 2 offline (dcmode.h order) */
    uint32_t baud, wire;
    uint8_t force;         /* 0 auto .. 4 sd (FORCE_* order) */
    const char *route_ser, *route_eth;
    /* written by the handler (the caller applies them) */
    uint32_t set_baud;     /* 0 = no change asked */
    uint8_t set_force;     /* 0xFF = no change asked */
} cable_api_t;

static const char *const cable_force_names[] = { "auto", "serial", "spi", "w5500", "sd" };
static const char *const cable_mode_names[] = { "serial", "spi", "offline" };

typedef struct { char *p; size_t cap, n; } cable_out_t;

static inline void cable_put(cable_out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static inline void cable_put(cable_out_t *o, const char *fmt, ...) {
    if (o->n >= o->cap) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(o->p + o->n, o->cap - o->n, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    o->n += (size_t)k < o->cap - o->n ? (size_t)k : o->cap - o->n - 1;
}

static inline void cable_get(cable_api_t *s, cable_out_t *o, const char *k, size_t kl) {
#define KEY(x) (kl == sizeof(x) - 1 && memcmp(k, x, kl) == 0)
    bool all = KEY("link.*");
    if (KEY("hello")) {
        cable_put(o, "ok hello.kind cable\nok hello.name DC-PicoLink\nok hello.version %s\nok hello.api 1\nok hello.caps api,link\n", s->version);
        return;
    }
    bool any = false;
    if (all || KEY("link.mode")) { cable_put(o, "ok link.mode %s\n", cable_mode_names[s->mode < 3 ? s->mode : 2]); any = true; }
    if (all || KEY("link.serial.baud")) { cable_put(o, "ok link.serial.baud %lu\n", (unsigned long)s->baud); any = true; }
    if (all || KEY("link.serial.wire")) { cable_put(o, "ok link.serial.wire %lu\n", (unsigned long)s->wire); any = true; }
    if (all || KEY("link.force")) { cable_put(o, "ok link.force %s\n", cable_force_names[s->force < 5 ? s->force : 0]); any = true; }
    if (all || KEY("link.route")) { cable_put(o, "ok link.route serial=%s network=%s\n", s->route_ser, s->route_eth); any = true; }
    if (!any) cable_put(o, "err %.*s unknown\n", (int)kl, k);
#undef KEY
}

static inline void cable_set(cable_api_t *s, cable_out_t *o, const char *a, size_t al) {
    const char *sp = memchr(a, ' ', al);
    if (!sp) { cable_put(o, "err %.*s bad\n", (int)al, a); return; }
    size_t kl = (size_t)(sp - a);
    const char *v = sp + 1; size_t vl = al - kl - 1;
    char val[24];
    if (vl >= sizeof val) { cable_put(o, "err %.*s range\n", (int)kl, a); return; }
    memcpy(val, v, vl); val[vl] = 0;
    if (kl == 16 && memcmp(a, "link.serial.baud", 16) == 0) {
        char *e; unsigned long b = strtoul(val, &e, 10);
        if (*e || b < 300 || b > 1562500) { cable_put(o, "err link.serial.baud range\n"); return; }
        s->set_baud = (uint32_t)b; s->baud = (uint32_t)b;
        cable_put(o, "ok link.serial.baud %lu\n", b);
    } else if (kl == 10 && memcmp(a, "link.force", 10) == 0) {
        for (uint8_t i = 0; i < 5; i++)
            if (strcmp(val, cable_force_names[i]) == 0) {
                s->set_force = i; s->force = i;
                cable_put(o, "ok link.force %s\n", val);
                return;
            }
        cable_put(o, "err link.force range\n");
    } else if ((kl == 9 && memcmp(a, "link.mode", 9) == 0) || (kl == 16 && memcmp(a, "link.serial.wire", 16) == 0) ||
               (kl == 10 && memcmp(a, "link.route", 10) == 0)) {
        cable_put(o, "err %.*s readonly\n", (int)kl, a);
    } else {
        cable_put(o, "err %.*s unknown\n", (int)kl, a);
    }
}

/* Answer one dc1 message. Returns the length of the answer in out (0 = no answer). */
static inline size_t cable_api_handle(cable_api_t *s, const char *msg, size_t n, char *out, size_t cap) {
    cable_out_t o = { out, cap, 0 };
    const char *p = msg, *end = msg + n, *line; size_t len;
    s->set_baud = 0; s->set_force = 0xFF;
    if (!link_next_line(&p, end, &line, &len) || !(len == 3 && memcmp(line, "dc1", 3) == 0)) return 0;
    cable_put(&o, "dc1\n");
    while (link_next_line(&p, end, &line, &len)) {
        const char *rest;
        if (len == 0) continue;
        if (link_word(line, len, "id", &rest)) cable_put(&o, "%.*s\n", (int)len, line);
        else if (link_word(line, len, "get", &rest)) cable_get(s, &o, rest, len - (size_t)(rest - line));
        else if (link_word(line, len, "set", &rest)) cable_set(s, &o, rest, len - (size_t)(rest - line));
        else if (link_word(line, len, "call", &rest)) {
            const char *sp = memchr(rest, ' ', len - (size_t)(rest - line));
            size_t kl = sp ? (size_t)(sp - rest) : len - (size_t)(rest - line);
            cable_put(&o, "err %.*s unknown\n", (int)kl, rest);
        }
        /* other line types (ok/err/ev from a peer, later additions): ignored */
    }
    return o.n;
}
