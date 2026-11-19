/*
 * The SD card in the cable's slot as files for the web page: saves to and from a computer
 * over Wi-Fi. The cable lends the card block by block (sdlink_model.h, LINK_CH_SD); FatFs
 * runs here. While the page has the card, the Dreamcast sees none.
 *
 * The card goes back to the Dreamcast when the page says so, after 10 min without a file
 * operation, or (on the cable's side) 60 s after the page stopped asking.
 * Only the web server's task calls in here (one request at a time); answers from the cable
 * arrive in the link task (sdfs_from_cable).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "ff.h"
#include "diskio_impl.h"
#include "link_model.h"
#include "sdlink_model.h"
#include "app.h"

#define ANSWER_MS     1500                /* per block, or per state question */
#define IDLE_GIVE_US  (10LL * 60 * 1000000)

static sdlk_esp_t e;
static portMUX_TYPE mx = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t wake;
static uint16_t tag;
static uint8_t ask;
static BYTE pdrv = 0xFF;
static FATFS *fs;                         /* mounted while not NULL */
static char drv[4];                       /* "N:" */
static int64_t last_op_us;
static bool lost;                         /* the cable took the card back while mounted */

/* link task: one packet from the cable */
void sdfs_from_cable(const uint8_t *p, size_t n) {
    portENTER_CRITICAL(&mx);
    bool moved = sdlk_esp_rx(&e, p, n);
    if (p[0] == 'S' && n > 1 && !(p[1] & SDLK_TAKEN) && fs) lost = true;
    portEXIT_CRITICAL(&mx);
    if (moved && wake) xSemaphoreGive(wake);
}

/* wait until done() or ms without progress */
static bool wait_for(bool (*done)(void), int ms) {
    for (;;) {
        portENTER_CRITICAL(&mx); bool d = done(); portEXIT_CRITICAL(&mx);
        if (d) return true;
        if (xSemaphoreTake(wake, pdMS_TO_TICKS(ms) + 1) != pdTRUE) {
            portENTER_CRITICAL(&mx); d = done(); portEXIT_CRITICAL(&mx);
            return d;
        }
    }
}
static bool got_state(void) { return e.state_seq && e.ask == ask; }
static bool got_all(void) { return e.failed || e.got == e.n; }
static bool got_ack(void) { return e.failed || e.written; }

/* ask the cable for its state (also keeps the loan alive) */
static bool ask_state(uint8_t op, uint8_t on) {
    uint8_t m[3];
    size_t k;
    portENTER_CRITICAL(&mx); ask++; portEXIT_CRITICAL(&mx);
    if (op == 'T') { m[0] = 'T'; m[1] = on; m[2] = ask; k = 3; } else { m[0] = '?'; m[1] = ask; k = 2; }
    xSemaphoreTake(wake, 0);
    if (!link_send(LINK_CH_SD, m, k)) return false;
    return wait_for(got_state, ANSWER_MS);
}

