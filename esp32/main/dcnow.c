/*
 * DCNow! reports (what dcnow.py does on a DreamPi). While a call is up, every 15 seconds:
 *   POST https://dcnow-2016.appspot.com/api/update/<sha256 of the MAC>/
 *   body  dns_query=<sha256 of the newest domain the Dreamcast looked up>   (empty if none)
 * Rules for which domain: ../../firmware/dcnow_model.h. The ID is the SHA-256 of the box's
 * Wi-Fi MAC written as "AA:BB:CC:DD:EE:FF", like DreamPi hashes its Pi's MAC.
 * Can be switched off (kept in flash).
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "dcnow_model.h"
#include "app.h"

static const char *TAG = "dcnow";
static dcnow_t dn;
static SemaphoreHandle_t lk;
static volatile bool online, enabled = true;
static bool src_on[2];
static volatile uint32_t ok_n, fail_n;
static volatile bool last_failed;

static void sha_hex(const char *s, char *out) {
    uint8_t h[32];
    mbedtls_sha256((const unsigned char *)s, strlen(s), h, 0);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

void dcnow_note_domain(const char *d) {
    xSemaphoreTake(lk, portMAX_DELAY);
    dcnow_note(&dn, d);
    xSemaphoreGive(lk);
}

void dcnow_set_online(int source, bool on) {
    if (source < 0 || source > 1) return;
    xSemaphoreTake(lk, portMAX_DELAY);
    bool was = src_on[0] || src_on[1];
    src_on[source] = on;
    bool now = src_on[0] || src_on[1];
    if (now && !was) dcnow_reset(&dn);                       /* a new session */
    xSemaphoreGive(lk);
    online = now;
    if (enabled && now != was) {
        const char *m = now ? "DCNow!: session started" : "DCNow!: session ended";
        log_add("esp32", m, strlen(m));
    }
}

bool dcnow_enabled(void) { return enabled; }

void dcnow_set_enabled(bool on) {
    enabled = on;
    nvs_handle_t h;
    if (nvs_open("dcl", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "dcnow", on); nvs_commit(h); nvs_close(h); }
}

void dcnow_info(uint32_t *ok, uint32_t *fail) { *ok = ok_n; *fail = fail_n; }

static bool post_once(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[24], id[65], dom[DCNOW_NAME], body[16 + 64 + 1] = "";
    snprintf(macs, sizeof macs, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    sha_hex(macs, id);
    xSemaphoreTake(lk, portMAX_DELAY);
    bool have = dcnow_pick(&dn, dom);
    xSemaphoreGive(lk);
    if (have) {
        char hx[65]; sha_hex(dom, hx);
        snprintf(body, sizeof body, "dns_query=%s", hx);
        char t[DCNOW_NAME + 40]; int k = snprintf(t, sizeof t, "DCNow!: domain sent: %s", dom);
        log_add("esp32", t, (size_t)k);
    }
    char url[160];
    snprintf(url, sizeof url, "https://" DCNOW_HOST DCNOW_PATH, id);
    esp_http_client_config_t c = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return false;
    esp_http_client_set_header(h, "User-Agent", DCNOW_UA);
    esp_http_client_set_header(h, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(h, body, (int)strlen(body));
    bool ok = esp_http_client_perform(h) == ESP_OK && esp_http_client_get_status_code(h) < 400;
    esp_http_client_cleanup(h);
    return ok;
}

static void dcnow_task(void *arg) {
    (void)arg;
    for (;;) {
        if (online && enabled) {
            bool ok = post_once();
            if (ok) ok_n++; else fail_n++;
            if (!ok && !last_failed) { const char *m = "DCNow!: report failed (no internet?); will keep trying"; log_add("esp32", m, strlen(m)); }
            if (ok && last_failed) { const char *m = "DCNow!: reports work again"; log_add("esp32", m, strlen(m)); }
            last_failed = !ok;
            for (int i = 0; i < DCNOW_EVERY_MS / 250 && online; i++) vTaskDelay(pdMS_TO_TICKS(250));
        } else {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}

void dcnow_start(void) {
    lk = xSemaphoreCreateMutex();
    dcnow_reset(&dn);
    nvs_handle_t h; uint8_t v = 1;
    if (nvs_open("dcl", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "dcnow", &v); nvs_close(h); }
    enabled = v != 0;
    xTaskCreate(dcnow_task, "dcnow", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "DCNow! reports %s", enabled ? "on" : "off");
}
