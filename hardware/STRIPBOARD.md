# Stripboard build (RP2040-Zero + ESP32-S3 SuperMini + SD module + W5500 Lite)

One pin map for this and for the soldered stack (hardware/ESP32.md): same firmware, same
`sdkconfig.zero-stack`. Not tried on hardware: **measure your boards first** (column spacing
assumed 6 holes for the Zero and SuperMini, 7 for the W5500's two headers, both W5500 headers
starting at the same end).

Strips run along the rows; the modules stand side by side, USB ends all on the same side.
Row p1..p9 = pin position counted from the USB end; the SD module sits at p5..p10, the W5500
at p3..p8. Columns (holes from the SD column): SD 0, Zero left 2, Zero right 8, ESP left 10,
ESP right 16, W5500 J2 18, W5500 J1 25.

| Row | Strip carries |
|---|---|
| p1 | 5V: Zero 5V, ESP 5V, the Dreamcast's 5V (after fuse and diode) |
| p2 | GND: Zero, ESP, the Dreamcast's GND, W5500 GND (jumper), SD GND (jumper) |
| p3 | 3V3 from the Zero to the SD module (jumper) and W5500 J1 (jumper) |
| p4 | Zero GP3 - ESP GPIO2 (HS); Zero GP29 alone (sense wire); ESP GPIO13 (W5500 MISO) alone |
| p5 | GP4 - GPIO3 (link MISO); Zero GP28 alone; ESP GPIO12 (W5500 SCLK) alone |
| p6 | GP5 - GPIO4 (link CS); ESP GPIO11 (W5500 MOSI) alone |
| p7 | GP6 - GPIO5 (link clock); GPIO10 - J2 CS (straight); Zero GP26 - SD CLK |
| p8 | GP7 - GPIO6 (link MOSI); GPIO9 - J2 INT (straight); Zero GP15 - SD MOSI |
| p9 | GP8 - GPIO7 (DR); Zero GP14 - SD CS |

**Fit no pin** at: Zero GP0, GP1, GP2, GP27; ESP GPIO43, 44, 1, 3V3, GPIO8.

**Cut the strips (21)**, in the free holes between the modules: p3 one, p4 four, p5 five, p6 four,
p7 three, p8 three, p9 one (see the drawing).

**Jumpers (8, copper side):**
* SD: GND (p2 to p5), MISO (Zero GP28 at p5 to SD MISO at p6), 3V3 (p3 to p10).
* W5500: GND (J2's p3 pin to the GND strip, one hole), 3V3 (p3 strip to J1's p4 pin),
  MISO (J1 p8 to ESP GPIO13 at p4), and a crossing pair for SCLK and MOSI (ESP GPIO12 at p5 to
  J2 p6, ESP GPIO11 at p6 to J2 p5): the module's header order is MOSI, SCLK, the SuperMini's
  GPIO12, GPIO11 is the other way round.

The W5500 sits on the SuperMini's native SPI pins (full speed) at the price of those four extra
jumpers; header order on GPIO9-13 would save them but puts MOSI, CS and MISO through the pin
matrix (about 26 MHz limit, less margin at 20 MHz).

Check every cut with a continuity tester before applying power. Check J1/J2's pin order against
your module (the drawing assumes both headers start at the same end).

## Power from the Dreamcast, protection

The 5V comes from the Dreamcast's serial pin 1 through a **1N5819 (bar towards the boards)** so a
USB cable plugged in later can't push power into the console. Optional but recommended: a
resettable fuse (PTC, about 0.5 A hold) in series before the diode, since the Dreamcast's 5V rail
is rated for controllers, not for a Wi-Fi board, an SD card, a W5500 and a USB modem; measure the
real peak current with a USB power meter first. Put 220-470 µF and 10 µF across the 5V and GND
strips next to the ESP32 (Wi-Fi bursts). Never connect the Dreamcast's 3.3 V to a board's 3V3
pin. Plug in only one USB at a time.

## Dreamcast serial cable

Solder to the Zero's bottom edge pads and these three holes on the stripboard:

| Dreamcast pin | Goes to |
|---|---|
| 1 +5V | fuse, then 1N5819 (bar towards the boards), then the 5V strip (hole a: p1 in the SD column) |
| 3 and 8 GND | GND strip (hole b: p2 in the SD column) |
| 4 RXD | GP11 |
| 5 TXD | GP10 |
| 6 RTS | GP12 |
| 7 CTS | GP9 |
| 10 3.3V | 10 kΩ to GP29 (hole c: p4 in the SD column, optional sense wire) |
| 2, 9 | not connected |
| (cable side) | optional: 1 kΩ from GP13 to the pin 7 wire (the cable asks the Dreamcast to stop sending; DreamPi does not use flow control and serial works without it) |

Measure the Dreamcast pins on your own cable first.
