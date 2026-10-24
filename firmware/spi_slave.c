/*
 * Core1: Dreamcast serial-port bus watcher.
 *   OFFLINE  - all DC-facing pins high-Z until the DC is up (see dcmode.h)
 *   SERIAL   - UART bridge (core0 moves the data); watch for SPI clocking
 *   SPI      - the DC drives its CTS pin as SPI clock (KallistiOS SCIF-SPI).
 *              Two emulated devices share the bus, chosen per traffic:
 *                * WIZnet W5500 (MACRAW socket 0)          -> USB network
 *                * SD card, SPI mode, SDHC                 -> image file on the Pi
 *
 * Serial/SPI detection uses the 1k resistor between GPIO6 and GPIO2:
 *   - In serial mode GPIO6 drives DC CTS (our RTS) through 1k and GPIO2 reads the
 *     line back. The DC's CTS pin is an input then, so GPIO2 == GPIO6.
 *   - KOS SCIF-SPI turns DC CTS into an output (SPI clock). The DC overpowers the
 *     1k, so GPIO2 != GPIO6 -> SPI mode immediately.
 *   - In SPI mode GPIO6 is released. While the bus is idle we briefly drive it
 *     both ways; if GPIO2 follows for ~200 ms the DC let go of the line -> serial.
 *
 * W5500 vs SD: both state machines listen to every frame (pio1 = W5500, pio0 = SD);
 * GPIO4's pin function decides which one drives MISO.
 *   - SD hosts always start a frame with 0xFF (wait-ready) followed by a command
 *     with a valid CRC7. Seen in W5500 mode -> switch to SD and execute it.
 *   - W5500 frames start with the address high byte, which KOS never sends as 0xFF
 *     for register access. A frame whose first byte isn't 0xFF in SD mode -> W5500.
 */
#include <stddef.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/structs/iobank0.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/timer.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/systick.h"
#include "dcmode.h"
#include "w5500_spi.pio.h"
#include "sd_spi.pio.h"

#define SPI_PIO   pio1
#define SPI_SM    0u
#define W5_PUSH(b) (SPI_PIO->txf[SPI_SM] = (uint32_t)(b) << 24)
static void w5_fast_reset(void);
#define W5_RESET() w5_fast_reset()
#include "w5500_model.h"
static void w5_map_init(void);

/* MR.RST: the DC reads MR back in the very next frame, ~3 us later, so the
   576 register bytes are restored by DMA from an image taken at init instead
   of w5_reset()'s memsets and loops. */
