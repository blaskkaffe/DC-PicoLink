/* serq_admit: whole packets or nothing towards the Dreamcast's serial line. */
#include <stdio.h>
#include "serq_model.h"

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main(void) {
    const uint32_t ring = 8192;
    CHECK(serq_admit(ring, 1500));                      /* empty queue: a full-size packet goes */
    CHECK(serq_admit(ring, 40));
    CHECK(!serq_admit(0, 40));                          /* full: nothing */
    CHECK(!serq_admit(LINK_MAX_WIRE, 40));              /* one piece's room is not enough for its frame */
    /* a queue where a big packet is refused still takes a small one */
    uint32_t room = 1500 + 1500 / 8 + 16 + LINK_MAX_WIRE - 1;
    CHECK(!serq_admit(room, 1500));
    CHECK(serq_admit(room, 100));
    printf("%s: %d failures\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
