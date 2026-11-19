/*
 * DCNow! reports (what dcnow.py does on a DreamPi). While a call is up, every 15 seconds:
 *   POST https://dcnow-2016.appspot.com/api/update/<sha256 of the MAC>/
 *   body  dns_query=<sha256 of the newest domain the Dreamcast looked up>   (empty if none)
 * Rules for which domain: ../../firmware/dcnow_model.h. The ID is the SHA-256 of the box's
 * Wi-Fi MAC written as "AA:BB:CC:DD:EE:FF", like DreamPi hashes its Pi's MAC; it can be swapped
 * for a cloned DreamPi's ID (see dcnow_clone).
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
#include "esp_netif.h"
#include "esp_timer.h"
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

/* The ID can be another device's (a DreamPi's): then dreamcast.online counts both as one.
   It gets there by being typed (a MAC or a 64-hex ID; only before the box has joined a Wi-Fi
   network the first time: DCNow! cannot remove entries, so typing must not be a way to make
   up devices) or by cloning a DreamPi on the same network.
   sel: 0 = this box's own ID, 1 = the other device's always, 2 = the other device's only while
   it is not on the network (two devices must not report as one at the same time). The box
   looks for it every 30 s (its settings service on port 1998, at the address it was cloned
   from or as dreampi.local); it is "away" after 3 misses, "back" at the first answer. The
   ID changes only while no session is running. */
static char shared_id[65];
static bool use_other;                       /* the ID in use is the other device's */
static uint8_t sel;
static bool owner_here;
static int misses;
static uint32_t src_ip;                      /* where it was cloned from (network order), 0 = unknown */
static char clone_msg[100];
static volatile bool cloning;
static int64_t next_probe_us;
static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static void save_other(void) {
    nvs_handle_t h;
    if (nvs_open("dcl", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "dcnow_id", shared_id); nvs_set_u8(h, "dcnow_sel", sel); nvs_set_u32(h, "dcnow_src", src_ip);
        nvs_commit(h); nvs_close(h);
    }
}
static void say_id(void) {
    char m[130]; int k = use_other ? snprintf(m, sizeof m, "DCNow!: reporting as %.12s... (the other device's ID)", shared_id)
                                   : snprintf(m, sizeof m, "DCNow!: reporting with this box's own ID");
    log_add("esp32", m, (size_t)k);
}
/* the ID in use follows sel and the other device's presence; not while a session runs (unless forced) */
static void refresh_eff(bool force) {
    bool want = shared_id[0] && (sel == 1 || (sel == 2 && !owner_here));
    if (want == use_other || (online && !force)) return;
    use_other = want;
    say_id();
}
static void set_other(const char *id, uint8_t mode) {
    if (!strcmp(id, shared_id) && mode == sel) return;
    memcpy(shared_id, id, 65);
    sel = shared_id[0] ? mode : 0;
    misses = 0;
    save_other();
    refresh_eff(true);
}

/* typed: "" = back to this box's own; a MAC (any separators) or a 64-hex ID. false if it is
   neither, or if the box has already been on a Wi-Fi network. */
bool dcnow_set_id(const char *in) {
    char id[65] = "";
    char hx[65]; size_t n = 0;
    for (const char *p = in; *p && n < 64; p++) if (is_hex(*p)) hx[n++] = (char)(*p >= 'a' && *p <= 'f' ? *p - 32 : *p);
    for (const char *p = in; *p; p++) if (!is_hex(*p) && *p != ':' && *p != '-' && *p != '.' && *p != ' ') return false;
    hx[n] = 0;
    if (n == 0) { src_ip = 0; set_other("", 0); return true; }
    if (wifi_joined_once()) return false;
    if (n == 12) {                                   /* a MAC: hashed as DreamPi does ("B8:27:EB:..") */
        char macs[18];
        snprintf(macs, sizeof macs, "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s", hx, hx + 2, hx + 4, hx + 6, hx + 8, hx + 10);
        sha_hex(macs, id);
    } else if (n == 64) {
        for (int i = 0; i < 64; i++) id[i] = (char)(hx[i] >= 'A' && hx[i] <= 'F' ? hx[i] + 32 : hx[i]);
        id[64] = 0;
    } else return false;
    src_ip = 0;
    set_other(id, 1);
    return true;
}
const char *dcnow_shared_id(void) { return shared_id; }
void dcnow_set_mode(int m) { if (shared_id[0] && m >= 0 && m <= 2) set_other(shared_id, (uint8_t)m); }

