/*
 * The DC API (`dc1`) as the box answers it: the same small text language DC-DreamDash's api
 * module speaks on a DreamPi (docs/unified/, DC-DreamDash docs/dcapi.md), so openMenu and the
 * other devices of the family can talk to a DreamPi or to this box the same way:
 *
 *   POST /dcapi   dc1 / id N / to X / get <key> | get <prefix>* / set <key> <value> / call <action> <arg>
 *
 * Capabilities offered here: `api`, `net` (as on the Pi) and `wifi` (this box only: the network
 * search, the choice and the password, so a Dreamcast can set up the box's Wi-Fi):
 *
 *   get net.selected / net.call / net.dcnet      call net.select dcnow|dcnet
 *   get wifi.state / ssid / ip / up / error      call wifi.scan -> ok wifi.scan <n>
 *   get wifi.scan.n, wifi.scan.<i>               "<rssi> <1 open|0 locked> <name>"
 *   set wifi.pass <password>                     (write only, kept in RAM until the next join)
 *   call wifi.join <name | #<i of the scan>>     saves the network and connects (uses wifi.pass)
 *
 * Not kept: `get events` (answered "unknown"); `set` of the other keys answers "readonly" (known
 * keys) or "unknown", as on the Pi. The ESP32 side is esp32/main/dcapi.c. Host-compilable
 * (tests/test_dcapi.c).
 */
#ifndef DCAPI_MODEL_H
#define DCAPI_MODEL_H
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

typedef struct {
    int selected;            /* the network the user selected: 0 = DCNow!, 1 = DCNet */
    int call;                /* the network of the call in progress: -1 none, 0 DCNow!, 1 DCNet */
    int dcnet_ok;            /* DCNet can be used right now (the box is on the Internet) */
    const char *version;     /* hello.version */
    const char *wifi_state;  /* none | connecting | connected | failed */
    const char *wifi_error;  /* why it failed ("" when it did not) */
    const char *wifi_ssid;   /* the saved home network ("" none) */
    const char *wifi_ip;     /* its address while connected ("" otherwise) */
    int scan_n;              /* results of the last scan; -1 = no scan yet */
} dcapi_state_t;

typedef struct {
    int (*select)(void *ctx, int dcnet);                /* 1 done, 0 can't right now */
    int (*scan)(void *ctx);                             /* runs a scan (seconds): the number found, -1 busy, -2 failed */
    int (*scan_get)(void *ctx, int i, int *rssi, int *open, char *ssid, size_t cap);   /* result i of the last scan; 1 = there */
    int (*set_pass)(void *ctx, const char *pass);       /* keeps the password for the next join; 1 = ok */
    int (*join)(void *ctx, const char *ssid);           /* saves ssid + the kept password, connects; 1 = started */
} dcapi_ops_t;

typedef struct { char *o; size_t cap, n; } dcapi_out_t;

static inline void dcapi_put(dcapi_out_t *w, const char *fmt, ...) {
    if (w->n + 1 >= w->cap) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(w->o + w->n, w->cap - w->n, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    w->n += (size_t)k < w->cap - w->n ? (size_t)k : w->cap - w->n - 1;
}

/* a name on one line: control characters would split the answer */
static inline void dcapi_line_safe(char *s) { for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7F) *s = '?'; }

/* what a read matches: exact key, or every key with the prefix */
typedef struct { const char *name; size_t plen; int prefix; int any; } dcapi_q_t;

static inline void dcapi_emit(dcapi_out_t *w, dcapi_q_t *q, const char *key, const char *val) {
    if (q->prefix ? strncmp(key, q->name, q->plen) : strcmp(key, q->name)) return;
    dcapi_put(w, "ok %s %s\n", key, val);
    q->any = 1;
}

/* Every key this box can read, in the order a prefix read lists them (alphabetical; the scan
   results after wifi.scan.n). A key without a value right now is left out. */
