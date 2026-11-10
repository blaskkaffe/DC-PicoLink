/* DC-PicoLink ESP32 side: shared declarations */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- link to the cable (link.c) ---- */
void link_start(void);
bool link_send(uint8_t ch, const void *p, size_t n);   /* false if the queue is full */
bool link_is_up(void);

typedef struct {
    uint32_t good, crc_bad, broken, lost;     /* from the cable */
    uint32_t sent, send_full;                 /* to the cable */
    uint32_t ser_in, ser_out, eth_in;         /* bytes / frames */
} link_stats_t;
void link_get_stats(link_stats_t *s);

void link_note_ser_out(size_t n);                     /* bytes sent to the DC (counter) */

/* What the cable said in its last answer to "get hello" / "get link.*" (text, one key per line) */
size_t link_cable_info(char *out, size_t cap);
/* Send a dc1 message to the cable; the answer appears in link_last_answer() */
bool link_dc1(const char *msg, size_t n);
size_t link_last_answer(char *out, size_t cap, uint32_t *seq);

/* ---- logs kept for the web page (main.c) ---- */
/* status lines from the cable and the ESP32's own notes; each has a running number */
void log_add(const char *src, const char *text, size_t n);
size_t log_since(uint32_t since, char *json_out, size_t cap, uint32_t *next);
/* serial bytes seen in both directions, for the monitor */
void ser_mon_add(bool from_dc, const uint8_t *p, size_t n);
size_t ser_mon_since(uint32_t since, char *json_out, size_t cap, uint32_t *next);

/* ---- Wi-Fi (wifi.c) ---- */
void wifi_start(void);
void wifi_set_sta(const char *ssid, const char *pass);
size_t wifi_info_json(char *out, size_t cap);

/* ---- web page (web.c) ---- */
void web_start(void);

/* JSON string escaping into out; returns bytes written (no terminator counted) */
size_t json_escape(char *out, size_t cap, const char *s, size_t n);
