/*
 * Updating the cable (RP2040) over the ESP32's USB port, for a cable built into the Dreamcast: no
 * BOOTSEL button, no computer. The page's firmware upload hands a .uf2 here (web.c post_update):
 *   1. the 1200-baud touch on the cable's USB serial port (SET_LINE_CODING 1200, DTR low; EP0 only,
 *      no extra host channel); a cable too old for the touch is asked over the link instead
 *      ("call link.update");
 *   2. the RP2040 comes back as 2E8A:0003, its boot ROM; its PICOBOOT interface (vendor class, two
 *      bulk endpoints) is claimed: 3 channels, the ones the cable had;
 *   3. the UF2 blocks as they arrive from the browser: each 4 KB flash sector erased the first time a
 *      block lands in it, the 256 bytes written and read back;
 *   4. REBOOT: the cable starts the new firmware and comes back as a cable.
 * A stop half way leaves the RP2040 in its boot ROM (it cannot be bricked this way): upload again.
 */
#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "app.h"

#if CONFIG_DCL_USB_MODEM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

usb_host_client_handle_t usbport_client(void);
usb_device_handle_t usbport_cable_dev(void);
uint8_t usbport_boot_addr(void);
void usbport_boot_forget(void);

#include "uf2_model.h"
#define PB_MAGIC    0x431FD10Bu

static SemaphoreHandle_t done;
static usb_transfer_t *xc, *xb;            /* control, bulk */
static void xfer_cb(usb_transfer_t *t) { (void)t; xSemaphoreGive(done); }

static bool wait_xfer(usb_transfer_t *t, int ms) {
    if (xSemaphoreTake(done, pdMS_TO_TICKS(ms)) != pdTRUE) return false;
    return t->status == USB_TRANSFER_STATUS_COMPLETED;
}
static bool ctrl(usb_device_handle_t d, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, const void *data, uint16_t len) {
    usb_setup_packet_t *s = (usb_setup_packet_t *)xc->data_buffer;
    s->bmRequestType = type; s->bRequest = req; s->wValue = val; s->wIndex = idx; s->wLength = len;
    if (len) memcpy(xc->data_buffer + sizeof *s, data, len);
    xc->num_bytes = (int)(sizeof *s + len);
    xc->device_handle = d; xc->bEndpointAddress = 0; xc->callback = xfer_cb; xc->timeout_ms = 1000;
    if (usb_host_transfer_submit_control(usbport_client(), xc) != ESP_OK) return false;
    return wait_xfer(xc, 2000);
}
/* one bulk transfer; IN sizes are rounded up to whole 64-byte packets as the host driver wants */
static int bulk(usb_device_handle_t d, uint8_t ep, void *buf, size_t n, int ms) {
    bool in = ep & 0x80;
    xb->device_handle = d; xb->bEndpointAddress = ep; xb->callback = xfer_cb; xb->timeout_ms = (uint32_t)ms;
    xb->num_bytes = in ? (int)((n + 63) & ~63u) : (int)n;
    if (in && xb->num_bytes == 0) xb->num_bytes = 64;
    if (!in && n) memcpy(xb->data_buffer, buf, n);
    if (usb_host_transfer_submit(xb) != ESP_OK) return -1;
    if (!wait_xfer(xb, ms + 500)) return -1;
    if (in && n) memcpy(buf, xb->data_buffer, (size_t)xb->actual_num_bytes < n ? (size_t)xb->actual_num_bytes : n);
    return xb->actual_num_bytes;
}

/* ---- PICOBOOT (the RP2040 boot ROM's vendor interface) ---- */
typedef struct __attribute__((packed)) {
    uint32_t magic, token;
    uint8_t id, size; uint16_t unused;
    uint32_t len;
    uint8_t args[16];
} pb_cmd_t;
static usb_device_handle_t bd;
static uint8_t b_itf, b_out, b_in;
static uint32_t token;
/* a command: the 32-byte command, its data (OUT or IN by bit 7 of the id), then the acknowledgement,
   an empty packet the other way */
