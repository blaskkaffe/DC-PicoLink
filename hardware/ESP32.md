# Wiring the ESP32-S3 (no DreamPi needed)

> ⚠️ Under development; the soldered stack is not tested on hardware yet (the ESP32 side has run on a breadboard build, see ../esp32/README.md); no guarantee that it works. Check the
> wiring twice before powering up (3V3 pins must not be joined), test with ../docs/TESTING.md.

The cable (RP2040) talks to an **ESP32-S3**, which gives the Dreamcast its internet
connection over Wi-Fi or a W5500 Ethernet module on the ESP32. The same cable firmware still works
with a Raspberry Pi or a PC over USB; see "Who gets the traffic" below. Two ways to connect
the boards:

* **UART link** (any RP2040 board with the Pico wiring, e.g. Pro Micro or Pico + any
  ESP32-S3): four wires, 3 Mbaud. Enough for serial; the W5500 path tops out around 300 KB/s.
* **SPI link, the RP2040-Zero + ESP32-S3 SuperMini stack** (`-DDC_BOARD=zero-stack`): the two
  boards soldered back to back, ~1.4 MB/s each way at once, enough for the Dreamcast's full
  W5500 speed (~1000 KB/s out, ~550 KB/s in). See [the stack](#the-rp2040-zero--esp32-s3-supermini-stack-spi-link).

**Status: the SPI link works on a breadboard build (the boards joined by wires; self-test
PASS, serial games online). The soldered stack, the W5500 emulation through the ESP32, the
W5500 module and the USB modem are host-tested only (timing simulation of the SPI link, PIO
simulations at both pin layouts).**

## UART link (Pico wiring)

| RP2040 (cable) | | ESP32-S3 | Signal |
|---|---|---|---|
| GPIO12 | → | GPIO6 | cable TX → ESP32 RX |
| GPIO13 | ← | GPIO5 | ESP32 TX → cable RX |
| GPIO14 | ← | GPIO7 | ESP32 RTS → cable CTS |
| GPIO15 | → | GPIO8 | cable RTS → ESP32 CTS |
| GND | — | GND | |

All four are edge pins on the RP2040-Zero. The ESP32 pins can be changed in
`idf.py menuconfig` → *DC-PicoLink ESP32*; they avoid the ESP32-S3's strapping pins
(0, 3, 45, 46). The Dreamcast side of the cable is wired as in [WIRING.md](WIRING.md).

Link: 3 Mbaud, 8N1, RTS/CTS. Keep the wires short (under ~15 cm) with a solid ground.

## The RP2040-Zero + ESP32-S3 SuperMini stack (SPI link)

The RP2040-Zero on top, face up; the ESP32-S3 SuperMini underneath, face down; both USB
ports at the same end. The Dreamcast ribbon leaves at the far end, between the boards.
Positions count from the USB end. Only pins that sit directly over each other are joined;
a pin marked "solder" goes through both boards. **Check the alignment with calipers before
soldering** (both boards are 18 mm wide on a 2.54 mm pitch, but the first pin's distance from
the USB end may differ slightly).

| Pos | Zero left ↔ SuperMini | Use | Zero right ↔ SuperMini | Use |
|---|---|---|---|---|
| 1 | 5V ↔ 5V | **solder** | GP0 ↔ GPIO43 | optional (ESP32 console TX; the Zero keeps GP0 a plain input) |
| 2 | GND ↔ GND | **solder** | GP1 ↔ GPIO44 | optional (ESP32 console RX; the Zero keeps GP1 a plain input) |
| 3 | 3V3 ↔ 3V3 | **never** (two regulators) | GP2 ↔ GPIO1 | **solder**: DR, ESP32 has data |
| 4 | GP29 ↔ GPIO13 | **never**: SD CS (out) / W5500 MISO (in) | GP3 ↔ GPIO2 | **solder**: HS, ESP32 ready |
| 5 | GP28 ↔ GPIO12 | **never**: SD MISO (in) / W5500 SCK (out) | GP4 ↔ GPIO3 | **solder**: SPI MISO (ESP32 → cable) |
| 6 | GP27 ↔ GPIO11 | **never**: SD MOSI (out) / W5500 MOSI (out): two drivers | GP5 ↔ GPIO4 | **solder**: SPI CS |
| 7 | GP26 ↔ GPIO10 | **never**: SD SCK (out) / W5500 CS (out): two drivers | GP6 ↔ GPIO5 | **solder**: SPI clock |
| 8 | GP15 ↔ GPIO9 | the strength pin (opposite corner to 5V): safe, both unused (the Zero keeps GP15 a plain input without pulls) | GP7 ↔ GPIO6 | **solder**: SPI MOSI (cable → ESP32) |
| 9 | GP14 ↔ GPIO8 | **never**: GP14 is the Dreamcast side's power-sense input (bottom-edge corner) | GP8 ↔ GPIO7 | not joined: GPIO7 is the W5500 module's INT, wired from outside |

Both boards have nine pins per side; the Zero's bottom edge (GP9-14) is the Dreamcast's.
Minimum: 5V, GND, the six link pins (right, 3-8), plus GP15 ↔ GPIO9 for strength. With a pin
header through both boards, **every position that has a header pin is joined**: fit pins only
where the table says solder (or pull/clip the others), never at 3-7 on the left.
Corners: 5V/GND top left, GP0/GP1 top right, GP15 bottom left, the SPI pins bottom right.

**Insulate between the boards.** The two backs face each other: the SuperMini's back has about
20 bare pads (GPIO14-21, 33-48 incl. the strapping pins 45/46, B+/B-) and the USB socket's
shield tabs; the Zero's back has the RP2040, the crystal and a column of test pads. Put Kapton
tape (or a thin insulating sheet) on one back, and keep the header's spacer between the boards
(or a gap of about 1 mm) so nothing touches.
None of the soldered ESP32 pins is a strapping pin except GPIO3 (JTAG source), which only
matters with a burned eFuse (not on these boards); the Zero's pull-up on GP4 keeps it high.
Not joined pins are wired from the outside:
the Zero's castellated edge pads, the SuperMini's holes from its outer face.

The Zero's bottom edge, the Dreamcast ([WIRING.md](WIRING.md), the same signals as GPIO2-7
there, moved to GPIO9-14):