#define W5_REG_BYTES (sizeof w5.common + sizeof w5.sreg)
static uint32_t w5_reset_img[W5_REG_BYTES / 4];
static int w5_reset_dma;
static void w5_fast_reset(void) {
    dma_channel_set_write_addr(w5_reset_dma, w5.common, false);
    dma_channel_transfer_from_buffer_now(w5_reset_dma, w5_reset_img, W5_REG_BYTES / 4);
    w5.sock_open = 0;
    w5.mac_filter = 0;
    w5.tx_mask = 2048 - 1;
    w5.rx_mask = 2048 - 1;
    w5.rx_rd = w5.rx_wr;
    /* no wait: the copy takes < 1 us, the DC's next read is several us away */
}
static void w5_fast_reset_init(void) {
    _Static_assert(offsetof(w5_t, sreg) == sizeof w5.common, "common and sreg must be contiguous");
    memcpy(w5_reset_img, w5.common, W5_REG_BYTES);      /* right after w5_reset() */
    w5_reset_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(w5_reset_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    dma_channel_configure(w5_reset_dma, &c, w5.common, w5_reset_img, W5_REG_BYTES / 4, false);
}

#define SD_PIO    pio0
static uint sd_sm, sd_off;
static int sd_dma;
static uint8_t sd_fill = 0xFF;
static void sd_set_fill(uint8_t v) {
    sd_fill = v;
    pio_sm_exec(SD_PIO, sd_sm, v ? pio_encode_mov_not(pio_x, pio_null) : pio_encode_mov(pio_x, pio_null));
}
static void sd_flush(void) {
    dma_channel_abort(sd_dma);
    pio_sm_drain_tx_fifo(SD_PIO, sd_sm);
}
#define SD_PUSH(b)       (SD_PIO->txf[sd_sm] = (uint32_t)(b) << 24)
#define SD_DMA(buf, len) dma_channel_transfer_from_buffer_now(sd_dma, (buf), (len))
#define SD_DMA_BUSY()    dma_channel_is_busy(sd_dma)
#define SD_FLUSH()       sd_flush()
#define SD_FILL(v)       sd_set_fill(v)
#define HOST_WRITE(p, n) false          /* core0-only helpers, unused here */
#define HOST_WRITE_AVAIL() 0u
#include "sd_model.h"

w5_t w5;
sd_shared_t sd;
sd_card_t sdc;
sd_host_t sdh;
volatile int dc_mode = MODE_OFFLINE;
volatile uint8_t dc_force = FORCE_AUTO;
volatile uint8_t dc_sense;
volatile uint32_t rts_stamp;
volatile uint32_t core1_beat;

static uint spi_off;
enum { P_W5500, P_SD };
static uint8_t proto = P_W5500;

/* SD card emulation is for the loader (openMenu) only: once a program has
   opened the W5500 socket (a game going online), SD commands are no longer
   looked for until the next SPI session (a new program) or power-off. Games
   don't use the SD card, so a game's network traffic can never be taken for
   SD. dcsd-mode sd still forces SD. */
static bool sd_locked;

/* per-frame state */
static uint32_t sd_frame_bytes;
static bool det_on;
static uint8_t det_n, det_ff, det_buf[6];

/* frame-end records, booked later (see w5_mark) */
typedef struct {
    uint16_t addr0, addr;
    uint8_t bsb, write, phase, late, aborted;
#if DC_DIAG
    uint8_t words, d0, d1, miso;    /* for the frame list */
#endif
    uint32_t t;                     /* when the frame ended (us) */
} w5_rec_t;
#define BOOK_N 64u
static w5_rec_t book[BOOK_N];
static uint32_t book_h, book_t;
static bool book_refresh;
static uint32_t t_mark;             /* time of the last frame end */
#define W5_MARK       0xFFFFFFFFu                         /* end of frame, from the PIO program */
enum { W5P_HDR = 0, W5P_BSB = 1, W5P_CTL = 2, W5P_DATA = 3, W5P_SKIP = 4 };   /* w5.phase */
#define W5_TXEMPTY    (1u << (PIO_FSTAT_TXEMPTY_LSB + SPI_SM))
#define W5_TXFULL     (1u << (PIO_FSTAT_TXFULL_LSB + SPI_SM))
#define W5_RXEMPTY    (1u << (PIO_FSTAT_RXEMPTY_LSB + SPI_SM))
#define SD_RXEMPTY    (1u << (PIO_FSTAT_RXEMPTY_LSB + sd_sm))

static inline uint32_t now_us(void) { return timer_hw->timerawl; }

/* Is the DC on? With the sense wire (once found, see dcmode.h): its 3.3 V.
   Without: it drives its TXD or RTS high. */
static inline bool dc_alive(void) {
#if DC_POWER_PIN >= 0
    if (dc_sense) return sio_hw->gpio_in & (1u << DC_POWER_PIN);
#endif
    return sio_hw->gpio_in & ((1u << PIN_RX) | (1u << PIN_CTS));   /* DC TXD or RTS high */
}
static inline uint32_t dc_offline_us(void) { return dc_sense ? DC_OFFLINE_SENSE_US : DC_OFFLINE_US; }
static inline uint32_t dc_online_delay_us(void) { return dc_sense ? DC_ONLINE_DELAY_SENSE_US : DC_ONLINE_DELAY_US; }

void w5500_spi_init(void) {
#if DC_POWER_PIN >= 0
    gpio_init(DC_POWER_PIN);
    gpio_set_dir(DC_POWER_PIN, GPIO_IN);
    gpio_pull_down(DC_POWER_PIN);
#endif
    w5.rx_mask = w5.tx_mask = 2047;
    /* core1 (the W5500 answer path) wins bus contention against core0 and DMA */
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;
    w5_reset();
    w5_fast_reset_init();
    w5_map_init();
    sd_card_init();

    /* W5500 shifter on pio1 */
    pio_sm_claim(SPI_PIO, SPI_SM);
    spi_off = pio_add_program(SPI_PIO, &w5500_spi_program);
    pio_sm_config c = w5500_spi_program_get_default_config(spi_off);
    sm_config_set_in_pins(&c, PIN_RX);
    sm_config_set_out_pins(&c, PIN_TX, 1);
    sm_config_set_set_pins(&c, PIN_TX, 1);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_out_shift(&c, false, false, 32);
    sm_config_set_jmp_pin(&c, PIN_CTS);                 /* the program ends frames itself */
    pio_sm_init(SPI_PIO, SPI_SM, spi_off + w5500_spi_offset_start, &c);
    pio_sm_set_pins_with_mask(SPI_PIO, SPI_SM, 1u << PIN_TX, 1u << PIN_TX);
    pio_sm_set_pindirs_with_mask(SPI_PIO, SPI_SM, 1u << PIN_TX, 1u << PIN_TX);

    /* SD shifter on pio0 (next to the UART programs) + its MISO DMA */
    sd_sm = pio_claim_unused_sm(SD_PIO, true);
    sd_off = pio_add_program(SD_PIO, &sd_spi_program);
    c = sd_spi_program_get_default_config(sd_off);
    sm_config_set_in_pins(&c, PIN_RX);
    sm_config_set_out_pins(&c, PIN_TX, 1);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_out_shift(&c, false, false, 32);
    pio_sm_init(SD_PIO, sd_sm, sd_off + sd_spi_offset_sync, &c);
    pio_sm_set_pindirs_with_mask(SD_PIO, sd_sm, 1u << PIN_TX, 1u << PIN_TX);
    sd_set_fill(0xFF);

    sd_dma = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(sd_dma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);   /* byte replicated to all lanes */
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(SD_PIO, sd_sm, true));
    dma_channel_configure(sd_dma, &dc, &SD_PIO->txf[sd_sm], NULL, 0, false);
}

/* ---- W5500 shifter control ----------------------------------------------
   The PIO program finds frame ends itself (CS high between two bytes) and puts
   a W5_MARK word in the RX FIFO, so the CPU never has to restart it per frame. */
static inline bool cs_high(void) { return sio_hw->gpio_in & (1u << PIN_CTS); }

static inline uint w5_pc(void) { return SPI_PIO->sm[SPI_SM].addr - spi_off; }

/* (Re)start the W5500 shifter at 'start'. mid_frame: the bus may be in a frame,
   so first wait (up to ~100 us) for CS to go idle; if it doesn't, the watchdog
   realigns the shifter at the next CS high. */
static void w5_sm_start(bool mid_frame) {
    if (mid_frame) for (uint32_t t0 = now_us(); !cs_high() && now_us() - t0 < 100; ) { }
    hw_clear_bits(&SPI_PIO->ctrl, 1u << SPI_SM);
    pio_sm_clear_fifos(SPI_PIO, SPI_SM);
    pio_sm_restart(SPI_PIO, SPI_SM);
    SPI_PIO->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + SPI_SM);
    pio_sm_exec(SPI_PIO, SPI_SM, pio_encode_jmp(spi_off + w5500_spi_offset_start));
    pio_sm_exec(SPI_PIO, SPI_SM, pio_encode_set(pio_pins, 1));       /* MISO idles high */
    hw_set_bits(&SPI_PIO->ctrl, 1u << SPI_SM);
}

/* Drop the stale MISO bytes prefetched past the end of a frame. Only valid while
   the shifter is in the header (no pulls there), i.e. right after a marker. */
static inline void w5_tx_flush(void) {
    uint32_t n = (SPI_PIO->flevel >> (PIO_FLEVEL_TX0_LSB + SPI_SM * 8)) & 0xF;
    while (n--) SPI_PIO->sm[SPI_SM].instr = pio_encode_pull(false, false);
    SPI_PIO->sm[SPI_SM].instr = pio_encode_set(pio_pins, 1);     /* MISO idles high between frames */
}

