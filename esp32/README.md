# DC-PicoLink ESP32 side

Firmware for an **ESP32-S3** (ESP32-S3-Zero) connected to the cable over UART, so a
Dreamcast can go online without a Raspberry Pi. Design and plan: the project's
`07-esp32-link.md` (steps E-1 to E-11). Wiring: [../hardware/ESP32.md](../hardware/ESP32.md).

**Status: step E-2 (written, builds, not yet tried on hardware).**

| Step | What | State |
|---|---|---|
| E-1 | link to the cable (shared framing `../firmware/link_model.h`) | done in the cable firmware |
| E-2 | Wi-Fi setup network + web page: link state, the cable's hello, status lines, serial monitor, dc1 box, home Wi-Fi | **this** |
| E-3 | DCNow! over serial PPP (Hayes stub, PPP server, DNS, NAT, DCNow! reports) | next |
| E-4 | DCNow! over the cable's W5500 frames | |
| E-5 | NAT rewrite list, UPnP + STUN, DreamPi's helpers | |
| E-6 | DCNET | |

## Build and flash

ESP-IDF 5.4:

```
cd esp32
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor      # the board's own USB port
```

CI builds it too (Actions → build → artifact `dc-picolink-esp32`): one merged image, flashed with
`esptool.py --chip esp32s3 write_flash 0x0 dc-picolink-esp32-<version>-merged.bin`
(hold BOOT while plugging in the board if it isn't found).

## Use

Join **DC-PicoLink-XXXX** (password `dreamcast`, change it in menuconfig) and open
http://192.168.4.1. Save your home network under *Wi-Fi*; the page is then also on the
address shown there. The setup network stays on.

## Files

| | |
|---|---|
| `main/link.c` | UART1 link: reader task, packet queue, asks the cable `get hello` every second |
| `main/wifi.c` | setup network + home network (NVS) |
| `main/web.c` | the page and `/api/*` |
| `main/main.c` | status-line ring, serial monitor ring, start-up |
| `../firmware/link_model.h` | framing, shared with the cable (tested in `firmware/tests/test_link.c`) |
