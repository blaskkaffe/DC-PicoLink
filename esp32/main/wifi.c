/*
 * Wi-Fi: always a setup network "DC-PicoLink-XXXX" (192.168.4.1), plus the home network
 * once one is saved (Settings on the web page). Saved in NVS ("wifi" / "ssid", "pass").
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "app.h"

static esp_netif_t *sta_if, *ap_if;
static char ap_name[33], sta_ssid[33];
static volatile bool sta_up;
static char sta_ip[16];

static void note(const char *fmt, const char *a) {
    char t[160];
    int k = snprintf(t, sizeof t, fmt, a);
    log_add("esp32", t, (size_t)k);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && sta_ssid[0]) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (sta_up) note("Wi-Fi: lost %s, trying again", sta_ssid);
        sta_up = false;
        if (sta_ssid[0]) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(sta_ip, sizeof sta_ip, IPSTR, IP2STR(&ev->ip_info.ip));
        sta_up = true;
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

size_t wifi_info_json(char *out, size_t cap) {
    char s[80], a[80];
    json_escape(s, sizeof s, sta_ssid, strlen(sta_ssid));
    json_escape(a, sizeof a, ap_name, strlen(ap_name));
    return (size_t)snprintf(out, cap, "{\"ap\":\"%s\",\"ssid\":\"%s\",\"up\":%s,\"ip\":\"%s\"}",
                            a, s, sta_up ? "true" : "false", sta_up ? sta_ip : "");
}

void wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_if = esp_netif_create_default_wifi_sta();
    ap_if = esp_netif_create_default_wifi_ap();
    (void)sta_if; (void)ap_if;
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
    note("Wi-Fi: setup network %s, page at http://192.168.4.1", ap_name);
    if (!ssid[0]) note("Wi-Fi: no home network saved yet%s", " (Settings on the page)");
}
