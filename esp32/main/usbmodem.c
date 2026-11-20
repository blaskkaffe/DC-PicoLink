/*
 * USB modem on the ESP32's own USB port (the board is the USB host).
 *
 * The Dreamcast's built-in modem is joined to a USB voice modem by a phone cord (with a line
 * voltage inducer if the Dreamcast needs one), exactly as with a DreamPi. What dreampi.py does:
 *   1. ATZ0, ATE0; voice mode (AT+FCLASS=8, AT+VLS=1, AT+VSM=1,8000, AT+VTX) and a dial tone
 *   2. the modem reports each key the Dreamcast dials (DLE + digit); 2 s without a key ends the
 *      number: stop the tone, hang up, reset (ATZ0, AT&F0, ATE0W2)
 *   3. 8 seconds after the number: ATA, wait for CONNECT
 *   4. the bytes are PPP (ppp_srv.c, the same server as for the serial cable) until NO CARRIER,
 *      or they go to DCNet (dcnet.c) when the number started with *69 or "*69" came just before
 *      it (DreamPi's netlink.py), or the page says every call goes to DCNet
 * The parsing is in ../../firmware/usbmodem_model.h (host-tested).
 *
 * The USB port then no longer shows the ESP32's console: build with sdkconfig.modem, which
 * moves the console to UART0 (GPIO43/44). The log on the web page is not affected.
 * To flash again: hold BOOT while plugging the board into the computer.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "sdkconfig.h"
#include "app.h"

#if CONFIG_DCL_USB_MODEM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usbmodem_model.h"
#include "dcnet_model.h"
#include "netlink_model.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "usbmodem";

enum { PH_CMD, PH_VOICE, PH_DATA };
enum { EV_RESP, EV_DIGIT, EV_GONE, EV_CALL_OVER };
typedef struct { uint8_t what; uint8_t val; } ev_t;

static QueueHandle_t evq;
static SemaphoreHandle_t devlk;              /* guards dev against a write during close */
static cdc_acm_dev_hdl_t dev;
static usbm_rx_t rx;
static volatile int phase = PH_CMD;
static volatile bool to_data;                /* the next CONNECT starts the data phase */
static char connect_line[64];
static uint8_t tone[USBM_TONE_N];

static const char *state = "no modem plugged in";
static volatile bool present;
static volatile bool yielding;               /* closed so the cable can have the USB channels (usbport.c) */
static volatile bool opening;                /* a modem is being opened (not present yet) */
static char usb_id[12] = "";
static struct { uint32_t calls, answered, failed, bytes_in, bytes_out; char dial[24]; } cnt;
static volatile bool via_dcnet;              /* this call goes to DCNet, not our PPP server */
static volatile bool to_stream;              /* the data goes to netlink.c (um_read) */
static StreamBufferHandle_t nlq;             /* modem -> netlink.c */
static volatile bool carrier;                /* the two modems are connected */
static volatile bool dcd_seen, dcd;          /* the modem reports DCD (serial state) */
static uint32_t stream_drop;
static int64_t prefix_ms = -100000;          /* when "*69" was last dialled on its own */

#define say say_line

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void push(uint8_t what, uint8_t val) { ev_t e = { what, val }; xQueueSend(evq, &e, 0); }

/* ---- from the modem (the CDC driver's task) ---- */
static bool data_cb(const uint8_t *d, size_t n, void *arg) {
    (void)arg;
    cnt.bytes_in += (uint32_t)n;
    size_t i = 0;
    if (phase == PH_CMD) {
        for (; i < n && phase == PH_CMD; i++) {
            int e = usbm_line_byte(&rx, d[i]);
            if (!e) continue;
            if (e == USBM_CONNECT) {
                memcpy(connect_line, rx.line, sizeof connect_line);
                if (to_data) { to_data = false; phase = PH_DATA; carrier = true; }
            }
            push(EV_RESP, (uint8_t)e);
        }
        ser_mon_add(true, d, i);                 /* the modem's answers show in the page's serial monitor */
    }
    if (phase == PH_VOICE) {
        for (; i < n; i++) { char c = usbm_voice_byte(&rx, d[i]); if (c) push(EV_DIGIT, (uint8_t)c); }
    } else if (phase == PH_DATA && i < n) {
        bool over = false;
        for (size_t k = i; k < n; k++) over |= usbm_data_byte(&rx, d[k]);
        if (over) carrier = false;
        if (to_stream) { size_t w = xStreamBufferSend(nlq, d + i, n - i, 0); stream_drop += (uint32_t)(n - i - w); }
        else if (via_dcnet) dcnet_from_dc(d + i, n - i);
        else ppp_modem_bytes(d + i, n - i);
        if (over) push(EV_RESP, USBM_NO_CARRIER);
    }
    return true;
}

