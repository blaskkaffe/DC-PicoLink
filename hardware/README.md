# Hardware

The full pinout of every build is in the main [README](../README.md#wiring-full-pinout).

* [WIRING.md](WIRING.md): the Pico wiring to the Dreamcast serial port through a DC SD Adapter V2
  (the SD-card-shaped plug build). An internal install uses the same signals soldered to the
  Dreamcast's serial port pads.
* [ESP32.md](ESP32.md): the RP2040-Zero + ESP32-S3 SuperMini stack (which pins are joined),
  the UART link for other boards, the W5500 module, power, the USB modem, updates and tests.
* [SD-SLOT.md](SD-SLOT.md): the SD card slot on the board's own SPI pins, and its files on
  the ESP32's web page.
* [ESP32-ONLY.md](ESP32-ONLY.md): why an ESP32 alone can't replace the RP2040 (measured).
* Planned: a smaller internal-mod version (see [docs/ROADMAP.md](../docs/ROADMAP.md)).
