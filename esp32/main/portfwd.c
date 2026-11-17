/*
 * Port forwarding for peer-to-peer games, as DreamPi's port_forwarding.py:
 *   on the home router, by UPnP: first each mapping deleted (delete_all), then DreamPi's game
 *   ports, each to the same port of the Dreamcast's own home address (dclan.c, usually .98;
 *   forward_all). Nothing else is mapped: not this box, not the Netlink ports (DreamPi does not
 *   map those either; mappings that earlier firmware made for them are removed).
 *   Without a home address for the Dreamcast (dclan.c could not pick one) the ports go to this box
 *   and lwIP's NAT passes them on to the Dreamcast, as before.
 * Switch on the page (on by default). The router needs UPnP switched on.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/lwip_napt.h"
#include "app.h"

/* port_forwarding.py PORTS (the 2300-2400 range is TCP and UDP) */
static const struct { uint16_t lo, hi; uint8_t proto; const char *game; } PORTS[] = {
    { 1028, 1028, 17, "Planet Ring" }, { 1285, 1285, 17, "Planet Ring" },
    { 3512, 3512, 6, "The Next Tetris" }, { 3512, 3512, 17, "The Next Tetris" },
    { 6001, 6001, 17, "Ooga Booga" }, { 6500, 6500, 17, "PBA Bowling / Starlancer" },
    { 7648, 7648, 17, "Planet Ring" }, { 7980, 7980, 17, "Alien Front Online" },
    { 9789, 9789, 17, "ChuChu Rocket!" }, { 13139, 13139, 17, "PBA Bowling" },
    { 13713, 13713, 17, "World Series Baseball 2K2" }, { 17219, 17219, 6, "Worms World Party" },
    { 37171, 37171, 17, "World Series Baseball 2K2" }, { 47624, 47624, 6, "PBA Bowling / Starlancer" },
    { 2300, 2400, 6, "PBA Bowling / Starlancer" }, { 2300, 2400, 17, "PBA Bowling / Starlancer" },
};
/* earlier firmware also mapped these to this box (Netlink); DreamPi never does: they are only removed now */
static const struct { uint16_t port; uint8_t proto; } OLD_OWN[] = { { 65432, 6 }, { 65433, 6 }, { 20001, 17 }, { 20002, 17 } };

static volatile bool enabled = true;
static volatile bool kick;
static uint32_t map_dc, map_me;                 /* what the NAT maps point at now (network order) */
static struct { int upnp_ok, upnp_fail, maps; char router[48], err[112], dc[16], outside[16]; int shared; } st;   /* shared: 1 = provider's NAT, 2 = another NAT in front */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char t[160];
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (k > (int)sizeof t - 1) k = sizeof t - 1;
    log_add("esp32", t, (size_t)k);
}

bool portfwd_enabled(void) { return enabled; }
void portfwd_set_enabled(bool on) {
    enabled = on; kick = true;
    nvs_handle_t h;
    if (nvs_open("dcl", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "portfwd", on); nvs_commit(h); nvs_close(h); }
}

static uint32_t my_ip(void) {                    /* the uplink's address (network order) */
    esp_netif_t *n = esp_netif_get_default_netif();
    esp_netif_ip_info_t i;
    if (!n || esp_netif_get_ip_info(n, &i) != ESP_OK) return 0;
    return i.ip.addr;
}

static uint32_t dc_ip(void) {
    uint32_t a = ppp_dc_ip();
    if (!a) a = dc_eth_dc_ip();
    if (!a) a = wired_dc_ip();
    return a;
}

