/* PPP frame filter (hdlc_model.h): TTL 64, PowerSmash double SYN, FCS, framing, "AT" outside frames */
#include <stdio.h>
#include <stdlib.h>
#include "../hdlc_model.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint8_t outbuf[64 * 1024]; static size_t outn;
static void emit(void *ctx, const uint8_t *p, size_t n) { (void)ctx; memcpy(outbuf + outn, p, n); outn += n; }

/* decode what the filter wrote: returns number of frames, fills frame[i] / len[i] (FCS checked and removed) */
static uint8_t fr[16][2000]; static size_t frl[16];
static int decode_out(void) {
    int nf = 0; size_t i = 0;
    while (i < outn) {
        CHECK(outbuf[i] == 0x7E); i++;
        size_t n = 0; bool esc = false;
        while (i < outn && outbuf[i] != 0x7E) {
            uint8_t c = outbuf[i++];
            CHECK(c >= 0x20 || c == 0x7D);                 /* control characters are always escaped */
            if (c == 0x7D) { esc = true; continue; }
            if (esc) { c ^= 0x20; esc = false; }
            fr[nf][n++] = c;
        }
        CHECK(i < outn && outbuf[i] == 0x7E); i++;
        CHECK(hdlc_fcs(0xFFFF, fr[nf], n) == 0xF0B8);
        frl[nf] = n - 2; nf++;
    }
    return nf;
}

/* build a raw (as the Dreamcast would send it) frame: escape per default ACCM */
static size_t mkframe(const uint8_t *body, size_t n, uint8_t *out) { return hdlc_encode(body, n, out); }

static size_t ip_packet(uint8_t *b, uint8_t ttl, uint8_t proto, uint32_t dst, uint16_t sport, uint8_t tcpflags, size_t payload) {
    size_t tl = proto == 6 ? 20 : 8;
    size_t total = 20 + tl + payload;
    memset(b, 0, total);
    b[0] = 0x45; b[2] = (uint8_t)(total >> 8); b[3] = (uint8_t)total; b[8] = ttl; b[9] = proto;
    b[12] = 10; b[13] = 64; b[14] = 0; b[15] = 2;
    b[16] = (uint8_t)(dst >> 24); b[17] = (uint8_t)(dst >> 16); b[18] = (uint8_t)(dst >> 8); b[19] = (uint8_t)dst;
    uint32_t s = 0; for (int i = 0; i < 20; i += 2) s += (uint32_t)(b[i] << 8 | b[i + 1]);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    uint16_t c = (uint16_t)~s; b[10] = (uint8_t)(c >> 8); b[11] = (uint8_t)c;
    b[20] = (uint8_t)(sport >> 8); b[21] = (uint8_t)sport; b[22] = 0; b[23] = 80;
    if (proto == 6) b[33] = tcpflags;
    for (size_t i = 0; i < payload; i++) b[20 + tl + i] = (uint8_t)(i * 7);
    return total;
}
static size_t ppp_ip(uint8_t *f, const uint8_t *ip, size_t n, bool compress) {
    size_t o = 0;
    if (!compress) { f[o++] = 0xFF; f[o++] = 0x03; f[o++] = 0x00; f[o++] = 0x21; }
    else f[o++] = 0x21;
    memcpy(f + o, ip, n);
    return o + n;
}
static bool ip_sum_ok(const uint8_t *ip) {
    uint32_t s = 0; for (int i = 0; i < 20; i += 2) s += (uint32_t)(ip[i] << 8 | ip[i + 1]);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return s == 0xFFFF;
}

static void send(hdlc_t *h, const uint8_t *body, size_t n, uint32_t ms) {
    uint8_t raw[4096]; size_t k = mkframe(body, n, raw);
    hdlc_feed(h, raw, k, ms);
}

