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

/* ---- status lines: ring of the last 200 ---- */
#define LOG_N   200
#define LOG_LEN 200
static struct { uint32_t num; int64_t t_ms; char src[6]; char text[LOG_LEN]; } logr[LOG_N];
static uint32_t log_next = 1;
static SemaphoreHandle_t log_lock;

void log_add(const char *src, const char *text, size_t n) {
    if (n >= LOG_LEN) n = LOG_LEN - 1;
    xSemaphoreTake(log_lock, portMAX_DELAY);
    typeof(logr[0]) *e = &logr[log_next % LOG_N];
    e->num = log_next++;
    e->t_ms = esp_timer_get_time() / 1000;
    strlcpy(e->src, src, sizeof e->src);
    memcpy(e->text, text, n); e->text[n] = 0;
    xSemaphoreGive(log_lock);
    ESP_LOGI(src, "%s", e->text);
}

/* JSON array of the lines numbered >= since (oldest first); *next = number to ask for next time */
size_t log_since(uint32_t since, char *out, size_t cap, uint32_t *next) {
    size_t o = 0;
    out[o++] = '[';
    xSemaphoreTake(log_lock, portMAX_DELAY);
    uint32_t first = log_next > LOG_N ? log_next - LOG_N : 1;
    if (since < first) since = first;
    uint32_t i = since;
    for (; i < log_next; i++) {
        typeof(logr[0]) *e = &logr[i % LOG_N];
        if (o + LOG_LEN * 6 + 64 > cap) break;
        o += (size_t)snprintf(out + o, cap - o, "%s{\"n\":%lu,\"t\":%lld,\"src\":\"%s\",\"text\":\"",
                              o > 1 ? "," : "", (unsigned long)e->num, (long long)e->t_ms, e->src);
        o += json_escape(out + o, cap - o, e->text, strlen(e->text));
        o += (size_t)snprintf(out + o, cap - o, "\"}");
    }
    *next = i;
    xSemaphoreGive(log_lock);
    o += (size_t)snprintf(out + o, cap - o, "]");
    return o;
}

/* ---- serial monitor: ring of the last 8 KiB, tagged by direction ---- */
#define MON_SIZE 8192u
static struct { uint8_t b; uint8_t from_dc; } mon[MON_SIZE];
static uint32_t mon_wr;
static SemaphoreHandle_t mon_lock;

void ser_mon_add(bool from_dc, const uint8_t *p, size_t n) {
    xSemaphoreTake(mon_lock, portMAX_DELAY);
    for (size_t i = 0; i < n; i++) { mon[mon_wr % MON_SIZE].b = p[i]; mon[mon_wr % MON_SIZE].from_dc = from_dc; mon_wr++; }
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
        int d = mon[i % MON_SIZE].from_dc;
        if (d != dir) {
            if (dir != -1) o += (size_t)snprintf(out + o, cap - o, "\"},");
            o += (size_t)snprintf(out + o, cap - o, "{\"d\":\"%s\",\"hex\":\"", d ? "dc" : "esp");
            dir = d;
        }
        o += (size_t)snprintf(out + o, cap - o, "%02x", mon[i % MON_SIZE].b);
    }
    if (dir != -1) o += (size_t)snprintf(out + o, cap - o, "\"}");
    *next = i;
    xSemaphoreGive(mon_lock);
    o += (size_t)snprintf(out + o, cap - o, "]");
    return o;
}

void app_main(void) {
    log_lock = xSemaphoreCreateMutex();
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
    vpn_start();
    web_start();
}