/* ---- FatFs disk over the link ---- */
static DSTATUS d_status(BYTE pd) { (void)pd; return (e.state & SDLK_TAKEN) ? 0 : STA_NOINIT; }
static DSTATUS d_init(BYTE pd) { return d_status(pd); }
static DRESULT d_read(BYTE pd, BYTE *buf, DWORD lba, UINT count) {
    (void)pd;
    while (count) {
        UINT k = count > SDLK_MAX_RUN ? SDLK_MAX_RUN : count;
        uint8_t m[8] = { 'R', 0, 0, 0, 0, 0, 0, (uint8_t)k };
        portENTER_CRITICAL(&mx);
        e.tag = ++tag; e.lba0 = lba; e.n = (uint8_t)k; e.got = 0; e.dst = buf; e.failed = false;
        m[1] = (uint8_t)tag; m[2] = (uint8_t)(tag >> 8);
        portEXIT_CRITICAL(&mx);
        sdlk_put32(m + 3, lba);
        xSemaphoreTake(wake, 0);
        bool sent = false;
        for (int i = 0; i < 50 && !(sent = link_send(LINK_CH_SD, m, 8)); i++) link_wait_room(20);
        bool ok = sent && wait_for(got_all, ANSWER_MS);
        portENTER_CRITICAL(&mx);
        ok = ok && !e.failed && e.got == k;
        e.dst = NULL; e.tag = 0;                       /* late answers go nowhere */
        portEXIT_CRITICAL(&mx);
        if (!ok) return RES_ERROR;
        buf += 512u * k; lba += k; count -= k;
    }
    return RES_OK;
}
static DRESULT d_write(BYTE pd, const BYTE *buf, DWORD lba, UINT count) {
    (void)pd;
    static uint8_t m[7 + 512];
    for (UINT i = 0; i < count; i++) {
        m[0] = 'W';
        portENTER_CRITICAL(&mx);
        e.tag = ++tag; e.failed = false; e.written = false;
        m[1] = (uint8_t)tag; m[2] = (uint8_t)(tag >> 8);
        portEXIT_CRITICAL(&mx);
        sdlk_put32(m + 3, lba + i);
        memcpy(m + 7, buf + 512u * i, 512);
        xSemaphoreTake(wake, 0);
        bool sent = false;
        for (int k = 0; k < 50 && !(sent = link_send(LINK_CH_SD, m, sizeof m)); k++) link_wait_room(20);
        bool ok = sent && wait_for(got_ack, ANSWER_MS);
        portENTER_CRITICAL(&mx);
        ok = ok && e.written;
        e.tag = 0;
        portEXIT_CRITICAL(&mx);
        if (!ok) return RES_ERROR;
    }
    return RES_OK;
}
static DRESULT d_ioctl(BYTE pd, BYTE cmd, void *b) {
    (void)pd;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;                     /* every write is on the card when it returns */
    case GET_SECTOR_COUNT: *(LBA_t *)b = e.blocks; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)b = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)b = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
static const ff_diskio_impl_t disk = { .init = d_init, .status = d_status, .read = d_read, .write = d_write, .ioctl = d_ioctl };

static void unmount(void) {
    if (!fs) return;
    f_mount(NULL, drv, 0);
    free(fs); fs = NULL;
}

/* take the card (and mount it) or give it back; NULL = done, else why not */
static const char *take(bool on) {
    if (!on) {
        unmount();
        ask_state('T', 0);
        say_line("SD card: given back to the Dreamcast");
        return NULL;
    }
    if (fs && !lost) return NULL;
    unmount(); lost = false;
    if (!ask_state('T', 1)) return "the cable did not answer (is it up to date?)";
    if (!(e.state & SDLK_TAKEN)) return e.why[0] ? e.why : "the cable said no";
    if (pdrv == 0xFF) {
        if (ff_diskio_get_drive(&pdrv) != ESP_OK) return "no free FatFs drive";
        ff_diskio_register(pdrv, &disk);
        drv[0] = (char)('0' + pdrv % 10); drv[1] = ':'; drv[2] = 0;   /* FatFs drives 0..9 */
    }
    fs = calloc(1, sizeof *fs);
    if (!fs) { ask_state('T', 0); return "out of memory"; }
    FRESULT r = f_mount(fs, drv, 1);
    if (r != FR_OK) {
        free(fs); fs = NULL;
        ask_state('T', 0);
        return r == FR_NO_FILESYSTEM ? "no FAT32 file system on the card" : "the card could not be read";
    }
    say_line("SD card: the web page has the card (%lu MB); the Dreamcast sees none until it is given back", (unsigned long)(e.blocks / 2048u));
    return NULL;
}

