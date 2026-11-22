/*
 * Wi-Fi: always a setup network "DC-PicoLink-XXXX" (192.168.4.1), plus the home network
 * once one is saved (Settings on the web page). Saved in NVS ("wifi" / "ssid", "pass").
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "app.h"
#include "wifiscan_model.h"

static esp_netif_t *sta_if, *ap_if;
static char ap_name[33], sta_ssid[33];
static volatile bool sta_up;
static char sta_ip[16];
static volatile int last_reason;                            /* why the last connection attempt failed (0 = none since the last try began) */

#define note say_line

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && sta_ssid[0]) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (sta_up) {
            const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
            say_line("Wi-Fi: lost %s (reason %d, signal %d dBm), trying again", sta_ssid, d ? d->reason : -1, d ? d->rssi : 0);
        }
        sta_up = false;
        { const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data; if (d) last_reason = d->reason; }
        if (sta_ssid[0]) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(sta_ip, sizeof sta_ip, IPSTR, IP2STR(&ev->ip_info.ip));
        sta_up = true;
        last_reason = 0;
        char t[96];
        snprintf(t, sizeof t, "%s, address %s", sta_ssid, sta_ip);
        note("Wi-Fi: connected to %s", t);
    }
}

static void load(char *ssid, char *pass) {
    nvs_handle_t h;
    ssid[0] = pass[0] = 0;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = 33; nvs_get_str(h, "ssid", ssid, &n);
    n = 65; nvs_get_str(h, "pass", pass, &n);
    nvs_close(h);
}

static void apply_sta(const char *ssid, const char *pass) {
    wifi_config_t c = { 0 };
    strlcpy((char *)c.sta.ssid, ssid, sizeof c.sta.ssid);
    strlcpy((char *)c.sta.password, pass, sizeof c.sta.password);
    c.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    strlcpy(sta_ssid, ssid, sizeof sta_ssid);
    last_reason = 0;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &c);
    if (ssid[0]) esp_wifi_connect();
}

void wifi_set_sta(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
    note("Wi-Fi: home network set to %s", ssid);
    apply_sta(ssid, pass);
}

bool wifi_sta_up(void) { return sta_up; }

size_t wifi_info_json(char *out, size_t cap) {
    char s[80], a[80];
    json_escape(s, sizeof s, sta_ssid, strlen(sta_ssid));
    json_escape(a, sizeof a, ap_name, strlen(ap_name));
    return (size_t)snprintf(out, cap, "{\"ap\":\"%s\",\"ssid\":\"%s\",\"up\":%s,\"ip\":\"%s\",\"state\":\"%s\",\"err\":\"%s\"}",
                            a, s, sta_up ? "true" : "false", sta_up ? sta_ip : "", wifi_state_text(), wifi_error_text());
}

/* ---- the search for networks (the page's list and the DC API's wifi.scan) ---- */
static wscan_t scan_last;
static int scan_cnt = -1;                                           /* results of the last scan; -1 = none yet */

/* A scan while the box is online makes the radio leave its channel for a second or two, which a
   Dreamcast in a game would feel: `force` (the Dreamcast itself asked, over the DC API) skips the
   check. Returns the number found, -1 = refused (busy), -2 = did not run. */
int wifi_scan(bool force) {
    if (!force && dc_session_busy()) return -1;
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE, .scan_time.active = { .min = 100, .max = 200 } };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -2;       /* waits until done (about 2-3 s) */
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) n = 40;
    wifi_ap_record_t *rec = n ? malloc(sizeof *rec * n) : NULL;
    if (n && !rec) { esp_wifi_clear_ap_list(); return -2; }
    if (n) esp_wifi_scan_get_ap_records(&n, rec); else esp_wifi_clear_ap_list();
    memset(&scan_last, 0, sizeof scan_last);
    for (int i = 0; i < n; i++)
        wscan_add(&scan_last, (const char *)rec[i].ssid, strnlen((const char *)rec[i].ssid, 32), rec[i].rssi, rec[i].authmode == WIFI_AUTH_OPEN);
    free(rec);
    scan_cnt = scan_last.n;
    note("Wi-Fi: found %d networks", scan_cnt);
    return scan_cnt;
}
int wifi_scan_count(void) { return scan_cnt; }
bool wifi_scan_get(int i, int *rssi, int *open, char *ssid, size_t cap) {
    if (i < 0 || i >= scan_cnt) return false;
    *rssi = scan_last.ap[i].rssi; *open = scan_last.ap[i].open;
    strlcpy(ssid, scan_last.ap[i].ssid, cap);
    return true;
}
size_t wifi_scan_json(char *out, size_t cap) {
    int n = wifi_scan(false);
    if (n == -1) return (size_t)snprintf(out, cap, "{\"ok\":false,\"err\":\"The Dreamcast is online: searching would interrupt it. Try again when it is idle.\"}");
    if (n < 0) return (size_t)snprintf(out, cap, "{\"ok\":false,\"err\":\"the scan did not run\"}");
    return wscan_json(&scan_last, out, cap);
}

const char *wifi_state_text(void) { return !sta_ssid[0] ? "none" : sta_up ? "connected" : last_reason ? "failed" : "connecting"; }
const char *wifi_error_text(void) { return sta_up ? "" : wscan_reason_text(last_reason); }
const char *wifi_saved_ssid(void) { return sta_ssid; }
const char *wifi_ip_text(void) { return sta_up ? sta_ip : ""; }

void wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_if = esp_netif_create_default_wifi_sta();
    ap_if = esp_netif_create_default_wifi_ap();
    (void)sta_if;
    {   /* the setup network's own address (the page is at http://192.168.4.1), a /24 of its own */
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_str_to_ip4(DCL_AP_IP, &ip.ip);
        ip.gw = ip.ip;
        esp_netif_str_to_ip4("255.255.255.0", &ip.netmask);
        esp_netif_dhcps_stop(ap_if);
        esp_netif_set_ip_info(ap_if, &ip);
        esp_netif_dhcps_start(ap_if);
    }
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ap_name, sizeof ap_name, "DC-PicoLink-%02X%02X", mac[4], mac[5]);
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ap_name, sizeof ap.ap.ssid);
    ap.ap.ssid_len = (uint8_t)strlen(ap_name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    const char *pw = CONFIG_DCL_AP_PASSWORD;
    if (strlen(pw) >= 8) { strlcpy((char *)ap.ap.password, pw, sizeof ap.ap.password); ap.ap.authmode = WIFI_AUTH_WPA2_PSK; }
    else ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    char ssid[33], pass[65];
    load(ssid, pass);
    wifi_config_t sc = { 0 };
    strlcpy((char *)sc.sta.ssid, ssid, sizeof sc.sta.ssid);
    strlcpy((char *)sc.sta.password, pass, sizeof sc.sta.password);
    strlcpy(sta_ssid, ssid, sizeof sta_ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);                          /* no modem sleep: it adds 100-300 ms spikes to game traffic */
    note("Wi-Fi: setup network %s, page at http://" DCL_AP_IP, ap_name);
    if (!ssid[0]) note("Wi-Fi: no home network saved yet%s", " (Settings on the page)");
}
