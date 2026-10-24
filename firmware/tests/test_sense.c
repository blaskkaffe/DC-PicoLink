/* Host test for sense_model.h: gcc -O1 -I. test/test_sense.c && ./a.out */
#include <stdio.h>
#include <stdlib.h>
#define PIN_RX 3
#define PIN_CTS 5
#define DC_POWER_PIN 7
static volatile unsigned char dc_sense;
static int notes;
#define SENSE_NOTE(t) (notes++, (void)(t))
#include "sense_model.h"

static uint32_t now;
#define P (1u << DC_POWER_PIN)
#define TX (1u << PIN_RX)
#define RTS (1u << PIN_CTS)
/* run for 'ms' with pins 'in', stepping every 'step_us' (core0 loop rate) */
static void run(uint32_t ms, uint32_t in, uint32_t step_us) {
    for (uint32_t t = 0; t < ms * 1000; t += step_us) { now += step_us; sense_step(now, in); }
}
static int fails;
static void check(const char *what, int want) {
    int ok = dc_sense == want;
    printf("%s %s (dc_sense=%d)\n", ok ? "PASS" : "FAIL", what, dc_sense);
    if (!ok) fails++;
}
static void reset(void) { dc_sense = 0; sns.high = sns.bad = false; }

static int one_pass(uint32_t start) {
    now = start;
    /* 1. no wire: GPIO7 low forever, DC on and off */
    reset();
    run(5000, 0, 37); run(60000, TX | RTS, 37); run(1000, 0, 37); run(20000, TX, 37);
    check("no wire: never in use", 0);

    /* 2. wire fitted, DC powers on: 3.3 V first, TXD/RTS ~450 ms later */
    reset();
    run(1000, 0, 37); run(99, P, 37);
    check("wire: not yet after 99 ms of 3.3 V", 0);
    run(2, P, 37);
    check("wire: in use after 100 ms of 3.3 V", 1);
    run(400, P, 37); run(10000, P | TX | RTS, 37);
    check("wire: stays in use while the DC runs", 1);

    /* 3. DC powers off: everything low (pull-downs) -> stays in use */
    run(30000, 0, 37);
    check("wire: stays in use with the DC off", 1);
    /* games parking TXD/RTS low with 3.3 V present */
    run(20000, P, 37);
    check("wire: TXD/RTS parked low, 3.3 V present", 1);

    /* 4. RP2040 starts with the DC already on (Pi reboot) */
    reset();
    run(150, P | TX | RTS, 37);
    check("boot with the DC already on: found", 1);

    /* 5. a short glitch on GPIO7 (50 ms) without the wire */
    reset();
    run(50, P, 37); run(1000, 0, 37); run(60, P, 37); run(10, 0, 37); run(60, P, 37); run(10, 0, 37);
    check("glitches under 100 ms: not taken for the wire", 0);

    /* 6. wire comes loose while the DC is on */
    reset();
    run(200, P, 37);
    run(1990, TX | RTS, 37);
    check("wire loose: still in use after 1.99 s", 1);
    run(20, TX | RTS, 37);
    check("wire loose: dropped after 2 s", 0);
    /* and found again when it is reconnected */
    run(120, P | TX | RTS, 37);
    check("wire reconnected: found again", 1);

    /* 7. wire loose during serial traffic: TXD toggles (gaps < 100 ms) */
    reset();
    run(200, P, 37);
    for (int i = 0; i < 300; i++) { run(5, TX, 37); run(3, 0, 37); }    /* 2.4 s */
    check("wire loose with serial traffic: dropped", 0);

    /* 8. DC powering down: TXD/RTS high for a moment after 3.3 V falls */
    reset();
    run(200, P | TX, 37);
    run(30, TX, 37); run(5000, 0, 37);
    for (int i = 0; i < 40; i++) { run(40, TX, 37); run(150, 0, 37); }   /* gaps > 100 ms */
    check("power-down and sparse TXD blips: stays in use", 1);

    /* 9. slow core0 loop (1 ms steps) still works */
    reset();
    run(120, P, 1000);
    check("1 ms sampling: found", 1);

    return fails;
}

int main(void) {
    /* even and odd start times, and the 32-bit timer wrapping in the middle */
    uint32_t starts[] = { 0, 1, 12345, 12346, 0xFFFFFFFFu - 3000000u, 0xFFFFFFFFu - 3000001u };
    for (unsigned i = 0; i < sizeof starts / sizeof *starts; i++) {
        printf("-- start %u\n", starts[i]);
        one_pass(starts[i]);
    }
    printf(fails ? "FAILED: %d\n" : "OK: sense wire detection, %d notes\n", fails ? fails : notes);
    return fails != 0;
}
