/*
 * Link to DC-PicoLink (framing in ../../firmware/link_model.h, shared with the cable).
 * Two wires to choose from in menuconfig:
 *  - UART (Pico / Pro Micro cable): UART1, 3 Mbaud, RTS/CTS. One task reads and
 *    dispatches; senders queue whole wire packets into the driver's TX ring.
 *  - SPI (RP2040-Zero stack): this board is the SPI slave of the cable, fixed-size
 *    chunks, HS/DR lines (../../firmware/link_spi_model.h). Senders queue whole wire
 *    packets into our own ring; one task keeps transactions loaded and dispatches.
 * Either way a mutex keeps packets from interleaving.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#if CONFIG_DCL_LINK_SPI
#include "driver/spi_slave.h"
#include "esp_heap_caps.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "link_spi_model.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "link_model.h"
#include "app.h"

#define LINK_PORT    UART_NUM_1
#define RX_BUF       (16 * 1024)
#define TX_BUF       (16 * 1024)

static const char *TAG = "link";
size_t link_room(void);
static SemaphoreHandle_t tx_lock, info_lock;
static SemaphoreHandle_t room_sem;     /* SPI: given when a transaction takes bytes out of the ring */
static uint8_t tx_seq;
static link_rx_t rx;
static link_rx_t urx;                 /* the cable on the USB port (usbport.c) */
static volatile bool usb_on;          /* a DC-PicoLink cable is on the USB port: the link runs there */
#if CONFIG_DCL_USB_MODEM
size_t usbl_room(void);
size_t usbl_backlog(void);
void usbl_put(const uint8_t *w, size_t k);
void usbl_kick(void);
#else
static size_t usbl_room(void) { return 0; }
static size_t usbl_backlog(void) { return 0; }
static void usbl_put(const uint8_t *w, size_t k) { (void)w; (void)k; }
static void usbl_kick(void) { }
#endif
static volatile int64_t last_rx_us = -10000000;
static link_stats_t st;
static char cable_info[512];          /* "key value" lines from the last hello answer */
static char last_answer[1024];
static uint32_t answer_seq;
static uint16_t next_id = 1;

bool link_is_up(void) { return esp_timer_get_time() - last_rx_us < 3000000; }

#if CONFIG_DCL_LINK_SPI
#define LSPI_HOST   SPI3_HOST          /* SPI2 (FSPI, IO-MUX pins 10-13) stays free for a W5500 */
#define LSPI_NBUF   3                  /* transactions kept loaded/queued */
#define RING_BITS   13                 /* 8 KiB of wire bytes for the cable: send_full stayed 0 at 16 KiB, and RAM is tight (the VPN) */
#define RING_SIZE   (1u << RING_BITS)
static uint8_t ring[RING_SIZE];
static uint32_t ring_wr, ring_rd;      /* free-running; guarded by tx_lock */
static uint8_t *lspi_txb[LSPI_NBUF], *lspi_rxb[LSPI_NBUF];
static spi_slave_transaction_t lspi_tr[LSPI_NBUF];
static uint16_t lspi_txn[LSPI_NBUF];   /* queued bytes in each loaded chunk */
static uint32_t lspi_loaded_bytes;     /* sum of lspi_txn: bytes handed to the hardware, not yet clocked */
static uint32_t lspi_xfers, lspi_short;

static void IRAM_ATTR lspi_post_setup(spi_slave_transaction_t *t) {
    (void)t;
    gpio_ll_set_level(&GPIO, CONFIG_DCL_LSPI_HS, 1);   /* loaded: the cable may clock */
}
static void IRAM_ATTR lspi_post_trans(spi_slave_transaction_t *t) {
    (void)t;
    gpio_ll_set_level(&GPIO, CONFIG_DCL_LSPI_HS, 0);   /* done: wait for the next one */
}

/* DR: we have bytes for the cable (queued, or in a loaded chunk). Call with tx_lock held. */
static void lspi_update_dr(void) {
    gpio_set_level(CONFIG_DCL_LSPI_DR, (ring_wr != ring_rd || lspi_loaded_bytes) ? 1 : 0);
}

/* bytes into the ring (tx_lock held): two copies at most, not one per byte */
static void wire_put(const uint8_t *w, size_t k) {
    uint32_t at = ring_wr & (RING_SIZE - 1), first = RING_SIZE - at;
    if (first > k) first = (uint32_t)k;
    memcpy(ring + at, w, first);
    memcpy(ring, w + first, k - first);
    ring_wr += (uint32_t)k;
    lspi_update_dr();
}