static bool pb(uint8_t id, const void *args, uint8_t asz, void *data, uint32_t len) {
    pb_cmd_t c = { .magic = PB_MAGIC, .token = ++token, .id = id, .size = asz, .len = len };
    if (asz) memcpy(c.args, args, asz);
    if (bulk(bd, b_out, &c, sizeof c, 1000) != (int)sizeof c) return false;
    bool in = id & 0x80;
    if (len) {
        int k = bulk(bd, in ? b_in : b_out, data, len, 3000);
        if (k != (int)len) return false;
    }
    if (in) return bulk(bd, b_out, NULL, 0, 3000) == 0;
    uint8_t z[1];
    return bulk(bd, b_in, z, 0, 5000) == 0;              /* erase of a sector: tens of ms */
}
static bool pb_addr(uint8_t id, uint32_t addr, uint32_t size, void *data, uint32_t len) {
    uint32_t a[2] = { addr, size };
    return pb(id, a, 8, data, len);
}

/* find the PICOBOOT interface: vendor class with a bulk OUT and a bulk IN endpoint */
static bool find_picoboot(void) {
    const usb_config_desc_t *cd;
    if (usb_host_get_active_config_descriptor(bd, &cd) != ESP_OK) return false;
    const uint8_t *p = (const uint8_t *)cd, *end = p + cd->wTotalLength;
    int itf = -1, cls = -1; uint8_t o = 0, i = 0;
    while (p + 2 <= end && p[0] >= 2) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && p[0] >= 9) {
            if (cls == 0xFF && o && i) break;
            itf = p[2]; cls = p[5]; o = i = 0;
        } else if (p[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && p[0] >= 7 && cls == 0xFF && (p[3] & 3) == 2) {
            if (p[2] & 0x80) i = p[2]; else o = p[2];
        }
        p += p[0];
    }
    if (cls != 0xFF || !o || !i) return false;
    b_itf = (uint8_t)itf; b_out = o; b_in = i;
    return true;
}

