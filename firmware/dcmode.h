#pragma once
#include <stdint.h>
#include "hardware/pio.h"

#define PIN_DC_CTS_SENSE  2   /* SD CLK  (DC CTS)  SPI SCLK / sense      */
#define PIN_RX            3   /* SD CMD  (DC TXD)  UART RX / SPI MOSI    */
#define PIN_TX            4   /* SD DAT0 (DC RXD)  UART TX / SPI MISO    */
#define PIN_CTS           5   /* SD CS   (DC RTS)  UART CTS / SPI CS     */
#define PIN_RTS           6   /* -> 1k -> GPIO2 -> DC CTS                */

enum { MODE_SERIAL = 0, MODE_W5500 = 1, MODE_OFFLINE = 2 };

/* Boot safety: every Dreamcast-facing pin stays high-impedance (input, no pulls)
   until the Dreamcast is up, and again when it powers off. Some consoles hang on
   a black screen if their serial RX (or other) pins see voltage during reset/boot.
   Without the sense wire the DC counts as up once it drives its own TXD or RTS
   high, which only happens after software has set up the serial port.
   DC_POWER_PIN = GPIO for the optional sense wire (~10k to the SD adapter's VDD
   pin = DC 3.3V). The firmware finds the wire by itself: once GPIO7 has been high
   for 100 ms, the wire is used until the RP2040 loses power (it runs from the Pi's
   USB). Without the wire GPIO7's pull-down keeps it low and nothing changes. If the
   DC drives TXD/RTS high for 2 s while GPIO7 stays low, the wire is gone: back to
   TXD/RTS.
   -DDC_POWER_PIN=-1 turns the sense wire off completely (if GPIO7 is used for
   something else on your board). */
#ifndef DC_POWER_PIN
#define DC_POWER_PIN      7
#endif
extern volatile uint8_t dc_sense;           /* 1: the sense wire is in use (set by core0) */
#define DC_ONLINE_DELAY_SENSE_US  700000u   /* 3.3V -> GRESN release is >= 400 ms */
#define DC_OFFLINE_SENSE_US        20000u
#define DC_ONLINE_DELAY_US         20000u
#define DC_OFFLINE_US           30000000u   /* games (AFO) park TXD/RTS low for 10-20 s */

extern volatile int dc_mode;
/* Mode override from the Pi (dcsd-mode): */
enum { FORCE_AUTO = 0, FORCE_SERIAL = 1, FORCE_SPI = 2, FORCE_W5500 = 3, FORCE_SD = 4 };
extern volatile uint8_t dc_force;            /* written by core1 only */
extern volatile uint32_t rts_stamp;      /* time_us of last RTS change (core0) */
extern volatile uint32_t core1_beat;     /* core1 loop heartbeat (diagnostics) */

/* DC_DIAG=1: the heavy diagnostics are built in: the PPP frame checker on serial
   data (~10 KB RAM, per-character work on core0), the logic-analyzer capture
   (32 KB RAM, pio1's last instruction slot; dcsd-capture), the per-frame list of
   each SPI session and the late-read detail (SysTick instrumentation on core1).
   The counters in the status lines (frames, late reads, underruns, bad RECV
   pointers, framing errors) are always there. cmake -DDC_DIAG=0 to leave them out. */
#ifndef DC_DIAG
#define DC_DIAG 1
#endif

/* UART PIO resources (pio0), owned by main.c */
extern PIO uart_pio;
extern uint uart_sm_tx, uart_sm_rx, uart_off_tx, uart_off_rx;

/* SPI-mode diagnostics (core1 counts, core0 reports) */
#define CAP_N 64                        /* frames listed per SPI session */
typedef struct {
    volatile uint32_t w5_frames, sd_frames, to_sd, to_w5, ver_reads, other_reads, short_frames;
    volatile uint32_t late_frames;      /* frames where the shifter waited for a MISO byte */
    volatile uint32_t underruns;        /* read bursts where a MISO byte went out as ff */
    volatile uint32_t book_lost;        /* frame records dropped (queue full): counts above too low */
    volatile uint32_t last_t;           /* time of the last frame end (any kind) */
    volatile uint8_t sd_locked;         /* W5500 socket opened this session: SD detection off */
#if DC_DIAG
    volatile uint32_t common_seq;       /* W5500 common-block accesses (capture trigger) */
    /* first frames of each SPI session: header bytes, data words, first data */
    volatile uint32_t cap_n;            /* frames captured (<= CAP_N) */
    volatile uint32_t cap_seq;          /* bumped when a new session starts */
    struct { uint16_t addr; uint8_t b2, words, d0, d1, miso, late; uint16_t dt_us; } cap[CAP_N];   /* late: bit0 late, bit1 aborted */
    volatile uint32_t cap_common;       /* common-block frames this session */
    volatile uint32_t late_n;           /* late reads recorded below (first 8 per session) */
    struct { uint16_t addr, cyc; uint8_t bsb, path, waiting, passes, booked; uint32_t since; } late_dbg[8];
    /* path 0: CPU was waiting in the header loop; 1: address word taken by the main
       loop (CPU was busy when the frame began). since: CPU cycles from the previous
       frame end to reading the address word; passes/booked: main-loop housekeeping
       passes / bookkeeping steps in between. waiting: RX words already queued
       behind the address word. cyc: CPU cycles from reading the address word to the answer. */
#endif
} spi_stats_t;
extern spi_stats_t spi_stats;

void w5500_spi_init(void);              /* call on core0 before launching core1 */
void core1_main(void);
