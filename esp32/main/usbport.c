/*
 * The ESP32's USB socket as a host for whatever is plugged in (modem builds, CONFIG_DCL_USB_MODEM):
 *   - a DC-PicoLink cable (CAFE:4DC5): the link over USB, the same packets as over SPI or UART
 *     (its interface with the bulk endpoints 0x07/0x87, see ../../firmware/usb_descriptors.c);
 *   - anything else: offered to the modem driver (usbmodem.c) by its VID:PID.
 * One device at a time is the plan; a hub in between works (CONFIG_USB_HOST_HUBS_SUPPORTED).
 *
 * The cable's bytes go to a task of their own (link_usb_rx), so the USB client task that finishes
 * transfers never waits on the PPP server or the web page's SD requests.
 */
#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "app.h"

#if CONFIG_DCL_USB_MODEM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

#define PL_VID      0xCAFE
#define PL_PID      0x4DC5
#define PL_EP_OUT   0x07
#define PL_EP_IN    0x87
#define URING       (8u * 1024u)          /* wire bytes to the cable, as the SPI link's ring */
#define XFER        1024u                 /* bytes per bulk transfer (a multiple of 64) */

static usb_host_client_handle_t client;
static QueueHandle_t otherq;              /* VID:PID of devices that are not a cable -> usbmodem.c */
static StreamBufferHandle_t rxs;          /* cable -> link parser task */

/* the cable on the port */
static usb_device_handle_t cdev;
static uint8_t citf;
static volatile bool active, gone;
static usb_transfer_t *tin, *tout;
static volatile bool in_busy, out_busy;
static uint8_t *uring;
static volatile uint32_t uwr, urd;        /* free-running; uwr under the link's tx lock */
static SemaphoreHandle_t kick_lk;
static uint32_t rx_lost;
static usb_device_handle_t want;          /* a cable waiting for the modem to free its channels */
static int64_t want_until;
bool usbmodem_yield(void);

/* ---- to the cable ---- */
size_t usbl_room(void) { return URING - (uwr - urd); }
size_t usbl_backlog(void) { return uwr - urd; }   /* urd moves when a transfer is done: in-flight bytes count */
void usbl_put(const uint8_t *w, size_t k) {
    uint32_t at = uwr & (URING - 1), first = URING - at;
    if (first > k) first = (uint32_t)k;
    memcpy(uring + at, w, first);
    memcpy(uring, w + first, k - first);
    uwr += (uint32_t)k;
}
void usbl_kick(void) {
    if (!active || gone) return;
    xSemaphoreTake(kick_lk, portMAX_DELAY);
    uint32_t n = uwr - urd;
    if (!out_busy && n) {
        if (n > XFER) n = XFER;
        for (uint32_t i = 0; i < n; i++) tout->data_buffer[i] = uring[(urd + i) & (URING - 1)];
        tout->num_bytes = (int)n;
        out_busy = true;
        if (usb_host_transfer_submit(tout) != ESP_OK) out_busy = false;
    }
    xSemaphoreGive(kick_lk);
}
static void out_cb(usb_transfer_t *t) {
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) urd += (uint32_t)t->actual_num_bytes;
    out_busy = false;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) return;
    link_tx_freed();
    usbl_kick();
}

/* ---- from the cable ---- */
static void in_cb(usb_transfer_t *t) {
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0) {
        size_t w = xStreamBufferSend(rxs, t->data_buffer, (size_t)t->actual_num_bytes, 0);
        rx_lost += (uint32_t)t->actual_num_bytes - (uint32_t)w;
    }
    if (gone || (t->status != USB_TRANSFER_STATUS_COMPLETED && t->status != USB_TRANSFER_STATUS_TIMED_OUT)) { in_busy = false; return; }
    /* room for a whole transfer first: the parser task catches up, nothing is lost */
    while (xStreamBufferSpacesAvailable(rxs) < XFER && !gone) vTaskDelay(1);
    if (gone || usb_host_transfer_submit(t) != ESP_OK) in_busy = false;
}
static void rx_task(void *arg) {
    (void)arg;
    static uint8_t b[512];
    for (;;) {
        size_t n = xStreamBufferReceive(rxs, b, sizeof b, portMAX_DELAY);
        if (n) link_usb_rx(b, n);
    }
}

