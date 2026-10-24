/* dctest: Dreamcast side of the serial / W5500 test tool. Host side: dctest.py.
   KOS build: make (needs KOS_BASE and a KOS environment). Host build for the selftest:
   gcc -DDCTEST_HOST dctest.c -o dctest-host (talks the serial protocol on stdin/stdout and
   serves UDP/TCP on localhost). Protocol: proto.h. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "proto.h"

#define DT_VERSION "1"
#ifndef DT_BUILD
#define DT_BUILD __DATE__ " " __TIME__
#endif
#define BAUD_START 115200
#define REVERT_MS 5000

/* ---------- hardware layer ---------- */
static void hal_log(const char *fmt, ...);
#ifdef DCTEST_HOST
#include <poll.h>
#include <time.h>
static uint64_t hal_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000ull + t.tv_nsec / 1000; }
static void hal_sleep_ms(int ms) { usleep(ms * 1000); }
static uint8_t in_buf[4096]; static int in_pos, in_len;
static int hal_getc(void) {
    if (in_pos == in_len) {
        struct pollfd p = {0, POLLIN, 0};
        if (poll(&p, 1, 2) <= 0) return -1;
        int n = read(0, in_buf, sizeof in_buf);
        if (n <= 0) exit(0);                       /* host closed the pipe */
        in_pos = 0; in_len = n;
    }
    return in_buf[in_pos++];
}
static void hal_write(const uint8_t *b, int n) { while (n > 0) { int k = write(1, b, n); if (k <= 0) exit(0); b += k; n -= k; } }
static void hal_flush(void) {}
static void hal_serial_init(int baud) { (void)baud; }
static void hal_serial_baud(int baud) { (void)baud; }
static void hal_serial_shutdown(void) {}
static int hal_net_up(void) { return 0; }
static void hal_net_down(void) {}
static int hal_start_pressed(void) { return 0; }
static void hal_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
/* Selftest: DCTEST_SIM_HW="nonet" (no W5500), "sd" (an SD card answers) */
static void hal_probe(uint8_t *w5, uint8_t *sd) {
    const char *e = getenv("DCTEST_SIM_HW");
    *w5 = (e && strstr(e, "nonet")) ? 0xFF : 0x04;
    *sd = (e && strstr(e, "sd")) ? 0x01 : 0xFF;
}
#else
#include <kos.h>
KOS_INIT_FLAGS(INIT_DEFAULT);                      /* no INIT_NET: the W5500 stays untouched until NETSTART */
static uint64_t hal_us(void) { return timer_us_gettime64(); }
static void hal_sleep_ms(int ms) { thd_sleep(ms); }
static int hal_getc(void) { return scif_read(); }
static void hal_write(const uint8_t *b, int n) { scif_write_buffer(b, n, 1); }
static void hal_flush(void) { scif_flush(); }
/* scif_set_parameters only stores the rate; scif_init applies it (and clears the IRQ enables). */
static void hal_serial_init(int baud) { scif_set_parameters(baud, 1); scif_init(); scif_set_irq_usage(1); }
static void hal_serial_baud(int baud) { hal_serial_init(baud); }
static void hal_serial_shutdown(void) { scif_shutdown(); }
static int hal_net_up(void) {
    net_init(0);                                    /* detects the W5500, then DHCP; failure leaves no address */
    if (!net_default_dev) return -1;
    for (int i = 0; i < 300 && !net_default_dev->ip_addr[0]; i++) thd_sleep(100);   /* DHCP, up to 30 s */
    if (!net_default_dev->ip_addr[0]) return -1;
    hal_log("net: %d.%d.%d.%d", net_default_dev->ip_addr[0], net_default_dev->ip_addr[1],
            net_default_dev->ip_addr[2], net_default_dev->ip_addr[3]);
    return 0;
}
static void hal_net_down(void) { net_shutdown(); }
static int hal_start_pressed(void) {
    maple_device_t *d = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *s = d ? (cont_state_t *)maple_dev_status(d) : NULL;
    return s && (s->buttons & CONT_START);
}
static void hal_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }

/* What answers on the serial port's SPI pins, the way the W5500 tester checks it. 0xFF = nothing.
   The cable (or adapter) sees this as a short SPI session and goes back to serial afterwards. */