/* ---- the NAT maps ---- */
static void nat_maps(uint32_t me, uint32_t dc) {
    if (me == map_me && dc == map_dc) return;
    int n = 0;
    LOCK_TCPIP_CORE();
    for (size_t i = 0; i < sizeof PORTS / sizeof PORTS[0]; i++)                     /* remove the old ones */
        for (uint32_t p = PORTS[i].lo; p <= PORTS[i].hi; p++) ip_portmap_remove(PORTS[i].proto, (uint16_t)p);
    if (me && dc && enabled)
        for (size_t i = 0; i < sizeof PORTS / sizeof PORTS[0]; i++)
            for (uint32_t p = PORTS[i].lo; p <= PORTS[i].hi; p++) n += ip_portmap_add(PORTS[i].proto, me, (uint16_t)p, dc, (uint16_t)p);
    UNLOCK_TCPIP_CORE();
    map_me = me; map_dc = dc; st.maps = n;
    if (dc && enabled) {
        snprintf(st.dc, sizeof st.dc, "%s", inet_ntoa(*(struct in_addr *)&dc));
        say("port forwarding: %d game ports go on to the Dreamcast at %s", n, st.dc);
    } else st.dc[0] = 0;
}

/* ---- UPnP (IGD) ---- */
/* as_home: the request goes out from the Dreamcast's home address (dclan.c), for routers in
   UPnP "secure mode" that only map ports to the address that asks */
static bool as_home;
static int http_req(uint32_t ip, uint16_t port, const char *req, size_t rn, char *ans, size_t cap) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    bool marked = false;
    if (as_home) {
        struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = INADDR_ANY };
        socklen_t ml = sizeof me;
        if (bind(s, (struct sockaddr *)&me, sizeof me) == 0 && getsockname(s, (struct sockaddr *)&me, &ml) == 0 && me.sin_port) {
            dclan_as_home(ntohs(me.sin_port), ip);
            marked = true;
        }
    }
    struct timeval tv = { .tv_sec = 4 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip };
    int got = -1;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0 && send(s, req, rn, 0) == (int)rn) {
        got = 0;
        for (;;) { int k = recv(s, ans + got, cap - 1 - (size_t)got, 0); if (k <= 0) break; got += k; if ((size_t)got >= cap - 1) break; }
        ans[got] = 0;
    }
    close(s);
    if (marked) dclan_as_home_end();
    return got;
}

static bool parse_url(const char *u, uint32_t *ip, uint16_t *port, char *path, size_t pc) {
    if (strncmp(u, "http://", 7)) return false;
    u += 7;
    char host[48]; size_t i = 0;
    while (*u && *u != ':' && *u != '/' && i + 1 < sizeof host) host[i++] = *u++;
    host[i] = 0;
    *port = 80;
    if (*u == ':') { *port = (uint16_t)atoi(u + 1); while (*u && *u != '/') u++; }
    snprintf(path, pc, "%s", *u ? u : "/");
    return inet_aton(host, (struct in_addr *)ip);
}

/* the router's description: the control URL of WANIPConnection (or WANPPPConnection), read
   in pieces so a large description needs little RAM */
static bool find_control(uint32_t ip, uint16_t port, const char *path, char *ctl, size_t cc, char *svc, size_t sc) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    struct timeval tv = { .tv_sec = 4 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip };
    char req[200];
    int rn = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s:%u\r\nConnection: close\r\n\r\n", path, inet_ntoa(a.sin_addr), (unsigned)port);
    bool ok = false;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0 && send(s, req, (size_t)rn, 0) == rn) {
        char *w = malloc(2049);                  /* window: the last 1 KB carried over + 1 KB new */
        if (!w) { close(s); return false; }
        size_t wn = 0; int found = 0;            /* 1 = in a WAN*Connection service, look for its controlURL */
        for (;;) {
            int k = recv(s, w + wn, 2048 - wn, 0);
            if (k <= 0) break;
            wn += (size_t)k; w[wn] = 0;
            static const char *const T[] = { "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANPPPConnection:1" };
            char *at = NULL;
            if (!found) for (int i = 0; i < 3 && !at; i++) if ((at = strstr(w, T[i]))) { snprintf(svc, sc, "%s", T[i]); found = 1; }
            if (at) { wn -= (size_t)(at - w); memmove(w, at, wn + 1); }   /* the window now starts at our service */
            if (found) {
                char *c = strstr(w, "<controlURL>");
                char *e = c ? strstr(c, "</controlURL>") : NULL;
                if (c && e) { c += 12; size_t l = (size_t)(e - c); if (l >= cc) l = cc - 1; memcpy(ctl, c, l); ctl[l] = 0; ok = true; break; }
            }
            if (wn > 1024) { memmove(w, w + wn - 1024, 1024); wn = 1024; w[wn] = 0; }   /* keep the tail */
        }
        free(w);
    }
    close(s);
    return ok;
}