static void event_cb(const cdc_acm_host_dev_event_data_t *ev, void *arg) {
    (void)arg;
    if (ev->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) push(EV_GONE, 0);
    else if (ev->type == CDC_ACM_HOST_SERIAL_STATE) {
        bool c = ev->data.serial_state.bRxCarrier;
        if (c) dcd_seen = true;
        if (dcd_seen) { if (dcd && !c && phase == PH_DATA) carrier = false; dcd = c; }
    }
}

/* ---- to the modem ---- */
size_t usbmodem_write(const void *data, size_t len) {
    const uint8_t *p = data; size_t done = 0;
    xSemaphoreTake(devlk, portMAX_DELAY);
    while (dev && done < len) {
        size_t c = len - done; if (c > 512) c = 512;
        if (cdc_acm_host_data_tx_blocking(dev, p + done, c, 500) != ESP_OK) break;
        done += c;
    }
    xSemaphoreGive(devlk);
    cnt.bytes_out += (uint32_t)done;
    return done;
}

void usbmodem_call_over(void) { push(EV_CALL_OVER, 0); }
/* usbport.c: the cable needs the modem's USB host channels (8 in all; hub + modem + cable need 9) */
bool usbmodem_yield(void) {
    if (!present && !opening) return false;
    if (!yielding) { yielding = true; if (present) push(EV_GONE, 0); }   /* an open in progress closes right after */
    return true;
}

/* wait for an event; false = time is up */
static bool wait_ev(ev_t *e, int ms) { return xQueueReceive(evq, e, pdMS_TO_TICKS(ms) + 1) == pdTRUE; }

static bool gone;                            /* the modem was unplugged (seen by the task) */

/* send a command, wait for `want` (USBM_OK or USBM_CONNECT). Other answers than ERROR/NO CARRIER/BUSY are skipped. */
static bool cmd(const char *c, int want, int ms) {
    ev_t e;
    while (xQueueReceive(evq, &e, 0) == pdTRUE)             /* old answers */
        if (e.what == EV_GONE) { gone = true; return false; }
    char t[48];
    int k = snprintf(t, sizeof t, "%s\r", c);
    ser_mon_add(false, (const uint8_t *)t, (size_t)k);
    if (usbmodem_write(t, (size_t)k) != (size_t)k) return false;
    int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        if (!wait_ev(&e, 50)) continue;
        if (e.what == EV_GONE) { gone = true; return false; }
        if (e.what != EV_RESP) continue;
        if (e.val == want) return true;
        if (e.val == USBM_ERROR || e.val == USBM_NO_CARRIER || e.val == USBM_BUSY) return false;
    }
    return false;
}

static void pause_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/* back to command mode and on-hook, whatever the modem was doing */
static void hang_up(void) {
    phase = PH_CMD; to_data = false; to_stream = false; carrier = false;
    usbm_init(&rx);
    pause_ms(1100);
    usbmodem_write("+++", 3);
    pause_ms(1100);
    cmd("ATH0", USBM_OK, 3000);
}

static bool modem_reset(void);
/* ---- for netlink.c (called from the modem task only) ---- */
bool um_cmd(const char *c, int ms) { return cmd(c, USBM_OK, ms); }
void um_hangup(void) { hang_up(); }
/* DreamPi's stop_dial_tone(): end of the tone, escape, on-hook, reset */
void um_stop_tone(void) {
    phase = PH_CMD; usbm_init(&rx);
    usbmodem_write("\0\x10\x03\r\n", 5);
    hang_up();
    if (!gone) modem_reset();
}
bool um_gone(void) { return gone; }
bool um_carrier(void) { return carrier && !gone; }
static void stream_reset(void) { xStreamBufferReset(nlq); }
/* ATA (or ATX1D for XBAND, which calls out): wait for CONNECT, then the data goes to um_read */
bool um_connect(bool originate, int ms) {
    stream_reset();
    to_stream = true; to_data = true; carrier = false;
    usbm_init(&rx);
    bool ok = cmd(originate ? "ATX1D" : "ATA", USBM_CONNECT, ms);
    if (!ok) { to_data = false; to_stream = false; phase = PH_CMD; }
    else say("USB modem: %s", connect_line);
    return ok;
}
/* bytes from the other modem; 0 = nothing within ms, -1 = the call is over */
int um_read(uint8_t *b, size_t cap, int ms) {
    size_t n = xStreamBufferReceive(nlq, b, cap, pdMS_TO_TICKS(ms) + (ms ? 1 : 0));
    if (n) return (int)n;
    ev_t e;
    while (xQueueReceive(evq, &e, 0) == pdTRUE) if (e.what == EV_GONE) gone = true;
    return (gone || !carrier) ? -1 : 0;
}

