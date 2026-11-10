/* PC test for esp32/main/ovpn.c: connects to a real OpenVPN server, pings through the tunnel. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../../main/ovpn.h"

static int sock;
static struct sockaddr_in srv;
static int got_echo, echo_len;
static uint32_t t0;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static int loss_pct;
static int lossy(void) { return loss_pct && rand() % 100 < loss_pct; }
static int op_send(void *c, const uint8_t *d, size_t n)
{
    (void)c;
    if (lossy()) return (int)n;
    return (int)sendto(sock, d, n, 0, (void *)&srv, sizeof srv);
}
static void op_log(void *c, const char *m) { (void)c; printf("  [%5u ms] %s\n", now_ms() - t0, m); }
static int op_rand(void *c, uint8_t *b, size_t n)
{
    (void)c;
    int f = open("/dev/urandom", O_RDONLY);
    int r = read(f, b, n) == (ssize_t)n ? 0 : -1;
    close(f);
    return r;
}
static void op_ip(void *c, const uint8_t *ip, size_t n)
{
    (void)c;
    if (n >= 28 && ip[9] == 1 && ip[20] == 0) { got_echo++; echo_len = (int)n; }   /* ICMP echo reply */
}

static uint16_t csum(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += p[i] << 8 | p[i + 1];
    if (n & 1) s += p[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static size_t make_ping(uint8_t *b, uint32_t src, uint32_t dst, int payload, int seq)
{
    size_t n = 28 + (size_t)payload;
    memset(b, 0, n);
    b[0] = 0x45; b[2] = n >> 8; b[3] = n; b[8] = 64; b[9] = 1;
    for (int i = 0; i < 4; i++) { b[12 + i] = src >> (24 - 8 * i); b[16 + i] = dst >> (24 - 8 * i); }
    uint16_t c = csum(b, 20); b[10] = c >> 8; b[11] = c;
    b[20] = 8; b[24] = 0x12; b[25] = 0x34; b[26] = seq >> 8; b[27] = seq;
    for (int i = 0; i < payload; i++) b[28 + i] = (uint8_t)i;
    c = csum(b + 20, n - 20); b[22] = c >> 8; b[23] = c;
    return n;
}

static char *slurp(const char *f)
{
    FILE *fp = fopen(f, "rb");
    if (!fp) { perror(f); exit(2); }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); rewind(fp);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, fp) != (size_t)n) exit(2);
    b[n] = 0; fclose(fp);
    return b;
}

static void run(ovpn_t *o, int ms)
{
    uint32_t end = now_ms() + (uint32_t)ms;
    while ((int32_t)(end - now_ms()) > 0) {
        struct pollfd p = {sock, POLLIN, 0};
        if (poll(&p, 1, 20) > 0) {
            uint8_t b[2000];
            ssize_t n = recv(sock, b, sizeof b, 0);
            if (n > 0 && !lossy()) ovpn_input(o, b, (size_t)n, now_ms());
        }
        ovpn_tick(o, now_ms());
        if (ovpn_state(o) == OVPN_FAILED) return;
    }
}

