// DC-PicoLink probe: can an ESP32-S3 alone answer the Dreamcast's W5500 SPI?
//
// No wiring and no Dreamcast needed. Flash it on a bare ESP32-S3 board and read the USB
// serial port. One core plays the Dreamcast (the SPI master peripheral); the other core is
// the candidate W5500 emulation: a bit-banged SPI slave that uses the ESP32-S3's "dedicated
// GPIO" CPU instructions, with interrupts off, the way core1 of the RP2040 runs. Both use the
// same four pads (GPIO4-7), so the signals go out through the pad drivers and back in through
// the input buffers like real ones.
//
// What it measures:
//  1. Pauses of the slave's core (a loop that reads the cycle counter), Wi-Fi off and on.
//  2. Frames at rising clock speeds, with even and with lopsided clocks (KOS's clock has
//     phases down to ~40 ns: like 12.5 MHz), Wi-Fi off and on.
//  3. A long run at the speeds that matter, Wi-Fi on.
// What it does not answer: the Dreamcast's own timing quirks. hardware/ESP32-ONLY.md.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "hal/gpio_ll.h"
#include "soc/spi_periph.h"
#include "esp_rom_gpio.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "xtensa/xtruntime.h"

#define P_CLK  4
#define P_MOSI 5
#define P_MISO 6
#define P_CS   7

static DRAM_ATTR uint8_t g_mem[4 * 256 + 32];   // [block select 0-3][address low byte]
static DRAM_ATTR volatile uint32_t g_frames;
static DRAM_ATTR volatile int g_run = 1;
static volatile int g_phase;                    // main -> core 1: what to do next
static volatile int g_done;                     // core 1 -> main: finished phase N

// Pin access for dcspi_bb.h: the ESP32-S3's dedicated-GPIO instructions (no bus access).
// Input bits: 0 = MOSI, 1 = clock, 2 = chip select. Output bit 0 = MISO.
// The waits are zero-overhead loops: 3 cycles per look at the pins (2 for DC_FALL).
#define DCSPI_ATTR IRAM_ATTR __attribute__((noinline, optimize("no-branch-count-reg")))
#define DCSPI_OM   const uint32_t om = 1; uint32_t lc = 0; asm volatile("" : "+r"(lc));
#define DC_RISE_CS(v, l) asm goto(" ee.get_gpio_in %0\n1: loop %1, 9f\n bbsi %0, 1, 2f\n bbsi %0, 2, 8f\n" \
                                  " ee.get_gpio_in %0\n9: j 1b\n8: j %l[" #l "]\n2:\n" : "=&r"(v) : "r"(lc) : : l)
#define DC_FALL()   do { uint32_t t_; asm volatile(" ee.get_gpio_in %0\n1: loop %1, 9f\n bbci %0, 1, 2f\n" \
                                                   " ee.get_gpio_in %0\n9: j 1b\n2:\n" : "=&r"(t_) : "r"(lc)); } while (0)
#define DC_OUT(w)   dedic_gpio_cpu_ll_write_mask(om, (w) >> 31)
#define DC_IDLE()   dedic_gpio_cpu_ll_write_mask(om, om)
#define DC_WAIT_CS_HIGH() do { uint32_t t_; asm volatile("1:\n ee.get_gpio_in %0\n bbci %0, 2, 1b\n" : "=r"(t_)); } while (0)
#define DC_WAIT_CS_LOW()  do { uint32_t t_; asm volatile("1:\n ee.get_gpio_in %0\n bbsi %0, 2, 1b\n" : "=r"(t_)); } while (0)
#define DC_KEEP(x)  asm volatile("" : "+r"(x))
#include "dcspi_bb.h"

static DRAM_ATTR uint8_t s_cap[DCSPI_CAP];

// Pauses of this core: a loop of a few cycles that reads the cycle counter.
typedef struct { uint32_t max, n16, n64, n256, n1k; } jit_t;
static jit_t g_jit[5];
#define JIT_CYCLES 1200000000u               // 5 s at 240 MHz

static void IRAM_ATTR __attribute__((noinline)) jitter(uint32_t total, jit_t *out)
{
    uint32_t start = esp_cpu_get_cycle_count(), prev = start, max = 0, n16 = 0, n64 = 0, n256 = 0, n1k = 0, c;
    do {
        c = esp_cpu_get_cycle_count();
        uint32_t d = c - prev;
        prev = c;
        if (d >= 16) {
            n16++;
            if (d > max) max = d;
            if (d >= 64) { n64++; if (d >= 256) { n256++; if (d >= 1024) n1k++; } }
        }
    } while (c - start < total);
    out->max = max; out->n16 = n16; out->n64 = n64; out->n256 = n256; out->n1k = n1k;
}

