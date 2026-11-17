# Vendored ESP-IDF components

Copied in so the build works offline (no component registry):

- `mdns/`: espressif/esp-protocols `components/mdns` (commit aba9f6d), Apache-2.0 (LICENSE inside).
  Announces the box as `dreampi.local` for dreamcast.online/now/configure.
- `usb_host_cdc_acm/`: espressif/esp-usb `host/class/cdc/usb_host_cdc_acm`, Apache-2.0.
  The USB modem port (`main/usbmodem.c`).

Examples, tests and the registry manifest were left out.
