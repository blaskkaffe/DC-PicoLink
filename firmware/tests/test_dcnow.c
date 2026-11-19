/* DCNow! settings-service answer (dcnow_parse_cfg): what "clone a DreamPi" reads. */
#include <stdio.h>
#include "dcnow_model.h"

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main(void) {
    char id[65];
    const char *h = "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef";
    char js[256];
    snprintf(js, sizeof js, "{\"mac_address\": \"%s\", \"is_enabled\": true}", h);
    CHECK(dcnow_parse_cfg(js, id));
    CHECK(!strcmp(id, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));   /* lower case */
    snprintf(js, sizeof js, "{\"is_enabled\":false,\"mac_address\":\"%s\"}", h);                /* DreamPi's own spacing */
    CHECK(dcnow_parse_cfg(js, id));
    CHECK(!dcnow_parse_cfg("{\"is_enabled\": true}", id));                                      /* some other web server */
    CHECK(!dcnow_parse_cfg("", id));
    CHECK(!dcnow_parse_cfg("{\"mac_address\": \"b8:27:eb:11:22:33\"}", id));                    /* a MAC, not an ID */
    snprintf(js, sizeof js, "{\"mac_address\": \"%.63s\"}", h);                                 /* 63 characters */
    CHECK(!dcnow_parse_cfg(js, id));
    snprintf(js, sizeof js, "{\"mac_address\": \"%sa\"}", h);                                   /* 65 characters */
    CHECK(!dcnow_parse_cfg(js, id));
    snprintf(js, sizeof js, "{\"mac_address\": \"%.63sg\"}", h);                                /* not hex */
    CHECK(!dcnow_parse_cfg(js, id));
    printf("%s: %d failures\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