static void hal_probe(uint8_t *w5, uint8_t *sd) {
    *w5 = *sd = 0xFF;
    scif_shutdown();
    if (scif_spi_init() != 0) return;
    static const uint8_t cmd0[6] = {0x40, 0, 0, 0, 0, 0x95};
    scif_spi_set_cs(1);
    for (int i = 0; i < 10; i++) scif_spi_rw_byte(0xFF);
    scif_spi_set_cs(0);                                      /* low = selected */
    scif_spi_rw_byte(0xFF); scif_spi_rw_byte(0xFF);
    for (int i = 0; i < 6; i++) scif_spi_rw_byte(cmd0[i]);
    for (int i = 0; i < 10; i++) { uint8_t r = scif_spi_rw_byte(0xFF); if (r != 0xFF) { *sd = r; break; } }
    scif_spi_set_cs(1);
    scif_spi_rw_byte(0xFF);
    scif_spi_set_cs(0);                                      /* W5500: VERSIONR, common block, read */
    scif_spi_rw_byte(0x00); scif_spi_rw_byte(0x39); scif_spi_rw_byte(0x00);
    *w5 = scif_spi_rw_byte(0x00);
    scif_spi_set_cs(1);
    scif_spi_shutdown();                                     /* back to serial registers */
}
#endif

static uint32_t hal_ms(void) { return (uint32_t)(hal_us() / 1000); }

/* ---------- state ---------- */
typedef struct {
    uint32_t frames_ok, crc_bad, resync, bytes_rx, bytes_tx, pat_bad, seq_gap, frames_tx,
             baud, uptime_ms, reverts, echo_frames, sink_frames;
} dt_stats;
static dt_stats st;
static uint32_t t_start, sink_seed, sink_next;
static int cur_baud = BAUD_START, prev_baud, revert_at;
static dt_parser ser_p, net_p, tcp_p;

enum { ACT_NONE, ACT_SETBAUD, ACT_NETSTART, ACT_NETSTOP, ACT_TCPSTART, ACT_PROBE };
static uint8_t probe_w5, probe_sd = 0xFF;
static int dcload_at_start;
static uint32_t act_seq;
static uint32_t act_arg;

typedef struct dt_io {
    void (*tx)(struct dt_io *, const uint8_t *, int);
    int fd;
    struct sockaddr_in peer;
} dt_io;

static void ser_tx(dt_io *io, const uint8_t *b, int n) { (void)io; hal_write(b, n); st.bytes_tx += n; }
static void udp_tx(dt_io *io, const uint8_t *b, int n) {
    sendto(io->fd, b, n, 0, (struct sockaddr *)&io->peer, sizeof io->peer); st.bytes_tx += n;
}
static void tcp_tx(dt_io *io, const uint8_t *b, int n) {
    int off = 0;
    while (off < n) { int k = send(io->fd, b + off, n - off, 0); if (k <= 0) return; off += k; }
    st.bytes_tx += n;
}

static void reply(dt_io *io, int type, int flags, uint32_t seq, const uint8_t *pay, int len) {
    static uint8_t out[DT_MAXFRAME];
    io->tx(io, out, dt_encode(out, type | DT_REPLY, flags, seq, pay, len));
    st.frames_tx++;
}

static void pack_stats(uint8_t *o) {
    st.crc_bad = ser_p.crc_bad + net_p.crc_bad + tcp_p.crc_bad;
    st.resync = ser_p.resync + net_p.resync + tcp_p.resync;
    st.baud = cur_baud; st.uptime_ms = hal_ms() - t_start;
    const uint32_t *s = (const uint32_t *)&st;
    for (int i = 0; i < 13; i++) dt_put32(o + 4 * i, s[i]);
}

static void probe_reply(dt_io *io, uint32_t seq) {
    uint8_t p[8] = {probe_w5, probe_sd, (uint8_t)dcload_at_start, 0};
    dt_put32(p + 4, cur_baud);
    reply(io, DT_PROBE, 0, seq, p, 8);
}

static void probe_show(void) {
    hal_log("hardware: W5500 %s | SD %s | serial: host linked",
            probe_w5 == 0x04 ? "found (version 4)" : "none", (probe_sd != 0xFF && !(probe_sd & 0x80)) ? "answers" : "none");
}

