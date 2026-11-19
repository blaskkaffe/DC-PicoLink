# SD card slot

The SD card sits on the RP2040's own SPI bus (SPI1), never on the Dreamcast's wires.
The same pins exist on all supported boards, so one firmware fits all of them.

| SD card / module pin | RP2040 | Pro Micro RP2040 | RP2040-Zero | Pico |
|---|---|---|---|---|
| CLK / SCK | GPIO26 (SPI1 SCK) | 26 (A0) | GP26 | GP26 (pin 31) |
| CMD / DI / MOSI | GPIO27 (SPI1 TX) | 27 (A1) | GP27 | GP27 (pin 32) |
| DAT0 / DO / MISO | GPIO28 (SPI1 RX) | 28 (A2) | GP28 | GP28 (pin 34) |
| DAT3 / CS | GPIO9 (zero-stack: GPIO29) | 9 | GP29 (zero-stack) | GP9 |
| card detect | not used: the firmware looks for a card every 2 s | | | |
| GND | GND | | | |
| VCC | see below | | | |

Avoided: Pico GP23–25 and GP29 (on-board use), RP2040-Zero GP16 (LED) and GP20–23
(pads underneath only). The Dreamcast side keeps GPIO2–7 (see WIRING.md).

## Example: 6-pin 3.3 V micro-SD module

Pin order on the module: GND, MISO, CLK, MOSI, CS, 3V3 (no card-detect pin).

| Module | GND | MISO | CLK | MOSI | CS | 3V3 |
|---|---|---|---|---|---|---|
| Board | GND | 28 | 26 | 27 | 9 | 3V3 |

## Which VCC

* **Module with a regulator and a level shifter** (the common blue AliExpress "Micro SD
  Card Adapter": a 3-pin regulator such as AMS1117-3.3 and a 14-pin 74LVC125): feed
  **5 V** to VCC, from the board's VBUS/5V pin. On 3.3 V its regulator drops the card's
  supply too low. Its logic pins are fine with the RP2040's 3.3 V signals.
* **Bare slot / module without a regulator**: 3V3 to VCC.
* Either way: a 10 µF capacitor across the card's supply close to the slot (cards draw
  current spikes when writing).

## What the firmware does

The card is the Dreamcast's SD card (the same emulation the Pi's `dcsd` serves; when `dcsd` is
connected it takes precedence). Status lines: `SD card: N MB SDHC/SDXC in the slot`, `SD card
slot: no card (no answer)`, `SD card: removed or not answering`. Any FAT32 card the Dreamcast's
SD loaders accept works; SDSC (2 GB and smaller) and SDHC/SDXC are handled. SPI clock 12.5 MHz
(`-DDC_SDC_HZ=...` to change). A run of blocks the Dreamcast reads is one CMD18 on the card, bytes move in bulk (about 1.4 MB/s at 12.5 MHz), and every block's CRC is checked: a block damaged on the wires is read again before the Dreamcast gets it. The search for a card
pauses while the Dreamcast uses the serial port (a search costs a few ms of the main loop); a
card that is already found stays in use.
