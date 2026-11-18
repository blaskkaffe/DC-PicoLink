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
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "dcnow_model.h"
#include "app.h"

static const char *TAG = "dcnow";
static dcnow_t dn;
static SemaphoreHandle_t lk;
static volatile bool online, enabled = true;
static bool src_on[4];
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
    if (source < 0 || source > 3) return;
    xSemaphoreTake(lk, portMAX_DELAY);
    bool was = src_on[0] || src_on[1] || src_on[2] || src_on[3];
    src_on[source] = on;
    bool now = src_on[0] || src_on[1] || src_on[2] || src_on[3];
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

/* the ID can be set to another device's (a DreamPi's), so dreamcast.online counts both as one:
   64 hex characters, from that DreamPi's MAC address or its log line "MAC address: <id>" */
static char shared_id[65];
static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
/* "" = this box's own; a MAC (any separators) or a 64-hex ID. false if it is neither. */
bool dcnow_set_id(const char *in) {
    char id[65] = "";
    char hx[65]; size_t n = 0;
    for (const char *p = in; *p && n < 64; p++) if (is_hex(*p)) hx[n++] = (char)(*p >= 'a' && *p <= 'f' ? *p - 32 : *p);
    for (const char *p = in; *p; p++) if (!is_hex(*p) && *p != ':' && *p != '-' && *p != '.' && *p != ' ') return false;
    hx[n] = 0;
    if (n == 12) {                                   /* a MAC: hashed as DreamPi does ("B8:27:EB:..") */
        char macs[18];
        snprintf(macs, sizeof macs, "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s", hx, hx + 2, hx + 4, hx + 6, hx + 8, hx + 10);
        sha_hex(macs, id);
    } else if (n == 64) {
        for (int i = 0; i < 64; i++) id[i] = (char)(hx[i] >= 'A' && hx[i] <= 'F' ? hx[i] + 32 : hx[i]);
        id[64] = 0;
    } else if (n) return false;
    if (!strcmp(id, shared_id)) return true;
    memcpy(shared_id, id, sizeof shared_id);
    nvs_handle_t h;
    if (nvs_open("dcl", NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, "dcnow_id", shared_id); nvs_commit(h); nvs_close(h); }
    char m[140]; int k = snprintf(m, sizeof m, shared_id[0] ? "DCNow!: reporting as %.12s... (shared with another device)" : "DCNow!: reporting with this box's own ID%s", shared_id);
    log_add("esp32", m, (size_t)k);
    return true;
}
const char *dcnow_shared_id(void) { return shared_id; }