static bool open_boot(int wait_ms) {
    int64_t until = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    do {
        uint8_t a = usbport_boot_addr();
        if (a && usb_host_device_open(usbport_client(), a, &bd) == ESP_OK) {
            const usb_device_desc_t *dd;
            if (usb_host_get_device_descriptor(bd, &dd) == ESP_OK && dd->idVendor == 0x2E8A && dd->idProduct == 0x0003) return true;
            usb_host_device_close(usbport_client(), bd);
            usbport_boot_forget();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    } while (esp_timer_get_time() < until);
    bd = NULL;
    return false;
}

/* ---- one update ---- */
static struct {
    bool on, failed;
    uint32_t blocks, total, written;
    uf2_erased_t erased;
    uint8_t blk[512]; size_t bn;
    char err[96];
} u;

static void fail(const char *m) { if (!u.failed) { u.failed = true; snprintf(u.err, sizeof u.err, "%s", m); } }

/* the cable into its boot ROM and PICOBOOT open; false: err says why */
static bool start(void) {
    if (!done) done = xSemaphoreCreateBinary();
    if (!xc) usb_host_transfer_alloc(64 + 8, 0, &xc);
    if (!xb) usb_host_transfer_alloc(512, 0, &xb);
    if (!done || !xc || !xb) { fail("no memory"); return false; }
    if (!open_boot(0)) {
        usb_device_handle_t cd = usbport_cable_dev();
        if (!cd && !link_is_up()) { fail("the cable is not on the ESP32's USB port (and not in its bootloader)"); return false; }
        if (cd) {
            say_line("cable update: restarting the cable into its USB bootloader (1200-baud touch)");
            static const uint8_t lc[7] = { 0xB0, 0x04, 0, 0, 0, 0, 8 };       /* 1200, 1 stop bit, no parity, 8 bits */
            ctrl(cd, 0x21, 0x20, 0, 0, lc, sizeof lc);                       /* SET_LINE_CODING, interface 0 */
            ctrl(cd, 0x21, 0x22, 0, 0, NULL, 0);                             /* SET_CONTROL_LINE_STATE: DTR low */
        }
        if (!open_boot(5000)) {
            if (!link_is_up()) { fail("the cable did not come back in its bootloader"); return false; }
            say_line("cable update: no answer to the touch (older firmware?): asking over the link");
            static const char m[] = "dc1\nid 9\ncall link.update\n";
            link_dc1(m, sizeof m - 1);
            if (!open_boot(6000)) { fail("the cable did not come back in its bootloader (on the stacked pins it cannot: use its USB)"); return false; }
        }
    }
    if (!find_picoboot()) { fail("the bootloader has no PICOBOOT interface"); return false; }
    if (usb_host_interface_claim(usbport_client(), bd, b_itf, 0) != ESP_OK) { fail("no USB channel for the bootloader"); return false; }
    ctrl(bd, 0x41, 0x41, 0, b_itf, NULL, 0);                                 /* INTERFACE_RESET */
    uint8_t ex = 1;
    if (!pb(0x01, &ex, 1, NULL, 0)) { fail("the bootloader did not answer"); return false; }   /* EXCLUSIVE_ACCESS */
    if (!pb(0x06, NULL, 0, NULL, 0)) { fail("the bootloader did not answer (EXIT_XIP)"); return false; }
    say_line("cable update: bootloader open, writing");
    return true;
}

static void block(const uint8_t *b) {
    uint32_t addr, nblk, sec;
    uf2_res_t r = uf2_parse(b, &addr, &nblk);                  /* uf2_model.h */
    if (r == UF2_SKIP) return;
    if (r != UF2_OK) { fail(uf2_res_text(r)); return; }
    if (!u.total) u.total = nblk;
    if (uf2_needs_erase(&u.erased, addr, &sec) && !pb_addr(0x03, sec, 4096, NULL, 0)) { fail("erase failed"); return; }
    if (!pb_addr(0x05, addr, 256, (void *)(b + 32), 256)) { fail("write failed"); return; }
    uint8_t rb[256];
    if (!pb_addr(0x84, addr, 256, rb, 256) || memcmp(rb, b + 32, 256)) { fail("what was written reads back different"); return; }
    u.written++;
    if (u.total && (u.written * 4 / u.total) != ((u.written - 1) * 4 / u.total) && u.written < u.total)
        say_line("cable update: %u of %u blocks", (unsigned)u.written, (unsigned)u.total);
}

/* web.c: the upload, in pieces as they arrive; first == true for the first piece */
bool cableflash_feed(const uint8_t *p, size_t n, bool first) {
    if (first) {
        memset(&u, 0, sizeof u);
        u.on = true;
        if (!start()) return false;
    }
    if (u.failed) return false;
    while (n) {
        size_t k = sizeof u.blk - u.bn;
        if (k > n) k = n;
        memcpy(u.blk + u.bn, p, k); u.bn += k; p += k; n -= k;
        if (u.bn == sizeof u.blk) { block(u.blk); u.bn = 0; if (u.failed) return false; }
    }
    return true;
}
/* the end of the upload: restart the cable into the new firmware; returns the text for the page */
const char *cableflash_end(bool upload_ok) {
    static char m[160];
    if (!u.on) return "cable update: nothing was sent";
    if (!u.failed && !upload_ok) fail("upload interrupted");
    if (!u.failed && u.bn) fail("the file does not end on a whole UF2 block");
    if (!u.failed && (!u.written || u.written != u.total)) fail("the file has fewer blocks than it says");
    if (!u.failed) {
        uint32_t r[3] = { 0, 0, 500 };                                     /* pc 0 = start normally, after 500 ms */
        pb(0x02, r, 12, NULL, 0);
        snprintf(m, sizeof m, "cable update: %u blocks written and checked; the cable restarts into the new firmware", (unsigned)u.written);
    } else {
        snprintf(m, sizeof m, "cable update: %s; the cable %s", u.err,
                 bd ? "waits in its bootloader: upload again" : "was not changed");
    }
    if (bd) {
        usb_host_interface_release(usbport_client(), bd, b_itf);
        usb_host_device_close(usbport_client(), bd);
        bd = NULL;
    }
    if (!u.failed) usbport_boot_forget();
    u.on = false;
    say_line("%s", m);
    return m;
}
#else
bool cableflash_feed(const uint8_t *p, size_t n, bool first) { (void)p; (void)n; (void)first; return false; }
const char *cableflash_end(bool upload_ok) { (void)upload_ok; return "cable update: this build has no USB host (the cable updates from a computer)"; }
#endif