/* every UPnP device that answers (up to SSDP_MAX), the gateway's own first: like miniupnpc's
   selectigd(), the first one that really offers port mapping is used (a TV or NAS may answer first) */
#define SSDP_MAX 4
static int ssdp(char loc[][160], uint32_t gw) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(1900) };
    inet_aton("239.255.255.250", &to.sin_addr);
    /* what miniupnpc asks for too: some routers answer only one of these */
    static const char *const ST[] = { "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                      "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANPPPConnection:1", "upnp:rootdevice" };
    int got = 0;
    bool have_gw = false;
    for (int round = 0; round < 3 && !have_gw; round++) {          /* again while the gateway itself has not answered */
        for (int i = 0; i < (int)(sizeof ST / sizeof ST[0]); i++) {
            char q[220];
            int n = snprintf(q, sizeof q, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", ST[i]);
            sendto(s, q, (size_t)n, 0, (struct sockaddr *)&to, sizeof to);
        }
        int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 3000000 && got < SSDP_MAX) {
            char b[700];
            int n = recv(s, b, sizeof b - 1, 0);
            if (n <= 0) continue;
            b[n] = 0;
            char *l = NULL;
            for (char *p = b; *p && !l; p++) if (!strncasecmp(p, "\nlocation:", 10)) l = p;
            if (!l) continue;
            l += 10; while (*l == ' ') l++;
            size_t k = strcspn(l, "\r\n"); if (k >= 160) k = 159;
            char one[160]; memcpy(one, l, k); one[k] = 0;
            if (strncmp(one, "http://", 7)) continue;
            bool dup = false;
            for (int j = 0; j < got; j++) if (!strcmp(loc[j], one)) dup = true;
            if (dup) continue;
            /* the gateway's own address goes first */
            char gws[16]; snprintf(gws, sizeof gws, "%s", inet_ntoa(*(struct in_addr *)&gw));
            size_t gl = strlen(gws);
            bool is_gw = gw && !strncmp(one + 7, gws, gl) && (one[7 + gl] == ':' || one[7 + gl] == '/');
            if (is_gw) { have_gw = true; for (int j = got; j > 0; j--) memcpy(loc[j], loc[j - 1], 160); memcpy(loc[0], one, 160); }
            else memcpy(loc[got], one, 160);
            got++;
        }
    }
    close(s);
    return got;
}

static char *body, *req, *ans;                    /* 900 + 1300 + 700, while upnp() runs */

static int add_map(uint32_t rip, uint16_t rport, const char *ctl, const char *svc, uint16_t port, uint8_t proto, uint32_t me, const char *desc) {
    if (!body || !req || !ans) return -1;
    char mine[16]; snprintf(mine, sizeof mine, "%s", inet_ntoa(*(struct in_addr *)&me));
    int bn = snprintf(body, 900,
        "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:AddPortMapping xmlns:u=\"%s\"><NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
        "<NewProtocol>%s</NewProtocol><NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient>"
        "<NewEnabled>1</NewEnabled><NewPortMappingDescription>DC-PicoLink: %s</NewPortMappingDescription>"
        "<NewLeaseDuration>0</NewLeaseDuration></u:AddPortMapping></s:Body></s:Envelope>\r\n",
        svc, (unsigned)port, proto == 6 ? "TCP" : "UDP", (unsigned)port, mine, desc);
    struct in_addr ra = { .s_addr = rip };
    int rn = snprintf(req, 1300, "POST %s HTTP/1.0\r\nHost: %s:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
        "SOAPAction: \"%s#AddPortMapping\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", ctl, inet_ntoa(ra), (unsigned)rport, svc, bn, body);
    int n = http_req(rip, rport, req, (size_t)rn, ans, 700);
    if (n <= 0) return -1;
    if (!strncmp(ans + 9, "200", 3)) return 0;
    char *e = strstr(ans, "<errorCode>");
    return e ? atoi(e + 11) : 500;
}

