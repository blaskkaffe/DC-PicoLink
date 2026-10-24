/* Composite device: CDC-ACM serial (coder's cable), vendor bulk SD block link, CDC-NCM (W5500) */
#include "tusb.h"
#include "pico/unique_id.h"
#include <string.h>

#define USB_VID 0xCAFE   /* TinyUSB test VID - fine for personal use */
#define USB_PID 0x4DC5

uint8_t tud_network_mac_address[6];   /* host-side MAC of the usb0 interface */

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID, .idProduct = USB_PID, .bcdDevice = 0x0400,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3,
    .bNumConfigurations = 1
};
uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&desc_device; }

enum { ITF_CDC = 0, ITF_CDC_DATA, ITF_SD, ITF_NCM, ITF_NCM_DATA, ITF_TOTAL };
enum { STR_LANG, STR_MANUF, STR_PRODUCT, STR_SERIAL, STR_CDC, STR_NCM, STR_MAC, STR_SD };

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_NCM_DESC_LEN)
static const uint8_t desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, 0x81, 8, 0x02, 0x82, 64),
    TUD_VENDOR_DESCRIPTOR(ITF_SD, STR_SD, 0x04, 0x84, 64),
    TUD_CDC_NCM_DESCRIPTOR(ITF_NCM, STR_NCM, STR_MAC, 0x85, 64, 0x06, 0x86,
                           CFG_TUD_NET_ENDPOINT_SIZE, CFG_TUD_NET_MTU),
};
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return desc_config; }

static const char *strings[] = {
    [STR_MANUF] = "DIY", [STR_PRODUCT] = "Dreamcast Coder's Cable + W5500 (RP2040)",
    [STR_CDC] = "DC Serial", [STR_NCM] = "DC W5500 Ethernet", [STR_SD] = "DC SD Card Link",
};
static uint16_t desc_str[48];

static void make_mac(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    tud_network_mac_address[0] = 0x02;                 /* locally administered */
    tud_network_mac_address[1] = 0xDC;
    for (int i = 0; i < 4; i++) tud_network_mac_address[2 + i] = id.id[4 + i];
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    char buf[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char *s;
    uint8_t n;
    if (index == STR_LANG) { desc_str[1] = 0x0409; n = 1; goto done; }
    if (index == STR_SERIAL) { pico_get_unique_board_id_string(buf, sizeof(buf)); s = buf; }
    else if (index == STR_MAC) {
        make_mac();
        for (int i = 0; i < 6; i++) {
            buf[2 * i]     = "0123456789ABCDEF"[tud_network_mac_address[i] >> 4];
            buf[2 * i + 1] = "0123456789ABCDEF"[tud_network_mac_address[i] & 15];
        }
        buf[12] = 0; s = buf;
    }
    else if (index < sizeof(strings) / sizeof(strings[0]) && strings[index]) s = strings[index];
    else return NULL;
    n = (uint8_t)strlen(s); if (n > 47) n = 47;
    for (uint8_t i = 0; i < n; i++) desc_str[1 + i] = s[i];
done:
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc_str;
}