/* DreamPi's Modem.reset(): ATZ0, AT&F0, ATE0W2 (W2 = CONNECT shows the line speed) */
static bool modem_reset(void) {
    if (!cmd("ATZ0", USBM_OK, 3000)) return false;
    pause_ms(1000);
    cmd("AT&F0", USBM_OK, 2000);
    if (!cmd("ATE0W2", USBM_OK, 2000)) cmd("ATE0", USBM_OK, 2000);
    return true;
}

static void dcnet_done(void) { push(EV_CALL_OVER, 0); }

/* one round: dial tone, hear the Dreamcast dial, answer, PPP until the call ends */
static void one_call(void) {
    phase = PH_CMD; to_data = false;
    usbm_init(&rx);
    state = "setting up the modem";
    if (!modem_reset() && !gone) {                          /* maybe still in a call or in voice mode */
        usbmodem_write("\0\x10\x03\r\n", 5);
        hang_up();
        if (!modem_reset()) { if (!gone) { state = "the modem does not answer AT commands"; pause_ms(5000); } return; }
    }
    if (gone) return;
    static bool said_voice;
    if (!(cmd("AT+FCLASS=8", USBM_OK, 2000) && cmd("AT+VLS=1", USBM_OK, 2000) &&
          cmd("AT+VSM=1,8000", USBM_OK, 2000) && cmd("AT+VTX", USBM_CONNECT, 3000))) {
        if (gone) return;
        state = "this modem has no voice mode (needed to hear the Dreamcast dial)";
        if (!said_voice) say("USB modem: %s; a Conexant-based modem as for DreamPi works", state);
        said_voice = true;
        cmd("ATH0", USBM_OK, 2000);
        pause_ms(10000);
        return;
    }
    said_voice = false;
    phase = PH_VOICE;
    state = "dial tone on, waiting for the Dreamcast to dial";
    static bool said_wait;
    if (!said_wait) say("USB modem: %s", state);
    said_wait = true;

    /* the tone, until the Dreamcast has dialled: like DreamPi, the number is over when no key
       came for 2 s. "*69" on its own (DCNet prefix) keeps the tone on for the number after it. */
    char dial[24]; size_t dn = 0;
    nl_dial_t nd;
    int64_t due = now_ms(), last_key = 0;
    for (;;) {
        if (now_ms() >= due) {
            if (usbmodem_write(tone, sizeof tone) != sizeof tone) pause_ms(100);
            due += 100;
            if (now_ms() - due > 500) due = now_ms();
        }
        if (dn && now_ms() - last_key > 2000) {
            dial[dn] = 0;
            nd = nl_classify(dial, netlink_servers());
            if (nd.mode == NL_DCNET_PREFIX) { prefix_ms = now_ms(); say("USB modem: *69 heard (DCNet prefix)"); dn = 0; continue; }
            if (nd.mode == NL_IDLE) { dn = 0; continue; }                   /* *70 (call waiting off) and the like */
            break;
        }
        if (!dn && netlink_idle_poll()) return;          /* an XBAND call came in over the network */
        ev_t e;
        if (!wait_ev(&e, 10)) continue;
        if (e.what == EV_GONE) { gone = true; return; }
        if (e.what == EV_DIGIT && dn < sizeof dial - 1) { dial[dn++] = (char)e.val; last_key = now_ms(); }
    }
    int64_t heard = now_ms();
    cnt.calls++; strlcpy(cnt.dial, dial, sizeof cnt.dial); said_wait = false;
    if (nd.mode != NL_PPP && nd.mode != NL_CAPCOM) {             /* Netlink, game server, XBAND */
        prefix_ms = -100000;
        say("USB modem: %s dialled: %s", dial, netlink_mode_name(nd.mode));
        state = "in a Netlink call";
        netlink_run(&nd);
        to_stream = false;
        state = "hanging up";
        if (!gone) hang_up();
        return;
    }
    bool capcom = nd.mode == NL_CAPCOM;
    char number[24];
    via_dcnet = !capcom && dcnet_wanted(dcnet_mode(), dial, now_ms() - prefix_ms < 10000, number, sizeof number);
    prefix_ms = -100000;
    say("USB modem: the Dreamcast dialled %s; answering%s", dial, via_dcnet ? " (DCNet)" : capcom ? " (Capcom matching: PPP)" : " in 8 s");
    state = "the Dreamcast is dialling";
    phase = PH_CMD;
    usbm_init(&rx);
    usbmodem_write("\0\x10\x03\r\n", 5);                    /* end of the tone */
    hang_up();
    if (gone) return;
    modem_reset();
    while (!via_dcnet && !capcom && now_ms() - heard < 8000) pause_ms(50);   /* DreamPi waits 8 s for PPP only */

    state = "answering";
    to_data = true;
    if (!cmd("ATA", USBM_CONNECT, 60000)) {
        to_data = false;
        if (gone) return;
        cnt.failed++;
        say("USB modem: the two modems did not connect (no CONNECT after ATA)");
        hang_up();
        return;
    }
    cnt.answered++;
    if (via_dcnet) {
        say("USB modem: answered, %s; passing the call to DCNet", connect_line);
        if (!dcnet_call(number, usbmodem_write, dcnet_done)) { via_dcnet = false; hang_up(); return; }
    } else {
        say("USB modem: answered, %s; starting PPP", connect_line);
        if (!ppp_modem_call_start(capcom)) {
            say("USB modem: PPP could not start (is Internet over serial switched off on the page?)");
            hang_up();
            return;
        }
    }
    state = "in a call";
    int64_t t0 = now_ms();
    for (;;) {
        ev_t e;
        if (!wait_ev(&e, 500)) {
            if (!via_dcnet && !ppp_srv_up() && now_ms() - t0 > 90000) { say("USB modem: connected but no PPP from the Dreamcast in 90 s; hanging up"); break; }
            continue;
        }
        if (e.what == EV_GONE) { gone = true; say("USB modem: unplugged during the call"); break; }
        if (e.what == EV_CALL_OVER) { if (!via_dcnet) say("USB modem: the Dreamcast ended the call"); break; }
        if (e.what == EV_RESP && e.val == USBM_NO_CARRIER) { say("USB modem: the line dropped (NO CARRIER)"); break; }
    }
    if (via_dcnet) { dcnet_hangup(); while (dcnet_active()) pause_ms(50); via_dcnet = false; }
    else ppp_modem_call_end();
    state = "hanging up";
    if (!gone) hang_up();
}

