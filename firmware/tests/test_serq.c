/* serq_model.h: the queues in front of the Dreamcast's serial line. */
#include <stdio.h>
#include <string.h>
#include "serq_model.h"

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

static int freed[256], nfreed;
static void drop(void *p) { freed[nfreed++] = (int)(intptr_t)p; }
#define P(n) ((void *)(intptr_t)(n))

int main(void) {
    /* classification */
    uint8_t udp[28] = { 0x45 }, tcp[40] = { 0x45 }, icmp[28] = { 0x45 }, v6[40] = { 0x60 };
    udp[9] = 17; tcp[9] = 6; icmp[9] = 1; v6[9] = 17;
    CHECK(serq_is_fast(udp, sizeof udp));
    CHECK(serq_is_fast(icmp, sizeof icmp));
    CHECK(!serq_is_fast(tcp, sizeof tcp));
    CHECK(!serq_is_fast(v6, sizeof v6));
    CHECK(!serq_is_fast(udp, 10));

    /* cable hold: 20 ms at the wire rate, within 64..2048 */
    CHECK(serq_hold(115200) == 230);
    CHECK(serq_hold(57600) == 115);
    CHECK(serq_hold(9600) == 64);
    CHECK(serq_hold(1562500) == 2048);
    CHECK(serq_hold(0) == 64);
    CHECK(serq_feed_ok(0) && !serq_feed_ok(1));

    /* fast goes first; each queue keeps its own order */
    serq_t s; memset(&s, 0, sizeof s);
    serq_put(&s, P(1), 1000, false, drop);
    serq_put(&s, P(2), 100, true, drop);
    serq_put(&s, P(3), 1000, false, drop);
    serq_put(&s, P(4), 100, true, drop);
    uint32_t l;
    CHECK(serq_next(&s, &l) == P(2) && l == 100);
    CHECK(serq_next(&s, &l) == P(4));
    CHECK(serq_next(&s, &l) == P(1) && l == 1000);
    CHECK(serq_next(&s, &l) == P(3));
    CHECK(serq_next(&s, &l) == NULL && serq_empty(&s));
    CHECK(s.fast.bytes == 0 && s.bulk.bytes == 0);

    /* fast full by bytes: the oldest go, the newest stays */
    nfreed = 0;
    for (int i = 1; i <= 4; i++) serq_put(&s, P(i), 1000, true, drop);      /* 4000 > 3072 */
    CHECK(nfreed == 1 && freed[0] == 1 && s.dropped == 1);
    CHECK(s.fast.bytes == 3000);
    serq_put(&s, P(5), 2500, true, drop);                                    /* needs 3 to go */
    CHECK(nfreed == 4 && freed[1] == 2 && freed[2] == 3 && freed[3] == 4);
    CHECK(serq_next(&s, NULL) == P(5) && serq_empty(&s));

    /* fast full by count */
    nfreed = 0; s.dropped = 0;
    for (int i = 0; i < 40; i++) serq_put(&s, P(100 + i), 10, true, drop);
    CHECK(nfreed == 8 && freed[0] == 100 && freed[7] == 107);
    CHECK(serq_next(&s, NULL) == P(108));
    serq_clear(&s, drop);
    CHECK(serq_empty(&s) && nfreed == 8 + 31 && s.fast.bytes == 0);

    /* bulk full: the new one is dropped, the queue keeps its order */
    nfreed = 0;
    for (int i = 1; i <= 6; i++) serq_put(&s, P(i), 1500, false, drop);      /* 9000 > 8192 */
    CHECK(nfreed == 1 && freed[0] == 6);
    CHECK(s.bulk.bytes == 7500);
    CHECK(serq_next(&s, NULL) == P(1));
    serq_clear(&s, drop);

    /* bigger than a queue: dropped at once */
    nfreed = 0;
    serq_put(&s, P(9), 4000, true, drop);
    CHECK(nfreed == 1 && freed[0] == 9 && serq_empty(&s));

    /* no-queue build: whole packets only */
    CHECK(serq_admit(8192, 1500));
    CHECK(!serq_admit(0, 40));
    CHECK(!serq_admit(LINK_MAX_WIRE, 40));
    uint32_t room = 1500 + 1500 / 8 + 16 + LINK_MAX_WIRE - 1;
    CHECK(!serq_admit(room, 1500) && serq_admit(room, 100));

    printf("%s: %d failures\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
