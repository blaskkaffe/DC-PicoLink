/* rts_model.h: when the cable obeys the Dreamcast's RTS */
#include <stdio.h>
#include <assert.h>
#include "rts_model.h"

static int changes;
static void run(rts_state_t *s, uint32_t *t, uint32_t us, uint32_t step, bool lvl) {
    for (uint32_t d = 0; d < us; d += step, *t += step) changes += rts_step(s, *t, lvl);
}

int main(void) {
    rts_state_t s; uint32_t t = 0;
    /* 1. pad high for 10 s (game, KOS, dctest): never obey */
    rts_reset(&s); changes = 0;
    run(&s, &t, 10000000, 4, true);
    assert(!s.obey && changes == 0);
    /* 2. pad low for 10 s: never obey */
    rts_reset(&s);
    run(&s, &t, 10000000, 4, false);
    assert(!s.obey && changes == 0);
    /* 3. one glitch per second (noise, power-on): never obey */
    rts_reset(&s);
    for (int i = 0; i < 20; i++) { run(&s, &t, 999000, 4, true); run(&s, &t, 1000, 4, false); }
    assert(!s.obey);
    /* 4. dcload transfer: toggles every 200 us -> obey within a millisecond */
    rts_reset(&s); changes = 0;
    uint32_t start = t;
    while (!s.obey) { run(&s, &t, 200, 4, true); run(&s, &t, 200, 4, false); }
    assert(t - start < 2000 && changes == 1);
    /* 5. DC holds RTS high for 1.5 s (its buffer full): still obeyed */
    run(&s, &t, 1500000, 4, true);
    assert(s.obey);
    /* 6. then a new program leaves RTS high: back to ignoring after 2 s quiet */
    run(&s, &t, 600000, 4, true);
    assert(!s.obey && changes == 2);
    /* 7. wraparound of the 32-bit microsecond clock */
    rts_reset(&s); t = 0xFFFFF000u;
    while (!s.obey) { run(&s, &t, 300, 4, true); run(&s, &t, 300, 4, false); }
    run(&s, &t, 2100000, 4, false);
    assert(!s.obey);
    printf("rts model: 7 cases ok\n");
    return 0;
}
