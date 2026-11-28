# Stack PCB: RP2040-Zero + ESP32-S3 SuperMini with an SD-card plug

> ⚠️ Not made or tested yet. Routed and checked with KiCad's DRC (0 errors, 0 unconnected); the
> SD-card contact positions come from KiCad's SD socket footprints, not from a measured card.

A 2-layer board that sits between the two modules of the stack ([../ESP32.md](../ESP32.md#the-rp2040-zero--esp32-s3-supermini-stack-spi-link)).
Its other end is shaped like a full-size SD card and goes into the slot of the DC SD adapter, so
the stack plugs straight into the Dreamcast's serial port without a cable. It carries:

* the Dreamcast lines from the Zero's bottom-edge pads (GP9-GP13) to the SD contacts, as in
  [../WIRING.md](../WIRING.md): GP9 → contact 5 (CLK, DC CTS), GP10 → 2 (CMD, DC TXD),
  GP11 → 7 (DAT0, DC RXD), GP12 → 1 (CS, DC RTS), GND → 3 and 6;
* R1 1 kΩ GP13 → GP9 (flow control) and R2 10 kΩ GP29 → contact 4 (the Dreamcast's 3.3 V:
  the power-sense input, built in);
* the slot card's microSD socket **under the ESP32**, with 10 µF + 100 nF and 10 kΩ pull-ups
  on CS, MOSI, MISO, DAT1 and DAT2 (SPI1: GP26 SCK, GP15 MOSI, GP28 MISO, GP14 CS, as
  [../SD-SLOT.md](../SD-SLOT.md));
* a 5 V input (JST XH 2-pin) through an SS14 Schottky into the stack's 5V. **The SD slot has no
  5 V** (contact 4 is the Dreamcast's 3.3 V, sized for an SD card, used for sensing only): power
  the stack from this input (the Dreamcast's serial pin 1 or a charger) or from a USB port.
  See [../ESP32.md#power](../ESP32.md#power).

The same pin map and firmware as the soldered stack: cable `-DDC_BOARD=zero-stack`, ESP32
`sdkconfig.zero-stack`.

![top and bottom](render.png)

## Parts

| Ref | Part | Footprint | Side |
|---|---|---|---|
| J2 | SD card contacts (part of the board) | – | bottom, card end |
| J4 | Molex 104031-0811 microSD socket (push-pull, 1.42 mm high) | Connector_Card | bottom, under the ESP32 |
| R3-R7 | 10 kΩ pull-ups (CS, MOSI, MISO, DAT1, DAT2) | 0805 | bottom |
| C1, C2 | 10 µF, 100 nF (card supply) | 0805 | bottom |
| R1 | 1 kΩ | 0805 | top |
| R2 | 10 kΩ | 0805 | top |
| D1 | SS14 (or any 1 A Schottky in SMA) | SMA | top |
| J3 | JST XH 2-pin, vertical (5 V in: pin 1 +, pin 2 GND) | THT | top |
| – | long 2.54 mm pin header (≥ 15 mm), 1×9 ×2 | | |

## Ordering

The Gerber zip from `make.sh` (or the one attached to the release) at any 2-layer fab:
**1.6 mm, ENIG** (gold: the SD contacts rub against the slot's springs; HASL leaves them
uneven). An SD card is 2.1 mm thick; SD extender boards are usually 1.6 mm and the springs make
contact, but if yours is loose in the slot, a strip of Kapton on the top face of the card end
takes up the play (or order 2.0 mm).

## Assembly

The top face (where the Zero sits) is the card's label side. Which way up the stack hangs
depends on how the adapter's slot takes a card.

1. SMD parts first, both sides: the microSD socket, R3-R7, C1, C2 underneath; R1, R2, D1 on top
   (D1's band toward the stack). Then J3.
2. **Pins.** The marks beside each hole (on both faces) say how it is fitted:
   * **F**: one long pin through all three boards (5V, GND; the six link pins GP3-GP8 ↔ ESP32
     GPIO2-7);
   * **Z**: a pin from the Zero into this board only, **cut flush underneath** before the ESP32
     goes on (3V3, GP29, GP28, GP26, GP15, GP14: these must never reach the ESP32's pins below,
     see the table in ESP32.md);
   * **O**: optional (GP0/GP1 ↔ the ESP32's console UART); **–**: no pin.
3. The Zero on top, face up, its USB at the square end. Solder the top joints.
4. GP9-GP13: the five holes in a row just past the Zero's bottom edge. Join each to the Zero's
   bottom-edge pad above it (a bent pin or a short bare wire).
5. Cut the Z pins flush underneath. Stick Kapton on the ESP32-S3 SuperMini's back (the socket,
   the resistors and the capacitors sit right under it; its back has bare pads), slide it face
   down onto the F pins, USB at the same end, and solder.
6. Before power: check with a meter that 3V3 (Zero) and 3V3 (ESP32) are **not** joined, and
   that SD contact 4 does not touch either 3V3.

The microSD card goes in from the USB end, between this board and the ESP32. The socket is a
push-pull type: pull the card out by its end.

**Tight spots (measure your parts):** the socket's edges are 0.4 mm from the solder pads of the
pin rows, and about level with the plastic strip of a standard pin header between this board
and the ESP32: use headers whose plastic strip can be pulled off there, or keep the gap with a
2.5 mm spacer only at the ends. The hole positions assume the Zero's and the SuperMini's first
pin about 1.3 mm from the USB end (as the stack table in ESP32.md: check with calipers).
The SuperMini's antenna is at its end away from USB, over this board's parts and tracks: Wi-Fi
range may be a little lower than with the boards in free air.

## Files

* `gen_board.py`: the whole board as a script (outline, parts, nets, labels); edit here, not in the
  .kicad_pcb.
* `make.sh`: generate → autoroute (Freerouting 1.9) → DRC → Gerbers, drill, pictures. Needs KiCad 7,
  Java and Xvfb: `FREEROUTING=/path/freerouting-1.9.0.jar ./make.sh build`.
* `ses_import.py`: puts Freerouting's routes onto the board (pcbnew's own import needs the GUI).
* `dc-picolink-stack.kicad_pcb`: the routed board, opens in KiCad 7 or later. There is no schematic:
  the nets are defined in `gen_board.py` (table above).
