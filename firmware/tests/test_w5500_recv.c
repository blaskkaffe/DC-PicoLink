/* Regression: RX_RSR must follow RECV at once (no w5_refresh() between frames).
   KOS re-reads RX_RSR right after RECV; a stale non-zero value made it parse a
   "packet" beyond the received data and write a bogus RX_RD.
   gcc -O1 test/test_w5500_recv.c -o t && ./t */
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#define W5_PUSH(b) ((void)(b))
#include "../w5500_model.h"
w5_t w5;
static uint16_t rd16(int bsb, int a) { return (uint16_t)(w5_read(bsb, a) << 8 | w5_read(bsb, a + 1)); }
static void wr16(int bsb, int a, uint16_t v) { w5_write(bsb, a, v >> 8); w5_write(bsb, a + 1, v & 255); }
int main(void) {
    w5_reset();
    w5_write(1, W5S_RXBUF, 16); w5_write(1, W5S_TXBUF, 16);
    w5_write(1, W5S_MR, 0x84);
    w5_write(1, W5S_CR, 0x01);                       /* OPEN */
    assert(w5.sock_open);
    uint8_t pkt[60] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    int got = 0;
    for (int round = 0; round < 1000; round++) {
        int n = round % 3 + 1;                       /* 1..3 packets arrive between polls */
        for (int i = 0; i < n; i++) { pkt[59] = (uint8_t)round; assert(w5_net_rx(pkt, 60)); }
        w5_refresh();                                /* the idle refresh that does happen */
        for (;;) {                                   /* KOS rx_poll, back to back, no refresh */
            uint16_t rsr = rd16(1, W5S_RX_RSR);
            if (!rsr) break;
            uint16_t rd = rd16(1, W5S_RX_RD);
            uint16_t len = rd16(3, rd);
            assert(len == 62);
            wr16(1, W5S_RX_RD, (uint16_t)(rd + len));
            w5_write(1, W5S_CR, 0x40);               /* RECV */
            got++;
        }
    }
    assert(w5.n_recv_bad == 0);
    printf("OK: %d packets, RX_RSR follows RECV, no bad RX_RD\n", got);

    /* Recovery: the DC misreads one length (in range, or beyond the data) and its
       pointer slips. KOS w5500_rx_poll() semantics: header at RX_RD, RX_RD +=
       header, RECV. It must be back in step within a couple of polls, and never
       loop on the same garbage (net7: 22k bad RECVs, RuneScape wedged). */
    const int slips[] = { +1, -1, +7, +40, +3000, -2 };
    for (unsigned k = 0; k < sizeof slips / sizeof *slips; k++) {
        for (int i = 0; i < 5; i++) { pkt[59] = (uint8_t)i; assert(w5_net_rx(pkt, 60)); }
        w5_refresh();
        uint32_t bad0 = w5.n_recv_bad;
        int polls = 0, good = 0, garbage = 0;
        bool slipped = false;
        while (rd16(1, W5S_RX_RSR) && polls < 100) {
            polls++;
            uint16_t rd = rd16(1, W5S_RX_RD);
            uint16_t len = rd16(3, rd);
            if (len == 62) good++; else garbage++;
            uint16_t adv = len;
            if (!slipped) { adv = (uint16_t)(len + slips[k]); slipped = true; }   /* the misread */
            wr16(1, W5S_RX_RD, (uint16_t)(rd + adv));
            w5_write(1, W5S_CR, 0x40);
        }
        assert(polls < 100);                         /* no endless loop */
        assert(garbage == 0);                        /* never handed a garbage length */
        assert(w5.n_recv_bad - bad0 == 1);           /* one resync */
        /* and the next packets parse normally again */
        for (int i = 0; i < 3; i++) assert(w5_net_rx(pkt, 60));
        w5_refresh();
        int after = 0;
        while (rd16(1, W5S_RX_RSR)) {
            uint16_t rd = rd16(1, W5S_RX_RD);
            uint16_t len = rd16(3, rd);
            assert(len == 62);
            wr16(1, W5S_RX_RD, (uint16_t)(rd + len));
            w5_write(1, W5S_CR, 0x40);
            after++;
        }
        assert(after == 3);
        printf("OK: slip %+d -> resynced after %d polls (%d good before), next packets clean\n", slips[k], polls, good);
    }
    return 0;
}
