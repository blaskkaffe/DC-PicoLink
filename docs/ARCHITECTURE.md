# Architecture

## Goal

A universal Dreamcast-to-computer adapter on the Dreamcast's serial port, usable in
some way from any host platform. It replaces a separate coder's cable, a W5500 adapter
and a serial SD card adapter, and works together with DreamPi or without it.

## The three functions

**Coder's cable.** A Dreamcast UART tuned for Dreamcast traffic: exact Dreamcast baud
rates (the same divider arithmetic dcload uses, so no mismatch up to 1562500), auto-baud
that follows the rate the Dreamcast really sends, FTDI-style 3x sampling, hardware flow
control. Covers dc-tool-ser/dcload-serial, the KOS console, and serial PPP (Netlink or
any PPP server on the host).

**W5500.** The Dreamcast sees a WIZnet W5500 on SCIF-SPI (socket 0, MACRAW). The host
sees a standard USB network interface (CDC-NCM) carrying the Ethernet frames, which it
routes or bridges to its own network like any USB Ethernet adapter.

**SD card.** The Dreamcast sees an SDHC card in SPI mode. Any SCIF-SPI SD program can
use it; openMenu 1.7.1 is the main user (default settings, per-game folders with a VMU
backup that is copied to the controller's VMU at launch and back afterwards).
* Today: blocks come from a disk image on the host (`dcsd` daemon, USB vendor interface).
* Planned: a physical SD slot on the board's own SPI pins, with the RP2040 emulating the
  card protocol towards the Dreamcast and using the card as storage. No host software
  needed; the card can be read in any PC, and the board shows it as a USB drive while
  the Dreamcast is off.

## Mode selection

The Dreamcast uses the same four signals (TXD, RXD, RTS, CTS) either as a UART or as an
SPI bus (CTS = clock, TXD = MOSI, RXD = MISO, RTS = chip select). Only one is possible
at a time.

* **Serial or SPI:** GPIO6 drives the Dreamcast's CTS through 1 kΩ and GPIO2 reads it
  back. When the Dreamcast turns CTS into its SPI clock, GPIO2 stops following GPIO6
  (16 clock edges within 1 ms → SPI). When the line is released for ~200 ms → serial.
* **SD or W5500 within SPI:** both share one chip select, which no real hardware
  combination does; only the emulation makes it possible, so the rule is simple:
  * SD is recognised (a frame starting with ≥ 2 × 0xFF and a command with a valid CRC7)
    until a program opens the W5500 socket. From then on the session is network only,
    until the next program (a new SPI session) or power-off. Games don't use the SD
    card, and W5500 traffic only starts long after boot.
  * A manual override (`dcsd-mode sd|w5500|spi|serial|auto`) forces a mode.

Every Dreamcast-facing pin stays high-impedance until the Dreamcast is up (some
consoles hang on boot otherwise). "Up" is the Dreamcast driving TXD/RTS high, or, with
the optional sense wire, its 3.3 V supply (found automatically).

## Firmware

RP2040, Pico SDK, runs from RAM (no flash-cache stalls), 250 MHz recommended.

* **core0:** TinyUSB (composite device: CDC-ACM serial, vendor interface, CDC-NCM), the
  UART bridge with DMA, auto-baud, the SD card's host link, status lines.
* **core1:** the bus watcher and everything timing-critical. In SPI mode it runs with
  interrupts off: a W5500 read must be answered within ~250 ns of the header, and KOS
  bit-bangs SPI with clock-low phases of ~40 ns.
* **PIO:** pio0: UART TX/RX, a baud meter, the SD shifter. pio1: the W5500 shifter (finds
  frame ends itself; MISO changes right after each rising edge, because KOS samples RXD
  late), and a logic-analyzer capture for diagnostics.
* Host-testable models: `w5500_model.h`, `sd_model.h`, `sense_model.h`, `baud_model.h`
  (tests in `firmware/tests`).
* Builds: release (diagnostics off) and debug (`DC_DIAG=1`: PPP frame checker, logic
  capture, per-frame lists, late-read detail).

## Host side

| Host | Serial | W5500 network | SD card |
|---|---|---|---|
| DreamPi (Raspberry Pi) | `/dev/dccable`, Netlink | `dcusb0`: DHCP, NAT, Dreamcast Live VPN address (`dcusb0-vpn`) | `dcsd` (until the SD slot) |
| Linux PC | works (CDC-ACM) | planned: NetworkManager/systemd-networkd sharing | — (SD slot) |
| macOS | works | planned: Internet Sharing | — (SD slot) |
| Windows | works | planned: built-in NCM driver (Windows 11) + Internet Connection Sharing | — (SD slot) |
| Android | possible | possible (USB tethering stack) | — |
| iOS | no USB serial / network access for apps | — | — |

DreamPi additions are kept as a separate add-on (own files, own services, own iptables
chains, no edits to DreamPi's scripts) so they can be merged into DreamPi later.

## Constraints worth knowing

* The serial-port SD adapter's two slots and this board share the same four wires. A
  real card in the adapter would see W5500 traffic on its chip select; that's why the SD
  card moves to the board's own SPI pins instead.
* The Dreamcast modem talks to the console over the G2 bus; its data only exists in
  decoded form there. Replacing a USB modem on the host therefore isn't a GPIO job: the
  internal-mod plan keeps a regular modem (see ROADMAP.md).
* USB full speed limits the network to ~1 MB/s; the Dreamcast's SCIF-SPI is slower
  than that for reads (~550 KB/s).
