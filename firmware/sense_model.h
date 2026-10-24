/* DC 3.3 V sense wire detection (see dcmode.h, DC_POWER_PIN). Host-testable:
   the includer provides PIN_RX, PIN_CTS, DC_POWER_PIN >= 0, `volatile uint8_t
   dc_sense` and SENSE_NOTE(text) (log line). sense_step() runs on core0 with the
   time (us) and the GPIO inputs.

   - Not in use: GPIO7 high for 100 ms -> in use until the RP2040 loses power
     (it is powered from the Pi's USB, not by the DC). Without the wire GPIO7's
     pull-down keeps it low, so the cable stays on TXD/RTS sensing.
   - In use: the DC drives TXD or RTS high (it is on) while GPIO7 stays low for
     2 s (gaps in TXD/RTS of up to 100 ms allowed) -> the wire is gone: back to
     TXD/RTS sensing; it is found again the next time GPIO7 is high for 100 ms. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SENSE_ON_US    100000u
#define SENSE_GONE_US 2000000u
#define SENSE_GAP_US   100000u

static struct { bool high, bad; uint32_t high_since, bad_since, bad_last; } sns;

static void sense_step(uint32_t now, uint32_t in) {
    bool v = (in >> DC_POWER_PIN) & 1;
    if (!dc_sense) {
        if (!v) { sns.high = false; return; }
        if (!sns.high) { sns.high = true; sns.high_since = now; return; }
        if (now - sns.high_since < SENSE_ON_US) return;
        dc_sense = 1;
        SENSE_NOTE("found (DC 3.3 V is high): used for DC power on/off from now on");
        sns.high = false; sns.bad = false;
        return;
    }
    bool dc_drives = in & ((1u << PIN_RX) | (1u << PIN_CTS));      /* DC TXD / RTS high */
    if (v) { sns.bad = false; return; }
    if (dc_drives) {
        if (!sns.bad) { sns.bad = true; sns.bad_since = now; }
        sns.bad_last = now;
        if (now - sns.bad_since >= SENSE_GONE_US) {
            dc_sense = 0;
            SENSE_NOTE("gone (DC on but no 3.3 V on the wire for 2 s): back to TXD/RTS sensing");
            sns.bad = false;
        }
    } else if (sns.bad && now - sns.bad_last > SENSE_GAP_US) sns.bad = false;
}