#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: harness client.conf port [mode]\n"); return 2; }
    const char *mode = argc > 3 ? argv[3] : "full";
    if (getenv("LOSS")) { loss_pct = atoi(getenv("LOSS")); srand(7); }
    char *text = slurp(argv[1]);
    ovpn_conf_t cf;
    int r = ovpn_conf_parse(text, &cf);
    CHECK(r == 0, "config parses");
    printf("config: remote %s:%d key-direction %d cipher %s ping %d restart %d\n", cf.host, cf.port, cf.key_direction, cf.cipher, cf.ping_s, cf.restart_s);
    if (!strcmp(mode, "parse")) {            /* no server: does the profile load? */
        ovpn_ops_t po = {.random = op_rand};
        ovpn_t *p = ovpn_new(&cf, &po);
        CHECK(p && ovpn_state(p) != OVPN_FAILED, p ? ovpn_error(p) : "ovpn_new");
        printf("profile ok: %s:%d\n", cf.host, cf.port);
        ovpn_free(p);
        ovpn_conf_free(&cf);
        free(text);
        return 0;
    }
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    srv.sin_family = AF_INET; srv.sin_port = htons((uint16_t)atoi(argv[2])); srv.sin_addr.s_addr = htonl(0x7f000001);
    connect(sock, (void *)&srv, sizeof srv);
    ovpn_ops_t ops = {.send = op_send, .ip_in = op_ip, .log = op_log, .random = op_rand};
    ovpn_t *o = ovpn_new(&cf, &ops);
    CHECK(o, "ovpn_new");
    t0 = now_ms();
    ovpn_start(o, now_ms());
    for (int i = 0; i < 400 && ovpn_state(o) != OVPN_UP && ovpn_state(o) != OVPN_FAILED; i++) run(o, 50);
    printf("state: %s %s\n", ovpn_state_name(ovpn_state(o)), ovpn_error(o));
    CHECK(ovpn_state(o) == OVPN_UP, "tunnel comes up");
    const ovpn_info_t *in = ovpn_info(o);
    printf("ifconfig %u.%u.%u.%u mask %08x gw %08x peer %08x routes %d cipher %s\n", in->ip >> 24, (in->ip >> 16) & 255,
           (in->ip >> 8) & 255, in->ip & 255, in->mask, in->gateway, in->peer, in->nroute, in->cipher);
    uint8_t pk[1600];
    if (!strcmp(mode, "reneg")) {
        /* the server renegotiates every few seconds: pings must keep being answered */
        int sent = 0, ok = 0, bad_run = 0;
        for (int i = 0; i < 70; i++) {
            got_echo = 0;
            size_t n = make_ping(pk, in->ip, in->gateway, 64, i);
            ovpn_send_ip(o, pk, n, now_ms()); sent++;
            for (int w = 0; w < 25 && !got_echo; w++) run(o, 20);
            if (got_echo) { ok++; bad_run = 0; } else if (++bad_run > 2) break;
            if (ovpn_state(o) != OVPN_UP) break;
            run(o, 400);
        }
        printf("  pings answered %d of %d, state %s\n", ok, sent, ovpn_state_name(ovpn_state(o)));
        CHECK(ovpn_state(o) == OVPN_UP && ok >= sent - 1, "tunnel survives renegotiation");
        printf("vpn: ok (renegotiation)\n");
        return 0;
    }
    if (!strcmp(mode, "up")) { ovpn_stop(o, now_ms()); printf("vpn: ok (up only)\n"); return 0; }

    uint32_t dst = in->gateway ? in->gateway : in->peer;
    int sizes[] = {0, 56, 400, 1200};
    for (int k = 0; k < 4; k++) {
        got_echo = 0;
        size_t n = make_ping(pk, in->ip, dst, sizes[k], k + 1);
        CHECK(ovpn_send_ip(o, pk, n, now_ms()) == 0, "send");
        for (int i = 0; i < 100 && !got_echo; i++) run(o, 20);
        CHECK(got_echo, "echo reply arrives");
        CHECK(echo_len == (int)n, "reply has the same size");
        printf("  ping %4d bytes: reply\n", sizes[k]);
    }
    /* keepalive: stay quiet for 12 s with ping 5 -> tunnel must stay up in both directions */
    if (!strcmp(mode, "full")) {
        run(o, 12000);
        CHECK(ovpn_state(o) == OVPN_UP, "stays up while idle");
        got_echo = 0;
        size_t n = make_ping(pk, in->ip, dst, 32, 9);
        ovpn_send_ip(o, pk, n, now_ms());
        for (int i = 0; i < 100 && !got_echo; i++) run(o, 20);
        CHECK(got_echo, "ping after idle");
        printf("  keepalive: ok\n");
    }
    const ovpn_stats_t *s = ovpn_stats(o);
    printf("tx %u rx %u bad_hmac %u bad_data %u resend %u\n", s->tx_pkts, s->rx_pkts, s->bad_hmac, s->bad_data, s->ctl_resend);
    ovpn_stop(o, now_ms());
    ovpn_free(o);
    printf("vpn: ok\n");
    return 0;
}
