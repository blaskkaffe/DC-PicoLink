/* Host test of wifiscan_model.h: one entry per name (strongest), sorted, hidden and non-UTF-8
   names left out, a full list keeps the strongest, JSON escaping. */
#include <stdio.h>
#include <string.h>
#include "wifiscan_model.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define ADD(w, s, r, o) wscan_add(&w, s, strlen(s), r, o)

int main(void) {
    wscan_t w = { .n = 0 };
    ADD(w, "Home", -70, false);
    ADD(w, "Cafe", -50, true);
    ADD(w, "Home", -60, false);              /* the same name, stronger: replaces */
    ADD(w, "Home", -80, false);              /* weaker: ignored */
    ADD(w, "", -30, false);                  /* hidden */
    wscan_add(&w, "x\0y", 3, -20, false);    /* a NUL inside the name: dropped */
    ADD(w, "Bad\xff" "name", -40, false);    /* not UTF-8 */
    ADD(w, "R\xc3\xa4v", -65, false);        /* ä is fine */
    CHECK(w.n == 3);
    CHECK(!strcmp(w.ap[0].ssid, "Cafe"));
    int home = -1, cafe = -1, rav = -1;
    for (int i = 0; i < w.n; i++) { if (!strcmp(w.ap[i].ssid, "Home")) home = i; if (!strcmp(w.ap[i].ssid, "Cafe")) cafe = i; if (!strcmp(w.ap[i].ssid, "R\xc3\xa4v")) rav = i; }
    CHECK(home >= 0 && w.ap[home].rssi == -60 && !w.ap[home].open);
    CHECK(cafe >= 0 && w.ap[cafe].open && cafe < home && home < rav);
    for (int i = 1; i < w.n; i++) CHECK(w.ap[i - 1].rssi >= w.ap[i].rssi);

    wscan_t f = { .n = 0 };
    char nm[16];
    for (int i = 0; i < 40; i++) { snprintf(nm, sizeof nm, "net%02d", i); wscan_add(&f, nm, strlen(nm), -90 + i, false); }
    CHECK(f.n == WSCAN_MAX);
    CHECK(f.ap[0].rssi == -51 && f.ap[WSCAN_MAX - 1].rssi == -74);   /* strongest 24 of -90..-51 */
    ADD(f, "weak", -100, false);
    CHECK(f.n == WSCAN_MAX && f.ap[WSCAN_MAX - 1].rssi == -74);
    ADD(f, "strong", -10, false);
    CHECK(f.n == WSCAN_MAX && !strcmp(f.ap[0].ssid, "strong") && f.ap[WSCAN_MAX - 1].rssi == -73);

    wscan_t q = { .n = 0 };
    ADD(q, "a\"b\\c", -50, true);
    ADD(q, "tab\there", -60, false);
    char j[256];
    size_t k = wscan_json(&q, j, sizeof j);
    CHECK(k == strlen(j));
    CHECK(!strcmp(j, "{\"ok\":true,\"aps\":[{\"s\":\"a\\\"b\\\\c\",\"r\":-50,\"o\":1},{\"s\":\"tab\\u0009here\",\"r\":-60,\"o\":0}]}"));
    wscan_t e = { .n = 0 };
    wscan_json(&e, j, sizeof j);
    CHECK(!strcmp(j, "{\"ok\":true,\"aps\":[]}"));
    char small[20];
    k = wscan_json(&q, small, sizeof small);
    CHECK(k == strlen(small) && k == sizeof small - 1);

    CHECK(!strcmp(wscan_reason_text(201), "network not found"));
    CHECK(!strcmp(wscan_reason_text(15), "wrong password?") && !strcmp(wscan_reason_text(202), "wrong password?") && !strcmp(wscan_reason_text(204), "wrong password?"));
    CHECK(!strcmp(wscan_reason_text(0), "") && !strcmp(wscan_reason_text(999), "could not connect"));

    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("wifiscan: all ok (strongest per name, sorted, hidden and non-UTF-8 dropped, full list, JSON)\n");
    return 0;
}