/* Restart both shifters for a new SPI session (sync: may be mid-frame). */
static void spi_restart(bool sync) {
    w5_sm_start(sync);
    w5.phase = W5P_HDR;

    hw_clear_bits(&SD_PIO->ctrl, 1u << sd_sm);
    dma_channel_abort(sd_dma);
    pio_sm_clear_fifos(SD_PIO, sd_sm);
    pio_sm_restart(SD_PIO, sd_sm);
    pio_sm_exec(SD_PIO, sd_sm, pio_encode_jmp(sd_off + (sync ? sd_spi_offset_sync : sd_spi_offset_start)));
    sd_set_fill(sd_fill);

    sd_frame_bytes = 0;
    det_on = true; det_n = 0; det_ff = 0;
    hw_set_bits(&SD_PIO->ctrl, 1u << sd_sm);
}

static void spi_stop(void) {
    hw_clear_bits(&SPI_PIO->ctrl, 1u << SPI_SM);
    hw_clear_bits(&SD_PIO->ctrl, 1u << sd_sm);
    dma_channel_abort(sd_dma);
    sd_frame_end();
    w5.sock_open = 0;
    w5.phase = W5P_HDR;
    proto = P_W5500;
}

spi_stats_t spi_stats;

/* W5500 words seen while in SD mode, since the last frame end */
static uint32_t sd_w5_words;

static void use_proto(uint8_t p) {
    if (p != proto) {
        if (p == P_SD) spi_stats.to_sd++;
        else {
            spi_stats.to_w5++;
            /* The W5500 shifter followed the frame all along. If its header word
               hasn't come yet, serve the frame; otherwise ignore it to the end. */
            w5.phase = sd_w5_words ? W5P_SKIP : W5P_HDR;
            if (!sd_w5_words) w5_tx_flush();       /* the dummy bytes queued in SD mode */
        }
    }
    proto = p;
    gpio_set_function(PIN_TX, p == P_SD ? GPIO_FUNC_PIO0 : GPIO_FUNC_PIO1);
}

static void enter_spi(void) {
    /* UART TX off; RX keeps listening on TXD (MOSI): between SPI sessions the
       DC prints its console there (KOS dbglog), which core0 forwards to the Pi */
    hw_clear_bits(&uart_pio->ctrl, (1u << uart_sm_tx) | (1u << uart_sm_rx));
    pio_sm_restart(uart_pio, uart_sm_rx);
    pio_sm_exec(uart_pio, uart_sm_rx, pio_encode_jmp(uart_off_rx));
    hw_set_bits(&uart_pio->ctrl, 1u << uart_sm_rx);
    sio_hw->gpio_oe_clr = 1u << PIN_RTS;                  /* release DC CTS */
    sd_locked = false; spi_stats.sd_locked = 0;           /* new program: SD allowed again */
    w5.n_rsd = 0;                                         /* resync details per session */
    spi_restart(true);                                    /* may be mid-frame */
    sd_w5_words = 0;
    book_t = book_h; book_refresh = true;                 /* no records from the last session */
    use_proto(dc_force == FORCE_SD ? P_SD : P_W5500);
    w5_refresh();
#if DC_DIAG
    spi_stats.cap_n = 0; spi_stats.cap_common = 0; spi_stats.late_n = 0; spi_stats.cap_seq++;
#endif
    dc_mode = MODE_W5500;
}

/* Release every Dreamcast-facing pin (high-Z). */
static void enter_offline(void) {
    sd_locked = false; spi_stats.sd_locked = 0;
    hw_clear_bits(&uart_pio->ctrl, (1u << uart_sm_tx) | (1u << uart_sm_rx));
    spi_stop();
    sio_hw->gpio_oe_clr = (1u << PIN_TX) | (1u << PIN_RTS);
    gpio_set_function(PIN_TX, GPIO_FUNC_SIO);
    dc_mode = MODE_OFFLINE;
}

static void enter_serial(void) {
    spi_stop();
    pio_sm_clear_fifos(uart_pio, uart_sm_tx);
    pio_sm_clear_fifos(uart_pio, uart_sm_rx);
    pio_sm_restart(uart_pio, uart_sm_tx);
    pio_sm_restart(uart_pio, uart_sm_rx);
    pio_sm_exec(uart_pio, uart_sm_tx, pio_encode_jmp(uart_off_tx));
    pio_sm_exec(uart_pio, uart_sm_rx, pio_encode_jmp(uart_off_rx));
    gpio_set_function(PIN_TX, GPIO_FUNC_PIO0);
    rts_stamp = now_us();
    sio_hw->gpio_clr = 1u << PIN_RTS;                     /* RTS asserted */
    sio_hw->gpio_oe_set = 1u << PIN_RTS;
    hw_set_bits(&uart_pio->ctrl, (1u << uart_sm_tx) | (1u << uart_sm_rx));
    dc_mode = MODE_SERIAL;
}

/* Count clock pulses on GPIO2 (DC CTS) with a tight sampling burst. A KOS
   SCIF-SPI clock pulse can be ~100 ns, far shorter than one pass of a loop that
   also reads the timer, so sampling and bookkeeping are kept apart.
   'ref' is the level GPIO2 should have if nobody but us drives it.
   Returns the number of transitions away from 'ref' seen in ~256 samples. */
static inline uint32_t __attribute__((always_inline)) clk_burst(uint32_t ref, uint32_t *prev) {
    uint32_t n = 0, p = *prev;
    for (int i = 0; i < 256; i++) {
        uint32_t m = ((sio_hw->gpio_in >> PIN_DC_CTS_SENSE) ^ ref) & 1;
        n += m & ~p;
        p = m;
    }
    *prev = p;
    return n;
}

/* Serial mode: watch for the DC driving its CTS pin (SPI clock). */
static void serial_watch(void) {
    uint32_t edges = 0, t_first = 0, prev = 0;
    uint32_t last_alive = now_us();
    for (;;) {
        uint32_t ref = (sio_hw->gpio_out >> PIN_RTS) & 1;   /* what we drive through 1k */
        uint32_t n = clk_burst(ref, &prev);
        uint32_t t = now_us();
        core1_beat++;
        if (dc_alive()) last_alive = t;
        else if (t - last_alive > dc_offline_us()) { enter_offline(); return; }
        if (dc_force >= FORCE_SPI) { enter_spi(); return; }
        if (((sio_hw->gpio_out >> PIN_RTS) & 1) != ref || t - rts_stamp < 50) { prev = 0; continue; }  /* our own RTS edge */
        if (!n) continue;
        /* SPI = a fast burst of clock pulses (KOS: 80 for SD init, 32 per W5500
           frame). Games may drive CTS themselves (AFO does), so a held level or
           a few slow toggles must not count. */
        if (edges == 0 || t - t_first > 1000) { edges = 0; t_first = t; }
        edges += n;
        if (edges >= 16 && dc_force != FORCE_SERIAL) { enter_spi(); return; }
    }
}