| Zero | Dreamcast (via the SD adapter) |
|---|---|
| GP9 | DC CTS (sense / SPI clock) |
| GP10 | DC TXD → cable RX |
| GP11 | DC RXD ← cable TX |
| GP12 | DC RTS (SPI CS) |
| GP13 | ---[1k]--- GP9 (the cable's RTS out) |
| GP14 | optional: 10k to the Dreamcast's 3.3 V (sense wire) |

**Wired Ethernet (optional, in the firmware, not yet tried on hardware):** a W5500 module on
the ESP32's own pins, all on the row next to 3V3 and GND:

| ESP32-S3 | W5500 module |
|---|---|
| 3V3, GND | 3.3V, GND |
| GPIO12 | SCLK |
| GPIO11 | MOSI |
| GPIO13 | MISO |
| GPIO10 | CS (SCS) |
| GPIO9 | INT (zero-stack build: GPIO7, since GPIO9 is the soldered strength pin) |

The firmware finds the module by itself ("wired port: W5500 module found", or "no W5500 module
found ... off"). With a cable plugged in, the port is the **uplink** when a router answers DHCP
(the wired network then is the way out, ahead of Wi-Fi), or the socket for the **Dreamcast's
broadband adapter** when nobody answers within 10 s (the Dreamcast gets 192.168.97.x, DNS and
NAT like on the cable's own network). In the stack GPIO10-13 sit over the Zero's GP26-29 (not joined, wired from the
outside), GPIO9 under GP15 (the soldered strength pin, unused), and INT goes to GPIO7 (under the
Zero's unused GP8, not joined).

SD slot: on the Zero's SPI1 (SCK GP26, MOSI GP27, MISO GP28, CS GP29), see [SD-SLOT.md](SD-SLOT.md).

**The link:** the cable is the SPI master (SPI0, ~15.6 MHz at 250 MHz, mode 1), the ESP32 the
slave (SPI3). Every transaction is 1024 bytes both ways; the packets inside are the same as on
the UART link. HS tells the cable that the ESP32 has a transaction loaded (the cable never
clocks one into an ESP32 that isn't ready), DR that the ESP32 has data. Details and the
reasoning: `firmware/link_spi_model.h`; the timing simulation in `firmware/tests/test_link_spi.c`.

**Power:** 5V and GND are shared, so either board's USB powers both. **Plug in only one USB
port at a time** (the RP2040-Zero has no diode on its USB 5 V). Power from the Dreamcast
(serial pin 1, +5 V) goes through a Schottky diode (e.g. SS14) into the 5V rail, pins 3 and 8
to GND; the current budget is not measured yet (see Power below).

**Firmware:** cable `-DDC_BOARD=zero-stack`; ESP32 with `sdkconfig.zero-stack` (link: SPI; pins
MOSI 6, MISO 3, SCLK 5, CS 4, HS 2, DR 1). Build commands: [../README.md#building](../README.md#building).

### Self-test: the first thing to look at

Every time the link comes up, the cable sends 2000 numbered test packets (8 to 1508 bytes)
to the ESP32, which sends each one straight back, and the cable checks every byte. It takes
a second or two and ends in one status line, on the cable's USB serial port and on the
ESP32's web page:

    esp32 link self-test: PASS, 2000 packets of 8-1508 bytes went there and back, ... KB/s each way

`FAIL` comes with the count of damaged and lost packets. Damaged packets are bit errors on
the six link wires (try `-DLINK_SPI_HZ=10000000`); nothing coming back at all means a wire is
missing or the ESP32 has an older firmware. No Dreamcast and no computer network is needed:
power the stack from either USB port and read the line. Expect roughly 1000 KB/s or more on
the SPI link (about 300 on the UART link).

### Testing the stack without a Dreamcast

1. Flash the cable with the **USB bridge test build** (`-DDC_BOARD=zero-stack -DDC_USB_BRIDGE=1`)
   and the ESP32 with the zero-stack configuration.
2. Plug only the Zero into a computer (it powers the ESP32 through the stack).
3. Join **DC-PicoLink-XXXX** (password `dreamcast`), open http://192.168.98.1: expect "link: up"
   and the cable's first line with `[TEST BUILD: USB network bridged to the ESP32]` and
   `esp32 link: up (SPI, Hz 15625000)`.
4. Save your home Wi-Fi on the page and turn **DCNow! off** (or the computer shows up as a
   Dreamcast online).
5. The computer gets a USB network adapter with an address 192.168.99.x from the ESP32.
   `ping 192.168.99.1`, then e.g. `curl --interface <adapter> http://example.com` (give the
   adapter a low priority so the computer doesn't send everything through it). A large
   download through it is a good soak test.
6. Every 10 s the cable reports `esp32 link (SPI): ... transactions, ... probes, ... timeouts`
   and, when anything goes wrong, bad checksums. **Bad checksums mean bit errors on the wires**:
   rebuild the cable with a lower clock (`-DLINK_SPI_HZ=10000000`) and
   report it. Probes or timeouts should stay at 0.

## Power

For now, power both boards over their own USB ports (a phone charger is fine), with GND
connected between them. Powering everything from the Dreamcast's serial port (+5 V on pin 1)
is planned but **not measured yet**: don't do it until the current budget is known. When it
is done, the 5 V goes through a Schottky diode so a USB cable plugged in later can't feed the
console, and the Dreamcast's 3.3 V (SD VDD) stays connected only to the optional sense wire.

## Who gets the traffic

A computer on USB wins, so a Pi or PC keeps working exactly as before:

* **Serial** (coder's cable, serial PPP) goes to the ESP32 while no program has the cable's
  USB serial port open. When DreamPi or dc-tool opens it, serial goes to USB.
* **W5500 network frames** go to the ESP32 while the cable is not mounted on a USB host.
* **Status lines** go to both (the Pi's `journalctl -u dcsd` and the ESP32's web page).

The cable logs `esp32 link: up ...` with where each kind of traffic goes, and
`esp32 link: down` after 3 s without a packet from the ESP32.

## First test

1. Flash the cable with this firmware (UF2) and the ESP32 (see [../esp32/README.md](../esp32/README.md)).
2. Join the Wi-Fi network **DC-PicoLink-XXXX** (password `dreamcast`), open http://192.168.98.1.
3. **Expect:** "link: up" at the top, and under *Cable* the lines `hello.kind cable`,
   `hello.name DC-PicoLink`, `hello.version <git describe>`, `link.mode ...`.
   *Status lines* shows the cable's first line (`DC-PicoLink firmware ... ready`).
4. Start a serial program on the Dreamcast (dcload-serial, a game that tries serial PPP):
   *Serial port* shows its bytes in orange; *Send* sends text back (e.g. `AT` → the game
   gets `AT\r`).
5. Under *dc1 to the cable* try `set link.serial.baud 115200`, then `get link.*`.
6. Plug the cable into a Pi/PC with DreamPi running: serial and network go to USB again.

## Second test: the Internet on the Dreamcast (E-3)

1. Save your home Wi-Fi on the page. Use the *Internet for the Dreamcast (serial)* card: both
   boxes ticked.
2. On the Dreamcast start a program that dials with the serial cable as its modem (a game's
   online setup, any serial-PPP test). Any phone number works, no login is checked.
3. **Expect** on the page: *Status lines* `Dreamcast dialled; starting PPP`, then
   `PPP is up: Dreamcast 10.64.0.2, DNS and gateway 10.64.0.1`; the card says `online`, DNS
   questions count up and `last:` shows the newest name, `DCNow!: session started` and
   `DCNow!: domain sent: ...`. The Dreamcast shows up on the DCNow! page after about 15 s.
4. If the call does not come up, copy the *Status lines* and the *Serial port* box (it shows the
   AT conversation until PPP starts) into your report.

## Third test: the Internet over the cable's network (E-4)

1. The cable must not be mounted by a computer (no USB host) and the ESP32 must be linked (page: link up).
2. Start a Dreamcast program that uses the network adapter driver (DHCP). Or set a static address:
   IP 192.168.99.60, mask 255.255.255.0, gateway and DNS 192.168.99.1.
   Gateway and DNS must be 192.168.99.1 here: 0.0.0.0 works over the modem (PPP fills them in)
   but not on this network (the DNS answers are thrown away, nothing leaves the network).
3. **Expect** in *Status lines*: `Ethernet: the Dreamcast is sending`, then
   `Ethernet: the Dreamcast got 192.168.99.x by DHCP`. In the card: *cable network (W5500)* counts
   frames in and out, `DNS:` counts questions, `DCNow!: ... sent` goes up every 15 s.
4. Try a web page or a game server. If frames come in but nothing goes out (or reverse), send the card's text.

## Fourth test: the VPN (E-7)

Needs a firmware built with the profiles in `esp32/vpn/` (the image you were given has them; keep it private).

1. Wi-Fi joined (page: Wi-Fi up). Tick *VPN (Dreamcast Live)* in the card and Save.
2. **Expect** in *Status lines* within about 10 s: `VPN: connecting to <server>`, then
   `VPN: connected to <server>, tunnel address 10.x.x.x`, then `VPN: tunnel works, N ms to the server's gateway`.
   The card's *VPN:* line shows the same, how long it has been up and how often it connected/failed.
3. If it says `cannot find <server>`: DNS/Wi-Fi. `server does not answer`: UDP 9091 blocked or the server is down;
   the next server in `server.list` is tried. `TLS handshake: ... (certificate not accepted)`: the profile or the
   server's certificate changed. Send me the status lines.
4. Leave it up for an hour or two: the server renegotiates keys (`VPN server renegotiates`, `VPN now sends with key N`)
   and the tunnel should stay up.


## Testing from a computer (no Dreamcast)

While no Dreamcast is on (the cable says "offline") and the ESP32 link is up, the Zero's USB port is
bridged to the ESP32: the serial port (open it with `screen`, then `ATD...` / PPP like a Dreamcast
would) goes to the ESP32's modem and PPP, and the USB network interface gets the ESP32's Dreamcast
network (DHCP 192.168.99.x, DNS, NAT out through Wi-Fi). The moment a Dreamcast is on, the usual
routes apply again. `link.route` in the cable's hello shows which one is active.

## The cable over USB (instead of link wires)

In the modem build the ESP32's USB-C socket also takes a DC-PicoLink cable: plug the Zero's (or
a Pico's) USB into it with a USB cable or an OTG adapter, and the link runs over USB with the
same packets as over SPI or UART. No link wires, any pin layout on the cable. The ESP32 supplies
the cable's 5 V over the USB cable, so power the ESP32 board well (its own USB is now the host
socket: feed its 5V pin). Log lines: `USB: DC-PicoLink cable plugged in; the link runs over USB`
on the ESP32, `esp32 link: over USB` on the cable. One device on the socket at a time (a modem
or a cable); a hub in between works.

## USB modem on the ESP32's USB socket

Build with `sdkconfig.modem` (image `...-modem-merged.bin`). The Dreamcast's own modem is joined
to a USB voice modem by a phone cord (plus a line voltage inducer if your Dreamcast needs one),
as with a DreamPi; the USB modem plugs into the ESP32's USB-C socket through an OTG adapter.

- VPN: the Dreamcast Live profiles are built in (`esp32/vpn/`); the VPN is on by default.
- The modem must be a USB CDC-ACM voice modem (the Conexant-based ones used with DreamPi).
- The socket has to give the modem 5 V: feed the stack through the 5V pin and check that 5 V
  appears on the socket (some boards have a diode in the way; bridge it or power the modem
  through a Y-cable).
- What happens: dial tone, the first dialled key is heard, 8 s later ATA, then PPP: the same
  PPP server, DNS, DCNow! and packet rules as for the serial cable. The page shows a
  "USB modem:" line, and the modem's answers appear in the serial monitor.
- The USB socket is no longer the ESP32's console in this build (console: UART0, GPIO43/44).
  The page's log is unchanged. Updates go through the page (below); over USB: hold BOOT while
  plugging into the computer.

## Updating a finished (soldered, closed) stack

On the page, *Firmware update*:
- **ESP32**: choose `dc-picolink-esp32-...-merged.bin` (or the app-only `dc_picolink_esp32.bin`)
  and *Update*. It is written to the other app slot and the box restarts into it. The new image
  is kept once Wi-Fi or the cable link has worked for 30 s; if it crashes or restarts before
  that, the bootloader goes back to the old one by itself.
- **Zero**: *Restart into USB update mode*, then plug the Zero's USB into a computer and copy the
  `.uf2` to the `RPI-RP2` drive (no BOOTSEL button needed). Any API client can do the same with
  `dc1` / `call link.update`.

The first image with this flash layout (two app slots, from commit 2026-11-19) has to go over
USB once, as the merged image. The merged image over USB **erases the saved settings** (Wi-Fi,
DCNow! ID, switches): the box starts as new. Uploaded VPN profiles have their own partition and
stay. Updates from the page keep everything.

## DCNet

Like DreamPi (netlink.py + dcnet.rpi): a call goes to flyinghead's DCNet instead of our own PPP
server when the number starts with `*69` (or `*69` is dialled just before it), or for every call
if the page says so (Internet for the Dreamcast > DCNet). The box finds the closest access point
(UDP 7655 at dcnet.flyca.st) and passes the modem bytes over TCP 7654 (7656 for the Power Smash /
VOOT numbers 0053600100xx). Dreamcast ISP settings for DCNet: any user name, password `password`.
Works for the USB modem and for the serial cable (`ATDT*69...`).

## Compared with a DreamPi (Kazade/dreampi, Sept 2026)

| DreamPi | here |
|---|---|
| dial tone, number ends after 2 s without a key, answer 8 s later (ATZ0, AT&F0, ATE0W2, ATA) | same |
| dnsmasq with dreampi_dns.conf from dreamcast.online, upstream 46.101.91.123 / 209.50.50.129 | same; if the router intercepts DNS (UDP 53), the questions go over TCP |
| TTL 64 for the Dreamcast's packets, double-SYN drop for ports 3200-3205 | same |
| DCNow! reports, config service on port 1998 (dreamcast.online/now/configure) | same, as `dreampi.local` |
| DCNet with `*69` | same |
| VPN (Dreamcast Live): the Dreamcast gets the tunnel address + 1, inbound limited to ICMP, TCP 65432/65433, UDP 20001/20002 | same; profiles built in (or uploaded on the page) |
| VPN server: one ping to each in server.list, the fastest is used | same (no answer from any: list order, shumania first); the page can pin one |
| dcnatrules: the address swaps in dnat.txt (old game servers to revived ones) | same, fetched at start and kept in flash |
| UPnP port forwarding (only with `--enable-port-forwarding`, off by default) | same port list, on by default (page switch); plus the Netlink ports, and the box's own NAT forwards the game ports on to the Dreamcast |
| Netlink: direct dial, matchmaking, game server codes (netlink_config.ini), XBAND server and calls, Capcom matching | same, over the USB modem; no SIP ring on XBAND calls |