static inline void dcapi_each(dcapi_out_t *w, dcapi_q_t *q, const dcapi_state_t *s, const dcapi_ops_t *ops, void *ctx, int hello_only) {
    char t[80];
    dcapi_emit(w, q, "hello.api", "1");
    dcapi_emit(w, q, "hello.caps", "api,net,wifi");
    dcapi_emit(w, q, "hello.kind", "gateway");
    dcapi_emit(w, q, "hello.name", "DC-PicoLink");
    dcapi_emit(w, q, "hello.version", s->version && *s->version ? s->version : "unknown");
    if (hello_only) return;
    if (s->call >= 0) dcapi_emit(w, q, "net.call", s->call == 1 ? "dcnet" : "dcnow");
    dcapi_emit(w, q, "net.dcnet", s->dcnet_ok ? "ok" : "off:offline");
    dcapi_emit(w, q, "net.selected", s->selected ? "dcnet" : "dcnow");
    if (s->wifi_error && *s->wifi_error) dcapi_emit(w, q, "wifi.error", s->wifi_error);
    if (s->wifi_ip && *s->wifi_ip) dcapi_emit(w, q, "wifi.ip", s->wifi_ip);
    if (s->scan_n >= 0) {
        snprintf(t, sizeof t, "%d", s->scan_n);
        dcapi_emit(w, q, "wifi.scan.n", t);
        for (int i = 0; i < s->scan_n && ops && ops->scan_get; i++) {
            int rssi = 0, open = 0; char name[34];
            if (!ops->scan_get(ctx, i, &rssi, &open, name, sizeof name)) break;
            dcapi_line_safe(name);
            char key[24];
            snprintf(key, sizeof key, "wifi.scan.%d", i);
            snprintf(t, sizeof t, "%d %d %s", rssi, open ? 1 : 0, name);
            dcapi_emit(w, q, key, t);
        }
    }
    if (s->wifi_ssid && *s->wifi_ssid) { snprintf(t, sizeof t, "%.32s", s->wifi_ssid); dcapi_line_safe(t); dcapi_emit(w, q, "wifi.ssid", t); }
    dcapi_emit(w, q, "wifi.state", s->wifi_state && *s->wifi_state ? s->wifi_state : "none");
    dcapi_emit(w, q, "wifi.up", s->wifi_state && !strcmp(s->wifi_state, "connected") ? "1" : "0");
}

static inline void dcapi_get(dcapi_out_t *w, const dcapi_state_t *s, const dcapi_ops_t *ops, void *ctx, const char *key) {
    if (!strcmp(key, "hello")) { dcapi_q_t q = { "hello.", 6, 1, 0 }; dcapi_each(w, &q, s, ops, ctx, 1); return; }
    if (!strcmp(key, "events")) { dcapi_put(w, "err events unknown\n"); return; }
    if (!strcmp(key, "wifi.pass")) { dcapi_put(w, "err wifi.pass writeonly\n"); return; }
    size_t kl = strlen(key);
    dcapi_q_t q = { key, kl, 0, 0 };
    if (kl && key[kl - 1] == '*') { q.prefix = 1; q.plen = kl - 1; }
    dcapi_each(w, &q, s, ops, ctx, 0);
    if (q.any) return;
    if (!strcmp(key, "net.call")) dcapi_put(w, "err net.call unavailable no call\n");
    else if (!strcmp(key, "wifi.ssid")) dcapi_put(w, "err wifi.ssid unavailable none saved\n");
    else if (!strcmp(key, "wifi.ip")) dcapi_put(w, "err wifi.ip unavailable not connected\n");
    else if (!strcmp(key, "wifi.error")) dcapi_put(w, "err wifi.error unavailable no error\n");
    else dcapi_put(w, "err %s unknown\n", key);
}

static inline int dcapi_known_key(const char *key) {
    static const char *const k[] = { "net.call", "net.dcnet", "net.selected", "wifi.error", "wifi.ip", "wifi.scan.n", "wifi.ssid", "wifi.state", "wifi.up" };
    if (!strncmp(key, "hello.", 6) || !strncmp(key, "wifi.scan.", 10)) return 1;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(k[i], key)) return 1;
    return 0;
}

static inline void dcapi_set(dcapi_out_t *w, const dcapi_ops_t *ops, void *ctx, const char *key, const char *value) {
    if (!strcmp(key, "wifi.pass")) {
        size_t n = strlen(value);
        if (n && (n < 8 || n > 63)) dcapi_put(w, "err wifi.pass range 8 to 63 characters (empty for an open network)\n");
        else if (ops && ops->set_pass && ops->set_pass(ctx, value)) dcapi_put(w, "ok wifi.pass set\n");
        else dcapi_put(w, "err wifi.pass unavailable\n");
        return;
    }
    dcapi_put(w, dcapi_known_key(key) ? "err %s readonly\n" : "err %s unknown\n", key);
}

static inline void dcapi_call(dcapi_out_t *w, const dcapi_ops_t *ops, void *ctx, const dcapi_state_t *s, const char *action, const char *arg) {
    while (*arg == ' ') arg++;
    if (!strcmp(action, "net.select")) {
        char net[12]; size_t m = 0;
        while (arg[m] && arg[m] != ' ' && m < sizeof net - 1) { net[m] = (char)tolower((unsigned char)arg[m]); m++; }
        net[m] = 0;
        if (strcmp(net, "dcnow") && strcmp(net, "dcnet")) { dcapi_put(w, "err net.select range dcnow or dcnet\n"); return; }
        if (ops && ops->select && ops->select(ctx, !strcmp(net, "dcnet"))) dcapi_put(w, "ok net.select %s\n", net);
        else dcapi_put(w, "err net.select unavailable\n");
    } else if (!strcmp(action, "wifi.scan")) {
        int n = ops && ops->scan ? ops->scan(ctx) : -2;
        if (n >= 0) dcapi_put(w, "ok wifi.scan %d\n", n);
        else if (n == -1) dcapi_put(w, "err wifi.scan unavailable busy\n");
        else dcapi_put(w, "err wifi.scan unavailable the scan did not run\n");
    } else if (!strcmp(action, "wifi.join")) {
        char name[34];
        if (*arg == '#') {
            int i = atoi(arg + 1), rssi, open;
            if (!isdigit((unsigned char)arg[1]) || s->scan_n < 0 || i < 0 || i >= s->scan_n || !ops || !ops->scan_get ||
                !ops->scan_get(ctx, i, &rssi, &open, name, sizeof name)) {
                dcapi_put(w, "err wifi.join range no scan result %s\n", arg + 1);
                return;
            }
        } else {
            size_t n = strlen(arg);
            if (!n || n > 32) { dcapi_put(w, "err wifi.join range name 1 to 32 characters or #<number of a scan result>\n"); return; }
            memcpy(name, arg, n + 1);
        }
        if (ops && ops->join && ops->join(ctx, name)) { dcapi_line_safe(name); dcapi_put(w, "ok wifi.join %s\n", name); }
        else dcapi_put(w, "err wifi.join unavailable\n");
    } else dcapi_put(w, "err %s unknown\n", action);
}

