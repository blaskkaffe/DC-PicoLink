/* Wire rate for a host baud request (see main.c). Host-testable: test/test_baud.c.
   Needs DC_BAUD_MATCH, <stdint.h>, <stdbool.h>. */
#pragma once
/* Above 115200 the rate dcload-serial really sets on the DC. dcload's scif_init:
   SCBRR = 50000000 / (32 * bps) - 1, integer math, stored in the 8-bit SCBRR.
   Above 1562500 that is 0 - 1, which wraps to 255: the DC runs at 6103 baud
   (e.g. -b 1600000 from CP2102N guides). The cable follows that too, so it
   always matches the DC, and logs a hint (see autobaud_task). */
static bool baud_wrapped;
static float dc_effective_baud(uint32_t req) {
#if DC_BAUD_MATCH
    if (req > 115200) {                          /* <= 115200 stays exact, like FTDI/CP210x */
        uint8_t scbrr = (uint8_t)(1562500u / req - 1u);
        baud_wrapped = req > 1562500u;
        return 1562500.0f / (float)(scbrr + 1u);
    }
#endif
    baud_wrapped = false;
    return (float)req;
}