/* ---------- frame handler (serial, UDP and TCP share it) ---------- */
static int handle(dt_io *io, const dt_frame *f) {
    static uint8_t tmp[DT_MAXPAY];
    if (f->type & DT_REPLY) return ACT_NONE;       /* our own beacon coming back */
    st.frames_ok++;
    switch (f->type) {
    case DT_HELLO: {
        static const char v[] = "dctest " DT_VERSION " " DT_BUILD;
        reply(io, DT_HELLO, 0, f->seq, (const uint8_t *)v, sizeof v - 1);
        break;
    }
    case DT_ECHO:
        st.echo_frames++;
        reply(io, DT_ECHO, f->flags, f->seq, f->pay, f->len);
        break;
    case DT_SINK:
        st.sink_frames++;
        if (f->seq != sink_next) { if (f->seq > sink_next) st.seq_gap += f->seq - sink_next; }
        sink_next = f->seq + 1;
        if (!(f->flags & 0x80)) {
            dt_pattern(f->flags & 15, sink_seed, f->seq, f->len, tmp);
            if (memcmp(tmp, f->pay, f->len)) st.pat_bad++;
        }
        break;
    case DT_STATS:
        pack_stats(tmp);
        reply(io, DT_STATS, 0, f->seq, tmp, 52);
        break;
    case DT_CLEAR: {
        uint32_t b = cur_baud, up = st.uptime_ms, rv = st.reverts;
        memset(&st, 0, sizeof st);
        ser_p.crc_bad = ser_p.resync = net_p.crc_bad = net_p.resync = tcp_p.crc_bad = tcp_p.resync = 0;
        st.baud = b; st.uptime_ms = up; st.reverts = rv;
        sink_seed = f->len >= 4 ? dt_get32(f->pay) : 0;
        sink_next = 0;
        reply(io, DT_CLEAR, 0, f->seq, NULL, 0);
        break;
    }
    case DT_SRC: {
        if (f->len < 16) break;
        uint32_t count = dt_get32(f->pay), seed = dt_get32(f->pay + 8), gap = dt_get32(f->pay + 12);
        int len = f->pay[4] | f->pay[5] << 8, pat = f->pay[6];
        if (len > DT_MAXPAY) len = DT_MAXPAY;
        static uint8_t out[DT_MAXFRAME];
        uint64_t next = hal_us();
        for (uint32_t i = 0; i < count; i++) {
            dt_pattern(pat, seed, i, len, tmp);
            io->tx(io, out, dt_encode(out, DT_DATA, pat, i, tmp, len));
            st.frames_tx++;
            if (gap) { next += gap; while (hal_us() < next) ; }
        }
        uint8_t n[4]; dt_put32(n, count);
        reply(io, DT_SRC, 0, f->seq, n, 4);
        break;
    }
    case DT_SETBAUD:
        if (f->len < 4) break;
        act_arg = dt_get32(f->pay);
        reply(io, DT_SETBAUD, 0, f->seq, f->pay, 4);
        return ACT_SETBAUD;
    case DT_NETSTART:
        if (f->len < 2) break;
        act_arg = f->pay[0] | f->pay[1] << 8;
        reply(io, DT_NETSTART, 0, f->seq, f->pay, 2);
        return ACT_NETSTART;
    case DT_TCPSTART:
        if (f->len < 2) break;
        act_arg = f->pay[0] | f->pay[1] << 8;
        reply(io, DT_TCPSTART, 0, f->seq, f->pay, 2);
        return ACT_TCPSTART;
    case DT_PROBE: {
        if (f->len >= 1 && f->pay[0] && io->fd < 0) { act_seq = f->seq; return ACT_PROBE; }   /* rerun: serial only */
        probe_reply(io, f->seq);
        break;
    }
    case DT_NETSTOP:
        reply(io, DT_NETSTOP, 0, f->seq, NULL, 0);
        return ACT_NETSTOP;
    }
    return ACT_NONE;
}

/* ---------- network ---------- */
static void tcp_run(int port) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) { hal_log("tcp: bind/listen failed"); close(ls); return; }
    hal_log("tcp: waiting on %d", port);
    int c = accept(ls, NULL, NULL);
    close(ls);
    if (c < 0) return;
    dt_io io = {tcp_tx, c, {0}};
    uint8_t buf[1500];
    tcp_p.n = tcp_p.pending = 0;
    int stop = 0;
    while (!stop) {
        int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        st.bytes_rx += n;
        for (int i = 0; i < n && !stop; i++) {
            dt_frame f;
            int r = dt_feed(&tcp_p, buf[i], &f);
            while (r) { if (handle(&io, &f) == ACT_NETSTOP) stop = 1; r = dt_poll(&tcp_p, &f); }
        }
    }
    close(c);
    hal_log("tcp: closed");
}