/* ---- web API: /api/sd?op=... ---- */
static void url_dec(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && s[1] && s[2]) { char h[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(h, NULL, 16); s += 2; }
        else *o++ = *s == '+' ? ' ' : *s;
    }
    *o = 0;
}
static bool arg(httpd_req_t *r, const char *key, char *out, size_t cap) {
    char q[400];
    if (httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK || httpd_query_key_value(q, key, out, cap) != ESP_OK) { out[0] = 0; return false; }
    url_dec(out);
    return true;
}
/* "0:" + the page's path (always starting with /) */
static bool full_path(httpd_req_t *r, char *out, size_t cap) {
    char p[280];
    if (!arg(r, "p", p, sizeof p) || p[0] != '/') return false;
    size_t a = strlen(drv), b = strlen(p);
    if (a + b + 1 > cap) return false;
    memcpy(out, drv, a); memcpy(out + a, p, b + 1);
    return true;
}
static void json_str(httpd_req_t *r, const char *s) {
    char b[600]; size_t o = 0;
    b[o++] = '"';
    for (; *s && o < sizeof b - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { b[o++] = '\\'; b[o++] = (char)c; }
        else if (c < 32) o += (size_t)snprintf(b + o, sizeof b - o, "\\u%04x", c);
        else b[o++] = (char)c;
    }
    b[o++] = '"';
    httpd_resp_send_chunk(r, b, (ssize_t)o);
}
static esp_err_t reply(httpd_req_t *r, const char *err) {
    httpd_resp_set_type(r, "application/json");
    char b[160];
    if (err) { snprintf(b, sizeof b, "{\"ok\":false,\"err\":\"%s\"}", err); }
    else snprintf(b, sizeof b, "{\"ok\":true}");
    return httpd_resp_sendstr(r, b);
}
static const char *fr_text(FRESULT f) {
    switch (f) {
    case FR_NO_FILE: case FR_NO_PATH: return "not found";
    case FR_EXIST: return "already there";
    case FR_DENIED: return "not allowed (a folder that is not empty, or the card is full)";
    case FR_INVALID_NAME: return "a name FAT cannot keep";
    case FR_DISK_ERR: case FR_NOT_READY: return "the card did not answer";
    default: return "failed";
    }
}
/* the card is mounted and still ours; else why not */
static const char *ready(void) {
    if (lost) { unmount(); lost = false; return "the cable gave the card back to the Dreamcast (take it again)"; }
    return fs ? NULL : "take the card first";
}

esp_err_t sdfs_get(httpd_req_t *r) {
    char op[12];
    arg(r, "op", op, sizeof op);
    if (!wake) wake = xSemaphoreCreateBinary();
    if (!strcmp(op, "ls") || !strcmp(op, "get")) last_op_us = esp_timer_get_time();
    if (!strcmp(op, "state") || !op[0]) {
        bool ok = ask_state('?', 0);
        if (fs && !lost && esp_timer_get_time() - last_op_us > IDLE_GIVE_US) { take(false); say_line("SD card: 10 min without a file operation; back to the Dreamcast"); }
        if (lost) { unmount(); lost = false; }
        char b[240];
        snprintf(b, sizeof b, "{\"cable\":%s,\"card\":%s,\"taken\":%s,\"mounted\":%s,\"dc_busy\":%s,\"pi\":%s,\"mb\":%lu,\"why\":",
                 ok ? "true" : "false", (e.state & SDLK_CARD) ? "true" : "false", (e.state & SDLK_TAKEN) ? "true" : "false",
                 fs ? "true" : "false", (e.state & SDLK_DC_BUSY) ? "true" : "false", (e.state & SDLK_PI) ? "true" : "false",
                 (unsigned long)(e.blocks / 2048u));
        httpd_resp_set_type(r, "application/json");
        httpd_resp_send_chunk(r, b, HTTPD_RESP_USE_STRLEN);
        json_str(r, e.why);
        httpd_resp_send_chunk(r, "}", 1);
        return httpd_resp_send_chunk(r, NULL, 0);
    }
    const char *why = ready();
    char path[310];
    if (!why && !full_path(r, path, sizeof path)) why = "bad path";
    if (why) return reply(r, why);
    if (!strcmp(op, "ls")) {
        FF_DIR *d = malloc(sizeof *d);
        FILINFO *fi = malloc(sizeof *fi);
        FRESULT fr = d && fi ? f_opendir(d, path) : FR_NOT_ENOUGH_CORE;
        if (fr != FR_OK) { free(d); free(fi); return reply(r, fr_text(fr)); }
        httpd_resp_set_type(r, "application/json");
        httpd_resp_send_chunk(r, "{\"ok\":true,\"e\":[", HTTPD_RESP_USE_STRLEN);
        bool first = true;
        while (f_readdir(d, fi) == FR_OK && fi->fname[0]) {
            if (fi->fattrib & (AM_HID | AM_SYS)) continue;
            char b[64];
            httpd_resp_send_chunk(r, first ? "{\"n\":" : ",{\"n\":", HTTPD_RESP_USE_STRLEN);
            first = false;
            json_str(r, fi->fname);
            snprintf(b, sizeof b, ",\"d\":%d,\"s\":%lu}", (fi->fattrib & AM_DIR) ? 1 : 0, (unsigned long)fi->fsize);
            httpd_resp_send_chunk(r, b, HTTPD_RESP_USE_STRLEN);
        }
        f_closedir(d); free(d); free(fi);
        httpd_resp_send_chunk(r, "]}", 2);
        return httpd_resp_send_chunk(r, NULL, 0);
    }
    if (!strcmp(op, "get")) {
        FIL *f = malloc(sizeof *f);
        FRESULT fr = f ? f_open(f, path, FA_READ) : FR_NOT_ENOUGH_CORE;
        if (fr != FR_OK) { free(f); return reply(r, fr_text(fr)); }
        const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
        char cd[340];
        snprintf(cd, sizeof cd, "attachment; filename=\"%.300s\"", name);
        httpd_resp_set_type(r, "application/octet-stream");
        httpd_resp_set_hdr(r, "Content-Disposition", cd);
        static uint8_t b[4096];
        UINT n = 0;
        esp_err_t er = ESP_OK;
        while (er == ESP_OK && f_read(f, b, sizeof b, &n) == FR_OK && n) er = httpd_resp_send_chunk(r, (const char *)b, (ssize_t)n);
        f_close(f); free(f);
        return httpd_resp_send_chunk(r, NULL, 0);
    }
    return reply(r, "unknown op");
}