/* the box's ID at DCNow!: SHA-256 of the Wi-Fi MAC as text (or the shared one) */
static void my_id(char *id) {
    if (shared_id[0]) { memcpy(id, shared_id, 65); return; }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[24];
    snprintf(macs, sizeof macs, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    sha_hex(macs, id);
}

/* ---- DreamPi's configuration service (config_server.py): http://dreampi.local:1998 ----
   dreamcast.online/now/configure asks it for the box's ID to link it to a user name, and
   switches the reports on (POST) or off (POST with "disable"). */
static esp_err_t cfg_reply(httpd_req_t *r) {
    char id[65], o[128];
    my_id(id);
    int k = snprintf(o, sizeof o, "{\"mac_address\": \"%s\", \"is_enabled\": %s}", id, enabled ? "true" : "false");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(r, o, k);
}
static esp_err_t cfg_get(httpd_req_t *r) { return cfg_reply(r); }
static esp_err_t cfg_post(httpd_req_t *r) {
    char b[256]; int n = 0;
    while (n < (int)sizeof b - 1 && n < (int)r->content_len) {
        int k = httpd_req_recv(r, b + n, sizeof b - 1 - (size_t)n);
        if (k <= 0) break;
        n += k;
    }
    b[n] = 0;
    bool on = !strstr(b, "disable");
    if (on != enabled) {
        dcnow_set_enabled(on);
        const char *m = on ? "DCNow!: switched on from dreamcast.online" : "DCNow!: switched off from dreamcast.online";
        log_add("esp32", m, strlen(m));
    }
    return cfg_reply(r);
}
static esp_err_t cfg_options(httpd_req_t *r) {                 /* a browser may ask first */
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Headers", "*");
    return httpd_resp_send(r, NULL, 0);
}

static void cfg_start(void) {
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = 1998; c.ctrl_port = 32769; c.stack_size = 3584;
    c.max_open_sockets = 2; c.lru_purge_enable = true;
    c.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t h;
    if (httpd_start(&h, &c) != ESP_OK) { const char *m = "DCNow!: the settings service (port 1998) could not start"; log_add("esp32", m, strlen(m)); return; }
    static const httpd_uri_t g = { .uri = "/*", .method = HTTP_GET, .handler = cfg_get };
    static const httpd_uri_t p = { .uri = "/*", .method = HTTP_POST, .handler = cfg_post };
    static const httpd_uri_t o = { .uri = "/*", .method = HTTP_OPTIONS, .handler = cfg_options };
    httpd_register_uri_handler(h, &g); httpd_register_uri_handler(h, &p); httpd_register_uri_handler(h, &o);
    /* the page looks for the box under DreamPi's name */
    if (mdns_init() == ESP_OK) { mdns_hostname_set("dreampi"); mdns_instance_name_set("DC-PicoLink (as DreamPi)"); }
    const char *m = "DCNow!: settings service on, as on a DreamPi (http://dreampi.local:1998, for dreamcast.online/now/configure)";
    log_add("esp32", m, strlen(m));
}

static char why[96];                                    /* the last failure, for the log */
static bool post_once(void) {
    char id[65], dom[DCNOW_NAME], body[16 + 64 + 1] = "";
    my_id(id);
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
    tls_lock();                                         /* one secure connection at a time: RAM */
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) { tls_unlock(); snprintf(why, sizeof why, "out of memory"); return false; }
    esp_http_client_set_header(h, "User-Agent", DCNOW_UA);
    esp_http_client_set_header(h, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(h, body, (int)strlen(body));
    esp_err_t e = esp_http_client_perform(h);
    int code = e == ESP_OK ? esp_http_client_get_status_code(h) : 0;
    bool ok = e == ESP_OK && code < 400;
    if (!ok) {                                       /* why: no route, no memory for TLS, or the server said no */
        if (e != ESP_OK) snprintf(why, sizeof why, "%s; largest free block %u", esp_err_to_name(e), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        else snprintf(why, sizeof why, "HTTP %d", code);
    }
    esp_http_client_cleanup(h);
    tls_unlock();
    return ok;
}

static void dcnow_task(void *arg) {
    (void)arg;
    for (;;) {
        if (online && enabled) {
            bool ok = post_once();
            if (ok) ok_n++; else fail_n++;
            if (!ok && !last_failed) { char m[140]; int k = snprintf(m, sizeof m, "DCNow!: report failed (%s); will keep trying", why); log_add("esp32", m, (size_t)(k < (int)sizeof m ? k : (int)sizeof m - 1)); }
            if (ok && last_failed) { const char *m = "DCNow!: reports work again"; log_add("esp32", m, strlen(m)); }
            last_failed = !ok;
            for (int i = 0; i < DCNOW_EVERY_MS / 250 && online; i++) { dnsd_fetch_poll(); dnat_fetch_poll(); vTaskDelay(pdMS_TO_TICKS(250)); }
        } else {
            dnsd_fetch_poll(); dnat_fetch_poll();
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
    { nvs_handle_t n; size_t l = sizeof shared_id;
      if (nvs_open("dcl", NVS_READONLY, &n) == ESP_OK) { if (nvs_get_str(n, "dcnow_id", shared_id, &l) != ESP_OK) shared_id[0] = 0; nvs_close(n); } }
    xTaskCreate(dcnow_task, "dcnow", 6144, NULL, 4, NULL);
    cfg_start();
    ESP_LOGI(TAG, "DCNow! reports %s", enabled ? "on" : "off");
}
