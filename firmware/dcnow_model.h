/*
 * DCNow! reporting rules (host-compilable). Same as DreamPi's dcnow.py (version 202609091030):
 * every 15 seconds one POST to /api/update/<sha256 of the MAC>/ with dns_query = sha256 of the
 * newest A-query domain. DreamPi reads the last 15 lines of dnsmasq's log; here a short list of
 * the last domains stands in for it. Domains shared by several games are reported once per
 * session, later polls send an empty update that keeps the session alive. Our own "appspot"
 * lookups are skipped.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define DCNOW_RING   5
#define DCNOW_NAME   128
#define DCNOW_HOST   "dcnow-2016.appspot.com"
#define DCNOW_PATH   "/api/update/%s/"
#define DCNOW_UA     "Mozilla/4.0 (compatible; MSIE 5.5; Windows NT), Dreamcast Now"
#define DCNOW_EVERY_MS 15000

typedef struct {
    char ring[DCNOW_RING][DCNOW_NAME];       /* newest at (head-1) */
    uint8_t head, count;
    bool sent_gameloft, sent_onsen;
} dcnow_t;

static inline void dcnow_reset(dcnow_t *d) { memset(d, 0, sizeof *d); }

/* an A-query went by */
static inline void dcnow_note(dcnow_t *d, const char *domain) {
    size_t n = strlen(domain);
    if (n >= DCNOW_NAME) return;
    memcpy(d->ring[d->head], domain, n + 1);
    d->head = (uint8_t)((d->head + 1) % DCNOW_RING);
    if (d->count < DCNOW_RING) d->count++;
}

/* The domain to hash for this poll, or false for an empty update. */
static inline bool dcnow_pick(dcnow_t *d, char *out) {
    for (int i = 1; i <= d->count; i++) {
        const char *dom = d->ring[(d->head + DCNOW_RING - i) % DCNOW_RING];
        bool gl = strstr(dom, "gameloft") != NULL;
        bool on = strstr(dom, "onsen0.overworks.isao.net") != NULL;
        if (gl || on) {
            bool *sent = gl ? &d->sent_gameloft : &d->sent_onsen;
            if (*sent) return false;
            *sent = true;
            strcpy(out, dom);
            return true;
        }
        if (strstr(dom, "appspot")) continue;        /* skip, look further back */
        strcpy(out, dom);
        return true;
    }
    return false;
}