/* the cable's link interface: a vendor-class interface with the endpoint 0x07 */
static int find_link_itf(const usb_config_desc_t *cd) {
    const uint8_t *p = (const uint8_t *)cd, *end = p + cd->wTotalLength;
    int itf = -1, cls = -1;
    while (p + 2 <= end && p[0] >= 2) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && p[0] >= 9) { itf = p[2]; cls = p[5]; }
        else if (p[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && p[0] >= 7 && cls == 0xFF && p[2] == PL_EP_OUT) return itf;
        p += p[0];
    }
    return -1;
}

static void take_cable(usb_device_handle_t d) {
    const usb_config_desc_t *cd;
    int itf = usb_host_get_active_config_descriptor(d, &cd) == ESP_OK ? find_link_itf(cd) : -1;
    if (itf < 0) {
        say_line("USB: a DC-PicoLink cable without the link interface is plugged in (its firmware is older: update it)");
        usb_host_device_close(client, d);
        return;
    }
    if (active) {
        say_line("USB: a second DC-PicoLink cable is plugged in; only the first one is used");
        usb_host_device_close(client, d);
        return;
    }
    if (usb_host_interface_claim(client, d, (uint8_t)itf, 0) != ESP_OK) {
        /* the host has 8 channels for all open endpoints: with a hub and a modem open there is no
           room for the cable. The cable wins: the modem closes, and the claim is tried again. */
        if (want != d && usbmodem_yield()) {
            want = d; want_until = esp_timer_get_time() + 3000000;
            return;
        }
        if (want == d && esp_timer_get_time() < want_until) return;       /* tried again in client_task */
        want = NULL;
        say_line("USB: the DC-PicoLink cable's link interface could not be opened%s",
                 usbmodem_yield() ? "" : " (with a hub: unplug other USB devices; the ESP32's USB host has 8 channels)");
        usb_host_device_close(client, d);
        return;
    }
    want = NULL;
    if (!uring) uring = malloc(URING);
    if (!tin) usb_host_transfer_alloc(XFER, 0, &tin);
    if (!tout) usb_host_transfer_alloc(XFER, 0, &tout);
    if (!uring || !tin || !tout) {
        say_line("USB: no memory for the cable's link");
        usb_host_interface_release(client, d, (uint8_t)itf);
        usb_host_device_close(client, d);
        return;
    }
    cdev = d; citf = (uint8_t)itf; gone = false;
    uwr = urd = 0;
    xStreamBufferReset(rxs);
    tin->device_handle = tout->device_handle = d;
    tin->bEndpointAddress = PL_EP_IN; tin->num_bytes = XFER; tin->callback = in_cb; tin->timeout_ms = 0;
    tout->bEndpointAddress = PL_EP_OUT; tout->callback = out_cb; tout->timeout_ms = 1000;
    active = true;
    link_usb_set(true);
    in_busy = true;
    if (usb_host_transfer_submit(tin) != ESP_OK) in_busy = false;
    say_line("USB: DC-PicoLink cable plugged in; the link runs over USB");
}

static void drop_cable(void) {
    if (!active) return;
    link_usb_set(false);
    active = false;
    usb_host_endpoint_halt(cdev, PL_EP_IN);  usb_host_endpoint_flush(cdev, PL_EP_IN);
    usb_host_endpoint_halt(cdev, PL_EP_OUT); usb_host_endpoint_flush(cdev, PL_EP_OUT);
    for (int i = 0; i < 100 && (in_busy || out_busy); i++) usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
    usb_host_interface_release(client, cdev, citf);
    usb_host_device_close(client, cdev);
    cdev = NULL;
    say_line("USB: DC-PicoLink cable unplugged");
}