/* DeletePortMapping, as DreamPi's delete_all() does before forward_all() */
static int del_map(uint32_t rip, uint16_t rport, const char *ctl, const char *svc, uint16_t port, uint8_t proto) {
    if (!body || !req || !ans) return -1;
    int bn = snprintf(body, 900,
        "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:DeletePortMapping xmlns:u=\"%s\"><NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
        "<NewProtocol>%s</NewProtocol></u:DeletePortMapping></s:Body></s:Envelope>\r\n", svc, (unsigned)port, proto == 6 ? "TCP" : "UDP");
    struct in_addr ra = { .s_addr = rip };
    int rn = snprintf(req, 1300, "POST %s HTTP/1.0\r\nHost: %s:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
        "SOAPAction: \"%s#DeletePortMapping\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", ctl, inet_ntoa(ra), (unsigned)rport, svc, bn, body);
    int n = http_req(rip, rport, req, (size_t)rn, ans, 700);
    if (n <= 0) return -1;
    return !strncmp(ans + 9, "200", 3) ? 0 : 1;         /* 1: there was none (714) or the router said no */
}

/* the router's own outside address (GetExternalIPAddress); "" when it has none (DS-Lite) */
static bool router_outside(uint32_t rip, uint16_t rport, const char *ctl, const char *svc, char *out, size_t oc) {
    int bn = snprintf(body, 900,
        "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:GetExternalIPAddress xmlns:u=\"%s\"></u:GetExternalIPAddress></s:Body></s:Envelope>\r\n", svc);
    struct in_addr ra = { .s_addr = rip };
    int rn = snprintf(req, 1300, "POST %s HTTP/1.0\r\nHost: %s:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
        "SOAPAction: \"%s#GetExternalIPAddress\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", ctl, inet_ntoa(ra), (unsigned)rport, svc, bn, body);
    if (http_req(rip, rport, req, (size_t)rn, ans, 700) <= 0 || strncmp(ans + 9, "200", 3)) return false;
    char *a = strstr(ans, "<NewExternalIPAddress>");
    out[0] = 0;
    if (!a) return true;
    a += 22;
    size_t i = 0;
    while (a[i] && a[i] != '<' && i + 1 < oc) { out[i] = a[i]; i++; }
    out[i] = 0;
    return true;
}

/* host order: an address that cannot be reached from the Internet */
static bool not_public(uint32_t h) {
    return !h || (h >> 24) == 10 || (h >> 24) == 127 || (h & 0xFFC00000u) == 0x64400000u /* 100.64/10 */
        || (h & 0xFFF00000u) == 0xAC100000u || (h & 0xFFFF0000u) == 0xC0A80000u || (h & 0xFFFFFF00u) == 0xC0000000u /* 192.0.0/24: DS-Lite */;
}

