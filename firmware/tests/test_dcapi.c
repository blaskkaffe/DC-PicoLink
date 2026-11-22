/* Host test of dcapi_model.h: the answers match DC-DreamDash's api module (docs/dcapi.md) for
   hello and net.*, and the Wi-Fi keys (scan, join, password) behave as written in the header. */
#include <stdio.h>
#include <string.h>
#include "dcapi_model.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int selected_to = -1, can = 1, scan_result = 3, joined = 0, pass_calls = 0;
static char joined_name[40], kept_pass[70];
static struct { const char *s; int r, o; } found[] = { { "Home Net", -48, 0 }, { "Cafe", -61, 1 }, { "Line\nBreak", -70, 0 } };
static int n_found = 0;

static int op_select(void *c, int dcnet) { (void)c; if (!can) return 0; selected_to = dcnet; return 1; }
static int op_scan(void *c) { (void)c; if (scan_result >= 0) n_found = scan_result; return scan_result; }
static int op_get(void *c, int i, int *r, int *o, char *s, size_t cap) { (void)c; if (i < 0 || i >= n_found) return 0; *r = found[i].r; *o = found[i].o; snprintf(s, cap, "%s", found[i].s); return 1; }
static int op_pass(void *c, const char *p) { (void)c; snprintf(kept_pass, sizeof kept_pass, "%s", p); pass_calls++; return 1; }
static int op_join(void *c, const char *s) { (void)c; if (!can) return 0; joined++; snprintf(joined_name, sizeof joined_name, "%s", s); return 1; }
static const dcapi_ops_t ops = { op_select, op_scan, op_get, op_pass, op_join };

static dcapi_state_t st(void) {
    dcapi_state_t s = { .selected = 0, .call = -1, .dcnet_ok = 1, .version = "v1.2", .wifi_state = "connected",
                        .wifi_error = "", .wifi_ssid = "Home Net", .wifi_ip = "192.168.1.50", .scan_n = -1 };
    return s;
}

static const char *ask(const char *req, const dcapi_state_t *s) {
    static char out[2048];
    size_t n = dcapi_handle(req, strlen(req), s, &ops, NULL, out, sizeof out);
    CHECK(n == strlen(out));
    return out;
}