static void wait_phase(int n) { while (g_phase < n) vTaskDelay(1); }

static void core1_task(void *arg)
{
    static const int in_pins[] = { P_MOSI, P_CLK, P_CS };
    static const int out_pins[] = { P_MISO };
    dedic_gpio_bundle_handle_t bin = NULL, bout = NULL;
    dedic_gpio_bundle_config_t ci = { .gpio_array = in_pins, .array_size = 3, .flags = { .in_en = 1 } };
    dedic_gpio_bundle_config_t co = { .gpio_array = out_pins, .array_size = 1, .flags = { .out_en = 1 } };
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&ci, &bin));
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&co, &bout));
    uint32_t off = 99, om = 0;
    dedic_gpio_get_in_offset(bin, &off);
    dedic_gpio_get_out_mask(bout, &om);
    // the master still reads its MISO from the same pad
    gpio_ll_input_enable(&GPIO, P_MISO);
    esp_rom_gpio_connect_in_signal(P_MISO, spi_periph_signal[SPI2_HOST].spiq_in, false);
    if (off != 0 || om != 1) {
        printf("probe: unexpected dedicated GPIO layout (in offset %u, out mask %x)\n", (unsigned)off, (unsigned)om);
        g_done = -1;
        vTaskDelete(NULL);
    }
    uint32_t old;
    // step 1: pauses with Wi-Fi off
    old = XTOS_SET_INTLEVEL(5); jitter(JIT_CYCLES, &g_jit[0]); XTOS_RESTORE_INTLEVEL(old);
    g_done = 1;
    // step 2: the slave, until main stops it
    wait_phase(2);
    old = XTOS_SET_INTLEVEL(5); dcspi_bb_responder(g_mem, s_cap, &g_frames, &g_run); XTOS_RESTORE_INTLEVEL(old);
    g_done = 2;
    // steps 3-6: pauses under four kinds of Wi-Fi activity (main sets each up)
    for (int i = 1; i <= 4; i++) {
        wait_phase(2 + i);
        old = XTOS_SET_INTLEVEL(5); jitter(JIT_CYCLES, &g_jit[i]); XTOS_RESTORE_INTLEVEL(old);
        g_done = 2 + i;
    }
    // step 7: the slave again, for good
    wait_phase(7);
    g_run = 1;
    old = XTOS_SET_INTLEVEL(5); dcspi_bb_responder(g_mem, s_cap, &g_frames, &g_run); XTOS_RESTORE_INTLEVEL(old);
    vTaskDelete(NULL);
}

static spi_device_handle_t s_dev;

// duty: share of the bit that the clock is high, in 1/256 (128 = even)
static int master_open(int hz, int duty)
{
    spi_device_interface_config_t dc = {
        .mode = 0, .clock_speed_hz = hz, .duty_cycle_pos = (uint16_t)duty, .spics_io_num = P_CS, .queue_size = 1,
    };
    if (spi_bus_add_device(SPI2_HOST, &dc, &s_dev) != ESP_OK) return -1;
    int khz = 0;
    spi_device_get_actual_freq(s_dev, &khz);
    return khz;
}

static bool wait_frame(uint32_t before)
{
    int64_t t0 = esp_timer_get_time();
    while (g_frames == before)
        if (esp_timer_get_time() - t0 > 2000) return false;
    return true;
}

typedef struct { uint32_t frames, wr, rd_sent, rd_early, first, other, lost; } result_t;

