/* Host test of netlink_model.h: dial strings (netlink.py digit_parser), the ini, the data
   exchange format with gap recovery, STUN, the PPP delimiter fix, matchmaking JSON. */
#include <stdio.h>
#include <string.h>
#include "netlink_model.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const char *INI =
    "[Metadata]\nversion = 202610081621\n[DCNet]\nenabled = yes\n"
    "[server:199401]\nname = Coup\nhost = saturncoup.duckdns.org\nport = 4821\nshared_secret = Abc!#Key\nauth_magic = AUTH\nauth_timeout = 5.0\n"
    "[server:199407]\nname = Touring Car ; comment\nhost = stcghost.duckdns.org\nport = 4827\nhandler = transparent\n"
    "[server:0352002700]\nhost = other.example\nport = 1\n";

/* netlink.py ppp_fix_frame_delimiters, as a reference */
static size_t ref_fix(const unsigned char *in, size_t n, int *prev, unsigned char *out) {
    if (!n) return 0;
    int ended = in[n - 1] == 0x7e;
    unsigned char t[512]; size_t k = 0;
    for (size_t i = 0; i < n; i++) { if (in[i] == 0x7e && k && t[k - 1] == 0x7e) continue; t[k++] = in[i]; }
    size_t o = 0;
    for (size_t i = 0; i < k; i++) { if (t[i] == 0x7e) { out[o++] = 0x7e; out[o++] = 0x7e; } else out[o++] = t[i]; }
    if (*prev && out[0] != 0x7e) { memmove(out + 1, out, o); out[0] = 0x7e; o++; }
    *prev = ended;
    return o;
}

