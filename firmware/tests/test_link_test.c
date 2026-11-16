/* Host test of link_test_model.h: a perfect echo passes; a lost, a damaged, a doubled packet
   and a dead peer are each reported as such; the window is respected. */
#include <stdio.h>
#include <string.h>
#include "link_test_model.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* fault: 0 none, 1 drop packet 700, 2 flip a byte in packet 900, 3 send packet 300 twice, 4 peer dies at 1000 */
static void run(int fault, link_test_t *t) {
    static uint8_t q[16][1536]; static size_t qn[16];
    unsigned head = 0, tail = 0; uint32_t now = 1000, maxout = 0;
    link_test_start(t, now);
    for (int step = 0; step < 4000000 && t->state == LINK_TEST_RUN; step++) {
        now += 100;
        uint8_t b[1536]; size_t n;
        while ((n = link_test_next(t, b)) != 0) {
            uint32_t i = t->sent;
            link_test_queued(t);
            if (fault == 1 && i == 700) continue;
            if (fault == 4 && i >= 1000) continue;
            if (fault == 2 && i == 900) b[n / 2] ^= 0x10;
            memcpy(q[tail % 16], b, n); qn[tail % 16] = n; tail++;
            if (fault == 3 && i == 300) { memcpy(q[tail % 16], b, n); qn[tail % 16] = n; tail++; }
        }
        if (t->sent - t->next_rx > maxout) maxout = t->sent - t->next_rx;
        if (head != tail && step % 3 == 0) { link_test_rx(t, q[head % 16], qn[head % 16], now); head++; }
        link_test_poll(t, now);
    }
    CHECK(maxout <= LINK_TEST_WINDOW);
}

int main(void) {
    link_test_t t;
    run(0, &t); CHECK(link_test_passed(&t)); CHECK(t.good == LINK_TEST_N && link_test_kb_per_s(&t) > 0);
    run(1, &t); CHECK(!link_test_passed(&t)); CHECK(t.lost == 1 && t.bad == 0 && t.good == LINK_TEST_N - 1);
    run(2, &t); CHECK(!link_test_passed(&t)); CHECK(t.bad == 1 && t.lost == 0);
    run(3, &t); CHECK(!link_test_passed(&t)); CHECK(t.bad == 1 && t.lost == 0 && t.good == LINK_TEST_N);
    run(4, &t); CHECK(!link_test_passed(&t)); CHECK(t.good == 1000 && t.lost == LINK_TEST_WINDOW);
    for (uint32_t i = 0; i < 5000; i++) CHECK(link_test_len(i) >= 8 && link_test_len(i) <= 1508);
    printf(fails ? "link self-test model: FAILED\n" : "link self-test model: all ok (echo, lost, damaged, doubled, dead peer)\n");
    return fails != 0;
}