static void upnp(uint32_t to, bool to_dc) {
    static char locs[SSDP_MAX][160];
    char ctl[160], svc[64], path[160];
    st.upnp_ok = st.upnp_fail = 0; st.err[0] = 0;
    esp_netif_ip_info_t ii; esp_netif_t *en = esp_netif_get_default_netif();
    uint32_t gw = en && esp_netif_get_ip_info(en, &ii) == ESP_OK ? ii.gw.addr : 0;
    int nl = ssdp(locs, gw);
    /* the gateway did not answer the search (some routers stop answering for a while): ask it
       straight at miniupnpd's usual place, port 5000 */
    bool gw_listed = false;
    if (gw) { char g[16]; snprintf(g, sizeof g, "%s", inet_ntoa(*(struct in_addr *)&gw)); size_t gl = strlen(g);
              for (int i = 0; i < nl; i++) if (!strncmp(locs[i] + 7, g, gl) && (locs[i][7 + gl] == ':' || locs[i][7 + gl] == '/')) gw_listed = true;
              if (!gw_listed && nl < SSDP_MAX) { for (int j = nl; j > 0; j--) memcpy(locs[j], locs[j - 1], 160); snprintf(locs[0], 160, "http://%s:5000/rootDesc.xml", g); nl++; } }
    if (!nl) { snprintf(st.err, sizeof st.err, "no UPnP router answered"); say("port forwarding: no UPnP router answered (is UPnP on in the router?)"); return; }
    uint32_t rip = 0; uint16_t rport = 0;
    int use = -1;
    for (int i = 0; i < nl && use < 0; i++)
        if (parse_url(locs[i], &rip, &rport, path, sizeof path) && find_control(rip, rport, path, ctl, sizeof ctl, svc, sizeof svc)) use = i;
    if (use < 0) {
        snprintf(st.err, sizeof st.err, "router offers no port mapping");
        say("port forwarding: %d UPnP device%s answered (first %s), none offers port mapping", nl, nl == 1 ? "" : "s", locs[0]);
        return;
    }
    if (!strncmp(ctl, "http://", 7)) { uint32_t i2; uint16_t p2; char pa[160]; if (parse_url(ctl, &i2, &p2, pa, sizeof pa)) { rip = i2; rport = p2; snprintf(ctl, sizeof ctl, "%s", pa); } }
    snprintf(st.router, sizeof st.router, "%s:%u", inet_ntoa(*(struct in_addr *)&rip), (unsigned)rport);
    body = malloc(900); req = malloc(1300); ans = malloc(700);
    /* is the router really on the Internet, or behind the provider's (or another) NAT? */
    char out[16] = "";
    if (body && router_outside(rip, rport, ctl, svc, out, sizeof out)) {
        struct in_addr oa = { 0 };
        uint32_t oh = inet_aton(out, &oa) ? lwip_ntohl(oa.s_addr) : 0;
        uint32_t pub = pubip_get();
        int shared = not_public(oh) ? 1 : (pub && oh != pub) ? 2 : 0;
        snprintf(st.outside, sizeof st.outside, "%s", out);
        if (shared != st.shared) {
            if (shared == 1)
                say("your IPv4 is shared by your provider (the router's outside address is %s): nothing on the Internet can reach the Dreamcast, so peer-to-peer games and Netlink waiting need the VPN; port forwarding skipped", out[0] ? out : "none");
            else if (shared == 2)
                say("there is another NAT between the router (outside address %s) and the Internet: peer-to-peer games need the VPN, or port forwarding on that device too; port forwarding here skipped", out);
            else if (st.shared) say("your router now has a public IPv4 address (%s): port forwarding works again", out);
        }
        st.shared = shared;
        if (shared) vpn_auto_on(shared == 1 ? "your IPv4 is shared by your provider" : "another NAT in front of the router");
        if (shared) { snprintf(st.err, sizeof st.err, shared == 1 ? "IPv4 shared by the provider" : "another NAT in front"); free(body); free(req); free(ans); body = req = ans = NULL; return; }
    }
    int first_err = 0;
    for (size_t i = 0; i < sizeof OLD_OWN / sizeof OLD_OWN[0]; i++) del_map(rip, rport, ctl, svc, OLD_OWN[i].port, OLD_OWN[i].proto);   /* (old ones were this box's: asked as this box) */
    as_home = to_dc;                                    /* the game ports: asked as the Dreamcast's address */
    /* delete_all(), then forward_all(): the game list only, each port to the same port */
    for (size_t i = 0; i < sizeof PORTS / sizeof PORTS[0] && enabled; i++)
        for (uint32_t p = PORTS[i].lo; p <= PORTS[i].hi && enabled; p++) {
            int dr = del_map(rip, rport, ctl, svc, (uint16_t)p, PORTS[i].proto);
            if (dr == -1 && as_home && !st.upnp_ok) {          /* nothing comes back to the home address: ask as this box */
                as_home = false;
                say("port forwarding: the router does not answer requests from the Dreamcast's address; asking as this box (a router in UPnP secure mode will refuse those)");
                del_map(rip, rport, ctl, svc, (uint16_t)p, PORTS[i].proto);
            }
            int r = add_map(rip, rport, ctl, svc, (uint16_t)p, PORTS[i].proto, to, PORTS[i].game);
            if (r == 0) st.upnp_ok++; else { st.upnp_fail++; if (!first_err) first_err = r; }
        }
    as_home = false;
    free(body); free(req); free(ans); body = req = ans = NULL;
    if (first_err == -1 && !st.upnp_ok) snprintf(st.err, sizeof st.err, "no answer from the router (or out of memory)");
    else if (first_err == 606 || first_err == 718 || first_err == 729)
        snprintf(st.err, sizeof st.err, "router said %d: its UPnP lets a device open ports only for itself (secure mode)", first_err);
    else if (first_err) snprintf(st.err, sizeof st.err, "router said %d for some ports", first_err);
    char tos[16]; snprintf(tos, sizeof tos, "%s", inet_ntoa(*(struct in_addr *)&to));
    say("port forwarding: router %s forwards %d game ports to %s %s%s%s", st.router, st.upnp_ok, to_dc ? "the Dreamcast's address (asked from it)" : "this box (NAT: no home address for the Dreamcast)", tos,
        st.upnp_fail ? "; refused " : "", st.upnp_fail ? st.err : "");
}