static void run(int frames, result_t *r)
{
    static uint8_t tx[64], rx[64], want[64];
    memset(r, 0, sizeof *r);
    for (int i = 0; i < frames; i++) {
        uint32_t rnd = esp_random();
        unsigned bsb = rnd & 3, addr = (rnd >> 2) & 0xFFFF, len = 1 + ((rnd >> 18) % 24);
        unsigned base = bsb << 8, lo = addr & 0xFF;
        if (lo > 228) { lo = 228; addr = (addr & 0xFF00) | lo; }   // the model doesn't wrap
        // write
        tx[0] = addr >> 8; tx[1] = addr; tx[2] = bsb << 3 | 4;
        esp_fill_random(tx + 3, len);
        spi_transaction_t t = { .length = (3 + len) * 8, .tx_buffer = tx, .rx_buffer = rx };
        uint32_t f = g_frames;
        spi_device_polling_transmit(s_dev, &t);
        r->frames++;
        if (!wait_frame(f)) r->lost++;
        bool bad = false;
        for (unsigned k = 0; k < len; k++)
            if (g_mem[base + lo + k] != tx[3 + k]) bad = true;
        if (bad) r->wr++;
        // read back what the slave holds now (so a bad write isn't counted twice)
        for (unsigned k = 0; k < len; k++) want[k] = g_mem[base + lo + k];
        tx[2] = bsb << 3;
        memset(tx + 3, 0xFF, len);
        f = g_frames;
        spi_device_polling_transmit(s_dev, &t);
        r->frames++;
        if (!wait_frame(f)) r->lost++;
        // The slave puts every bit out right after the rising edge (the Dreamcast's rule).
        // A master that samples a little after the edge therefore sees every bit one clock
        // early: both views are a correct reply. "first bit late": only the very first reply
        // bit missed that early sample (the Dreamcast gives it a whole bit more).
        bool sent = !memcmp(rx + 3, want, len), early = true, early1 = true;
        for (unsigned k = 0; k < len; k++) {
            uint8_t g = (uint8_t)((rx[2 + k] & 1) << 7 | rx[3 + k] >> 1);
            if (g != want[k]) early = false;
            if ((g ^ want[k]) & (k ? 0xFF : 0x7F)) early1 = false;
        }
        if (sent) r->rd_sent++; else if (early) r->rd_early++; else if (early1) r->first++; else r->other++;
        if (r->lost > 100) return;                 // out of step at this speed: no point going on
        if ((i & 63) == 63) vTaskDelay(1);         // let the idle task run (watchdog)
    }
}

typedef struct { int hz, duty; const char *name; } speed_t;
static const speed_t speeds[] = {
    {  4000000, 128, " 4.0 MHz even 125/125 ns" },
    {  8000000, 128, " 8.0 MHz even   62/62 ns" },
    {  8000000,  77, " 8.0 MHz high 37, low 87" },
    {  8000000, 179, " 8.0 MHz high 87, low 37" },
    { 10000000, 128, "10.0 MHz even   50/50 ns" },
    { 10000000,  96, "10.0 MHz high 37, low 62" },
    { 10000000, 160, "10.0 MHz high 62, low 37" },
    { 11428571, 128, "11.4 MHz even   44/44 ns" },
    { 13333333, 128, "13.3 MHz even   37/37 ns" },
    { 16000000, 128, "16.0 MHz even   31/31 ns" },
    { 20000000, 128, "20.0 MHz even   25/25 ns" },
};
#define NSPEED (sizeof speeds / sizeof speeds[0])

static void row(const speed_t *s, int frames)
{
    if (master_open(s->hz, s->duty) < 0) { printf("probe: could not set %s\n", s->name); return; }
    result_t r;
    run(frames, &r);
    printf("probe: %s %8u %11u %9u %12u %11u %10u %6u  %s\n", s->name, (unsigned)r.frames, (unsigned)r.wr,
           (unsigned)r.rd_sent, (unsigned)r.rd_early, (unsigned)r.first, (unsigned)r.other, (unsigned)r.lost,
           r.lost > 100 ? "FAIL (stopped early)" : (r.wr | r.lost) ? "FAIL" : r.other ? "writes ok, reads?" : "ok");
    spi_bus_remove_device(s_dev);
    vTaskDelay(1);
}

static void head(const char *title)
{
    printf("\nprobe: %s\n", title);
    printf("probe: clock                      frames  bad writes  reads ok  ok, 1 early  1st bit late  bad reads   lost\n");
}

static void show_jit(const char *what, const jit_t *j)
{
    printf("probe: %s: longest pass %u ns; passes over 67 ns: %u, over 267 ns: %u, over 1 us: %u, over 4 us: %u\n",
           what, (unsigned)(j->max * 25 / 6), (unsigned)j->n16, (unsigned)j->n64, (unsigned)j->n256, (unsigned)j->n1k);
}

// Wi-Fi activity on the other core: bit 0 = send 500 raw frames a second, bit 1 = scan
// (hops through all channels), bit 2 = listen to everything on the air (promiscuous).
static volatile int g_load;
static volatile uint32_t g_tx_ok, g_rx_seen;

static void on_air(void *buf, wifi_promiscuous_pkt_type_t type) { (void)buf; (void)type; g_rx_seen++; }

static void wifi_load(void *arg)
{
    static uint8_t frame[64] = {
        0x80, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,      // a beacon to everybody
        0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0x01, 0x04,
        0x00, 0x05, 'p', 'r', 'o', 'b', 'e',
    };
    int tick = 0;
    for (;;) {
        int l = g_load;
        if (l & 1)
            for (int k = 0; k < 5; k++)
                if (esp_wifi_80211_tx(WIFI_IF_AP, frame, 43, false) == ESP_OK) g_tx_ok++;
        if ((l & 2) && tick % 400 == 0) esp_wifi_scan_start(NULL, false);
        tick++;
        vTaskDelay(1);                       // one tick is 10 ms: 5 frames a tick = 500 a second
    }
}