int main(void) {
    hdlc_t h; hdlc_init(&h, emit, NULL);
    uint8_t ip[1600], f[1700];

    /* TTL 30 -> 65 (lwIP forwards it as 64), checksum stays right, payload untouched */
    size_t n = ip_packet(ip, 30, 17, 0x08080808, 5000, 0, 100);
    send(&h, f, ppp_ip(f, ip, n, false), 0);
    outn = 0; send(&h, f, ppp_ip(f, ip, n, false), 0);
    int nf = decode_out();                                   CHECK(nf == 1);
    CHECK(frl[0] == 4 + n && fr[0][0] == 0xFF && fr[0][1] == 3 && fr[0][2] == 0 && fr[0][3] == 0x21);
    CHECK(fr[0][4 + 8] == 65);                               CHECK(ip_sum_ok(fr[0] + 4));
    CHECK(!memcmp(fr[0] + 4 + 12, ip + 12, n - 12));         /* everything after the TTL/checksum is the same */
    CHECK(h.ipf.ttl_fixed == 2);

    /* already 65: unchanged; protocol field compression and no address/control are understood */
    outn = 0; n = ip_packet(ip, 65, 17, 0x08080808, 5000, 0, 10);
    send(&h, f, ppp_ip(f, ip, n, true), 0);
    nf = decode_out();                                       CHECK(nf == 1 && frl[0] == 1 + n);
    CHECK(fr[0][0] == 0x21 && fr[0][1 + 8] == 65);           CHECK(h.ipf.ttl_fixed == 2);
    outn = 0; n = ip_packet(ip, 3, 1, 0x01010101, 0, 0, 20);
    send(&h, f, ppp_ip(f, ip, n, true), 0);
    nf = decode_out();                                       CHECK(nf == 1 && fr[0][1 + 8] == 65 && ip_sum_ok(fr[0] + 1));

    /* big packet, bytes that need escaping everywhere, split across feeds */
    outn = 0; n = ip_packet(ip, 64, 17, 0x7E7D7E7D, 7000, 0, 1400);
    for (size_t i = 20 + 8; i < n; i++) ip[i] = (uint8_t)(i % 5 == 0 ? 0x7E : i % 5 == 1 ? 0x7D : i % 32);
    uint8_t raw[8192]; size_t k = mkframe(f, ppp_ip(f, ip, n, false), raw);
    uint32_t before = h.ipf.ttl_fixed;
    for (size_t i = 0; i < k; i += 7) hdlc_feed(&h, raw + i, k - i < 7 ? k - i : 7, 0);
    nf = decode_out();                                       CHECK(nf == 1 && h.ipf.ttl_fixed == before + 1);
    CHECK(!memcmp(fr[0] + 4 + 20, ip + 20, n - 20));

    /* LCP / IPCP: same bytes in, same bytes out (content), not touched */
    outn = 0; uint8_t lcp[] = { 0xFF, 0x03, 0xC0, 0x21, 1, 7, 0, 10, 2, 4, 5, 0xDC };
    send(&h, lcp, sizeof lcp, 0);
    nf = decode_out();                                       CHECK(nf == 1 && frl[0] == sizeof lcp && !memcmp(fr[0], lcp, sizeof lcp));

    /* PowerSmash: SYN from 3202; the second to the same address inside a second is dropped, and keeps being dropped
       while retries come faster than once a second (DreamPi's "recent --update") */
    outn = 0; n = ip_packet(ip, 64, 6, 0x0A0B0C0D, 3202, 0x02, 0);
    send(&h, f, ppp_ip(f, ip, n, false), 1000);              CHECK(decode_out() == 1);
    outn = 0; send(&h, f, ppp_ip(f, ip, n, false), 1005);    CHECK(decode_out() == 0 && h.ipf.syn_dropped == 1);
    outn = 0; send(&h, f, ppp_ip(f, ip, n, false), 1900);    CHECK(decode_out() == 0 && h.ipf.syn_dropped == 2);   /* 895 ms after the drop at 1005 */
    outn = 0; send(&h, f, ppp_ip(f, ip, n, false), 2950);    CHECK(decode_out() == 1 && h.ipf.syn_dropped == 2);   /* 1050 ms after the drop at 1900: passes */
    outn = 0; send(&h, f, ppp_ip(f, ip, n, false), 3100);    CHECK(decode_out() == 0 && h.ipf.syn_dropped == 3);   /* and that one counts again */
    /* a different destination is its own entry; other source ports, SYN+ACK and other protocols are not looked at */
    outn = 0; n = ip_packet(ip, 64, 6, 0x0A0B0C0E, 3205, 0x02, 0);
    send(&h, f, ppp_ip(f, ip, n, false), 4001);              CHECK(decode_out() == 1);
    outn = 0; n = ip_packet(ip, 64, 6, 0x0A0B0C0E, 3206, 0x02, 0);
    send(&h, f, ppp_ip(f, ip, n, false), 4002);              send(&h, f, ppp_ip(f, ip, n, false), 4003);   CHECK(decode_out() == 2);
    outn = 0; n = ip_packet(ip, 64, 6, 0x0A0B0C0F, 3200, 0x12, 0);
    send(&h, f, ppp_ip(f, ip, n, false), 5000);              send(&h, f, ppp_ip(f, ip, n, false), 5001);   CHECK(decode_out() == 2);
    outn = 0; n = ip_packet(ip, 64, 17, 0x0A0B0C10, 3200, 0x02, 0);
    send(&h, f, ppp_ip(f, ip, n, false), 5000);              send(&h, f, ppp_ip(f, ip, n, false), 5001);   CHECK(decode_out() == 2);
    outn = 0; n = ip_packet(ip, 64, 6, 0x0A0B0C11, 3200, 0x02, 0);
    ip[6] = 0x00; ip[7] = 0x08;                              /* fragment offset: not a first fragment, not looked at */
    send(&h, f, ppp_ip(f, ip, n, false), 6000);              send(&h, f, ppp_ip(f, ip, n, false), 6001);   CHECK(decode_out() == 2);
    /* the clock wraps */
    outn = 0; n = ip_packet(ip, 64, 6, 0x0A0B0C12, 3201, 0x02, 0);
    send(&h, f, ppp_ip(f, ip, n, false), 0xFFFFFF00u);       send(&h, f, ppp_ip(f, ip, n, false), 0x20u);  CHECK(decode_out() == 1);   /* 288 ms apart: second dropped */
    /* more than 8 destinations: oldest entry makes room, nothing crashes */
    outn = 0;
    for (uint32_t d = 0; d < 20; d++) { n = ip_packet(ip, 64, 6, 0x0C000000u + d, 3203, 0x02, 0); send(&h, f, ppp_ip(f, ip, n, false), 10000 + d); }
    CHECK(decode_out() == 20);

    /* bad FCS: dropped. aborted frame (7D 7E): dropped. too long: dropped, next one fine. */
    outn = 0; uint32_t fb = h.fcs_bad; n = ip_packet(ip, 64, 17, 0x08080808, 1, 0, 10);
    k = mkframe(f, ppp_ip(f, ip, n, false), raw); raw[k - 3] ^= 0x55;   /* inside the FCS or last data byte */
    hdlc_feed(&h, raw, k, 0);                                CHECK(decode_out() == 0 && h.fcs_bad == fb + 1);
    uint8_t ab[] = { 0x7E, 0xFF, 0x7D, 0x7E };               /* abort */
    hdlc_feed(&h, ab, sizeof ab, 0);                         CHECK(h.aborted == 1 && outn == 0);
    uint8_t longf[HDLC_MAX_FRAME + 400]; memset(longf, 0x41, sizeof longf);
    hdlc_feed(&h, (const uint8_t *)"\x7e", 1, 0); hdlc_feed(&h, longf, sizeof longf, 0); hdlc_feed(&h, (const uint8_t *)"\x7e", 1, 0);
    CHECK(h.too_long == 1 && outn == 0);
    n = ip_packet(ip, 64, 17, 0x08080808, 1, 0, 10);
    send(&h, f, ppp_ip(f, ip, n, false), 0);                 CHECK(decode_out() == 1);

    /* "AT" where a frame should start = the Dreamcast is back at its modem */
    hdlc_t g; hdlc_init(&g, emit, NULL); outn = 0;
    hdlc_feed(&g, (const uint8_t *)"\x7e\x7e", 2, 0);         CHECK(!g.saw_at);
    hdlc_feed(&g, (const uint8_t *)"A", 1, 0);               CHECK(!g.saw_at);
    hdlc_feed(&g, (const uint8_t *)"T\r", 2, 0);             CHECK(g.saw_at);
    hdlc_init(&g, emit, NULL);
    hdlc_feed(&g, (const uint8_t *)"\x7e" "A" "\x7e" "T", 4, 0);   CHECK(!g.saw_at);
    hdlc_init(&g, emit, NULL);                                /* AT inside a frame is data */
    hdlc_feed(&g, (const uint8_t *)"\x7e" "XATAT", 6, 0);    CHECK(!g.saw_at);
    hdlc_init(&g, emit, NULL);                                /* before the first flag too, and right after a frame */
    hdlc_feed(&g, (const uint8_t *)"at\r", 3, 0);            CHECK(g.saw_at);

    printf("hdlc: ok (frames %u, ttl fixed %u, syn dropped %u)\n", h.frames, h.ipf.ttl_fixed, h.ipf.syn_dropped);
    return 0;
}
