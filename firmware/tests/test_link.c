/* Link framing (link_model.h) and the cable's dc1 answers (cable_api.h). */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "../link_model.h"
#include "../cable_api.h"

static uint32_t rnd = 12345;
static uint32_t rand32(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint8_t wire[64 * LINK_MAX_WIRE];

int main(void) {
    /* known CRC: "123456789" -> cbf43926 */
    CHECK(link_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);

    /* round trips of random packets, many sizes (all COBS block edges), zero-heavy data */
    static uint8_t pay[LINK_MAX_PAYLOAD + 10];
    link_rx_t r; link_rx_init(&r);
    unsigned packets = 0; size_t bytes = 0;
    for (unsigned round = 0; round < 4000; round++) {
        size_t n = round <= LINK_MAX_PAYLOAD ? round : rand32() % (LINK_MAX_PAYLOAD + 1);
        int zmode = round % 3;
        for (size_t i = 0; i < n; i++)
            pay[i] = zmode == 0 ? (uint8_t)rand32() : zmode == 1 ? (rand32() % 4 ? 0 : (uint8_t)rand32()) : (uint8_t)(rand32() | 1);
        uint8_t ch = (uint8_t)(round % LINK_CH_COUNT), seq = (uint8_t)round;
        size_t w = link_build(ch, seq, pay, n, wire);
        CHECK(w > 0 && w <= LINK_MAX_WIRE);
        CHECK(wire[w - 1] == 0);
        for (size_t i = 0; i + 1 < w; i++) CHECK(wire[i] != 0);       /* no zero inside */
        bool got = false;
        for (size_t i = 0; i < w; i++) {
            bool done = link_rx_byte(&r, wire[i]);
            CHECK(done == (i == w - 1));
            got |= done;
        }
        CHECK(got && r.ch == ch && r.seq == seq && r.n == n && (n == 0 || memcmp(r.payload, pay, n) == 0));
        packets++; bytes += w;
    }
    CHECK(r.good == packets && r.lost == 0 && r.crc_bad == 0);
    CHECK(link_build(0, 0, pay, LINK_MAX_PAYLOAD + 1, wire) == 0);

    /* damage: a flipped bit is a CRC (or COBS) error, never a wrong packet; next packet is fine */
    for (unsigned t = 0; t < 2000; t++) {
        size_t n = rand32() % 600;
        for (size_t i = 0; i < n; i++) pay[i] = (uint8_t)rand32();
        size_t w = link_build(LINK_CH_SER, (uint8_t)(t + 7), pay, n, wire);
        size_t at = rand32() % (w - 1);
        uint8_t flip = (uint8_t)(1u << (rand32() % 8));
        if ((wire[at] ^ flip) == 0) flip = 0x80 == flip ? 0x40 : 0x80;
        wire[at] ^= flip;
        link_rx_t d; link_rx_init(&d);
        bool any = false;
        for (size_t i = 0; i < w; i++) if (link_rx_byte(&d, wire[i])) any = true;
        if (any) CHECK(d.n == n && memcmp(d.payload, pay, n) == 0);    /* a flip in an unused spot? must be identical */
        else CHECK(d.crc_bad + d.cobs_bad + d.bad_hdr + d.too_long == 1);
        size_t w2 = link_build(LINK_CH_ETH, 1, pay, n, wire);
        any = false;
        for (size_t i = 0; i < w2; i++) if (link_rx_byte(&d, wire[i])) any = true;
        CHECK(any && d.ch == LINK_CH_ETH && d.n == n);
    }

    /* garbage then a packet (start-up: the other side was mid-packet) */
    {
        link_rx_t d; link_rx_init(&d);
        for (int i = 0; i < 5000; i++) link_rx_byte(&d, (uint8_t)(rand32() | 1));   /* overlong */
        link_rx_byte(&d, 0);
        size_t w = link_build(LINK_CH_STAT, 9, "hi", 2, wire);
        bool ok = false;
        for (size_t i = 0; i < w; i++) ok |= link_rx_byte(&d, wire[i]);
        CHECK(ok && d.n == 2 && memcmp(d.payload, "hi", 2) == 0);
        CHECK(d.too_long == 1);
    }

    /* lost packets counted by seq */
    {
        link_rx_t d; link_rx_init(&d);
        uint8_t seqs[] = { 5, 6, 9, 10, 0 };
        for (int k = 0; k < 5; k++) {
            size_t w = link_build(LINK_CH_SER, seqs[k], "x", 1, wire);
            for (size_t i = 0; i < w; i++) link_rx_byte(&d, wire[i]);
        }
        CHECK(d.good == 5 && d.lost == 2 + 245);
    }

    /* unknown version / channel ignored */
    {
        link_rx_t d; link_rx_init(&d);
        uint8_t pk[8] = { LINK_MAGIC, 2, 0, 0 };
        uint32_t c = link_crc32(0, pk, 4);
        for (int i = 0; i < 4; i++) pk[4 + i] = (uint8_t)(c >> (8 * i));
        size_t w = link_cobs_encode(pk, 8, wire); wire[w++] = 0;
        bool ok = false;
        for (size_t i = 0; i < w; i++) ok |= link_rx_byte(&d, wire[i]);
        CHECK(!ok && d.bad_hdr == 1);
    }

    /* the cable's dc1 answers */
    {
        char out[512];
        cable_api_t s = { .version = "v1.2-3-gabc", .mode = 0, .baud = 57600, .wire = 57600, .force = 0,
                          .route_ser = "esp32", .route_eth = "usb" };
        const char *q = "dc1\nid 17\nget hello\n";
        size_t n = cable_api_handle(&s, q, strlen(q), out, sizeof out); out[n] = 0;
        CHECK(strstr(out, "dc1\nid 17\nok hello.kind cable\nok hello.name DC-PicoLink\nok hello.version v1.2-3-gabc\nok hello.api 1\nok hello.caps api,link\n") == out);
        q = "dc1\nset link.serial.baud 115200\nget link.serial.baud\nset link.serial.baud 99\nset link.force w5500\nset link.mode spi\nget nope.x\ncall sys.commit\nok ignored line";
        n = cable_api_handle(&s, q, strlen(q), out, sizeof out); out[n] = 0;
        CHECK(strcmp(out, "dc1\nok link.serial.baud 115200\nok link.serial.baud 115200\nerr link.serial.baud range\n"
                          "ok link.force w5500\nerr link.mode readonly\nerr nope.x unknown\nerr sys.commit unknown\n") == 0);
        CHECK(s.set_baud == 115200 && s.set_force == 3);
        q = "dc1\nget link.*";
        n = cable_api_handle(&s, q, strlen(q), out, sizeof out); out[n] = 0;
        CHECK(strcmp(out, "dc1\nok link.mode serial\nok link.serial.baud 115200\nok link.serial.wire 57600\nok link.force w5500\n"
                          "ok link.route serial=esp32 network=usb\n") == 0);
        CHECK(s.set_baud == 0 && s.set_force == 0xFF);
        q = "hello\nget hello";
        CHECK(cable_api_handle(&s, q, strlen(q), out, sizeof out) == 0);
        /* tiny output buffer: truncated, never overrun */
        char small[20];
        q = "dc1\nget hello";
        n = cable_api_handle(&s, q, strlen(q), small, sizeof small);
        CHECK(n < sizeof small);
    }

    printf("OK: %u packets (%zu wire bytes) round-trip, 2000 damaged packets rejected, seq loss counted, dc1 answers\n",
           packets, bytes);
    return 0;
}