/* Answers one request. Returns the length of the answer (always starts with "dc1"). */
static inline size_t dcapi_handle(const char *req, size_t n, const dcapi_state_t *s, const dcapi_ops_t *ops, void *ctx,
                                  char *out, size_t cap) {
    dcapi_out_t w = { out, cap, 0 };
    if (!cap) return 0;
    out[0] = 0;
    /* split into trimmed, non-empty lines (at most 16, each at most 95 characters) */
    char line[16][96];
    int nl = 0;
    for (size_t i = 0; i < n && nl < 16;) {
        size_t j = i, k = 0;
        char t[96];
        while (j < n && req[j] != '\n') { if (req[j] != '\r' && k < sizeof t - 1) t[k++] = req[j]; j++; }
        i = j + 1;
        t[k] = 0;
        char *a = t; while (*a == ' ' || *a == '\t') a++;
        size_t l = strlen(a); while (l && (a[l - 1] == ' ' || a[l - 1] == '\t')) a[--l] = 0;
        if (!*a) continue;
        strcpy(line[nl++], a);
    }
    if (!nl || strcmp(line[0], "dc1")) {
        dcapi_put(&w, "dc1\nerr request bad first line must be dc1\n");
        return w.n;
    }
    char rid[12] = "", to[40] = "";
    for (int i = 1; i < nl; i++) {
        char *sp = strchr(line[i], ' ');
        const char *rest = sp ? sp + 1 : "";
        while (*rest == ' ') rest++;
        size_t wl = sp ? (size_t)(sp - line[i]) : strlen(line[i]);
        if (wl == 2 && !strncmp(line[i], "id", 2)) { size_t m = 0; while (rest[m] && rest[m] != ' ' && m < 10) { rid[m] = rest[m]; m++; } rid[m] = 0; }
        else if (wl == 2 && !strncmp(line[i], "to", 2)) snprintf(to, sizeof to, "%s", rest);
    }
    dcapi_put(&w, "dc1\n");
    if (rid[0]) dcapi_put(&w, "id %s\n", rid);
    if (to[0] && strcasecmp(to, "dreampi") && strcasecmp(to, "gateway") && strcasecmp(to, "dc-picolink") && strcasecmp(to, "dc-dreamdash")) {
        dcapi_put(&w, "err to unavailable no relay to other devices yet\n");
        return w.n;
    }
    dcapi_state_t cur = *s;
    for (int i = 1; i < nl; i++) {
        char *sp = strchr(line[i], ' ');
        char word[16];
        size_t wl = sp ? (size_t)(sp - line[i]) : strlen(line[i]);
        snprintf(word, sizeof word, "%.*s", (int)(wl < 15 ? wl : 15), line[i]);
        const char *rest = sp ? sp + 1 : "";
        while (*rest == ' ') rest++;
        if (!strcmp(word, "id") || !strcmp(word, "to")) continue;
        char key[48];
        const char *ks = strchr(rest, ' ');
        size_t kl = ks ? (size_t)(ks - rest) : strlen(rest);
        snprintf(key, sizeof key, "%.*s", (int)(kl < 47 ? kl : 47), rest);
        const char *arg = ks ? ks + 1 : "";
        if (!strcmp(word, "get")) dcapi_get(&w, &cur, ops, ctx, key);
        else if (!strcmp(word, "set")) dcapi_set(&w, ops, ctx, key, arg + strspn(arg, " "));
        else if (!strcmp(word, "call")) {
            dcapi_call(&w, ops, ctx, &cur, key, arg);
            if (!strcmp(key, "wifi.scan") && ops && ops->scan_get) {      /* the count of the scan just made */
                int c = 0, r, o; char nm[34];
                while (c < 40 && ops->scan_get(ctx, c, &r, &o, nm, sizeof nm)) c++;
                cur.scan_n = c;
            }
        } else dcapi_put(&w, "err %s unknown\n", word);
    }
    return w.n;
}
#endif