esp_err_t sdfs_post(httpd_req_t *r) {
    char op[12], v[4];
    arg(r, "op", op, sizeof op);
    if (!wake) wake = xSemaphoreCreateBinary();
    last_op_us = esp_timer_get_time();
    if (!strcmp(op, "take")) {
        arg(r, "on", v, sizeof v);
        return reply(r, take(v[0] == '1'));
    }
    const char *why = ready();
    char path[310];
    if (!why && !full_path(r, path, sizeof path)) why = "bad path";
    if (why) return reply(r, why);
    if (!strcmp(op, "rm")) { FRESULT fr = f_unlink(path); return reply(r, fr == FR_OK ? NULL : fr_text(fr)); }
    if (!strcmp(op, "mkdir")) { FRESULT fr = f_mkdir(path); return reply(r, fr == FR_OK ? NULL : fr_text(fr)); }
    if (!strcmp(op, "put")) {
        FIL *f = malloc(sizeof *f);
        FRESULT fr = f ? f_open(f, path, FA_WRITE | FA_CREATE_ALWAYS) : FR_NOT_ENOUGH_CORE;
        if (fr != FR_OK) { free(f); return reply(r, fr_text(fr)); }
        static uint8_t b[4096];
        size_t left = r->content_len;
        UINT w = 0;
        while (left && fr == FR_OK) {
            int k = httpd_req_recv(r, (char *)b, left < sizeof b ? left : sizeof b);
            if (k == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (k <= 0) { fr = FR_INT_ERR; break; }
            fr = f_write(f, b, (UINT)k, &w);
            if (fr == FR_OK && w != (UINT)k) fr = FR_DENIED;      /* full */
            left -= (size_t)k;
        }
        FRESULT fc = f_close(f); free(f);
        if (fr == FR_OK) fr = fc;
        if (fr != FR_OK) { f_unlink(path); return reply(r, fr == FR_INT_ERR ? "the upload broke off" : fr_text(fr)); }
        say_line("SD card: %s written from the web page (%u bytes)", path + strlen(drv), (unsigned)r->content_len);
        return reply(r, NULL);
    }
    return reply(r, "unknown op");
}