/* this box's own ID at DCNow!: SHA-256 of the Wi-Fi MAC as text */
static void own_id(char *id) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[24];
    snprintf(macs, sizeof macs, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    sha_hex(macs, id);
}
static void my_id(char *id) {
    if (use_other && shared_id[0]) memcpy(id, shared_id, 65); else own_id(id);
}

size_t dcnow_id_json(char *o, size_t cap) {
    char own[65]; own_id(own);
    return (size_t)snprintf(o, cap, "{\"own\":\"%.12s\",\"other\":\"%.12s\",\"mode\":%u,\"use\":%s,\"here\":%s,\"locked\":%s,\"busy\":%s,\"msg\":\"%s\"}",
        own, shared_id, (unsigned)sel, use_other ? "true" : "false", owner_here ? "true" : "false",
        wifi_joined_once() ? "true" : "false", cloning ? "true" : "false", clone_msg);
}

static uint32_t my_ip(void) {
    esp_netif_ip_info_t me = { 0 };
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_netif_get_ip_info(sta, &me) != ESP_OK) return 0;
    return me.ip.addr;
}

/* ask a settings service (port 1998, the one dreamcast.online uses) for its ID; NULL = got it,
   else why not */
static const char *probe(uint32_t addr, int timeout_ms, char *id) {
    char url[48], buf[320];
    esp_ip4_addr_t a = { .addr = addr };
    snprintf(url, sizeof url, "http://" IPSTR ":1998/", IP2STR(&a));
    esp_http_client_config_t c = { .url = url, .timeout_ms = timeout_ms };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    int n = -1;
    if (h && esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0) n = esp_http_client_read(h, buf, sizeof buf - 1);
    if (h) { esp_http_client_close(h); esp_http_client_cleanup(h); }
    if (n <= 0) return "the DreamPi's settings service did not answer";
    buf[n] = 0;
    return dcnow_parse_cfg(buf, id) ? NULL : "the answer had no DCNow! ID in it";
}

/* every 30 s, called from the DCNow! task: is the device whose ID we use on the network? */
void dcnow_presence_poll(void) {
    if (sel != 2 || !shared_id[0] || cloning) return;
    int64_t now = esp_timer_get_time();
    if (now < next_probe_us) return;
    next_probe_us = now + 30 * 1000000LL;
    uint32_t me = my_ip();
    if (!me) return;
    char id[65];
    bool found = src_ip && src_ip != me && !probe(src_ip, 1500, id) && !strcmp(id, shared_id);
    if (!found) {
        esp_ip4_addr_t a = { 0 };
        if (mdns_query_a("dreampi", 1500, &a) == ESP_OK && a.addr && a.addr != me && !probe(a.addr, 1500, id) && !strcmp(id, shared_id)) {
            found = true;
            if (a.addr != src_ip) { src_ip = a.addr; save_other(); }
        }
    }
    if (found) { misses = 0; owner_here = true; }
    else if (++misses >= 3) owner_here = false;
    refresh_eff(false);
}

/* Clone a DreamPi: copy its ID. Without an address, "dreampi.local" is looked up; this box
   answers to that name too, so an answer from itself does not count. */
