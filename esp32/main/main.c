/*
 * DC-PicoLink ESP32 side, step E-2: link to the cable, Wi-Fi, and a web page that shows
 * the cable's status lines and the serial traffic. PPP / DCNow! (E-3) and the W5500
 * network (E-4) come next and plug into link.c's deliver().
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "app.h"

/* ---- JSON helper ---- */
size_t json_escape(char *out, size_t cap, const char *s, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 7 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 0x20 || c > 0x7E) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    if (o < cap) out[o] = 0;
    return o;
}

/* ---- status lines: the last 120, their text packed in 6 KB (lines are ~90 characters, so
   usually all 120 fit; long ones push the oldest out sooner) ---- */
#define LOG_N   120
#define LOG_LEN 200
#define LOG_TXT 6144u
static struct { uint32_t num, t_ms, pos; uint8_t len; char src[7]; } logi[LOG_N];
static char logt[LOG_TXT];
static uint32_t log_tw;                          /* text bytes written so far */
static uint32_t log_next = 1;
static SemaphoreHandle_t log_lock;
static SemaphoreHandle_t tls_mx;

void tls_lock(void) { xSemaphoreTake(tls_mx, portMAX_DELAY); }
void tls_unlock(void) { xSemaphoreGive(tls_mx); }

void log_add(const char *src, const char *text, size_t n) {
    if (n >= LOG_LEN) n = LOG_LEN - 1;
    xSemaphoreTake(log_lock, portMAX_DELAY);
    typeof(logi[0]) *e = &logi[log_next % LOG_N];
    e->num = log_next++;
    e->t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    strlcpy(e->src, src, sizeof e->src);
    e->pos = log_tw; e->len = (uint8_t)n;
    for (size_t i = 0; i < n; i++) logt[(log_tw + i) % LOG_TXT] = text[i];
    log_tw += (uint32_t)n;
    xSemaphoreGive(log_lock);
    ESP_LOGI(src, "%.*s", (int)n, text);
}

/* JSON array of the lines numbered >= since (oldest first); *next = number to ask for next time */
size_t log_since(uint32_t since, char *out, size_t cap, uint32_t *next) {
    size_t o = 0;
    out[o++] = '[';
    xSemaphoreTake(log_lock, portMAX_DELAY);
    uint32_t first = log_next > LOG_N ? log_next - LOG_N : 1;
    while (first < log_next && log_tw - logi[first % LOG_N].pos > LOG_TXT) first++;     /* text overwritten */
    if (since < first) since = first;
    uint32_t i = since;
    char t[LOG_LEN];
    for (; i < log_next; i++) {
        typeof(logi[0]) *e = &logi[i % LOG_N];
        if (o + LOG_LEN * 6 + 64 > cap) break;
        for (size_t k = 0; k < e->len; k++) t[k] = logt[(e->pos + k) % LOG_TXT];
        o += (size_t)snprintf(out + o, cap - o, "%s{\"n\":%lu,\"t\":%lu,\"src\":\"%s\",\"text\":\"",
                              o > 1 ? "," : "", (unsigned long)e->num, (unsigned long)e->t_ms, e->src);
        o += json_escape(out + o, cap - o, t, e->len);
        o += (size_t)snprintf(out + o, cap - o, "\"}");
    }
    *next = i;
    xSemaphoreGive(log_lock);
    o += (size_t)snprintf(out + o, cap - o, "]");
    return o;
}

/* ---- serial monitor: ring of the last 4 KiB, one direction bit per byte ---- */
#define MON_SIZE 2048u
static uint8_t mon_b[MON_SIZE], mon_d[MON_SIZE / 8];
static uint32_t mon_wr;
static SemaphoreHandle_t mon_lock;

void ser_mon_add(bool from_dc, const uint8_t *p, size_t n) {
    xSemaphoreTake(mon_lock, portMAX_DELAY);
    for (size_t i = 0; i < n; i++, mon_wr++) {
        uint32_t k = mon_wr % MON_SIZE;
        mon_b[k] = p[i];
        if (from_dc) mon_d[k / 8] |= (uint8_t)(1u << (k % 8)); else mon_d[k / 8] &= (uint8_t)~(1u << (k % 8));
    }
    xSemaphoreGive(mon_lock);
}

/* JSON: [{"d":"dc"|"esp","hex":"..."}...] in runs of one direction, bytes >= since */
size_t ser_mon_since(uint32_t since, char *out, size_t cap, uint32_t *next) {
    size_t o = 0;
    out[o++] = '[';
    xSemaphoreTake(mon_lock, portMAX_DELAY);
    uint32_t first = mon_wr > MON_SIZE ? mon_wr - MON_SIZE : 0;
    if (since < first || since > mon_wr) since = first;
    uint32_t i = since;
    int dir = -1;
    for (; i < mon_wr && o + 16 < cap; i++) {
        uint32_t k = i % MON_SIZE;
        int d = mon_d[k / 8] >> (k % 8) & 1;
        if (d != dir) {
            if (dir != -1) o += (size_t)snprintf(out + o, cap - o, "\"},");
            o += (size_t)snprintf(out + o, cap - o, "{\"d\":\"%s\",\"hex\":\"", d ? "dc" : "esp");
            dir = d;
        }
        o += (size_t)snprintf(out + o, cap - o, "%02x", mon_b[k]);
    }
    if (dir != -1) o += (size_t)snprintf(out + o, cap - o, "\"}");
    *next = i;
    xSemaphoreGive(mon_lock);
    o += (size_t)snprintf(out + o, cap - o, "]");
    return o;
}

void app_main(void) {
    log_lock = xSemaphoreCreateMutex();
    tls_mx = xSemaphoreCreateMutex();
    mon_lock = xSemaphoreCreateMutex();
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    const char *hi = "DC-PicoLink ESP32 side starting (steps E-1..E-4: link, web page, Internet over serial and over the cable's network)";
    log_add("esp32", hi, strlen(hi));
    wifi_start();
    link_start();
    ppp_srv_start();
    dc_eth_start();
    wired_start();
    usbmodem_start();
    vpn_start();
    pubip_start();
    netlink_start();
    portfwd_start();
    web_start();
}
