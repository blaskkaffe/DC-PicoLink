/*
 * The Wi-Fi search list on the page: what a scan found, cleaned for the page. One entry per
 * network name (the strongest signal), no hidden networks (empty name) and no names that are
 * not valid UTF-8 (the page could not give them back to the box unchanged), strongest first,
 * at most WSCAN_MAX. The ESP32 side is esp32/main/wifi.c. Host-compilable (tests/test_wifiscan.c).
 */
#ifndef WIFISCAN_MODEL_H
#define WIFISCAN_MODEL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define WSCAN_MAX 24

typedef struct { char ssid[33]; int8_t rssi; uint8_t open; } wscan_ap_t;
typedef struct { wscan_ap_t ap[WSCAN_MAX]; int n; } wscan_t;

static inline bool wscan_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int k = *p < 0x80 ? 0 : (*p >> 5) == 6 ? 1 : (*p >> 4) == 14 ? 2 : (*p >> 3) == 30 ? 3 : -1;
        if (k < 0 || (k == 1 && *p < 0xC2)) return false;
        p++;
        while (k--) { if ((*p & 0xC0) != 0x80) return false; p++; }
    }
    return true;
}

/* `ssid` is not necessarily terminated: n bytes */
static inline void wscan_add(wscan_t *w, const char *ssid, size_t n, int rssi, bool open) {
    if (!n || n > 32 || !ssid[0] || memchr(ssid, 0, n)) return;      /* hidden, too long, or a NUL inside */
    wscan_ap_t a;
    memcpy(a.ssid, ssid, n); a.ssid[n] = 0;
    a.rssi = (int8_t)(rssi < -127 ? -127 : rssi > 0 ? 0 : rssi);
    a.open = open;
    if (!wscan_utf8(a.ssid)) return;
    for (int i = 0; i < w->n; i++) {
        if (strcmp(w->ap[i].ssid, a.ssid)) continue;
        if (a.rssi <= w->ap[i].rssi) return;                       /* the same name, weaker: ignore */
        memmove(&w->ap[i], &w->ap[i + 1], (size_t)(w->n - i - 1) * sizeof a);
        w->n--;
        break;
    }
    int at = w->n;
    while (at > 0 && w->ap[at - 1].rssi < a.rssi) at--;
    if (at >= WSCAN_MAX) return;                                     /* weaker than all of a full list */
    if (w->n == WSCAN_MAX) w->n--;
    memmove(&w->ap[at + 1], &w->ap[at], (size_t)(w->n - at) * sizeof a);
    w->ap[at] = a;
    w->n++;
}

/* {"ok":true,"aps":[{"s":"name","r":-55,"o":0},...]} ; returns the length */
static inline size_t wscan_json(const wscan_t *w, char *out, size_t cap) {
    size_t o = 0;
#define WPUT(c) do { if (o + 1 < cap) out[o] = (char)(c); o++; } while (0)
#define WSTR(s) do { for (const char *q_ = (s); *q_; q_++) WPUT(*q_); } while (0)
    WSTR("{\"ok\":true,\"aps\":[");
    for (int i = 0; i < w->n; i++) {
        char t[24];
        if (i) WPUT(',');
        WSTR("{\"s\":\"");
        for (const unsigned char *p = (const unsigned char *)w->ap[i].ssid; *p; p++) {
            if (*p == '"' || *p == '\\') { WPUT('\\'); WPUT(*p); }
            else if (*p < 0x20 || *p == 0x7F) { snprintf(t, sizeof t, "\\u%04x", *p); WSTR(t); }
            else WPUT(*p);
        }
        snprintf(t, sizeof t, "\",\"r\":%d,\"o\":%d}", w->ap[i].rssi, w->ap[i].open ? 1 : 0);
        WSTR(t);
    }
    WSTR("]}");
#undef WPUT
#undef WSTR
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return o < cap ? o : cap - 1;
}

/* Why a connection failed, for people (ESP-IDF's disconnect reasons that matter at set-up) */
static inline const char *wscan_reason_text(int r) {
    switch (r) {
    case 201: return "network not found";
    case 15: case 202: case 204: return "wrong password?";
    case 2: case 203: return "the router refused the connection";
    case 200: case 205: return "no stable signal";
    case 0: return "";
    default: return "could not connect";
    }
}
#endif