static char clone_ip[16];
static void clone_task(void *arg) {
    (void)arg;
    esp_ip4_addr_t a = { 0 };
    const char *why_not = NULL;
    if (clone_ip[0]) {
        a.addr = esp_ip4addr_aton(clone_ip);
        if (!a.addr) why_not = "that is not an IPv4 address";
    } else if (mdns_query_a("dreampi", 3000, &a) != ESP_OK || !a.addr) {
        why_not = "no DreamPi answered on this network (type its address to try that)";
    }
    uint32_t me = my_ip();
    if (!why_not && !me) why_not = "this box is not on a Wi-Fi network";
    if (!why_not && a.addr == me) why_not = "only this box answered, not a DreamPi (type the DreamPi's address)";
    char id[65] = "";
    if (!why_not) why_not = probe(a.addr, 4000, id);
    if (!why_not) {
        char own[65]; own_id(own);
        if (!strcmp(own, id)) why_not = "that ID is this box's own";
    }
    if (!why_not) {
        src_ip = a.addr;
        owner_here = true; misses = 0;                       /* it just answered */
        set_other(id, 2);
        save_other();
        snprintf(clone_msg, sizeof clone_msg, "cloned the DreamPi at " IPSTR " (%.12s...)", IP2STR(&a), id);
    } else snprintf(clone_msg, sizeof clone_msg, "clone failed: %s", why_not);
    log_add("esp32", clone_msg, strlen(clone_msg));
    cloning = false;
    vTaskDelete(NULL);
}
void dcnow_clone(const char *ip) {
    if (cloning) return;
    cloning = true;
    snprintf(clone_ip, sizeof clone_ip, "%s", ip ? ip : "");
    snprintf(clone_msg, sizeof clone_msg, "looking for a DreamPi...");
    if (xTaskCreate(clone_task, "dclone", 5120, NULL, 3, NULL) != pdPASS) { cloning = false; snprintf(clone_msg, sizeof clone_msg, "clone failed: out of memory"); }
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
        static char said[DCNOW_NAME];                /* each new game's domain once, not every 15 s */
        if (strcmp(said, dom)) {
            snprintf(said, sizeof said, "%s", dom);
            char t[DCNOW_NAME + 40]; int k = snprintf(t, sizeof t, "DCNow!: domain sent: %s", dom);
            log_add("esp32", t, (size_t)k);
        }
    }
    char url[160];
    snprintf(url, sizeof url, "https://" DCNOW_HOST DCNOW_PATH, id);
    esp_err_t e;
    int code = https_req(url, NULL, 15000, DCNOW_UA, body, &e);
    bool ok = e == ESP_OK && code > 0 && code < 400;
    if (!ok) {                                       /* why: no route, no memory for TLS, or the server said no */
        if (e != ESP_OK) snprintf(why, sizeof why, "%s; largest free block %u", esp_err_to_name(e), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        else snprintf(why, sizeof why, "HTTP %d", code);
    }
    return ok;
}

static void dcnow_task(void *arg) {
    (void)arg;
    for (;;) {
        if (online && enabled && inet_up()) {
            bool ok = post_once();
            if (ok) ok_n++; else fail_n++;
            if (!ok && !last_failed) { char m[140]; int k = snprintf(m, sizeof m, "DCNow!: report failed (%s); will keep trying", why); log_add("esp32", m, (size_t)(k < (int)sizeof m ? k : (int)sizeof m - 1)); }
            if (ok && last_failed) { const char *m = "DCNow!: reports work again"; log_add("esp32", m, strlen(m)); }
            last_failed = !ok;
            for (int i = 0; i < DCNOW_EVERY_MS / 250 && online; i++) { dnsd_fetch_poll(); dnat_fetch_poll(); dcnow_presence_poll(); vTaskDelay(pdMS_TO_TICKS(250)); }
        } else {
            dnsd_fetch_poll(); dnat_fetch_poll(); dcnow_presence_poll();
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
    { nvs_handle_t n; size_t l = sizeof shared_id; uint8_t sel8 = 1;     /* an ID saved by an older version is in use */
      if (nvs_open("dcl", NVS_READONLY, &n) == ESP_OK) {
          if (nvs_get_str(n, "dcnow_id", shared_id, &l) != ESP_OK) shared_id[0] = 0;
          nvs_get_u8(n, "dcnow_sel", &sel8); nvs_get_u32(n, "dcnow_src", &src_ip); nvs_close(n); }
      sel = shared_id[0] ? (sel8 > 2 ? 1 : sel8) : 0;
      use_other = shared_id[0] && sel == 1; }       /* sel 2 starts on the own ID until the other device has been looked for */
    xTaskCreate(dcnow_task, "dcnow", 6144, NULL, 4, NULL);
    cfg_start();
    ESP_LOGI(TAG, "DCNow! reports %s", enabled ? "on" : "off");
}