static void pauses(int step, int load, const char *what)
{
    uint32_t t0 = g_tx_ok, r0 = g_rx_seen;
    esp_wifi_set_promiscuous((load & 4) != 0);
    g_load = load;
    vTaskDelay(pdMS_TO_TICKS(300));
    g_phase = step;
    while (g_done < step) vTaskDelay(1);
    show_jit(what, &g_jit[step - 2]);
    printf("probe:    (during it: %u frames sent, %u heard)\n", (unsigned)(g_tx_ok - t0), (unsigned)(g_rx_seen - r0));
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));       // time to open the serial port
    printf("\nprobe: DC-PicoLink ESP32-only probe 6b (" __DATE__ " " __TIME__ ")\n");

    // Wi-Fi is prepared first: its start-up writes to flash, and a flash write waits for
    // the other core, which will never answer once the slave runs.
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) { nvs_flash_erase(); nvs_flash_init(); }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wc);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    wifi_config_t ap = { .ap = { .ssid = "dc-picolink-probe", .ssid_len = 17, .channel = 6,
                                 .max_connection = 2, .authmode = WIFI_AUTH_OPEN } };
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    // First start: the radio calibrates and saves the result to flash. That write would hang
    // later (seen on the first board: "saving new calibration data", then the watchdog).
    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    spi_bus_config_t bc = {
        .mosi_io_num = P_MOSI, .miso_io_num = P_MISO, .sclk_io_num = P_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_DISABLED));
    // a device once, so that the CS pad is driven high before the slave looks at it
    master_open(1000000, 128);
    spi_bus_remove_device(s_dev);
    gpio_set_direction(P_CS, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(P_CS, 1);

    for (unsigned i = 0; i < sizeof g_mem; i++) g_mem[i] = (uint8_t)(i * 7 + 3);
    xTaskCreatePinnedToCore(core1_task, "responder", 4096, NULL, 5, NULL, 1);
    while (!g_done) vTaskDelay(1);
    if (g_done < 0) return;
    printf("\nprobe: pauses of the slave's core: a loop that takes about 42 ns per pass, run for 5 s each time.\n");
    printf("probe: Any pass over 67 ns is a pause of 25 ns or more, enough to cost a bit at speed.\n");
    show_jit("Wi-Fi off", &g_jit[0]);

    g_phase = 2;
    vTaskDelay(pdMS_TO_TICKS(50));
    head("Wi-Fi off, 20 000 writes + 20 000 reads per line");
    row(&speeds[2], 20000); row(&speeds[3], 20000); row(&speeds[8], 20000);

    // stop the slave (it looks at the flag at a frame end)
    g_run = 0;
    master_open(1000000, 128);
    { uint8_t z[4] = { 0, 0, 0, 0xFF }; spi_transaction_t t = { .length = 32, .tx_buffer = z }; spi_device_polling_transmit(s_dev, &t); }
    spi_bus_remove_device(s_dev);
    while (g_done < 2) vTaskDelay(1);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_promiscuous_rx_cb(on_air);
    xTaskCreatePinnedToCore(wifi_load, "wifi_load", 4096, NULL, 4, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("\nprobe: which Wi-Fi activity pauses the slave's core?\n");
    pauses(3, 0, "Wi-Fi on, idle (access point beacons only)");
    pauses(4, 1, "Wi-Fi on, sending 500 frames a second");
    pauses(5, 5, "Wi-Fi on, sending and listening to everything on the air");
    pauses(6, 2, "Wi-Fi on, scanning (changes channel all the time)");

    g_load = 5;                              // send + listen, no scan: like an adapter in use
    g_phase = 7;
    vTaskDelay(pdMS_TO_TICKS(50));
    head("Wi-Fi on, sending and listening, NO scan: 200 000 + 200 000 per line");
    row(&speeds[2], 200000);
    row(&speeds[3], 200000);
    row(&speeds[8], 200000);
    g_load = 7; esp_wifi_set_promiscuous(true);
    head("Wi-Fi on, sending, listening AND scanning: 100 000 + 100 000 per line");
    row(&speeds[2], 100000);
    row(&speeds[8], 100000);

    printf("\nprobe: done. Bad writes and lost frames are what counts; 'reads ok' and 'ok, 1 early' are both\n");
    printf("probe: correct replies, and 'bad reads' with clean writes can be this test's master sampling at the\n");
    printf("probe: moment the data changes. Send everything from 'probe 6' on.\n");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
