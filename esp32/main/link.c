/*
 * UART link to DC-PicoLink (framing in ../../firmware/link_model.h, shared with the cable).
 * UART1, 3 Mbaud, RTS/CTS. One task reads and dispatches; senders queue whole wire
 * packets into the driver's TX ring (guarded by a mutex so packets never interleave).
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "link_model.h"
#include "app.h"

#define LINK_PORT    UART_NUM_1
#define RX_BUF       (16 * 1024)
#define TX_BUF       (16 * 1024)

static const char *TAG = "link";
static SemaphoreHandle_t tx_lock, info_lock;
static uint8_t tx_seq;
static link_rx_t rx;
static volatile int64_t last_rx_us = -10000000;
static link_stats_t st;
static char cable_info[512];          /* "key value" lines from the last hello answer */
static char last_answer[1024];
static uint32_t answer_seq;
static uint16_t next_id = 1;

bool link_is_up(void) { return esp_timer_get_time() - last_rx_us < 3000000; }

bool link_send(uint8_t ch, const void *p, size_t n) {
    static uint8_t w[LINK_MAX_WIRE];
    bool ok = false;
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    size_t free_b = 0;
    uart_get_tx_buffer_free_size(LINK_PORT, &free_b);
    if (free_b >= LINK_MAX_WIRE) {
        size_t k = link_build(ch, tx_seq, p, n, w);
        if (k) {
            tx_seq++;
            uart_write_bytes(LINK_PORT, w, k);
            st.sent++;
            ok = true;
        }
    }
    if (!ok) st.send_full++;
    xSemaphoreGive(tx_lock);
    return ok;
}

bool link_dc1(const char *msg, size_t n) { return link_send(LINK_CH_API, msg, n); }

void link_get_stats(link_stats_t *s) {
    *s = st;
    s->good = rx.good; s->crc_bad = rx.crc_bad; s->lost = rx.lost;
    s->broken = rx.cobs_bad + rx.too_long + rx.bad_hdr;
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
            if (o + rl + 2 < sizeof info) { memcpy(info + o, rest, rl); o += rl; info[o++] = '\n'; }
        }
    }
    if (is_info) { info[o] = 0; memcpy(cable_info, info, o + 1); }
    xSemaphoreGive(info_lock);
}

static void deliver(void) {
    switch (rx.ch) {
    case LINK_CH_STAT:
        log_add("cable", (const char *)rx.payload, rx.n);
        break;
    case LINK_CH_API:
        on_api((const char *)rx.payload, rx.n);
        break;
    case LINK_CH_SER:
        st.ser_in += rx.n;
        ser_from_dc(rx.payload, rx.n);        /* E-3: Hayes stub + PPP server (also feeds the monitor) */
        break;
    case LINK_CH_ETH:
        st.eth_in++;
        eth_from_dc(rx.payload, rx.n);        /* E-4: second network interface */
        break;
    default:
        break;
    }
}

static void rx_task(void *arg) {
    (void)arg;
    static uint8_t buf[512];
    bool was_up = false;
    for (;;) {
        int n = uart_read_bytes(LINK_PORT, buf, sizeof buf, pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++)
            if (link_rx_byte(&rx, buf[i])) { last_rx_us = esp_timer_get_time(); deliver(); }
        bool up = link_is_up();
        if (up != was_up) {
            char t[96];
            int k = snprintf(t, sizeof t, up ? "link to the cable: up" : "link to the cable: down (nothing heard for 3 s)");
            log_add("esp32", t, (size_t)k);
            was_up = up;
        }
    }
}

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

void link_start(void) {
    tx_lock = xSemaphoreCreateMutex();
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

void link_note_ser_out(size_t n) { st.ser_out += (uint32_t)n; }