static void modem_task(void *arg) {
    (void)arg;
    cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = 3000, .out_buffer_size = 1024, .in_buffer_size = 1024,
        .event_cb = event_cb, .data_cb = data_cb,
    };
    for (;;) {
        cdc_acm_dev_hdl_t h = NULL;
        uint16_t vid, pid;
        /* only devices that are not a DC-PicoLink cable (usbport.c takes those): "any" would
           open the cable's own serial interface as a modem */
        if (!usbport_next_other(&vid, &pid, UINT32_MAX)) continue;
        snprintf(usb_id, sizeof usb_id, "%04x:%04x", vid, pid);
        /* with a cable on the port too (through a hub) the modem's status endpoint stays closed: the
           ESP32's USB host has 8 channels, hub 2 + cable 3 + modem 3 without it. The end of a call
           is then seen from NO CARRIER and PPP, not the carrier line. */
        /* The cable can turn up while this runs (both plugged in at start): the cable flag is read
           again for every try, and a modem opened with its status endpoint just before the cable
           took its channels is closed and opened again without it. */
        esp_err_t oe = ESP_FAIL;
        opening = true;
        for (int t = 0; t < 4; t++) {
            if (t) pause_ms(700);
            cfg.no_notification = usbport_cable();
            oe = cdc_acm_host_open(vid, pid, 0, &cfg, &h);
            if (oe != ESP_OK) continue;
            if (yielding) { cdc_acm_host_close(h); h = NULL; break; }        /* the cable needs the channels: offered again after it */
            if (!cfg.no_notification && usbport_cable()) { cdc_acm_host_close(h); h = NULL; oe = ESP_FAIL; continue; }
            break;
        }
        opening = false;
        if (yielding && !h) {
            yielding = false;
            say("USB modem %s: waits until the DC-PicoLink cable is open, then opens next to it", usb_id);
            state = "reopening next to the cable";
            continue;
        }
        if (oe != ESP_OK || !h) {
            if (usbport_cable())
                say("USB: modem %s could not be opened next to the DC-PicoLink cable (%s): the ESP32's USB host has 8 channels; hub 2 + cable 3 + modem 3 fill them, so nothing else may be on the hub", usb_id, esp_err_to_name(oe));
            else
                say("USB: device %s is plugged in but is not a modem this firmware can use (%s; it needs a USB CDC-ACM modem)", usb_id, esp_err_to_name(oe));
            state = usbport_cable() ? "plugged in, but no USB channels left next to the cable" : "a USB device is plugged in, but not a usable modem";
            continue;
        }
        xSemaphoreTake(devlk, portMAX_DELAY); dev = h; xSemaphoreGive(devlk);
        cdc_acm_host_set_control_line_state(h, true, true);
        present = true; gone = false;
        xQueueReset(evq);
        say("USB modem %s plugged in%s", usb_id, cfg.no_notification ? " (next to the DC-PicoLink cable: without its status endpoint, to fit the USB host's 8 channels)" : "");
        while (!gone) one_call();
        present = false;
        ppp_modem_call_end();
        xSemaphoreTake(devlk, portMAX_DELAY); dev = NULL; xSemaphoreGive(devlk);
        cdc_acm_host_close(h);
        if (yielding) {
            say("USB modem: closed for a moment so the DC-PicoLink cable fits next to it (8 USB host channels); it opens again without its status endpoint");
            state = "reopening next to the cable";
            yielding = false;
        } else {
            say("USB modem unplugged");
            state = "no modem plugged in";
        }
    }
}