static void task(void *arg) {
    (void)arg;
    uint32_t upnp_for = 0; int64_t upnp_at = 0;
    for (;;) {
        uint32_t me = inet_up() ? my_ip() : 0;
        uint32_t home = me ? lwip_htonl(dclan_dc_ip()) : 0;  /* the Dreamcast's own address (DreamPi), 0 = none */
        /* with a home address the router sends the ports straight to the Dreamcast; NAT maps only without */
        nat_maps(enabled && !home ? me : 0, enabled && !home ? dc_ip() : 0);
        uint32_t to = home ? home : me;
        if (home && enabled) { char a[16]; snprintf(a, sizeof a, "%s", inet_ntoa(*(struct in_addr *)&home)); if (strcmp(a, st.dc)) snprintf(st.dc, sizeof st.dc, "%s", a); }
        int64_t now = esp_timer_get_time();
        if (enabled && to && (to != upnp_for || now - upnp_at > 30LL * 60 * 1000000 || kick)) {   /* new target, every 30 min, switched on */
            kick = false;
            vTaskDelay(pdMS_TO_TICKS(3000));
            upnp(to, home != 0);
            upnp_for = to; upnp_at = esp_timer_get_time();
        }
        if (!enabled) { upnp_for = 0; kick = false; }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

size_t portfwd_info_json(char *o, size_t cap) {
    return (size_t)snprintf(o, cap, "{\"on\":%s,\"shared\":%d,\"outside\":\"%s\",\"router\":\"%s\",\"upnp_ok\":%d,\"upnp_failed\":%d,\"error\":\"%s\",\"maps\":%d,\"dc\":\"%s\"}",
        enabled ? "true" : "false", st.shared, st.outside, st.router, st.upnp_ok, st.upnp_fail, st.err, st.maps, st.dc);
}

void portfwd_start(void) {
    nvs_handle_t h; uint8_t v = 1;
    if (nvs_open("dcl", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "portfwd", &v); nvs_close(h); }
    enabled = v;
    xTaskCreate(task, "portfwd", 4096, NULL, 3, NULL);
}
