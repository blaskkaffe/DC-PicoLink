/* Hayes modem stub (hayes_model.h) */
#include <stdio.h>
#include <stdlib.h>
#include "../hayes_model.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

/* feed a string, collect everything written back; returns the last event that was not NONE */
static hayes_ev_t feed(hayes_t *h, const char *s, size_t n, char *all) {
    hayes_ev_t ev = HAYES_NONE;
    all[0] = 0;
    for (size_t i = 0; i < n; i++) {
        char o[128]; size_t ol;
        hayes_ev_t e = hayes_byte(h, (uint8_t)s[i], o, sizeof o, &ol);
        strcat(all, o);
        if (e != HAYES_NONE) ev = e;
    }
    return ev;
}
#define FEED(h, s, all) feed(h, s, strlen(s), all)

int main(void) {
    hayes_t h; char a[512];
    hayes_init(&h);

    CHECK(FEED(&h, "ATZ\r", a) == HAYES_NONE);                CHECK(!strcmp(a, "ATZ\r\r\nOK\r\n"));
    CHECK(FEED(&h, "AT\r", a) == HAYES_NONE);                 CHECK(!strcmp(a, "AT\r\r\nOK\r\n"));
    CHECK(FEED(&h, "atz0\r", a) == HAYES_NONE);               CHECK(!strcmp(a, "atz0\r\r\nOK\r\n"));
    CHECK(FEED(&h, "AT&F E1 S0=0 &C1\r", a) == HAYES_NONE);   CHECK(strstr(a, "\r\nOK\r\n"));
    CHECK(FEED(&h, "AT+MS=V34,0,,33600\r", a) == HAYES_NONE); CHECK(strstr(a, "\r\nOK\r\n"));

    /* dial: the number is ignored, CONNECT carries the speed */
    CHECK(FEED(&h, "ATDT#555\r", a) == HAYES_DIAL);           CHECK(strstr(a, "\r\nCONNECT 33600\r\n"));
    h.baud = 115200;
    CHECK(FEED(&h, "ATD1234\r", a) == HAYES_DIAL);            CHECK(strstr(a, "\r\nCONNECT 115200\r\n"));
    CHECK(FEED(&h, "ATA\r", a) == HAYES_DIAL);

    /* settings stick: echo off, numeric results, quiet */
    CHECK(FEED(&h, "ATE0\r", a) == HAYES_NONE);               CHECK(!strcmp(a, "ATE0\r\r\nOK\r\n"));   /* the echo of this line is still on the wire */
    CHECK(FEED(&h, "AT\r", a) == HAYES_NONE);                 CHECK(!strcmp(a, "\r\nOK\r\n"));              /* echo is off now */
    hayes_init(&h);
    CHECK(FEED(&h, "ATE0V0\r", a) == HAYES_NONE);
    CHECK(FEED(&h, "AT\r", a) == HAYES_NONE);                 CHECK(!strcmp(a, "0\r"));
    CHECK(FEED(&h, "ATD\r", a) == HAYES_DIAL);                CHECK(!strcmp(a, "1\r"));
    CHECK(FEED(&h, "ATQ1\r", a) == HAYES_NONE);
    CHECK(FEED(&h, "AT\r", a) == HAYES_NONE);                 CHECK(!strcmp(a, ""));
    CHECK(FEED(&h, "ATZ\r", a) == HAYES_NONE);                /* ATZ resets echo/verbose/quiet */
    CHECK(FEED(&h, "AT\r", a) == HAYES_NONE);                 CHECK(!strcmp(a, "AT\r\r\nOK\r\n"));

    /* bytes one by one, backspace, junk lines are ignored without an answer */
    hayes_init(&h);
    CHECK(FEED(&h, "ATX\x08Z\r", a) == HAYES_NONE);           CHECK(strstr(a, "\r\nOK\r\n"));
    CHECK(FEED(&h, "hello world\r", a) == HAYES_NONE);        CHECK(!strstr(a, "ERROR") && !strstr(a, "OK"));
    CHECK(FEED(&h, "\r", a) == HAYES_NONE);                   CHECK(!strstr(a, "OK"));              /* empty line */
    CHECK(FEED(&h, "AT\r\nAT\r\n", a) == HAYES_NONE);         CHECK(strstr(a, "OK"));              /* CR LF endings */

    /* an over-long line is dropped, the next one works */
    char big[300]; memset(big, 'A', sizeof big - 2); big[sizeof big - 2] = '\r'; big[sizeof big - 1] = 0;
    CHECK(FEED(&h, big, a) == HAYES_NONE);                    CHECK(!strstr(a, "OK"));
    CHECK(FEED(&h, "ATZ\r", a) == HAYES_NONE);                CHECK(strstr(a, "OK"));

    /* PPP flag with nothing before it: PPP starts, nothing is written */
    hayes_init(&h);
    CHECK(feed(&h, "\x7e", 1, a) == HAYES_PPP_FLAG);          CHECK(a[0] == 0);
    /* a flag in the middle of a line is not a start */
    CHECK(FEED(&h, "AT", a) == HAYES_NONE);
    CHECK(feed(&h, "\x7e", 1, a) == HAYES_NONE);

    /* NO CARRIER */
    hayes_init(&h);
    size_t k = hayes_no_carrier(&h, a, sizeof a);             CHECK(k && !strcmp(a, "\r\nNO CARRIER\r\n"));
    h.verbose = false;
    k = hayes_no_carrier(&h, a, sizeof a);                    CHECK(!strcmp(a, "3\r"));

    printf("hayes: ok\n");
    return 0;
}