/* refill chunk i from the ring and hand it to the driver */
static void lspi_load(int i) {
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    lspi_loaded_bytes -= lspi_txn[i];
    lspi_txn[i] = (uint16_t)link_chunk_fill(ring, RING_SIZE - 1, &ring_rd, ring_wr, lspi_txb[i], LINK_SPI_CHUNK);
    lspi_loaded_bytes += lspi_txn[i];
    lspi_update_dr();
    bool freed = lspi_txn[i] != 0;
    xSemaphoreGive(tx_lock);
    if (freed) { xSemaphoreGive(room_sem); ppp_serial_room(); }   /* the serial queue may go on (ppp_srv.c) */
    lspi_tr[i].length = LINK_SPI_CHUNK * 8;
    lspi_tr[i].tx_buffer = lspi_txb[i];
    lspi_tr[i].rx_buffer = lspi_rxb[i];
    lspi_tr[i].user = (void *)(intptr_t)i;
    ESP_ERROR_CHECK(spi_slave_queue_trans(LSPI_HOST, &lspi_tr[i], portMAX_DELAY));
}
#else
static void wire_put(const uint8_t *w, size_t k) { uart_write_bytes(LINK_PORT, w, k); }
#endif

/* one packet to the cable, or false if the queue has no room for the largest packet */
bool link_send(uint8_t ch, const void *p, size_t n) {
    static uint8_t w[LINK_MAX_WIRE];
    bool ok = false;
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    bool usb = usb_on;
    if (link_room() >= LINK_MAX_WIRE) {
        size_t k = link_build(ch, tx_seq, p, n, w);
        if (k) { tx_seq++; if (usb) usbl_put(w, k); else wire_put(w, k); st.sent++; ok = true; }
    }
    if (!ok) st.send_full++;
    xSemaphoreGive(tx_lock);
    if (usb && ok) usbl_kick();
    return ok;
}

bool link_dc1(const char *msg, size_t n) { return link_send(LINK_CH_API, msg, n); }

/* free bytes in the queue to the cable; and bytes still waiting for it (ring + loaded chunks) */
size_t link_room(void) {
    if (usb_on) return usbl_room();
#if CONFIG_DCL_LINK_SPI
    return RING_SIZE - (ring_wr - ring_rd);          /* a word read: a snapshot is enough */
#else
    size_t f = 0; uart_get_tx_buffer_free_size(LINK_PORT, &f); return f;
#endif
}
size_t link_backlog(void) {
    if (usb_on) return usbl_backlog();
#if CONFIG_DCL_LINK_SPI
    return (ring_wr - ring_rd) + lspi_loaded_bytes;
#else
    return TX_BUF - link_room();
#endif
}
/* wait until the cable has taken bytes (SPI) or at most ms; never from the link's reader task */
void link_wait_room(int ms) {
    xSemaphoreTake(room_sem, pdMS_TO_TICKS(ms) + 1);
}

void link_get_stats(link_stats_t *s) {
    *s = st;
    s->good = rx.good; s->crc_bad = rx.crc_bad; s->lost = rx.lost;
    s->broken = rx.cobs_bad + rx.too_long + rx.bad_hdr;
}

/* the cable's network frames come from a computer on its USB (no Dreamcast): not a Dreamcast session */
bool link_eth_is_computer(void) {
    xSemaphoreTake(info_lock, portMAX_DELAY);
    bool c = strstr(cable_info, "network=usb<->esp32") != NULL;
    xSemaphoreGive(info_lock);
    return c;
}

size_t link_cable_info(char *out, size_t cap) {
    xSemaphoreTake(info_lock, portMAX_DELAY);
    size_t n = strlen(cable_info);
    if (n >= cap) n = cap - 1;
    memcpy(out, cable_info, n); out[n] = 0;
    xSemaphoreGive(info_lock);
    return n;
}

size_t link_last_answer(char *out, size_t cap, uint32_t *seq) {
    xSemaphoreTake(info_lock, portMAX_DELAY);
    size_t n = strlen(last_answer);
    if (n >= cap) n = cap - 1;
    memcpy(out, last_answer, n); out[n] = 0;
    *seq = answer_seq;
    xSemaphoreGive(info_lock);
    return n;
}

static volatile uint32_t wire_baud;    /* the serial line's rate (the cable's hello, every second) */
uint32_t link_wire_baud(void) { return wire_baud; }