int main(void) {
    nl_servers_t sv; nl_servers_defaults(&sv);
    int cur = -1; char line[200]; const char *p = INI;
    while (*p) { size_t k = strcspn(p, "\n"); memcpy(line, p, k); line[k] = 0; nl_servers_line(&sv, &cur, line); p += k + (p[k] == '\n'); }
    CHECK(sv.n == 3);
    const nl_server_t *s = nl_server_find(&sv, "199401");
    CHECK(s && s->port == 4821 && !strcmp(s->secret, "Abc!#Key") && !s->transparent && !strcmp(s->name, "Coup"));
    s = nl_server_find(&sv, "199407");
    CHECK(s && s->transparent && !strcmp(s->name, "Touring Car"));
    s = nl_server_find(&sv, "0352002700");
    CHECK(s && !strcmp(s->host, "other.example") && s->port == 1);

    nl_dial_t d;
    d = nl_classify("0", &sv);          CHECK(d.mode == NL_WAIT);
    d = nl_classify("00", &sv);         CHECK(d.mode == NL_WAIT);
    d = nl_classify("199401", &sv);     CHECK(d.mode == NL_SERVER);
    d = nl_classify("199403", &sv);     CHECK(d.mode == NL_PPP);
    d = nl_classify("*70", &sv);        CHECK(d.mode == NL_IDLE);
    d = nl_classify("*69", &sv);        CHECK(d.mode == NL_DCNET_PREFIX);
    d = nl_classify("18002071194", &sv); CHECK(d.mode == NL_XBAND_SERVER);
    d = nl_classify("0642542154", &sv); CHECK(d.mode == NL_CAPCOM);
    d = nl_classify("#012#", &sv);      CHECK(d.mode == NL_WAIT && !strcmp(d.target, "012"));
    d = nl_classify("#192*168*1*20*1#", &sv); CHECK(d.mode == NL_XBAND_CALL && !strcmp(d.target, "192.168.1.20"));
    d = nl_classify("#010*001*002*003#", &sv); CHECK(d.mode == NL_CALL && !strcmp(d.target, "10.1.2.3"));
    d = nl_classify("#010001002003#", &sv); CHECK(d.mode == NL_CALL && !strcmp(d.target, "10.1.2.3"));
    d = nl_classify("#112#", &sv);      CHECK(d.mode == NL_CALL && !strcmp(d.target, "112"));
    d = nl_classify("#999#", &sv);      CHECK(d.mode == NL_CALL && !strcmp(d.target, "999"));
    d = nl_classify("18005551234", &sv); CHECK(d.mode == NL_PPP);

    char ip[20]; uint16_t port;
    CHECK(nl_ip12_to_dotted("081002003104", ip, sizeof ip) && !strcmp(ip, "81.2.3.104"));
    char t12[13]; nl_ip_to_12(0x51020368, t12); CHECK(!strcmp(t12, "081002003104"));
    const char *js = "{\"status\": \"found opponent\", \"opponent ip_address\": [\"081002003104\", \"20002\"]}";
    char st[32];
    CHECK(nl_json_str(js, "status", st, sizeof st) && !strcmp(st, "found opponent"));
    CHECK(nl_json_opponent(js, ip, sizeof ip, &port) && !strcmp(ip, "81.2.3.104") && port == 20002);
    CHECK(nl_json_opponent("{\"opponent ip_address\":[\"081002003104\",20001]}", ip, sizeof ip, &port) && port == 20001);
    CHECK(!nl_json_opponent("{\"status\":\"waiting\"}", ip, sizeof ip, &port));

    /* data exchange: in order, with loss, duplicates, gaps longer than the history */
    static nl_tx_t tx; static uint8_t dg[8][NL_KEEP * (NL_CHUNK_MAX + 40)]; size_t dn[8];
    memset(&tx, 0, sizeof tx);
    for (int i = 0; i < 8; i++) { char m[8]; int k = snprintf(m, sizeof m, "m%d", i); dn[i] = nl_tx_pack(&tx, (uint8_t *)m, (size_t)k, dg[i], sizeof dg[i]); }
    CHECK(!memcmp(dg[1], "m1<dataSplit>1<packetSplit>m0<dataSplit>0", dn[1]));
    nl_rx_t rx = { 0 }; char got[64] = ""; const uint8_t *pl; size_t pn; bool more;
    int order[] = { 0, 3, 3, 2, 7 };                     /* 1,2 lost; 3 recovers 1..3; dup 3; old 2; 7 recovers 4..7 */
    for (int j = 0; j < 5; j++) {
        int i = order[j];
        do { if (!nl_rx_take(&rx, dg[i], dn[i], &pl, &pn, &more)) break; strncat(got, (const char *)pl, pn); } while (more);
    }
    CHECK(!strcmp(got, "m0m1m2m3m4m5m6m7"));
    memset(&tx, 0, sizeof tx); memset(&rx, 0, sizeof rx); got[0] = 0;
    for (int i = 0; i < 8; i++) { char m[8]; int k = snprintf(m, sizeof m, "m%d", i); dn[i] = nl_tx_pack(&tx, (uint8_t *)m, (size_t)k, dg[i], sizeof dg[i]); }
    for (int i = 0; i < 8; i += 7) do { if (!nl_rx_take(&rx, dg[i], dn[i], &pl, &pn, &more)) break; strncat(got, (const char *)pl, pn); } while (more);
    CHECK(!strcmp(got, "m0m7"));                          /* netlink.py: expected one not there, so the newest */
    CHECK(!nl_rx_take(&rx, (const uint8_t *)"PING_SHIRO", 10, &pl, &pn, &more));

    /* STUN */
    uint8_t tid[12] = { 1,2,3,4,5,6,7,8,9,10,11,12 }, q[20];
    CHECK(nl_stun_request(q, tid) == 20 && q[1] == 1 && q[4] == 0x21);
    uint8_t a[64] = { 0x01, 0x01, 0, 24, 0x21, 0x12, 0xA4, 0x42 };
    memcpy(a + 8, tid, 12);
    uint8_t attr1[] = { 0x00, 0x01, 0, 8, 0, 1, 0x4E, 0x21, 1, 1, 1, 1 };                       /* MAPPED 1.1.1.1:20001 */
    uint8_t attr2[] = { 0x00, 0x20, 0, 8, 0, 1, 0x4E ^ 0x21, 0x22 ^ 0x12, 81 ^ 0x21, 2 ^ 0x12, 3 ^ 0xA4, 104 ^ 0x42 };
    memcpy(a + 20, attr1, 12); memcpy(a + 32, attr2, 12);
    uint32_t mip; uint16_t mp;
    CHECK(nl_stun_answer(a, 44, tid, &mip, &mp) && mip == 0x51020368 && mp == 0x4E22);
    a[3] = 12;
    CHECK(nl_stun_answer(a, 32, tid, &mip, &mp) && mip == 0x01010101 && mp == 20001);
    tid[0] = 9; CHECK(!nl_stun_answer(a, 32, tid, &mip, &mp));

    /* PPP delimiter fix vs the Python logic */
    const unsigned char *cases[] = { (const unsigned char *)"~ab~cd~", (const unsigned char *)"xy~~~z", (const unsigned char *)"q", (const unsigned char *)"~" };
    int prev_r = 0; bool prev_m = false;
    for (int i = 0; i < 4; i++) {
        unsigned char o1[64], o2[64];
        size_t n = strlen((const char *)cases[i]);
        size_t a1 = nl_ppp_fix(cases[i], n, &prev_m, o1), a2 = ref_fix(cases[i], n, &prev_r, o2);
        CHECK(a1 == a2 && !memcmp(o1, o2, a1) && prev_m == (bool)prev_r);
    }

    uint8_t au[80];
    CHECK(nl_auth("AUTH", "key", au, sizeof au) == 8 && !memcmp(au, "AUTH\x03key", 8));

    printf(fails ? "netlink: %d FAILED\n" : "netlink: all ok\n", fails);
    return fails != 0;
}