/* a device appeared: the cable, or something for the modem driver */
static void look_at(uint8_t addr) {
    usb_device_handle_t d;
    if (usb_host_device_open(client, addr, &d) != ESP_OK) return;
    const usb_device_desc_t *dd;
    if (usb_host_get_device_descriptor(d, &dd) != ESP_OK) { usb_host_device_close(client, d); return; }
    if (dd->bDeviceClass == USB_CLASS_HUB) { usb_host_device_close(client, d); return; }   /* the hub itself */
    if (dd->idVendor == PL_VID && dd->idProduct == PL_PID) { take_cable(d); return; }
    uint32_t id = (uint32_t)dd->idVendor << 16 | dd->idProduct;
    usb_host_device_close(client, d);
    xQueueSend(otherq, &id, 0);
}

static void client_cb(const usb_host_client_event_msg_t *m, void *arg) {
    (void)arg;
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) look_at(m->new_dev.address);
    else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (active && m->dev_gone.dev_hdl == cdev) gone = true;
        if (want && m->dev_gone.dev_hdl == want) { usb_host_device_close(client, want); want = NULL; }
    }
}

static void client_task(void *arg) {
    (void)arg;
    /* devices that were there before this client started */
    uint8_t addrs[8]; int n = 0;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) == ESP_OK)
        for (int i = 0; i < n; i++) look_at(addrs[i]);
    for (;;) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(50));
        if (want && !active) take_cable(want);                      /* the modem is closing: again */
        if (gone && active) {
            drop_cable();
            /* the devices still there (a modem waiting behind the cable): offer them again */
            uint8_t a[8]; int k = 0;
            if (usb_host_device_addr_list_fill(sizeof a, a, &k) == ESP_OK)
                for (int i = 0; i < k; i++) look_at(a[i]);
        }
    }
}

static void lib_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t f;
        usb_host_lib_handle_events(portMAX_DELAY, &f);
        if (f & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

/* the next plugged-in device that is not a cable (for the modem driver) */
bool usbport_next_other(uint16_t *vid, uint16_t *pid, uint32_t wait_ms) {
    uint32_t id;
    if (xQueueReceive(otherq, &id, wait_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms)) != pdTRUE) return false;
    *vid = (uint16_t)(id >> 16); *pid = (uint16_t)id;
    return true;
}
bool usbport_cable(void) { return active; }

/* the USB host and this client; false = the port is off */
bool usbport_start(char *why, size_t cap) {
    otherq = xQueueCreate(4, sizeof(uint32_t));
    rxs = xStreamBufferCreate(4096, 1);
    kick_lk = xSemaphoreCreateMutex();
    const usb_host_config_t hc = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    esp_err_t he = ESP_FAIL;
    for (int t = 0; t < 5 && (he = usb_host_install(&hc)) != ESP_OK; t++) vTaskDelay(pdMS_TO_TICKS(300));
    if (he != ESP_OK) {
        snprintf(why, cap, "the USB host could not start (%s; free %u bytes, largest block %u)", esp_err_to_name(he),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        return false;
    }
    xTaskCreate(lib_task, "usb_lib", 4096, NULL, 10, NULL);
    const usb_host_client_config_t cc = { .is_synchronous = false, .max_num_event_msg = 8,
                                          .async = { .client_event_callback = client_cb, .callback_arg = NULL } };
    if (usb_host_client_register(&cc, &client) != ESP_OK) { snprintf(why, cap, "the USB port client could not start"); return false; }
    xTaskCreate(client_task, "usb_port", 4096, NULL, 9, NULL);
    xTaskCreate(rx_task, "usb_link_rx", 4096, NULL, 10, NULL);
    return true;
}
#endif
