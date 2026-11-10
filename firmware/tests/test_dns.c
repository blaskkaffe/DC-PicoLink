/* DNS helper (dns_model.h) and the DCNow! rules (dcnow_model.h) */
#include <stdio.h>
#include <stdlib.h>
#include "../dns_model.h"
#include "../dcnow_model.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static size_t mkq(uint8_t *b, uint16_t id, const char *name, uint16_t type) {
    size_t o = 0;
    b[o++] = (uint8_t)(id >> 8); b[o++] = (uint8_t)id; b[o++] = 0x01; b[o++] = 0;     /* RD */
    b[o++] = 0; b[o++] = 1; for (int i = 0; i < 6; i++) b[o++] = 0;
    const char *p = name;
    while (*p) {
        const char *e = strchr(p, '.'); size_t l = e ? (size_t)(e - p) : strlen(p);
        b[o++] = (uint8_t)l; memcpy(b + o, p, l); o += l; p += l; if (*p == '.') p++;
    }
    b[o++] = 0; b[o++] = (uint8_t)(type >> 8); b[o++] = (uint8_t)type; b[o++] = 0; b[o++] = 1;
    return o;
}

int main(void) {
    uint8_t q[512], r[512]; dns_q_t d;

    /* parse */
    size_t n = mkq(q, 0x1234, "Dreamcast.Online", DNS_A);
    CHECK(dns_parse(q, n, &d));                     CHECK(d.id == 0x1234 && !strcmp(d.name, "dreamcast.online") && d.qtype == DNS_A && d.qclass == 1 && d.qend == n);
    CHECK(!dns_parse(q, n - 1, &d));                /* cut */
    q[2] |= 0x80; CHECK(!dns_parse(q, n, &d)); q[2] &= 0x7F;      /* a response */
    q[2] |= 0x10; CHECK(!dns_parse(q, n, &d)); q[2] &= ~0x10;     /* opcode != 0 */
    q[5] = 2; CHECK(!dns_parse(q, n, &d)); q[5] = 1;              /* two questions */
    uint8_t bad[] = { 0,1, 1,0, 0,1, 0,0,0,0,0,0, 0xC0, 12, 0,1, 0,1 };   /* compressed name in a question */
    CHECK(!dns_parse(bad, sizeof bad, &d));
    CHECK(!dns_parse(q, 5, &d));

    /* replies */
    n = mkq(q, 0xBEEF, "host.example", DNS_A);       dns_parse(q, n, &d);
    size_t k = dns_reply_a(q, &d, 0x01020304, 60, r, sizeof r);
    CHECK(k == n + 16 && r[0] == 0xBE && r[1] == 0xEF && (r[2] & 0x80) && (r[2] & 1) && (r[3] & 0x0F) == 0 && r[7] == 1);
    CHECK(!memcmp(r + n, "\xC0\x0C\x00\x01\x00\x01\x00\x00\x00\x3C\x00\x04\x01\x02\x03\x04", 16));
    k = dns_reply_rcode(q, &d, DNS_NXDOMAIN, r, sizeof r);
    CHECK(k == n && (r[3] & 0x0F) == 3 && r[7] == 0 && r[5] == 1);
    CHECK(dns_reply_a(q, &d, 1, 1, r, n + 3) == 0);

    /* ip text */
    uint32_t ip;
    CHECK(dns_ip("46.101.91.123", 13, &ip) && ip == 0x2E655B7B);
    CHECK(!dns_ip("1.2.3", 5, &ip) && !dns_ip("1.2.3.4.5", 9, &ip) && !dns_ip("256.1.1.1", 9, &ip) && !dns_ip("a.b.c.d", 7, &ip) && !dns_ip("1.2.3.", 6, &ip));

    /* configuration: built in, then dnsmasq lines */
    dns_conf_t c; dns_conf_defaults(&c);
    CHECK(c.n_up == 2 && dns_upstream(&c, "x.y", 0) == 0x2E655B7B && dns_upstream(&c, "x.y", 1) == 0xD1323281 && dns_upstream(&c, "x.y", 2) == 0x2E655B7B);
    const char *txt =
        "# comment\n"
        "address=/Master.GameSpy.com/192.168.1.33\r\n"
        "address=/example.org/10.0.0.1\n"
        "address=/sub.example.org/10.0.0.2\n"
        "server=/old.game/8.8.4.4\n"
        "server=9.9.9.9#53\n"
        "log-queries\n"
        "address=/bad/notanip\n"
        "address=nodomain\n"
        "   server=1.1.1.1   \n"
        "\n";
    CHECK(dns_conf_text(&c, txt, strlen(txt)) == 6);
    CHECK(c.n_ovr == 3 && c.n_dom == 1 && c.n_up == 4);
    CHECK(dns_override(&c, "master.gamespy.com", &ip) && ip == 0xC0A80121);
    CHECK(dns_override(&c, "www.example.org", &ip) && ip == 0x0A000001);
    CHECK(dns_override(&c, "a.sub.example.org", &ip) && ip == 0x0A000002);          /* longest match */
    CHECK(!dns_override(&c, "notexample.org", &ip) && !dns_override(&c, "example.com", &ip));
    CHECK(dns_upstream(&c, "foo.old.game", 0) == 0x08080404 && dns_upstream(&c, "old.game", 3) == 0x08080404);
    CHECK(dns_upstream(&c, "other.net", 2) == 0x09090909 && dns_upstream(&c, "other.net", 3) == 0x01010101);

    /* limits: more lines than fit are refused, not overrun */
    dns_conf_t big; dns_conf_defaults(&big);
    char line[64]; int took = 0;
    for (int i = 0; i < 200; i++) { snprintf(line, sizeof line, "address=/h%d.test/1.2.3.4", i); took += dns_conf_line(&big, line, strlen(line)); }
    CHECK(took == DNS_OVR_MAX && big.n_ovr == DNS_OVR_MAX);

    /* domain-needed and bogus-priv */
    n = mkq(q, 1, "router", DNS_A);                  dns_parse(q, n, &d); CHECK(dns_local_only(&d));
    n = mkq(q, 1, "dreamcast.online", DNS_A);        dns_parse(q, n, &d); CHECK(!dns_local_only(&d));
    n = mkq(q, 1, "1.1.168.192.in-addr.arpa", DNS_PTR); dns_parse(q, n, &d); CHECK(dns_local_only(&d));
    n = mkq(q, 1, "2.0.0.10.in-addr.arpa", DNS_PTR); dns_parse(q, n, &d); CHECK(dns_local_only(&d));
    n = mkq(q, 1, "1.20.16.172.in-addr.arpa", DNS_PTR); dns_parse(q, n, &d); CHECK(dns_local_only(&d));
    n = mkq(q, 1, "1.32.32.172.in-addr.arpa", DNS_PTR); dns_parse(q, n, &d); CHECK(!dns_local_only(&d));
    n = mkq(q, 1, "8.8.8.8.in-addr.arpa", DNS_PTR);  dns_parse(q, n, &d); CHECK(!dns_local_only(&d));

    /* DCNow!: newest domain, shared domains once, appspot skipped */
    dcnow_t w; dcnow_reset(&w); char dom[DCNOW_NAME];
    CHECK(!dcnow_pick(&w, dom));                                                   /* nothing yet: empty update */
    dcnow_note(&w, "dreamcast.online");
    CHECK(dcnow_pick(&w, dom) && !strcmp(dom, "dreamcast.online"));
    CHECK(dcnow_pick(&w, dom) && !strcmp(dom, "dreamcast.online"));                /* re-sent every poll, as DreamPi does */
    dcnow_note(&w, "dcnow-2016.appspot.com");
    CHECK(dcnow_pick(&w, dom) && !strcmp(dom, "dreamcast.online"));                /* skipped, looks further back */
    dcnow_note(&w, "gameloft.example.com");
    CHECK(dcnow_pick(&w, dom) && !strcmp(dom, "gameloft.example.com"));
    CHECK(!dcnow_pick(&w, dom));                                                   /* shared: only once */
    dcnow_note(&w, "onsen0.overworks.isao.net");
    CHECK(dcnow_pick(&w, dom) && !strcmp(dom, "onsen0.overworks.isao.net"));
    CHECK(!dcnow_pick(&w, dom));
    for (int i = 0; i < 12; i++) { char t[40]; snprintf(t, sizeof t, "g%d.example.net", i); dcnow_note(&w, t); }
    CHECK(dcnow_pick(&w, dom) && !strcmp(dom, "g11.example.net"));
    dcnow_t z; dcnow_reset(&z);
    for (int i = 0; i < 7; i++) dcnow_note(&z, "dcnow-2016.appspot.com");
    CHECK(!dcnow_pick(&z, dom));                                                   /* only our own lookups: empty */
    dcnow_reset(&w);
    dcnow_note(&w, "gameloft.example.com"); CHECK(dcnow_pick(&w, dom));            /* new session: reported again */

    printf("dns: ok\n");
    return 0;
}