/* an answer from the cable: keep it; "ok hello.* / ok link.*" lines also go to cable_info */
static void on_api(const char *p, size_t n) {
    xSemaphoreTake(info_lock, portMAX_DELAY);
    size_t k = n < sizeof last_answer - 1 ? n : sizeof last_answer - 1;
    memcpy(last_answer, p, k); last_answer[k] = 0;
    answer_seq++;
    const char *q = p, *end = p + n, *line; size_t len;
    bool is_info = false;
    char info[sizeof cable_info]; size_t o = 0;
    while (link_next_line(&q, end, &line, &len)) {
        const char *rest;
        if (link_word(line, len, "ok", &rest) && (len - (size_t)(rest - line)) > 0 &&
            (strncmp(rest, "hello.", 6) == 0 || strncmp(rest, "link.", 5) == 0)) {
            if (strncmp(rest, "hello.kind", 10) == 0) is_info = true;
            size_t rl = len - (size_t)(rest - line);
            if (rl > 17 && strncmp(rest, "link.serial.wire ", 17) == 0) wire_baud = (uint32_t)strtoul(rest + 17, NULL, 10);
            if (o + rl + 2 < sizeof info) { memcpy(info + o, rest, rl); o += rl; info[o++] = '\n'; }
        }
    }
    if (is_info) { info[o] = 0; memcpy(cable_info, info, o + 1); }
    xSemaphoreGive(info_lock);
}

static void deliver(const link_rx_t *r) {
    switch (r->ch) {
    case LINK_CH_STAT:
        log_add("cable", (const char *)r->payload, r->n);
        break;
    case LINK_CH_API:
        on_api((const char *)r->payload, r->n);
        break;
    case LINK_CH_SER:
        st.ser_in += r->n;
        ser_from_dc(r->payload, r->n);        /* E-3: Hayes stub + PPP server (also feeds the monitor) */
        break;
    case LINK_CH_ETH:
        st.eth_in++;
        eth_from_dc(r->payload, r->n);        /* E-4: second network interface */
        break;
    case LINK_CH_SD:                          /* the slot card for the web page (sdfs.c) */
        sdfs_from_cable(r->payload, r->n);
        break;
    case LINK_CH_TEST:                        /* the cable's self-test: straight back */
        link_send(LINK_CH_TEST, r->payload, r->n);   /* once: only this task frees room, so waiting here never helped */
        st.test_echo++;
        break;
    default:
        break;
    }
}

/* ---- the link over USB (usbport.c, modem builds) ---- */
void link_usb_set(bool on) {
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    memset(&urx, 0, sizeof urx);
    usb_on = on;
    xSemaphoreGive(tx_lock);
}
void link_usb_rx(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (link_rx_byte(&urx, p[i])) { last_rx_us = esp_timer_get_time(); deliver(&urx); }
}
/* a transfer took bytes to the cable */
void link_tx_freed(void) { xSemaphoreGive(room_sem); ppp_serial_room(); }

static void note_up_down(bool *was_up) {
    bool up = link_is_up();
    if (up != *was_up) {
        say_line(up ? "link to the cable: up" : "link to the cable: down (nothing heard for 3 s)");
        *was_up = up;
    }
}