int main(void) {
    dcapi_state_t s = st();

    CHECK(!strcmp(ask("dc1\nid 7\nget hello\n", &s),
        "dc1\nid 7\nok hello.api 1\nok hello.caps api,net,wifi\nok hello.kind gateway\nok hello.name DC-PicoLink\nok hello.version v1.2\n"));
    CHECK(!strcmp(ask("dc1\nget net.*\n", &s), "dc1\nok net.dcnet ok\nok net.selected dcnow\n"));          /* no call: net.call left out */
    CHECK(!strcmp(ask("dc1\nget net.call\n", &s), "dc1\nerr net.call unavailable no call\n"));
    s.call = 1; s.selected = 1;
    CHECK(!strcmp(ask("dc1\nget net.*\n", &s), "dc1\nok net.call dcnet\nok net.dcnet ok\nok net.selected dcnet\n"));
    s.dcnet_ok = 0;
    CHECK(!strcmp(ask("dc1\nget net.dcnet\n", &s), "dc1\nok net.dcnet off:offline\n"));
    s = st();

    /* selecting */
    CHECK(!strcmp(ask("dc1\nid 9\ncall net.select DCNet\n", &s), "dc1\nid 9\nok net.select dcnet\n") && selected_to == 1);
    CHECK(!strcmp(ask("dc1\ncall net.select dcnow extra\n", &s), "dc1\nok net.select dcnow\n") && selected_to == 0);
    selected_to = -1;
    CHECK(!strcmp(ask("dc1\ncall net.select flycast\n", &s), "dc1\nerr net.select range dcnow or dcnet\n") && selected_to == -1);
    CHECK(!strcmp(ask("dc1\ncall net.select\n", &s), "dc1\nerr net.select range dcnow or dcnet\n"));
    can = 0;
    CHECK(!strcmp(ask("dc1\ncall net.select dcnet\n", &s), "dc1\nerr net.select unavailable\n"));
    can = 1;
    CHECK(!strcmp(ask("dc1\ncall net.reboot\n", &s), "dc1\nerr net.reboot unknown\n"));

    /* Wi-Fi: state, no scan yet */
    CHECK(!strcmp(ask("dc1\nget wifi.*\n", &s), "dc1\nok wifi.ip 192.168.1.50\nok wifi.ssid Home Net\nok wifi.state connected\nok wifi.up 1\n"));
    CHECK(!strcmp(ask("dc1\nget wifi.scan.n\n", &s), "dc1\nerr wifi.scan.n unknown\n"));
    s.wifi_state = "failed"; s.wifi_error = "wrong password?"; s.wifi_ip = "";
    CHECK(!strcmp(ask("dc1\nget wifi.*\n", &s), "dc1\nok wifi.error wrong password?\nok wifi.ssid Home Net\nok wifi.state failed\nok wifi.up 0\n"));
    CHECK(!strcmp(ask("dc1\nget wifi.ip\n", &s), "dc1\nerr wifi.ip unavailable not connected\n"));
    s.wifi_state = "none"; s.wifi_error = ""; s.wifi_ssid = "";
    CHECK(!strcmp(ask("dc1\nget wifi.ssid\nget wifi.error\nget wifi.state\n", &s),
                  "dc1\nerr wifi.ssid unavailable none saved\nerr wifi.error unavailable no error\nok wifi.state none\n"));

    /* scan, then read the results (the state of the request is refreshed by the scan) */
    CHECK(!strcmp(ask("dc1\nid 2\ncall wifi.scan\nget wifi.scan.*\n", &s),
        "dc1\nid 2\nok wifi.scan 3\nok wifi.scan.n 3\nok wifi.scan.0 -48 0 Home Net\nok wifi.scan.1 -61 1 Cafe\nok wifi.scan.2 -70 0 Line?Break\n"));
    s.scan_n = 3;                                                                                     /* a later request: the box reports 3 from its last scan */
    CHECK(!strcmp(ask("dc1\nget wifi.scan.1\n", &s), "dc1\nok wifi.scan.1 -61 1 Cafe\n"));
    CHECK(!strcmp(ask("dc1\nget wifi.scan.7\n", &s), "dc1\nerr wifi.scan.7 unknown\n"));
    scan_result = -1;
    CHECK(!strcmp(ask("dc1\ncall wifi.scan\n", &s), "dc1\nerr wifi.scan unavailable busy\n"));
    scan_result = -2;
    CHECK(!strcmp(ask("dc1\ncall wifi.scan\n", &s), "dc1\nerr wifi.scan unavailable the scan did not run\n"));
    scan_result = 3;

    /* password: write only, 8 to 63 characters or empty */
    CHECK(!strcmp(ask("dc1\nset wifi.pass secret123\n", &s), "dc1\nok wifi.pass set\n") && !strcmp(kept_pass, "secret123"));
    CHECK(!strcmp(ask("dc1\nset wifi.pass pass word with spaces\n", &s), "dc1\nok wifi.pass set\n") && !strcmp(kept_pass, "pass word with spaces"));
    pass_calls = 0;
    CHECK(!strcmp(ask("dc1\nset wifi.pass short\n", &s), "dc1\nerr wifi.pass range 8 to 63 characters (empty for an open network)\n") && !pass_calls);
    CHECK(!strcmp(ask("dc1\nset wifi.pass\n", &s), "dc1\nok wifi.pass set\n") && !kept_pass[0]);
    CHECK(!strcmp(ask("dc1\nget wifi.pass\n", &s), "dc1\nerr wifi.pass writeonly\n"));
    CHECK(!strcmp(ask("dc1\nget wifi.p*\n", &s), "dc1\nerr wifi.p* unknown\n"));                  /* a prefix read never lists the password */
    CHECK(strstr(ask("dc1\nget wifi.*\n", &s), "pass") == NULL);

    /* join: by name (spaces allowed) or by the number of a scan result */
    CHECK(!strcmp(ask("dc1\ncall wifi.join My Home Network\n", &s), "dc1\nok wifi.join My Home Network\n") && !strcmp(joined_name, "My Home Network"));
    CHECK(!strcmp(ask("dc1\ncall wifi.join #1\n", &s), "dc1\nok wifi.join Cafe\n") && !strcmp(joined_name, "Cafe"));
    joined = 0;
    CHECK(!strcmp(ask("dc1\ncall wifi.join #3\n", &s), "dc1\nerr wifi.join range no scan result 3\n") && !joined);
    CHECK(!strcmp(ask("dc1\ncall wifi.join #x\n", &s), "dc1\nerr wifi.join range no scan result x\n") && !joined);
    CHECK(!strcmp(ask("dc1\ncall wifi.join\n", &s), "dc1\nerr wifi.join range name 1 to 32 characters or #<number of a scan result>\n") && !joined);
    CHECK(!strcmp(ask("dc1\ncall wifi.join 123456789012345678901234567890123\n", &s), "dc1\nerr wifi.join range name 1 to 32 characters or #<number of a scan result>\n") && !joined);
    s.scan_n = -1;
    CHECK(!strcmp(ask("dc1\ncall wifi.join #0\n", &s), "dc1\nerr wifi.join range no scan result 0\n") && !joined);   /* no scan was made */
    CHECK(!strcmp(ask("dc1\ncall wifi.scan\ncall wifi.join #0\n", &s), "dc1\nok wifi.scan 3\nok wifi.join Home Net\n") && !strcmp(joined_name, "Home Net"));   /* scan and join in one request */
    can = 0;
    CHECK(!strcmp(ask("dc1\ncall wifi.join Cafe\n", &s), "dc1\nerr wifi.join unavailable\n"));
    can = 1;

    /* set, unknown keys and verbs, events */
    CHECK(!strcmp(ask("dc1\nset net.selected dcnet\nset hello.name x\nset net.call dcnow\nset wifi.ssid x\nset wifi.up 1\nset foo 1\n", &s),
                  "dc1\nerr net.selected readonly\nerr hello.name readonly\nerr net.call readonly\nerr wifi.ssid readonly\nerr wifi.up readonly\nerr foo unknown\n"));
    CHECK(!strcmp(ask("dc1\nget foo\nget foo.*\nget events since=3\nfrob x\n", &s),
                  "dc1\nerr foo unknown\nerr foo.* unknown\nerr events unknown\nerr frob unknown\n"));

    /* requests: first line, other targets, blank lines and CRLF, several lines in order */
    CHECK(!strcmp(ask("hello\nget hello\n", &s), "dc1\nerr request bad first line must be dc1\n"));
    CHECK(!strcmp(ask("", &s), "dc1\nerr request bad first line must be dc1\n"));
    CHECK(!strcmp(ask("dc1\nid 3\nto dreampi\nget net.selected\n", &s), "dc1\nid 3\nok net.selected dcnow\n"));
    CHECK(!strcmp(ask("dc1\nto Gateway\nget net.selected\n", &s), "dc1\nok net.selected dcnow\n"));
    CHECK(!strcmp(ask("dc1\nid 4\nto cable\nget net.selected\n", &s), "dc1\nid 4\nerr to unavailable no relay to other devices yet\n"));
    CHECK(!strcmp(ask("dc1\r\n\r\n  id 5  \r\n get net.selected \r\n", &s), "dc1\nid 5\nok net.selected dcnow\n"));
    CHECK(!strcmp(ask("dc1\nid 12345678901234\nget net.selected\n", &s), "dc1\nid 1234567890\nok net.selected dcnow\n"));   /* the id is cut at 10 */

    /* a small buffer never overruns and stays terminated */
    char tiny[24];
    size_t n = dcapi_handle("dc1\nget hello\n", 14, &s, &ops, NULL, tiny, sizeof tiny);
    CHECK(n == strlen(tiny) && n < sizeof tiny);

    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("dcapi: all ok (hello, net keys and select, wifi state, scan, password, join, set, unknown, targets, bad requests)\n");
    return 0;
}
