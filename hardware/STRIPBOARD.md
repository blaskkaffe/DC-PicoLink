# Stripboard build (RP2040-Zero + ESP32-S3 SuperMini + SD module + W5500 Lite)

One pin map for this and for the soldered stack (hardware/ESP32.md): same firmware, same
`sdkconfig.zero-stack`. Not tried on hardware: **measure your boards first** (column spacing
assumed 6 holes for the Zero and SuperMini, 7 for the W5500's two headers).

Strips run along the rows; the modules stand side by side, USB ends all on the same side.
Row p1..p9 = pin position counted from the USB end; the SD module sits at p5..p10.
Columns (holes from the SD column): SD 0, Zero left 2, Zero right 8, ESP left 10, ESP right 16,
W5500 J2 18, W5500 J1 25.

| Row | Strip carries |
|---|---|
| p1 | 5V: Zero 5V, ESP 5V |
| p2 | GND: Zero, ESP, W5500 J2 and J1 |
| p3 | 3V3 from the Zero to the SD module (jumper) and W5500 J1 (jumper) |
| p4 | Zero GP3 - ESP GPIO2 (HS); ESP GPIO13 - J2 MOSI; Zero GP29 alone (sense wire) |
| p5 | Zero GP4 - GPIO3 (link MISO); GPIO12 - J2 SCLK; Zero GP28 alone |
| p6 | GP5 - GPIO4 (link CS); GPIO11 - J2 CS |
| p7 | GP6 - GPIO5 (link clock); GPIO10 - J2 INT; Zero GP26 - SD CLK |
| p8 | GP7 - GPIO6 (link MOSI); GPIO9 - J1 MISO (jumper from p7); Zero GP15 - SD MOSI |
| p9 | GP8 - GPIO7 (DR); Zero GP14 - SD CS |

**Fit no pin** at: Zero GP0, GP1, GP2, GP27; ESP GPIO43, 44, 1, 3V3, GPIO8.

**Cut the strips (18)**, in the free holes between the modules: p3 twice (before and after J2's
second GND), p4 three, p5 four, p6 three, p7 three, p8 two, p9 one (see the drawing).

**Jumpers (5, copper side):** SD GND (p2 to p5), SD MISO (Zero GP28 at p5 to SD MISO at p6),
SD 3V3 (p3 to p10), W5500 3V3 (ESP-side p3 strip to J1's p3), W5500 MISO (J1 p7 to p8).

Check every cut with a continuity tester before applying power. Check J1/J2's pin order against
your module (the drawing assumes both headers start at the same end).

## Dreamcast serial cable

Solder to the Zero's bottom edge pads and these three holes; never to a board's 3V3 pin.

| Dreamcast pin | Goes to |
|---|---|
| 1 +5V | 1N5819 (bar towards the Zero) to the 5V strip (p1) |
| 3 and 8 GND | GND strip (p2) |
| 4 RXD | GP11 |
| 5 TXD | GP10 |
| 6 RTS | GP12 |
| 7 CTS | GP9, and 1 kΩ from GP13 to this wire |
| 10 3.3V | 10 kΩ to GP29 (the isolated p4 hole on the Zero side) |
| 2, 9 | not connected |

Measure the Dreamcast pins on your own cable first. Plug in only one USB at a time.