#if CONFIG_DCL_LINK_SPI
static void rx_task(void *arg) {
    (void)arg;
    bool was_up = false;
    for (int i = 0; i < LSPI_NBUF; i++) lspi_load(i);
    for (;;) {
        spi_slave_transaction_t *t;
        if (spi_slave_get_trans_result(LSPI_HOST, &t, pdMS_TO_TICKS(20)) == ESP_OK) {
            int i = (int)(intptr_t)t->user;
            size_t n = t->trans_len / 8;
            lspi_xfers++;
            if (n != LINK_SPI_CHUNK) lspi_short++;
            for (size_t k = 0; k < n; k++)
                if (!usb_on && link_rx_byte(&rx, lspi_rxb[i][k])) { last_rx_us = esp_timer_get_time(); deliver(&rx); }
            lspi_load(i);
        }
        note_up_down(&was_up);
    }
}
#else
static void rx_task(void *arg) {
    (void)arg;
    static uint8_t buf[512];
    bool was_up = false;
    for (;;) {
        int n = uart_read_bytes(LINK_PORT, buf, sizeof buf, pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++)
            if (!usb_on && link_rx_byte(&rx, buf[i])) { last_rx_us = esp_timer_get_time(); deliver(&rx); }
        note_up_down(&was_up);
    }
}
#endif
/* ask the cable who it is, every second (also keeps the link up on its side) */
static void hello_task(void *arg) {
    (void)arg;
    for (;;) {
        char m[64];
        int k = snprintf(m, sizeof m, "dc1\nid %u\nget hello\nget link.*\n", (unsigned)next_id++);
        link_send(LINK_CH_API, m, (size_t)k);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#if CONFIG_DCL_LINK_SPI
void link_start(void) {
    tx_lock = xSemaphoreCreateMutex();
    room_sem = xSemaphoreCreateBinary();
    info_lock = xSemaphoreCreateMutex();
    link_rx_init(&rx);
    gpio_config_t o = { .pin_bit_mask = (1ULL << CONFIG_DCL_LSPI_HS) | (1ULL << CONFIG_DCL_LSPI_DR),
                        .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&o));
    gpio_set_level(CONFIG_DCL_LSPI_HS, 0);
    gpio_set_level(CONFIG_DCL_LSPI_DR, 0);
    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_DCL_LSPI_MOSI, .miso_io_num = CONFIG_DCL_LSPI_MISO,
        .sclk_io_num = CONFIG_DCL_LSPI_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LINK_SPI_CHUNK,
    };
    spi_slave_interface_config_t slv = {
        .spics_io_num = CONFIG_DCL_LSPI_CS, .mode = LINK_SPI_MODE, .queue_size = LSPI_NBUF,
        .post_setup_cb = lspi_post_setup, .post_trans_cb = lspi_post_trans,
    };
    ESP_ERROR_CHECK(spi_slave_initialize(LSPI_HOST, &bus, &slv, SPI_DMA_CH_AUTO));
    /* a cable that is off or resetting leaves the lines floating: CS idles high, SCLK low */
    gpio_set_pull_mode(CONFIG_DCL_LSPI_CS, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(CONFIG_DCL_LSPI_SCLK, GPIO_PULLDOWN_ONLY);
    gpio_set_pull_mode(CONFIG_DCL_LSPI_MOSI, GPIO_PULLUP_ONLY);
    for (int i = 0; i < LSPI_NBUF; i++) {
        lspi_txb[i] = heap_caps_malloc(LINK_SPI_CHUNK, MALLOC_CAP_DMA);
        lspi_rxb[i] = heap_caps_malloc(LINK_SPI_CHUNK, MALLOC_CAP_DMA);
        assert(lspi_txb[i] && lspi_rxb[i]);
    }
    ESP_LOGI(TAG, "SPI slave (cable is master), %u-byte chunks: MOSI %d MISO %d SCLK %d CS %d HS %d DR %d",
             (unsigned)LINK_SPI_CHUNK, CONFIG_DCL_LSPI_MOSI, CONFIG_DCL_LSPI_MISO, CONFIG_DCL_LSPI_SCLK,
             CONFIG_DCL_LSPI_CS, CONFIG_DCL_LSPI_HS, CONFIG_DCL_LSPI_DR);
    xTaskCreate(rx_task, "link_rx", 4096, NULL, 10, NULL);
    xTaskCreate(hello_task, "link_hello", 3072, NULL, 5, NULL);
}
#else
void link_start(void) {
    tx_lock = xSemaphoreCreateMutex();
    room_sem = xSemaphoreCreateBinary();
    info_lock = xSemaphoreCreateMutex();
    link_rx_init(&rx);
    uart_config_t c = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
        .rx_flow_ctrl_thresh = 100,          /* RTS drops at 100 of 128 FIFO bytes */
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_PORT, RX_BUF, TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_PORT, &c));
    ESP_ERROR_CHECK(uart_set_pin(LINK_PORT, CONFIG_DCL_LINK_TX, CONFIG_DCL_LINK_RX,
                                 CONFIG_DCL_LINK_RTS, CONFIG_DCL_LINK_CTS));
    gpio_pullup_en(CONFIG_DCL_LINK_RX);
    gpio_pullup_en(CONFIG_DCL_LINK_CTS);
    ESP_LOGI(TAG, "UART1 %u baud, TX %d RX %d RTS %d CTS %d", (unsigned)LINK_BAUD,
             CONFIG_DCL_LINK_TX, CONFIG_DCL_LINK_RX, CONFIG_DCL_LINK_RTS, CONFIG_DCL_LINK_CTS);
    xTaskCreate(rx_task, "link_rx", 4096, NULL, 10, NULL);
    xTaskCreate(hello_task, "link_hello", 3072, NULL, 5, NULL);
}
#endif

void link_note_ser_out(size_t n) { st.ser_out += (uint32_t)n; }