static uint32_t cap_words;                          /* words of this frame so far */
#if DC_DIAG
static uint8_t cap_d[2], cap_miso;                  /* first data bytes / first MISO byte, for the frame list */
#endif

/* ---- fast path for the header ----------------------------------------------
   Per block-select value: where its bytes live and the address mask, so the
   first MISO byte is one table lookup (no switch, no function call). Same
   mapping as w5_read(): common, socket registers, socket 0 TX/RX buffers,
   anything else reads 0. */
static const uint16_t mask63 = 63, mask0 = 0;
static uint8_t zero_byte;
static struct { uint8_t *base; const volatile uint16_t *mask; } w5_map[32];
static void w5_map_init(void) {
    for (int b = 0; b < 32; b++) { w5_map[b].base = &zero_byte; w5_map[b].mask = &mask0; }
    w5_map[0].base = w5.common; w5_map[0].mask = &mask63;
    for (int s = 0; s < 8; s++) { w5_map[s * 4 + 1].base = w5.sreg[s]; w5_map[s * 4 + 1].mask = &mask63; }
    w5_map[2].base = w5.s0tx; w5_map[2].mask = &w5.tx_mask;
    w5_map[3].base = w5.s0rx; w5_map[3].mask = &w5.rx_mask;
}

/* ---- the header: answer within ~1/4 us -----------------------------------
   The address arrives after 16 bits; while the 5 block-select bits come in,
   the answer for every block select is looked up (common, the 8 sockets'
   registers, socket 0 TX and RX buffers; everything else reads 0). At the 21st
   bit the answer is then one indexed load and store. */
#if DC_DIAG
/* late-answer diagnostics (core1 SysTick, CPU cycles) */
static uint32_t dbg_t_addr, dbg_lvl, dbg_path, dbg_cyc;
static uint32_t dbg_mark_cyc, dbg_passes, dbg_booked, dbg_since, dbg_p, dbg_b;   /* since the last frame end */
static inline uint32_t cyc_now(void) { return systick_hw->cvr; }        /* counts down, 24 bit */
static inline uint32_t rx_level(void) { return (SPI_PIO->flevel >> (PIO_FLEVEL_RX0_LSB + SPI_SM * 8)) & 0xF; }
#endif
static uint32_t w5_cand[4];                          /* MISO words (byte << 24), bsb 0..3 */

/* The four blocks KOS reads: common, socket 0 registers, socket 0 TX and RX
   buffers. This is all that runs between the address word and the block
   select (5 header bits, as little as ~400 ns). */
static inline __attribute__((always_inline)) void w5_cand4(uint32_t a) {
    uint32_t r = a & 63;
    uint32_t *c = w5_cand;
    c[0] = (uint32_t)w5.common[r] << 24;
    c[1] = (uint32_t)w5.sreg[0][r] << 24;
    c[2] = (uint32_t)w5.s0tx[a & w5.tx_mask] << 24;
    c[3] = (uint32_t)w5.s0rx[a & w5.rx_mask] << 24;
}

/* Any other block (sockets 1-7, reserved): the table path. KOS only writes
   those, where MISO doesn't matter. */
static inline uint32_t w5_other(uint32_t bsb, uint32_t a) {
    return (uint32_t)w5_map[bsb].base[a & *w5_map[bsb].mask] << 24;
}

#if DC_DIAG
/* Diagnostics for the late-read report; only called once the candidates are
   ready, so it never delays the answer. */
static inline __attribute__((always_inline)) void w5_addr_dbg(uint32_t t, uint32_t lvl, uint32_t path) {
    dbg_t_addr = t; dbg_lvl = lvl; dbg_path = path;
    dbg_since = (dbg_mark_cyc - t) & 0xFFFFFF; dbg_p = dbg_passes; dbg_b = dbg_booked;
}
#endif

static inline __attribute__((always_inline)) void w5_addr_word(uint32_t v) {
    uint32_t a = v & 0xFFFF;
    w5_cand4(a);                                     /* first: the answer depends on it */
#if DC_DIAG
    w5_addr_dbg(cyc_now(), rx_level(), 1);           /* via the main loop */
#endif
    w5.addr = w5.addr0 = (uint16_t)a;
    w5.phase = W5P_BSB;
    cap_words = 1;
}

static bool w5_late;                                 /* this frame's first answer missed its pull */
/* late: the shifter had already pulled byte 0 (it went out as ff), so nothing
   was queued for it; the control-word handler queues bytes 1.. and the rest of
   the frame stays aligned. */
static inline __attribute__((always_inline)) void w5_bsb_done(uint32_t bsb, uint32_t m, bool late) {
    w5_late = late;
#if DC_DIAG
    if (late) dbg_cyc = (dbg_t_addr - cyc_now()) & 0xFFFFFF;
#endif
    w5.bsb = (uint8_t)bsb; w5.raddr = (uint16_t)(w5.addr + 1); w5.phase = W5P_CTL;
#if DC_DIAG
    cap_miso = (uint8_t)(m >> 24);
#else
    (void)m;
#endif
    cap_words = 2;
}

/* The control word is pushed one PIO instruction before byte 0 is pulled: if it
   is already in the RX FIFO, byte 0 is gone (or about to be). */
static inline bool w5_too_late(void) { return !(SPI_PIO->fstat & W5_RXEMPTY); }

static inline __attribute__((always_inline)) void w5_bsb_word(uint32_t v) {
    uint32_t bsb = v & 31;
    uint32_t m = bsb < 4 ? w5_cand[bsb] : w5_other(bsb, w5.addr);
    bool late = w5_too_late();
    if (!late) SPI_PIO->txf[SPI_SM] = m;
    w5_bsb_done(bsb, m, late);
}