size_t usbmodem_info_json(char *out, size_t cap) {
    return (size_t)snprintf(out, cap,
        "{\"built\":true,\"present\":%s,\"id\":\"%s\",\"state\":\"%s\",\"calls\":%lu,\"answered\":%lu,\"failed\":%lu,"
        "\"bytes_in\":%lu,\"bytes_out\":%lu,\"dialled\":\"%s\",\"dcnet\":%s}",
        present ? "true" : "false", present ? usb_id : "", state, (unsigned long)cnt.calls, (unsigned long)cnt.answered,
        (unsigned long)cnt.failed, (unsigned long)cnt.bytes_in, (unsigned long)cnt.bytes_out, cnt.dial, via_dcnet ? "true" : "false");
}

void usbmodem_start(void) {
    evq = xQueueCreate(16, sizeof(ev_t));
    nlq = xStreamBufferCreate(4096, 1);
    devlk = xSemaphoreCreateMutex();
    for (int i = 0; i < USBM_TONE_N; i++) {                  /* sin(2 pi k/800) for the two tones of sample i */
        int16_t a = (int16_t)lrintf(sinf(2.0f * (float)M_PI * (float)((i * 35) % USBM_TONE_N) / USBM_TONE_N) * 32767.0f);
        int16_t b = (int16_t)lrintf(sinf(2.0f * (float)M_PI * (float)((i * 44) % USBM_TONE_N) / USBM_TONE_N) * 32767.0f);
        int v = 128 + (a * 40 + b * 40) / 32767;
        tone[i] = (uint8_t)(v == USBM_DLE ? USBM_DLE + 1 : v);
    }
    char why[120];
    /* started first thing (main.c), while internal DMA memory is still free */
    if (!usbport_start(why, sizeof why)) { say("USB port: %s; port off", why); state = "USB host failed to start"; return; }
    const cdc_acm_host_driver_config_t dc = { .driver_task_stack_size = 6144, .driver_task_priority = 9, .xCoreID = 0 };
    if (cdc_acm_host_install(&dc) != ESP_OK) { say("USB modem port: the modem driver could not start; port off"); state = "modem driver failed to start"; return; }
    xTaskCreate(modem_task, "usbmodem", 4096, NULL, 5, NULL);
    say("USB port: on (a modem or a DC-PicoLink cable can be plugged in, also through a hub; the console is on UART0)");
    ESP_LOGI(TAG, "USB host started");
}

#else
size_t usbmodem_write(const void *data, size_t len) { (void)data; (void)len; return 0; }
bool um_cmd(const char *c, int ms) { (void)c; (void)ms; return false; }
bool um_connect(bool originate, int ms) { (void)originate; (void)ms; return false; }
int um_read(uint8_t *b, size_t cap, int ms) { (void)b; (void)cap; (void)ms; return -1; }
bool um_carrier(void) { return false; }
bool um_gone(void) { return true; }
void um_hangup(void) { }
void um_stop_tone(void) { }
void usbmodem_call_over(void) { }
size_t usbmodem_info_json(char *out, size_t cap) { return (size_t)snprintf(out, cap, "{\"built\":false}"); }
void usbmodem_start(void) { }
#endif
