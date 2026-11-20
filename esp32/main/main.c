/*
 * DC-PicoLink ESP32 side, step E-2: link to the cable, Wi-Fi, and a web page that shows
 * the cable's status lines and the serial traffic. PPP / DCNow! (E-3) and the W5500
 * network (E-4) come next and plug into link.c's deliver().
 */
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "freertos/task.h"
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

/* a status line (shared by all parts: one copy of the formatting code; lines are rare, so the
   call costs nothing that matters) */
void say_line(const char *fmt, ...) {
    char t[180];
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    log_add("esp32", t, (size_t)(k < (int)sizeof t ? k : (int)sizeof t - 1));
}

/* one HTTPS request, one at a time (tls_lock: RAM). The body goes to ev as it arrives, so
   nothing is copied twice. post: a form to send (POST), NULL = GET. Returns the HTTP status,
   0 if it failed (*er says why). */
int https_req(const char *url, http_event_handle_cb ev, int timeout_ms, const char *ua, const char *post, esp_err_t *er) {
    esp_http_client_config_t c = { .url = url, .event_handler = ev, .timeout_ms = timeout_ms,
                                   .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 1024,
                                   .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET };
    tls_lock();
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_err_t e = ESP_ERR_NO_MEM;
    int code = 0;
    if (h) {
        if (ua) esp_http_client_set_header(h, "User-Agent", ua);
        if (post) {
            esp_http_client_set_header(h, "Content-Type", "application/x-www-form-urlencoded");
            esp_http_client_set_post_field(h, post, (int)strlen(post));
        }
        e = esp_http_client_perform(h);
        if (e == ESP_OK) code = esp_http_client_get_status_code(h);
        esp_http_client_cleanup(h);
    }
    tls_unlock();
    if (er) *er = e;
    return code;
}

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
        if (o + (size_t)e->len * 6 + 64 > cap) break;          /* room for this line, escaped */
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

/* ---- serial monitor: the last 2 KiB of serial bytes, one direction bit per byte. Off by
   default (a switch on the page; off again at restart): its buffer exists only while it is on,
   and while off each call costs one test ---- */
#define MON_SIZE 2048u
static uint8_t *mon_b, *mon_d;                 /* MON_SIZE bytes + MON_SIZE/8 direction bits */
static volatile bool mon_on;
static uint32_t mon_wr;
static SemaphoreHandle_t mon_lock;

bool ser_mon_on(void) { return mon_on; }
void ser_mon_enable(bool on) {
    xSemaphoreTake(mon_lock, portMAX_DELAY);
    if (on && !mon_b) {
        mon_b = calloc(1, MON_SIZE + MON_SIZE / 8);
        mon_d = mon_b ? mon_b + MON_SIZE : NULL;
        mon_wr = 0;
    } else if (!on && mon_b) { free(mon_b); mon_b = mon_d = NULL; mon_wr = 0; }
    mon_on = mon_b != NULL;
    xSemaphoreGive(mon_lock);
}

void ser_mon_add(bool from_dc, const uint8_t *p, size_t n) {
    if (!mon_on) return;
    xSemaphoreTake(mon_lock, portMAX_DELAY);
    if (!mon_b) { xSemaphoreGive(mon_lock); return; }
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
    if (!mon_b) { xSemaphoreGive(mon_lock); *next = 0; out[o++] = ']'; return o; }
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

/* memory report: 5 minutes after start, then every 30: how much RAM is left and how much of each
   task's stack was never used (what each stack can safely be cut to) */
static void mem_report(void *arg) {
    (void)arg;
    static const char *const names[] = { "vpn", "dcnow", "dnsd", "ppp_ctl", "link_rx", "link_hello", "portfwd", "pubip",
                                         "dclan", "dc_eth", "usbmodem", "usb_lib", "wired", "dcnet", "tiT", "mdns", "httpd",
                                         "esp_timer", "sys_evt", "wifi" };
    char m[LOG_LEN];
    int k = snprintf(m, sizeof m, "memory: free %u, largest block %u, lowest ever %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    log_add("esp32", m, (size_t)k);
    k = snprintf(m, sizeof m, "unused stack (bytes):");
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        TaskHandle_t t = xTaskGetHandle(names[i]);
        if (!t) continue;
        if (k > (int)sizeof m - 24) { log_add("esp32", m, (size_t)k); k = snprintf(m, sizeof m, "unused stack, more:"); }
        k += snprintf(m + k, sizeof m - (size_t)k, " %s %u", names[i], (unsigned)uxTaskGetStackHighWaterMark(t));
    }
    log_add("esp32", m, (size_t)k);
}

/* after an update from the page: the new image counts as good once Wi-Fi or the cable link has
   worked for 30 s; until then a crash or restart makes the bootloader go back to the old one */
static esp_timer_handle_t ota_t;
static void ota_check(void *arg) {
    (void)arg;
    static int ok_for;
    esp_timer_handle_t t = ota_t;
    esp_ota_img_states_t stt;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &stt) != ESP_OK || stt != ESP_OTA_IMG_PENDING_VERIFY) { esp_timer_stop(t); return; }
    ok_for = (inet_up() || link_is_up()) ? ok_for + 5 : 0;      /* Wi-Fi or the wired uplink, or the cable */
    if (ok_for < 30) return;
    esp_ota_mark_app_valid_cancel_rollback();
    const char *m = "ESP32 update: the new firmware works; kept";
    log_add("esp32", m, strlen(m));
    esp_timer_stop(t);
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
    usbmodem_start();                /* first: the USB host needs internal DMA memory before Wi-Fi and the rest take it */
    wifi_start();
    link_start();
    ppp_srv_start();
    dc_eth_start();
    wired_start();
    vpn_start();
    pubip_start();
    netlink_start();
    dclan_start();
    portfwd_start();
    web_start();
    const esp_timer_create_args_t oa = { .callback = ota_check, .name = "otachk" };
    if (esp_timer_create(&oa, &ota_t) == ESP_OK) esp_timer_start_periodic(ota_t, 5ULL * 1000000);
    static esp_timer_handle_t mt;
    const esp_timer_create_args_t ma = { .callback = mem_report, .name = "memrep" };
    if (esp_timer_create(&ma, &mt) == ESP_OK) esp_timer_start_periodic(mt, 30ULL * 60 * 1000000);
    static esp_timer_handle_t m1;
    const esp_timer_create_args_t m1a = { .callback = mem_report, .name = "memrep1" };
    if (esp_timer_create(&m1a, &m1) == ESP_OK) esp_timer_start_once(m1, 5ULL * 60 * 1000000);
}