/* One word of a W5500 frame (outside the header fast path). */
static inline __attribute__((always_inline)) void fast_word(uint32_t v) {
    if (w5.phase == W5P_HDR) { w5_addr_word(v); return; }
    if (w5.phase == W5P_BSB) { w5_bsb_word(v); return; }
    switch (w5.phase) {
    case W5P_CTL:
        w5_hdr_ctl(v); cap_words = 3;
        break;
    case W5P_DATA:
        /* Read burst: 3 MISO bytes are queued ahead; an empty TX FIFO here means
           the shifter already sent ff for a byte (CPU fell behind). */
        if (!w5.write && (SPI_PIO->fstat & W5_TXEMPTY)) spi_stats.underruns++;
#if DC_DIAG
        if (cap_words < 5) cap_d[cap_words - 3] = (uint8_t)v;
#endif
        cap_words++;
        w5_data((uint8_t)v);
        break;
    default:                                          /* W5P_SKIP: keep the shifter going */
        W5_PUSH(0xFF);
        break;
    }
}

/* W5500 mode: look for an SD command at the start of the frame. */
static void sd_detect(uint8_t b) {
    if (det_n == 0) {
        if (b == 0xFF) { if (++det_ff > 32) det_on = false; return; }
        /* >= 2 x 0xFF: KOS sd_wait_ready() always clocks two or more. A W5500
           frame can start with one (an RX/TX buffer address 0xFFxx); with only
           one required, some of those read bursts were taken for SD commands
           (net7, RuneScape: 4 in 8 min, each one corrupting a received packet). */
        if (det_ff < 2 || (b & 0xC0) != 0x40) { det_on = false; return; }
    }
    det_buf[det_n++] = b;
    if (det_n < 6) return;
    det_on = false;
    if (((sd_crc7(det_buf, 5) << 1) | 1) != det_buf[5]) return;
    sd_w5_words = 3;                                  /* this frame is SD now, to its end */
    use_proto(P_SD);
    sd_frame_bytes = 1 + det_ff + 6;
    for (int i = 0; i < 6; i++) sd_rx(det_buf[i]);
}

static inline void short_delay(void) { for (volatile int i = 0; i < 24; i++) { } }

static uint32_t cap_t;

static void sd_restart(void) {
    hw_clear_bits(&SD_PIO->ctrl, 1u << sd_sm);
    dma_channel_abort(sd_dma);
    pio_sm_clear_fifos(SD_PIO, sd_sm);
    pio_sm_restart(SD_PIO, sd_sm);
    pio_sm_exec(SD_PIO, sd_sm, pio_encode_jmp(sd_off + sd_spi_offset_start));
    sd_set_fill(sd_fill);
    hw_set_bits(&SD_PIO->ctrl, 1u << sd_sm);
}

/* ---- W5500 end of frame ----------------------------------------------------
   In KOS's hw_init two register writes follow each other with only a mutex
   unlock/lock between them, and writes clock faster than reads: the next
   frame's block select can arrive ~2 us after the marker. So the marker does
   only what can't wait (drop the stale MISO bytes, latch the late flag, reset
   the frame state) and queues a record; stats, the frame list, w5_refresh()
   and the SD shifter realign run later from the main loop (w5_book), when no
   header is coming in. */

static inline __attribute__((always_inline)) void w5_mark(bool aborted) {
    w5_tx_flush();
    uint32_t late = w5_late && w5.phase >= W5P_CTL && !w5.write;   /* late writes are harmless */
#if DC_DIAG
    if (late) {
        uint32_t i = spi_stats.late_n;
        if (i < 8) {
            spi_stats.late_dbg[i].addr = w5.addr0; spi_stats.late_dbg[i].bsb = w5.bsb;
            spi_stats.late_dbg[i].path = (uint8_t)dbg_path; spi_stats.late_dbg[i].waiting = (uint8_t)dbg_lvl;
            spi_stats.late_dbg[i].cyc = (uint16_t)(dbg_cyc > 65535 ? 65535 : dbg_cyc);
            spi_stats.late_dbg[i].since = dbg_since;
            spi_stats.late_dbg[i].passes = (uint8_t)(dbg_p > 255 ? 255 : dbg_p);
            spi_stats.late_dbg[i].booked = (uint8_t)(dbg_b > 255 ? 255 : dbg_b);
            spi_stats.late_n = i + 1;
        }
    }
#endif
    w5_late = false;
    uint32_t t = timer_hw->timerawl;
    if (aborted && w5.phase == W5P_HDR && t - t_mark > 200) {
        spi_stats.short_frames++;                     /* CS noise: count it, nothing to list */
    } else if (book_h - book_t < BOOK_N) {
        w5_rec_t *r = &book[book_h++ % BOOK_N];
        r->addr0 = w5.addr0; r->addr = w5.addr; r->bsb = w5.bsb; r->write = w5.write;
        r->phase = w5.phase;
#if DC_DIAG
        r->words = (uint8_t)(cap_words > 255 ? 255 : cap_words);
        r->d0 = cap_d[0]; r->d1 = cap_d[1]; r->miso = cap_miso;
#endif
        r->late = late != 0; r->aborted = aborted; r->t = t;
    } else spi_stats.book_lost++;
    w5.phase = W5P_HDR;
    cap_words = 0;
#if DC_DIAG
    cap_d[0] = cap_d[1] = 0;
#endif
    sd_frame_bytes = 0;
    det_on = false;                     /* armed again once the SD shifter is realigned (w5_book) */
    book_refresh = true;
    t_mark = t;
#if DC_DIAG
    dbg_mark_cyc = cyc_now(); dbg_passes = 0; dbg_booked = 0;
#endif
}

