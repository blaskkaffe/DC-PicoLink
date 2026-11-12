/* When the cable obeys the Dreamcast's RTS (SD CS pad, GPIO5). Host-testable.

   Measured 2026-11-12: with the Dreamcast on and a program that does not drive RTS
   (SCIF MCE off: every game, current KallistiOS, dctest) the pad reads high all the
   time (the SH4's RTS2 pin doubles as the MD8 mode pin; it floats up on the DC side).
   Waiting for it then blocks every byte to the Dreamcast. dcload-serial does drive it
   (MCE on) and toggles it while data flows. So:
   - default: ignore RTS;
   - obey it only while it is seen toggling: RTS_EDGES edges within RTS_WINDOW_US;
   - back to ignoring only when RTS has been HIGH for RTS_QUIET_US without a break (a new
     program that leaves it undriven). Still and low is harmless ("send"), so it does not
     end obeying: Quake 3 (serial) pauses its toggling for seconds at a time (2026-11-12 log).
   Leaving serial mode (DC off, SPI) resets everything (rts_reset).
   rts_step() is called often with the time (us) and the pad level; it returns true
   when the decision changed (rts.obey holds the new one). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RTS_EDGES       4u
#define RTS_WINDOW_US   1000000u
#define RTS_QUIET_US    2000000u

typedef struct { bool obey, level, have; uint32_t edges, win_start, last_edge, high_since; } rts_state_t;

static inline void rts_reset(rts_state_t *s) { *s = (rts_state_t){0}; }

static inline bool rts_step(rts_state_t *s, uint32_t now, bool level) {
    if (!s->have) { s->have = true; s->level = level; s->win_start = s->high_since = now; return false; }
    if (level != s->level) {
        s->level = level;
        s->last_edge = now;
        if (level) s->high_since = now;
        if (now - s->win_start > RTS_WINDOW_US) { s->win_start = now; s->edges = 0; }
        s->edges++;
        if (!s->obey && s->edges >= RTS_EDGES) { s->obey = true; return true; }
        return false;
    }
    if (s->obey && level && now - s->high_since > RTS_QUIET_US) {
        s->obey = false; s->edges = 0; s->win_start = now;
        return true;
    }
    return false;
}
