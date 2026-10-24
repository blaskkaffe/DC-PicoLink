# Wiring the RP2040 to the Dreamcast

The RP2040 connects to the Dreamcast's serial port through a **DC SD Adapter V2**.
The adapter has no electronics of its own on these lines: it just maps the serial
port's signals to the contacts of its SD slot. So you wire the RP2040 to the SD
slot's contacts. The easiest way is an **SD-card extender / "SD sniffer" board**
pushed into the slot, which gives you solder pads. You can also solder to a cut-down
SD card or to the slot itself.

## Connections

| RP2040 | Pico pin | SD contact (full-size) | Dreamcast signal | Direction |
|--------|----------|------------------------|------------------|-----------|
| GPIO2  | 4        | 5 – CLK                | CTS (SPI clock)  | DC → RP2040 (sense) |
| GPIO3  | 5        | 2 – CMD                | TXD (SPI MOSI)   | DC → RP2040 |
| GPIO4  | 6        | 7 – DAT0               | RXD (SPI MISO)   | RP2040 → DC |
| GPIO5  | 7        | 1 – DAT3 / CS          | RTS (SPI CS)     | DC → RP2040 |
| GPIO6  | 9        | – (1 kΩ to GPIO2)      | CTS drive        | RP2040 → DC, through 1 kΩ |
| GND    | 3 or 8   | 3 and/or 6 – VSS       | GND              | |
| GPIO7  | 10       | 4 – VDD, through 10 kΩ | DC 3.3 V sense   | optional, see below |

Leave SD contacts 8 (DAT1) and 9 (DAT2) unconnected.

"Pico pin" is the physical pin number on a Raspberry Pi Pico. On other RP2040
boards (RP2040-Zero, XIAO RP2040, ...) go by the GPIO numbers printed on the board.

If you use a **microSD** adapter or breakout instead of a full-size SD one, the
contact numbers differ (same signals):

| Signal | full-size SD | microSD |
|--------|--------------|---------|
| DAT3 / CS | 1 | 2 |
| CMD    | 2 | 3 |
| VSS    | 3, 6 | 6 |
| VDD    | 4 | 4 |
| CLK    | 5 | 5 |
| DAT0   | 7 | 7 |

```
            RP2040                                SD slot of the DC SD Adapter V2
         +----------+
         |    GPIO2 |----------------+---------------- 5  CLK   (DC CTS)
         |          |                |
         |    GPIO6 |----[ 1 kΩ ]----+
         |          |
         |    GPIO3 |--------------------------------- 2  CMD   (DC TXD)
         |    GPIO4 |--------------------------------- 7  DAT0  (DC RXD)
         |    GPIO5 |--------------------------------- 1  DAT3  (DC RTS)
         |      GND |--------------------------------- 3/6 VSS  (GND)
         |          |
         |    GPIO7 |----[ 10 kΩ ]-------------------- 4  VDD   (DC 3.3 V)   optional
         +----------+
              |
             USB  -> Raspberry Pi (DreamPi)
```

## Why the 1 kΩ resistor

The Dreamcast's CTS line is an input in serial mode, where the RP2040 drives it
(RTS → CTS flow control) from GPIO6. But KallistiOS's SCIF-SPI driver (W5500 and SD
card) turns that same pin into an **output**, its SPI clock. The 1 kΩ resistor means
both sides can drive the line at once without damage: at most 3.3 mA flows. GPIO2
sits directly on the line and reads what is really on it. That's also how the
firmware notices SPI mode: GPIO2 stops following GPIO6.

Don't replace it with a wire or leave it out. Any value from 680 Ω to 2.2 kΩ works.

## The optional 3.3 V sense wire (GPIO7, 10 kΩ)

Optional: the same firmware works with or without it and finds the wire by itself. Without the wire the cable decides that the Dreamcast is
on when it drives its TXD or RTS high, which happens once dcload or KOS has set up
the serial port.

With the wire, the firmware notices the Dreamcast's 3.3 V the first time it is high
for 100 ms and uses it from then on, until the RP2040 loses power:
* the pins go live 700 ms after the Dreamcast powers on, and are released within
  20 ms of it powering off (instead of after 30 s of a silent port);
* the log shows `power sense GPIO7: found (DC 3.3 V is high) ...`, and the first
  line after connecting ends in `power sense wire in use`.

If the wire comes loose while the Dreamcast is on, the cable notices within 2 s
(TXD/RTS high, GPIO7 low) and goes back to TXD/RTS sensing.

The 10 kΩ limits the current into GPIO7 to well under 1 mA, even when the RP2040
itself is unpowered.

## Don'ts

* **Never connect SD VDD (DC 3.3 V) to the RP2040's 3V3, VSYS or VBUS.** The RP2040
  is powered over USB from the Pi. Only GND is shared.
* Don't add pull-up resistors on the data lines. The firmware keeps every
  Dreamcast-facing pin high-impedance until the Dreamcast is up, because some consoles
  hang on a black screen if they see voltage on the serial pins during boot.
* Keep the wires short (under ~10 cm) with a solid ground. The SPI clock has
  ~40 ns pulses, and long or loose wires cause misread bits. Twisting CLK with a
  ground wire helps.

## Checking the wiring

With the cable on the Pi and dcsd running (`journalctl -fu dcsd`):
* while the Dreamcast is off, the cable reports every 10 s
  `offline: waiting for the DC ... pins CLK=.. TXD=.. RXD=.. RTS=.. CTSout=..`;
* start dcload-serial or any KOS program: TXD and RTS go to 1 and the log shows
  `mode: offline -> serial`;
* a W5500 program shows `mode: serial -> SPI (SD/W5500)`, then frame counts and
  `VERSIONR` reads. "short/aborted frames" that keep climbing while a program is
  using the network point to a loose CLK or CS wire.