/* One queued frame's bookkeeping; then, with the queue empty, the refresh. */
static void w5_book(void) {
#if DC_DIAG
    dbg_booked++;
#endif
    if (book_t == book_h) {
        if (!book_refresh) return;
        book_refresh = false;
        if (!sd_locked && w5.sock_open) {            /* a game is online: no more SD (see sd_locked) */
            sd_locked = true; spi_stats.sd_locked = 1;
            det_on = false;
        }
        /* The SD shifter only listens in W5500 mode; realign it to the frame
           (only while CS is high, or it would start mid-byte). */
        if (!dma_channel_is_busy(sd_dma) && cs_high()) {
            hw_clear_bits(&SD_PIO->ctrl, 1u << sd_sm);
            hw_xor_bits(&SD_PIO->sm[sd_sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);   /* clears both FIFOs */
            hw_xor_bits(&SD_PIO->sm[sd_sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
            hw_set_bits(&SD_PIO->ctrl, 1u << (PIO_CTRL_SM_RESTART_LSB + sd_sm));
            SD_PIO->sm[sd_sm].instr = pio_encode_jmp(sd_off + sd_spi_offset_start);
            SD_PIO->sm[sd_sm].instr = sd_fill ? pio_encode_mov_not(pio_x, pio_null) : pio_encode_mov(pio_x, pio_null);
            hw_set_bits(&SD_PIO->ctrl, 1u << sd_sm);
            /* SD detection only on a frame the SD shifter sees from its first
               byte. Without the realign (back-to-back frames) its FIFO still
               holds bytes of earlier frames, and in net7 that stale mix was
               now and then taken for an SD command (RuneScape: 4 in 8 min).
               CS still high after the restart: the shifter is surely waiting
               for the next frame's start, not half-way into it. */
            det_on = cs_high() && !sd_locked && dc_force != FORCE_W5500; det_n = 0; det_ff = 0;
        }
        w5_refresh();
        return;
    }
    w5_rec_t *r = &book[book_t % BOOK_N];
    bool skip = r->phase == W5P_SKIP;
    bool real = !r->aborted && r->phase == W5P_DATA;
    spi_stats.last_t = r->t;
    if (r->late && !skip) spi_stats.late_frames++;
#if DC_DIAG
    uint32_t ci = spi_stats.cap_n;
    /* the frame list: real frames; and once the driver is past its probe
       (3 common-block accesses), aborted ones too, to see where it goes wrong */
    if (ci < CAP_N && !skip && (real || ci < 4 || spi_stats.cap_common >= 3)) {
        spi_stats.cap[ci].addr = r->addr0;
        spi_stats.cap[ci].b2 = (uint8_t)(r->bsb << 3 | (r->write ? 4 : 0));
        spi_stats.cap[ci].words = r->words;
        spi_stats.cap[ci].d0 = r->d0; spi_stats.cap[ci].d1 = r->d1;
        spi_stats.cap[ci].miso = r->miso;
        spi_stats.cap[ci].late = (uint8_t)(r->late | (real ? 0 : 2));
        spi_stats.cap[ci].dt_us = (uint16_t)(ci ? (r->t - cap_t > 65535 ? 65535 : r->t - cap_t) : 0);
        cap_t = r->t;                              /* bus time of the frame end */
        spi_stats.cap_n = ci + 1;
    }
#endif
    if (!real) spi_stats.short_frames++;
    else {
        spi_stats.w5_frames++;
#if DC_DIAG
        if (r->bsb == 0 && r->addr0 < 0x40) { spi_stats.common_seq++; spi_stats.cap_common++; }
#endif
        if (!r->write && r->bsb == 0) {
            if (r->addr0 <= W5C_VERSIONR && r->addr > W5C_VERSIONR) spi_stats.ver_reads++;
            else spi_stats.other_reads++;
        }
    }
    book_t++;
}

/* SD end of frame */
static void sd_frame_done(void) {
    while (!(SD_PIO->fstat & SD_RXEMPTY)) sd_rx((uint8_t)SD_PIO->rxf[sd_sm]);
    sd_frame_end();
    spi_stats.sd_frames++;
    sd_restart();
    w5.phase = W5P_HDR;
    sd_frame_bytes = 0;
    sd_w5_words = 0;
    det_on = !sd_locked && dc_force != FORCE_W5500; det_n = 0; det_ff = 0;
    w5_refresh();
}

static volatile uint32_t frames_done;

/* A frame ended (marker from the shifter, or 'aborted' by the watchdog). */
static void frame_end(bool aborted) {
    frames_done++;
    if (proto == P_W5500) {
        w5_mark(aborted);
    } else {
        sd_frame_done();
    }
}

/* CS low and no header yet: wait for the address and block-select words in a
   tight loop and answer at once. Returns false after ~200 us without a word
   (CS floating, not a frame), so the idle checks still run. */
static bool __attribute__((noinline)) w5_header(void) {
    io_ro_32 *fst = &SPI_PIO->fstat, *gin = &sio_hw->gpio_in;
    const uint32_t rxe = W5_RXEMPTY, cs = 1u << PIN_CTS;
    uint32_t n = 40000;
    uint32_t a = 0;
#if DC_DIAG
    uint32_t t_addr = 0, lvl = 0;
#endif
    bool fresh = false;                               /* address word read here */
    if (w5.phase == W5P_HDR) {
        while (*fst & rxe) {
            if ((*gin & cs) || !--n) return n != 0;
        }
        uint32_t v = SPI_PIO->rxf[SPI_SM];
        if (v == W5_MARK) { frame_end(false); return true; }
        a = v & 0xFFFF;
        w5_cand4(a);                                  /* the only work before the answer */
#if DC_DIAG
        t_addr = cyc_now(); lvl = rx_level();         /* two loads; the stores wait */
#endif
        fresh = true;
    }
    /* The 5 block-select bits, then the answer: ~12 cycles from the word
       reaching the FIFO to the MISO byte in the TX FIFO. Gives up when CS goes
       high (a spike or a short frame: the watchdog's job) or after ~200 us. */
    uint32_t f, v;
    /* fstat bit 8 = RXEMPTY(SM0), gpio_in bit 5 = CS; both tested by shifting
       them into the sign bit. Offsets: FSTAT 0x04, TXF0 0x10, RXF0 0x20. */
    _Static_assert(SPI_SM == 0 && PIN_CTS == 5 && PIO_FSTAT_RXEMPTY_LSB == 8, "asm below assumes these");
    __asm volatile(
        ".syntax unified             \n"
        "1: ldr  %[f], [%[pio], #4]  \n"
        "   lsls %[f], %[f], #23     \n"
        "   bpl  2f                  \n"     /* RX not empty */
        "   ldr  %[f], [%[gin]]      \n"
        "   lsls %[f], %[f], #26     \n"
        "   bmi  3f                  \n"     /* CS high */
        "   subs %[n], #1            \n"
        "   bne  1b                  \n"
        "   movs %[f], #1            \n"     /* timed out: f != 0 */
        "   b    3f                  \n"
        "2: ldr  %[v], [%[pio], #32] \n"
        "   lsls %[f], %[v], #27     \n"
        "   lsrs %[f], %[f], #27     \n"     /* bsb */
        "   cmp  %[f], #3            \n"
        "   bhi  4f                  \n"     /* not one of the four: table path */
        "   lsls %[f], %[f], #2      \n"
        "   ldr  %[f], [%[cand], %[f]] \n"
        "   ldr  %[n], [%[pio], #4]  \n"     /* control word already pushed? */
        "   lsls %[n], %[n], #23     \n"
        "   bpl  5f                  \n"     /* yes: byte 0 is gone, don't queue it */
        "   str  %[f], [%[pio], #16] \n"
        "   movs %[f], #0            \n"     /* f = 0: answered in time */
        "   b    3f                  \n"
        "5: movs %[f], #3            \n"     /* f = 3: too late */
        "   b    3f                  \n"
        "4: movs %[f], #2            \n"
        "3:                          \n"
        ".syntax divided             \n"
        : [f] "=&l" (f), [v] "=&l" (v), [n] "+l" (n)
        : [pio] "l" (SPI_PIO), [gin] "l" (gin), [cand] "l" (w5_cand)
        : "cc", "memory");
    if (fresh) {                                      /* frame state, after the answer */
#if DC_DIAG
        w5_addr_dbg(t_addr, lvl, 0);
#endif
        w5.addr = w5.addr0 = (uint16_t)a;
        w5.phase = W5P_BSB;
        cap_words = 1;
    }
    if (f == 2) { w5_bsb_word(v); return true; }      /* other block */
    if (f == 3) { w5_bsb_done(v & 31, w5_cand[v & 31], true); return true; }
    if (f) return n != 0;                             /* not answered */
    w5_bsb_done(v & 31, w5_cand[v & 31], false);
    return true;
}

/* ---- frame engine -------------------------------------------------------------
   Handles whole frames back to back (header + answer, control word, data,
   end marker) without going round the main loop between words: with KOS
   builds that leave only ~0.2 us between frames, the main loop's per-word
   housekeeping was enough to fall a frame behind and stay there. Returns
   0 when the bus is idle (CS high, nothing queued), 1 after ~200 us without a
   word while CS is low (CS floating, or an aborted frame for the watchdog),
   2 when the frame turned out to be for the SD card. */
static int __attribute__((noinline)) w5_frames(void) {
    io_ro_32 *fst = &SPI_PIO->fstat, *gin = &sio_hw->gpio_in;
    for (;;) {
        if (w5.phase <= W5P_BSB) {
            if (!w5_header()) return 1;
            if (w5.phase <= W5P_BSB) {                /* header not complete */
                if ((*fst & W5_RXEMPTY) && (*gin & (1u << PIN_CTS))) return 0;
                continue;
            }
        }
        /* control word, data bytes, end marker */
        uint32_t spins = 0;
        for (;;) {
            if (!(*fst & W5_RXEMPTY)) {
                uint32_t v = SPI_PIO->rxf[SPI_SM];
                spins = 0;
                if (v == W5_MARK) { frame_end(false); break; }
                fast_word(v);
                continue;
            }
            if (det_on && w5.phase == W5P_DATA && !(SD_PIO->fstat & SD_RXEMPTY)) {
                sd_detect((uint8_t)SD_PIO->rxf[sd_sm]);
                if (proto != P_W5500) return 2;
                continue;
            }
            if (++spins > 40000) return 1;
        }
        /* next frame already under way? carry on; otherwise back to the main loop */
        if ((*fst & W5_RXEMPTY) && (*gin & (1u << PIN_CTS))) return 0;
    }
}

/* ---- stuck-shifter watchdog -------------------------------------------------
   The program checks CS only between whole bytes, which is all a real frame
   ever ends on. CS can also rise mid-byte (a frame shorter than the 3-byte
   header, an aborted transfer, or the floating CS line after the DC lets go of
   the port); the shifter then sits at a clock wait with CS high. Restart it. */
static void w5_watchdog(void) {
    if (!cs_high()) return;
    uint pc = w5_pc();
    if (pc != w5500_spi_offset_hdr + 1 && pc != w5500_spi_offset_hdr &&
        pc != w5500_spi_offset_bits + 1 && pc != w5500_spi_offset_bits + 2) return;
    short_delay();
    if (!cs_high() || w5_pc() != pc) return;
    /* this frame's words first, then restart (CS is high: at 'start') */
    while (!(SPI_PIO->fstat & W5_RXEMPTY)) {
        uint32_t v = SPI_PIO->rxf[SPI_SM];
        if (v == W5_MARK) frame_end(false);
        else if (proto == P_W5500) fast_word(v);
        else sd_w5_words++;
    }
    w5_sm_start(false);
    frame_end(true);
}

/* ---- W5500 timing ----------------------------------------------------------
   KOS bit-bangs a register read in ~4 us (130 ns per bit), with CS high for as
   little as a few hundred ns between frames. MISO must be queued within ~3.5 bit
   times of the 21st header bit; KOS clocks each byte faster than its average
   rate and pauses between bytes, so that can be as little as ~250 ns. So core1
   runs with interrupts off and polls the shifter's RX FIFO first thing in every
   pass of its loop; while a
   W5500 header is coming in it does nothing else. Everything else is kept short
   and only runs between frames or in the data phase (where 3 MISO bytes are
   queued ahead). */
static void spi_service(void) {
    uint32_t last_act = now_us(), last_probe = last_act, last_alive = last_act;
    uint32_t seen_frames = frames_done;
    int released = 0;
    uint32_t tick = 0, hdr_spin = 0;
    uint32_t irq = save_and_disable_interrupts();
#if DC_DIAG
    systick_hw->rvr = 0x00FFFFFF; systick_hw->cvr = 0; systick_hw->csr = 0x5;   /* CPU clock, no interrupt */
#endif
    io_ro_32 *gin = &sio_hw->gpio_in;
    io_ro_32 *fst = &SPI_PIO->fstat;
    for (;;) {
        if (!(*fst & W5_RXEMPTY)) {
            if (proto == P_W5500) {
                hdr_spin = w5_frames() == 1 ? 40000 : 0;
            } else {
                uint32_t v = SPI_PIO->rxf[SPI_SM];
                if (v == W5_MARK) frame_end(false);
                else sd_w5_words++;
                hdr_spin = 0;
            }
            continue;
        }
        /* In a W5500 header: poll only. After ~200 us of CS low without a word,
           the DC has let go of the line (CS floats): run the checks below too. */
        if (proto == P_W5500 && !(*gin & (1u << PIN_CTS)) && hdr_spin < 40000) {
            hdr_spin = w5_frames() == 1 ? 40000 : 0;
            continue;
        }
        if (*gin & (1u << PIN_CTS)) hdr_spin = 0;
#if DC_DIAG
        dbg_passes++;
#endif

        if (proto == P_W5500) {
            /* SD probe check once the W5500 header is done (no tight timing then) */
            if (det_on && w5.phase == W5P_DATA && !(SD_PIO->fstat & SD_RXEMPTY)) {
                sd_detect((uint8_t)SD_PIO->rxf[sd_sm]);
                continue;
            }
        } else {
            if (!(SD_PIO->fstat & SD_RXEMPTY)) {
                uint8_t b = (uint8_t)SD_PIO->rxf[sd_sm];
                if (sd_frame_bytes++ == 0 && b != 0xFF && dc_force != FORCE_SD) {   /* not an SD host: W5500 */
                    sd_frame_end();
                    use_proto(P_W5500);
                    continue;
                }
                sd_rx(b);
                sd_poll();
                continue;
            }
            sd_poll();
        }
        w5_watchdog();
        /* Slow work only when no header can be close: the bus idle (CS high
           >= 10 us since the last frame; back-to-back frames have well under
           1 us of CS high), or deep in a long data burst. */
        bool idle = (*gin & (1u << PIN_CTS)) && now_us() - t_mark >= 10;
        if (proto == P_W5500 && (idle || (w5.phase == W5P_DATA && cap_words >= 7))) w5_book();
        if (frames_done != seen_frames) { seen_frames = frames_done; last_act = now_us(); }
        /* idle */
        if ((++tick & 255) == 0) {
            core1_beat++;                          /* cheap power-off check */
            uint32_t t = now_us();
            if (dc_alive()) last_alive = t;
            else if (t - last_alive > dc_offline_us()) { restore_interrupts(irq); enter_offline(); return; }
        }
        if (w5.phase != W5P_HDR || !idle) continue;   /* in a frame or between close frames */
        static uint32_t last_ref;
        if (now_us() - last_ref >= 20) {   /* keep RX size etc. current (also done per frame) */
            last_ref = now_us();
            if (*gin & (1u << PIN_CTS)) w5_refresh();
        }
        if (dc_force == FORCE_SERIAL) { restore_interrupts(irq); enter_serial(); return; }
        if (dc_force == FORCE_W5500 && proto != P_W5500) use_proto(P_W5500);
        if (dc_force == FORCE_SD && proto != P_SD) use_proto(P_SD);
        if (dc_force >= FORCE_SPI) continue;                 /* stay in SPI */
        uint32_t t = now_us();
        if (t - last_act > 5000 && t - last_probe > 10000) {
            last_probe = t;
            sio_hw->gpio_set = 1u << PIN_RTS;
            sio_hw->gpio_oe_set = 1u << PIN_RTS;
            short_delay();
            bool hi = sio_hw->gpio_in & (1u << PIN_DC_CTS_SENSE);
            sio_hw->gpio_clr = 1u << PIN_RTS;
            short_delay();
            bool lo = !(sio_hw->gpio_in & (1u << PIN_DC_CTS_SENSE));
            sio_hw->gpio_oe_clr = 1u << PIN_RTS;
            if (hi && lo) {
                if (++released >= 20) { restore_interrupts(irq); enter_serial(); return; }
            } else {
                released = 0;
            }
        }
    }
}

/* Offline: all pins high-Z until the DC is up. Goes straight to SPI mode if the
   DC is already clocking the bus (e.g. an SD probe right after boot). */
/* Rising edges on GPIO2 (SPI clock) and GPIO3 (DC TXD) in one sampling burst. */
static inline void __attribute__((always_inline)) edge_burst(uint32_t *prev, uint32_t *clk_n, uint32_t *txd_n) {
    uint32_t c = 0, x = 0, p = *prev;
    for (int i = 0; i < 256; i++) {
        uint32_t in = sio_hw->gpio_in;
        uint32_t r = in & ~p;
        c += (r >> PIN_DC_CTS_SENSE) & 1;
        x += (r >> PIN_RX) & 1;
        p = in;
    }
    *prev = p; *clk_n = c; *txd_n = x;
}

static void offline_watch(void) {
    uint32_t since = 0, edges = 0, t_edge0 = 0, prev = sio_hw->gpio_in;
    uint32_t tx_edges = 0, t_tx0 = 0;
    bool up = false;
    for (;;) {
        uint32_t n, tn;
        edge_burst(&prev, &n, &tn);
        uint32_t t = now_us();
        core1_beat++;
        /* Serial data from the DC (e.g. a game sending "AT" while it otherwise
           holds TXD low, as AFO does): >= 20 TXD edges within 100 ms. */
        if (tn) {
            if (tx_edges == 0 || t - t_tx0 > 100000) { tx_edges = 0; t_tx0 = t; }
            tx_edges += tn;
            if (tx_edges >= 20) {
                if (dc_force >= FORCE_SPI) enter_spi(); else enter_serial();
                return;
            }
        }
        if (dc_alive()) {
            if (!up) { up = true; since = t; edges = 0; }
            if (n) {
                if (edges == 0 || t - t_edge0 > 1000) { edges = 0; t_edge0 = t; }
                edges += n;
                if (edges >= 16 && dc_force != FORCE_SERIAL) { enter_spi(); return; }   /* SPI clock burst */
            }
            if (t - since >= dc_online_delay_us()) {
                if (dc_force >= FORCE_SPI) enter_spi(); else enter_serial();
                return;
            }
        } else {
            up = false;
        }
    }
}


void core1_main(void) {
    for (;;) {
        switch (dc_mode) {
        case MODE_OFFLINE: offline_watch(); break;
        case MODE_SERIAL:  serial_watch();  break;
        default:           spi_service();   break;
        }
    }
}