static int net_run(int port) {
    hal_serial_shutdown();
    if (hal_net_up()) { hal_log("net: no link / no address"); hal_net_down(); hal_serial_init(cur_baud); return -1; }
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (u < 0 || bind(u, (struct sockaddr *)&a, sizeof a) < 0) { hal_log("net: bind failed"); hal_net_down(); hal_serial_init(cur_baud); return -1; }
    dt_io io = {udp_tx, u, {0}};
    io.peer.sin_family = AF_INET; io.peer.sin_port = htons(port + 1); io.peer.sin_addr.s_addr = htonl(INADDR_BROADCAST);   /* beacon port */
    int one = 1; setsockopt(u, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    for (int i = 0; i < 5; i++) { reply(&io, DT_HELLO, 0, 0, (const uint8_t *)"dctest", 6); hal_sleep_ms(300); }   /* beacon */
    hal_log("net: UDP %d ready", port);
    uint8_t buf[DT_MAXFRAME];
    for (int stop = 0; !stop;) {
        socklen_t sl = sizeof io.peer;
        int n = recvfrom(u, buf, sizeof buf, 0, (struct sockaddr *)&io.peer, &sl);
        if (n <= 0) continue;
        st.bytes_rx += n;
        net_p.n = net_p.pending = 0;
        for (int i = 0; i < n; i++) {
            dt_frame f;
            if (!dt_feed(&net_p, buf[i], &f)) continue;
            int act = handle(&io, &f);
            if (act == ACT_NETSTOP) stop = 1;
            else if (act == ACT_TCPSTART) tcp_run(act_arg);
        }
    }
    close(u);
    hal_net_down();
    hal_serial_init(cur_baud);                   /* the cable returns to serial once the DC releases the SPI pins */
    return 0;
}

/* ---------- serial loop ---------- */
static void check_revert(void) {
    if (revert_at && (int32_t)(hal_ms() - revert_at) >= 0) {
        cur_baud = prev_baud; hal_serial_baud(cur_baud); revert_at = 0; st.reverts++;
        hal_log("baud: no frame after the switch, back to %d", cur_baud);
    }
}

int main(void) {
#ifndef DCTEST_HOST
    /* Loaded by dc-tool-ser, KOS leaves the SCIF to dcload: scif_init does nothing and the SPI
       (W5500) driver refuses to start. We drive the port ourselves. */
    dcload_at_start = dcload_type;
    dcload_type = DCLOAD_TYPE_NONE;
    dbgio_dev_select("fb");
#endif
    t_start = hal_ms();
    hal_log("dctest " DT_VERSION " " DT_BUILD " - serial %d, Start quits", cur_baud);
    hal_serial_init(cur_baud);
    hal_probe(&probe_w5, &probe_sd);                 /* what is on the SPI pins, like the W5500 tester */
    hal_serial_init(cur_baud);
    hal_sleep_ms(500);
    probe_show();
    hal_log("probe: W5500 version register %02X, SD CMD0 answer %02X", probe_w5, probe_sd);
    uint32_t last_rx = hal_ms(), last_show = 0, seen = 0;
    int ticks = 0;
    for (;;) {
        int c = hal_getc();
        if (c < 0) {
            check_revert();
            uint32_t now = hal_ms();
            if (now - last_rx > 300 && now - last_show > 2000 && st.frames_ok != seen) {   /* idle: show progress */
                last_show = now; seen = st.frames_ok;
                hal_log("%u frames, %u crc bad, %u skipped, %u rx / %u tx bytes", st.frames_ok,
                        ser_p.crc_bad, ser_p.resync, st.bytes_rx, st.bytes_tx);
            }
            if (!(++ticks & 0xFFF) && hal_start_pressed()) break;
            continue;
        }
        last_rx = hal_ms();                       /* cheap enough: one timer read per byte */
        st.bytes_rx++;
        if (!(st.bytes_rx & 0xFF)) check_revert();
        dt_frame f;
        dt_io io = {ser_tx, -1, {0}};
        int r = dt_feed(&ser_p, c, &f);
        while (r) {
            revert_at = 0;                        /* a good frame at the new rate confirms it */
            switch (handle(&io, &f)) {
            case ACT_SETBAUD:
                hal_flush(); hal_sleep_ms(30);
                prev_baud = cur_baud; cur_baud = act_arg; hal_serial_baud(cur_baud);
                revert_at = hal_ms() + REVERT_MS;
                hal_log("baud: %d", cur_baud);
                break;
            case ACT_PROBE:
                hal_flush(); hal_sleep_ms(30);
                hal_probe(&probe_w5, &probe_sd);
                hal_serial_init(cur_baud);
                hal_sleep_ms(500);                  /* the cable needs ~200 ms of released lines to return to serial */
                probe_reply(&io, act_seq);
                probe_show();
                break;
            case ACT_NETSTART:
                hal_flush(); hal_sleep_ms(30);
                net_run(act_arg);
                ser_p.n = ser_p.pending = 0;
                break;
            }
            r = (ser_p.n || ser_p.pending) ? dt_poll(&ser_p, &f) : 0;
        }
    }
    hal_serial_shutdown();
    return 0;
}
