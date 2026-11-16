// DC-PicoLink probe: can an ESP32-S3 alone answer the Dreamcast's W5500 SPI?
//
// No wiring and no Dreamcast needed. Flash it on a bare ESP32-S3 board and read the USB
// serial port. One core plays the Dreamcast (the SPI master peripheral, at rising clock
// speeds); the other core is the candidate W5500 emulation: a bit-banged SPI slave that uses
// the ESP32-S3's "dedicated GPIO" CPU instructions, with interrupts off, the same way core1
// of the RP2040 runs. Both use the same four pads (GPIO4-7), so the signals go out through
// the pad drivers and back in through the input buffers like real ones.
//
// What it answers: at which clock speed does the bit-banged slave start losing bits, with
// and without Wi-Fi running. KOS on the Dreamcast writes at about 8 Mbit/s with clock
// phases down to ~40 ns (like a 12.5 MHz clock) and reads at about 4.4 Mbit/s. So the
// candidate needs to be clean at 13.3 MHz to be worth building on, and 16 MHz for margin.
//
// What it does not answer: the Dreamcast's own timing quirks (its late RXD sampling, its
// uneven clock). The frame format and the "answer right after the rising edge" rule are the
// same as firmware/w5500_spi.pio.
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

#define P_CLK  4
#define P_MOSI 5
#define P_MISO 6
#define P_CS   7

// input bundle order: CLK, MOSI, CS
#define B_CLK  1u
#define B_MOSI_SHIFT 1
#define B_CS   4u

static DRAM_ATTR uint8_t g_mem[4 * 256 + 32];   // [block select 0-3][address low byte]
static DRAM_ATTR volatile uint32_t g_frames;
static DRAM_ATTR volatile int g_run = 1;
static volatile int g_ready;

// Pin access for dcspi_bb.h: the ESP32-S3's dedicated-GPIO instructions (one CPU cycle,
// no bus access). Input bits: 0 = clock, 1 = MOSI, 2 = chip select.
static DRAM_ATTR uint32_t s_om;                      // MISO's dedicated output bit
#define DCSPI_ATTR IRAM_ATTR __attribute__((noinline))
#define DC_RISE(v)  asm volatile("1:\n ee.get_gpio_in %0\n bbci %0, 0, 1b\n" : "=r"(v))
#define DC_RISE_CS(v, l) asm goto("1:\n ee.get_gpio_in %0\n bbsi %0, 0, 2f\n bbci %0, 2, 1b\n j %l[" #l "]\n2:\n" \
                                  : "=r"(v) : : : l)
#define DC_FALL()   do { uint32_t t_; asm volatile("1:\n ee.get_gpio_in %0\n bbsi %0, 0, 1b\n" : "=r"(t_)); } while (0)
#define DC_MOSI(v)  (((v) >> B_MOSI_SHIFT) & 1)
#define DC_OUT(w)   dedic_gpio_cpu_ll_write_mask(om, (uint32_t)((int32_t)(w) >> 31) & om)
#define DC_IDLE()   dedic_gpio_cpu_ll_write_mask(om, om)
#define DC_WAIT_CS_HIGH() do { uint32_t t_; asm volatile("1:\n ee.get_gpio_in %0\n bbci %0, 2, 1b\n" : "=r"(t_)); } while (0)
#define DC_WAIT_CS_LOW()  do { uint32_t t_; asm volatile("1:\n ee.get_gpio_in %0\n bbsi %0, 2, 1b\n" : "=r"(t_)); } while (0)
#define DC_KEEP(x)  asm volatile("" : "+r"(x))
#define DCSPI_OM    const uint32_t om = s_om;
#include "dcspi_bb.h"

static DRAM_ATTR uint8_t s_cap[DCSPI_CAP];

static void core1_task(void *arg)
{
    static const int in_pins[] = { P_CLK, P_MOSI, P_CS };
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
    if (off != 0 || !om) {
        printf("probe: unexpected dedicated GPIO layout (in offset %u, out mask %x)\n", (unsigned)off, (unsigned)om);
        g_ready = -1;
        vTaskDelete(NULL);
    }
    g_ready = 1;
    portDISABLE_INTERRUPTS();
    s_om = om;
    dcspi_bb_responder(g_mem, s_cap, &g_frames, &g_run);
    portENABLE_INTERRUPTS();
    g_ready = 2;
    vTaskDelete(NULL);
}

static spi_device_handle_t s_dev;

static int master_open(int hz)
{
    spi_device_interface_config_t dc = {
        .mode = 0, .clock_speed_hz = hz, .spics_io_num = P_CS, .queue_size = 1,
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

typedef struct { uint32_t frames, wr, first, other, lost; } result_t;

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
        if (rx[3] != want[0]) r->first++;
        else if (memcmp(rx + 3, want, len)) r->other++;
    }
}

static void sweep(const char *title)
{
    static const int hz[] = { 1000000, 4000000, 8000000, 10000000, 13333333, 16000000, 20000000 };
    printf("\nprobe: %s\n", title);
    printf("probe:   clock    frames   bad writes   bad first byte   bad later byte   lost frames\n");
    for (unsigned i = 0; i < sizeof hz / sizeof hz[0]; i++) {
        int khz = master_open(hz[i]);
        if (khz < 0) { printf("probe: could not set %d Hz\n", hz[i]); continue; }
        result_t r;
        run(20000, &r);
        printf("probe: %5d kHz %8u %12u %16u %16u %13u  %s\n", khz, (unsigned)r.frames, (unsigned)r.wr,
               (unsigned)r.first, (unsigned)r.other, (unsigned)r.lost,
               (r.wr | r.first | r.other | r.lost) ? "FAIL" : "ok");
        spi_bus_remove_device(s_dev);
        vTaskDelay(1);
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));       // time to open the serial port
    printf("\nprobe: DC-PicoLink ESP32-only probe (" __DATE__ " " __TIME__ ")\n");

    // Wi-Fi is prepared first: its start-up writes to flash, and a flash write waits for
    // the other core, which will never answer once the responder runs.
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) { nvs_flash_erase(); nvs_flash_init(); }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wc);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    spi_bus_config_t bc = {
        .mosi_io_num = P_MOSI, .miso_io_num = P_MISO, .sclk_io_num = P_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_DISABLED));
    // a device once, so that the CS pad is driven high before the responder looks at it
    master_open(1000000);
    spi_bus_remove_device(s_dev);
    gpio_set_direction(P_CS, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(P_CS, 1);

    for (unsigned i = 0; i < sizeof g_mem; i++) g_mem[i] = (uint8_t)(i * 7 + 3);
    xTaskCreatePinnedToCore(core1_task, "responder", 4096, NULL, 5, NULL, 1);
    while (!g_ready) vTaskDelay(1);
    if (g_ready < 0) return;
    vTaskDelay(pdMS_TO_TICKS(50));

    sweep("Wi-Fi off");

    wifi_config_t ap = { .ap = { .ssid = "dc-picolink-probe", .ssid_len = 17, .channel = 6,
                                 .max_connection = 2, .authmode = WIFI_AUTH_OPEN } };
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_scan_start(NULL, false);
    sweep("Wi-Fi on (access point up, scanning)");
    esp_wifi_scan_start(NULL, false);
    sweep("Wi-Fi on, second pass");

    printf("\nprobe: done. Needed for the Dreamcast: no FAIL up to 13333 kHz; 16000 kHz clean means margin.\n");
    printf("probe: send the whole table back, including the lines that failed.\n");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
