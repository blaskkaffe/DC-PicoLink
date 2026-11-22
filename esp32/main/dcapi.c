/*
 * The DC API on the box's web port, the way DC-DreamDash offers it on a DreamPi (the shared
 * language `dc1`, docs/unified/ and DC-DreamDash docs/dcapi.md, openmenu.md), so openMenu and
 * the other family devices can read and switch the network the same way on either:
 *
 *   POST /dcapi            dc1 text: get hello / net.* / wifi.*, call net.select dcnow|dcnet,
 *                          call wifi.scan, set wifi.pass, call wifi.join   (dcapi_model.h)
 *   GET  /dcapi            what `get hello`, `get net.*` and the Wi-Fi state answer, for a browser
 *   POST /dcnow, /dcnet    openMenu's "Use DCNow!" / "Use DCNET" buttons (204)
 *   GET  /tag              DCNOW / DCNET (/tag?text: "Running DCNow!" / "Running DCNet!")
 *   GET  /openmenu/poll    the network lines of openMenu's poll: openmenu 1, NET, CALL, DCNET
 *                          (the game list, players and events lines are not kept by this box)
 *
 * The choice applies from the next call; a call in progress keeps its network. A request that
 * carries another site's Origin is refused (a page in a browser must not switch the network);
 * the Dreamcast sends none. No PIN: like the page itself.
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "app.h"
#include "dcapi_model.h"

const char *netsel_call(void) { return dcnet_active() ? "dcnet" : ppp_srv_up() ? "dcnow" : NULL; }
bool dc_session_busy(void) { return dcnet_active() || ppp_srv_up() || dc_eth_is_active(); }

static void state(dcapi_state_t *s) {
    const char *c = netsel_call();
    s->selected = dcnet_selected();
    s->call = !c ? -1 : !strcmp(c, "dcnet") ? 1 : 0;
    s->dcnet_ok = inet_up();
    s->version = esp_app_get_description()->version;
    s->wifi_state = wifi_state_text();
    s->wifi_error = wifi_error_text();
    s->wifi_ssid = wifi_saved_ssid();
    s->wifi_ip = wifi_ip_text();
    s->scan_n = wifi_scan_count();
}

static int do_select(void *ctx, int dcnet) { dcnet_select(dcnet != 0, (const char *)ctx); return 1; }

/* the Wi-Fi part: the password of `set wifi.pass` waits here (RAM only) until `call wifi.join` */
static char pend_pass[64];
static int do_scan(void *ctx) { (void)ctx; return wifi_scan(true); }                /* the Dreamcast asked: allowed even in a session */
static int do_scan_get(void *ctx, int i, int *rssi, int *open, char *ssid, size_t cap) { (void)ctx; return wifi_scan_get(i, rssi, open, ssid, cap); }
static int do_set_pass(void *ctx, const char *p) { (void)ctx; strlcpy(pend_pass, p, sizeof pend_pass); return 1; }
static int do_join(void *ctx, const char *ssid) {
    (void)ctx;
    wifi_set_sta(ssid, pend_pass);
    memset(pend_pass, 0, sizeof pend_pass);
    return 1;
}
static const dcapi_ops_t ops = { do_select, do_scan, do_scan_get, do_set_pass, do_join };

/* Origin (when there is one) must be this box: a browser page of another site is refused */
static bool origin_ok(httpd_req_t *r) {
    char o[100], h[64];
    if (httpd_req_get_hdr_value_str(r, "Origin", o, sizeof o) != ESP_OK) return true;     /* no Origin: not a browser page */
    if (httpd_req_get_hdr_value_str(r, "Host", h, sizeof h) != ESP_OK) return false;
    const char *p = strstr(o, "://");
    return p && !strcasecmp(p + 3, h);
}

static esp_err_t refuse(httpd_req_t *r) { return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "another site's request"); }

static esp_err_t h_dcapi_post(httpd_req_t *r) {
    if (!origin_ok(r)) return refuse(r);
    char b[600];
    if (r->content_len >= sizeof b) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "too long");
    size_t got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, b + got, r->content_len - got);
        if (n <= 0) return httpd_resp_send_err(r, HTTPD_408_REQ_TIMEOUT, "no body");
        got += (size_t)n;
    }
    char *o = malloc(2400);                                   /* a scan list is up to 24 lines */
    if (!o) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    dcapi_state_t s; state(&s);
    size_t n = dcapi_handle(b, got, &s, &ops, (void *)"DC API", o, 2400);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    esp_err_t e = httpd_resp_send(r, o, (ssize_t)n);
    memset(b, 0, sizeof b);                                   /* the body may hold a password */
    free(o);
    return e;
}

static esp_err_t h_dcapi_get(httpd_req_t *r) {
    char o[700];
    static const char q[] = "dc1\nget hello\nget net.*\nget wifi.state\nget wifi.ssid\nget wifi.ip\nget wifi.error\n";
    dcapi_state_t s; state(&s);
    size_t n = dcapi_handle(q, sizeof q - 1, &s, &ops, NULL, o, sizeof o);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    return httpd_resp_send(r, o, (ssize_t)n);
}

static esp_err_t pick(httpd_req_t *r, bool dcnet) {
    if (!origin_ok(r)) return refuse(r);
    char buf[16];
    while (r->content_len && httpd_req_recv(r, buf, sizeof buf) > 0) { }                /* nothing in the body matters */
    dcnet_select(dcnet, "openMenu");
    httpd_resp_set_status(r, "204 No Content");
    return httpd_resp_send(r, NULL, 0);
}
static esp_err_t post_dcnow(httpd_req_t *r) { return pick(r, false); }
static esp_err_t post_dcnet(httpd_req_t *r) { return pick(r, true); }

static esp_err_t get_tag(httpd_req_t *r) {
    char q[16];
    bool text = httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK && !strncmp(q, "text", 4);
    bool net = dcnet_selected();
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, text ? (net ? "Running DCNet!\n" : "Running DCNow!\n") : (net ? "DCNET\n" : "DCNOW\n"));
}

static esp_err_t get_poll(httpd_req_t *r) {
    char o[120];
    const char *c = netsel_call();
    int n = snprintf(o, sizeof o, "openmenu 1\nNET %s\n", dcnet_selected() ? "dcnet" : "dcnow");
    if (c) n += snprintf(o + n, sizeof o - (size_t)n, "CALL %s\n", c);
    snprintf(o + n, sizeof o - (size_t)n, inet_up() ? "DCNET ok\n" : "DCNET off offline\n");
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, o);
}

void dcapi_register(void *httpd_handle) {
    httpd_handle_t h = (httpd_handle_t)httpd_handle;
    static const httpd_uri_t u[] = {
        { .uri = "/dcapi", .method = HTTP_POST, .handler = h_dcapi_post },
        { .uri = "/dcapi", .method = HTTP_GET, .handler = h_dcapi_get },
        { .uri = "/dcnow", .method = HTTP_POST, .handler = post_dcnow },
        { .uri = "/dcnet", .method = HTTP_POST, .handler = post_dcnet },
        { .uri = "/tag", .method = HTTP_GET, .handler = get_tag },
        { .uri = "/openmenu/poll", .method = HTTP_GET, .handler = get_poll },
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) httpd_register_uri_handler(h, &u[i]);
}
