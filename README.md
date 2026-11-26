# DC-PicoLink

> **Part of the DC- Dreamcast family** (branch `unified`): [DC-DreamDash](https://github.com/blaskkaffe/DC-DreamDash/tree/unified) · [DC-PicoLink](https://github.com/blaskkaffe/DC-PicoLink/tree/unified) · [DC-iBlueControlMod](https://github.com/blaskkaffe/DC-iBlueControlMod/tree/unified) · [DC-BlueRetro](https://github.com/blaskkaffe/DC-BlueRetro/tree/unified) · [DC-openMenu-Virtual-Folder-Bundle](https://github.com/blaskkaffe/DC-openMenu-Virtual-Folder-Bundle/tree/unified) · [DC-Pico2Maple](https://github.com/blaskkaffe/DC-Pico2Maple/tree/unified). They work alone and together. The shared plan, feature list, vocabulary and licenses overview are in [docs/unified/](docs/unified/README.md).

> ⚠️ **Under development. No guarantee that anything here works.**
> Use it at your own risk, on hardware you can afford to lose: wiring mistakes or firmware
> bugs can damage a Dreamcast, a modem or the boards. Tried on hardware: the RP2040 cable with
> DreamPi, and the **RP2040-Zero + ESP32-S3 SuperMini stack** on a breadboard (the boards joined
> by wires) over the serial cable: Wi-Fi, PPP, DNS, VPN, UPnP, DCNow!; Quake, Quake III, Unreal
> Tournament, Alien Front Online. Written and host-tested but **not tried on hardware yet**: the
> soldered stack, the SD slot and its files over Wi-Fi, W5500 emulation through the ESP32, the
> BBA port, the USB modem, DCNet, Netlink. Expect bugs; the checklist is
> [docs/TESTING.md](docs/TESTING.md). When you report a problem, include the first status line
> (firmware version) and the log.

An RP2040-based Dreamcast serial-port adapter: a full coder's cable (serial and serial
PPP), W5500 Ethernet emulation over USB networking, and SD card emulation (used by
openMenu for its settings and VMU backups). Works with DreamPi or a regular computer,
as a cheaper, faster alternative to separate coder's cables and W5500 adapters, and (being
built) with an ESP32-S3 instead of a computer: [hardware/ESP32.md](hardware/ESP32.md), [esp32/](esp32).

One small board on the Dreamcast's serial port shows up on the computer as one USB
device with three functions. It works out by itself which one the Dreamcast is using:

| The Dreamcast thinks it has… | Used for | On the computer |
|---|---|---|
| a **coder's cable** (UART) | dc-tool-ser / dcload-serial, the KOS console, serial PPP games (e.g. AFO through DreamPi's Netlink) | a USB serial port (`/dev/dccable` on DreamPi) |
| a **WIZnet W5500** Ethernet chip | KallistiOS homebrew and patched games with the W5500 driver | a USB network interface (`dcusb0`) the computer routes or bridges |
| an **SD card** | openMenu (settings, per-game VMU backups), other SCIF-SPI SD programs | a disk image on the host (DreamPi), or a microSD in the board's own slot (written, not tried on hardware); with the ESP32, the slot card's files over Wi-Fi |

## Status

Working on hardware with DreamPi (Raspberry Pi, Linux):

* Serial: dc-tool-ser / dcload-serial 1.0.6 uploads clean at 115200 and 1.5 Mbaud
  (1.6 Mbaud is above what the Dreamcast's SCIF can do), the KOS console, Netlink serial
  PPP. Exact Dreamcast baud rates and auto-baud. No flow control from the Dreamcast, as
  DreamPi 2.1 (games and current KallistiOS leave its RTS floating, and waiting for it
  blocked the cable: DEVLOG, Nov 2026).
* W5500: KOS networking, DreamKarts and RuneScape online, JaderFox tester, ping, Quake 3
  server list and play (with `dcusb0-masterfix`).
* Being tested: AFO over serial (drops while fetching game info), Quake 3 map images over
  serial, Unreal Tournament (needs its server's address), Dreamcast Now! for W5500.
* SD card: works with KOS's SD driver; openMenu 1.7.1 doesn't find the card yet.

On a DreamPi the W5500 Dreamcast is meant to look exactly like DreamPi's modem Dreamcast
(DreamPi is the reference; its behaviour is written down in the project's DreamPi spec):

| | DreamPi add-on (`host/dreampi`) |
|---|---|
| Dreamcast Live VPN address (T+1) | `dcusb0-vpn` |
| Own LAN address (DreamPi's, e.g. .98): Internet traffic, router port forwards | `LAN_ALIAS=auto` |
| DreamPi's DNS (community DNS + DreamPi's overrides) | `DNS=auto` |
| TTL 64, PowerSmash double-SYN filter | `dcusb0-vpn` / DreamPi's own rule |
| DreamPi's game packet helpers (dcgamespy, dc2k2, dcvoip, dcdaytona) | fed / kept correct for `dcusb0` |
| Dreamcast Now! "online" | `dcusb0-dcnow` |
| Quake 3 server list (master ignores the word `dreamcast`) | `dcusb0-masterfix` (ours) |

The ESP32 (no computer) applies the same rules itself; what is still missing is listed in
[hardware/ESP32.md](hardware/ESP32.md) and the project's DreamPi spec. Tried over the serial cable
on a breadboard build (see the warning at the top).

Other hosts (Linux PC, macOS, Windows, Android) are planned: see [docs/ROADMAP.md](docs/ROADMAP.md).

## The stack: RP2040-Zero + ESP32-S3 SuperMini (no computer needed)

Tried on a breadboard build over the serial cable; the soldered stack is next. The two boards
sit back to back; the RP2040 does everything towards the Dreamcast, the ESP32 the Internet
side. Pinout: [Wiring](#wiring-full-pinout) below, details in [hardware/ESP32.md](hardware/ESP32.md);
ESP32 details: [esp32/README.md](esp32/README.md).

| | What it does | Like DreamPi |
|---|---|---|
| Internet | home Wi-Fi, or a W5500 Ethernet module on the ESP32 as uplink | |
| Setup | own Wi-Fi network `DC-PicoLink-XXXX`, web page with status, log, settings | |
| Serial cable | modem emulation (`AT`, `ATD`), PPP server, NAT | serial PPP |
| W5500 emulation | the Dreamcast's W5500 network: DHCP, DNS, NAT | `dcusb0` |
| SD card | a microSD on the Zero's SPI1 is the Dreamcast's SD card | (dcsd image) |
| SD files | the slot card's files on the web page: copy saves to and from a computer or phone | |
| USB modem | a USB voice modem on the ESP32: dial tone, number, answer, PPP | yes |
| BBA | the Dreamcast's Broadband Adapter on the ESP32's W5500 module | |
| DNS | DreamPi's DNS list from dreamcast.online, revival DNS; over TCP when the router intercepts DNS | yes |
| DCNow! | reports, and `dreampi.local:1998` for dreamcast.online/now/configure | yes |
| Network switch | two buttons at the top of the page: DCNow! (default) or DCNet, from the next call, whatever number the game dials; `*69` still goes to DCNet while DCNow! is selected (tick box). Also `call net.select dcnow\|dcnet` in the DC API (`POST /dcapi`), `POST /dcnow`, `/dcnet`, `GET /tag`, `/openmenu/poll` | yes |
| Wi-Fi from the console | `call wifi.scan`, `get wifi.scan.*`, `set wifi.pass ...`, `call wifi.join <name or #n>` in the DC API, and the page's Search button (list, filter, tap to pick) | |
| DCNet | `*69` before the number, or every call (the switch) | yes |
| Netlink | direct dial, matchmaking, game server codes, XBAND, Capcom matching (USB modem) | yes |
| VPN | Dreamcast Live, profiles built in (okayed by Dreamcast Live); the Dreamcast gets its own VPN address; auto mode pings every server and takes the fastest | yes |
| Own home address | as DreamPi: the Dreamcast gets .98 on your network (this box .99, answers ARP for it, no NAT); 1:1 with the VPN up | yes |
| Port forwarding | DreamPi's game ports by UPnP straight to the Dreamcast's .98, nothing else; says when the provider shares your IPv4 (then the VPN is switched on) | yes (optional there) |
| Game packet rules | TTL 64, PowerSmash double SYN, Planet Ring / Daytona public address, DNAT address swaps (dnat.txt) | yes |
| Serial line queue | game (UDP) packets go ahead of downloads on the slow serial line | |
| Updates | both boards from the web page (ESP32 image; the Zero restarts into its USB drive) | |

Test before trusting it: [docs/TESTING.md](docs/TESTING.md).

## Wiring (full pinout)

The stack: an RP2040-Zero and an ESP32-S3 SuperMini, back to back. Every signal below is 3.3 V
logic. The older single-board layouts (a Pico or Pro Micro with the Dreamcast on GPIO2-7, a UART
link to the ESP32) are only for testing: [hardware/WIRING.md](hardware/WIRING.md). More detail:
[hardware/ESP32.md](hardware/ESP32.md), [hardware/SD-SLOT.md](hardware/SD-SLOT.md).

### Dreamcast side

The usual way in is a **DC SD Adapter V2** in the serial port with an SD extender / sniffer
board in its slot (the adapter only maps the port's pins to SD contacts). An internal install
solders the same signals to the serial port's pins. Names are the Dreamcast's own (its TXD is
what it sends).

| Dreamcast signal | Serial port pin | SD contact: full-size / microSD | RP2040-Zero | Needed for |
|---|---|---|---|---|
| TXD (SPI MOSI) | 5 | 2 CMD / 3 | GP10 | serial, W5500, SD |
| RXD (SPI MISO) | 4 | 7 DAT0 / 7 | GP11 | serial, W5500, SD |
| RTS (SPI CS) | 6 | 1 DAT3 / 2 | GP12 | W5500, SD (serial only reads it) |
| CTS (SPI clock) | 7 | 5 CLK / 5 | GP9 | W5500, SD (the cable reads the line) |
| CTS drive | – | – | GP13 **→ 1 kΩ →** GP9 | serial flow control; a resistor on the board, no wire to the Dreamcast |
| GND | 3, 8 | 3 + 6 VSS / 6 | GND | everything |
| 3.3 V sense (optional) | 10 | 4 VDD / 4 | GP29 **via 10 kΩ** (about 10 kΩ; 9.1 kΩ is fine) | knowing when the Dreamcast is on; never to the board's 3V3 |
| +5 V | 1 | – | not connected | see [Power](#power-and-usb) |

**Which wires are needed.** Serial alone works with TXD, RXD and GND. W5500 and the SD card use
all four signal wires (TXD, RXD, RTS, CTS), because there they are the SPI bus: so with all three
functions, wire all four. CTS is one pin on the Dreamcast that does two jobs. In serial mode it is
an input: the cable pulls it high through GP13 to say "stop sending" when its buffer is nearly
full. In W5500/SD mode the Dreamcast turns it into the SPI clock, an output. GP9 reads what is
really on the line (which is also how the cable notices SPI mode), and the 1 kΩ lets both sides
drive it without harm ([why](hardware/WIRING.md#why-the-1-kω-resistor)). Without the GP13
resistor serial still works, but the cable can no longer slow the Dreamcast down. The sense wire
is optional: without it the cable decides the Dreamcast is on when it sees TXD or RTS high.
Serial port pins 2 and 9 and SD contacts 8 and 9 (DAT1, DAT2) stay unconnected. The pin numbers come from coder's
cable guides: measure before soldering.

### The stack: which pins are joined

RP2040-Zero on top, face up; ESP32-S3 SuperMini underneath, face down; both USB sockets at
the same end; positions count from the USB end. **Solder only what the table says**, and put
insulation (Kapton) between the two boards' backs.

| Pos | Zero left ↔ SuperMini | | Zero right ↔ SuperMini | |
|---|---|---|---|---|
| 1 | 5V ↔ 5V | **solder** | GP0 ↔ GPIO43 | optional (ESP32 console TX) |
| 2 | GND ↔ GND | **solder** | GP1 ↔ GPIO44 | optional (ESP32 console RX) |
| 3 | 3V3 ↔ 3V3 | **never** (two regulators) | GP2 ↔ GPIO1 | not used (optional, strength) |
| 4 | GP29 ↔ GPIO13 | **never** (GP29 is the power sense / W5500 MISO) | GP3 ↔ GPIO2 | **solder**: HS (ESP32 ready) |
| 5 | GP28 ↔ GPIO12 | **never** (SD MISO / W5500 SCLK) | GP4 ↔ GPIO3 | **solder**: SPI MISO (ESP32 → Zero) |
| 6 | GP27 ↔ GPIO11 | **never** (GP27 free / W5500 MOSI) | GP5 ↔ GPIO4 | **solder**: SPI CS |
| 7 | GP26 ↔ GPIO10 | **never** (SD SCK / W5500 CS) | GP6 ↔ GPIO5 | **solder**: SPI clock |
| 8 | GP15 ↔ GPIO9 | **never** (SD MOSI / W5500 INT) | GP7 ↔ GPIO6 | **solder**: SPI MOSI (Zero → ESP32) |
| 9 | GP14 ↔ GPIO8 | **never** (GP14 is the SD card's CS) | GP8 ↔ GPIO7 | **solder**: DR (ESP32 has data) |

Left 4-9 are the Zero's power sense (GP29), SD slot pins (GP28, GP26, GP15, GP14) and the free GP27 over the ESP32's W5500 pins (GPIO9-13):
all used, each from the outside (the Zero's edge pads, the SuperMini's holes from its outer face), none joined.
The same pin map serves the stripboard layout ([hardware/STRIPBOARD.md](hardware/STRIPBOARD.md)): there the boards stand side by side. The Zero's bottom
edge GP9-13 is the Dreamcast (table above). GP16 is the Zero's own RGB LED (red: no ESP32,
yellow: damaged link packets, green: link up, blue: link up and the Dreamcast on).

### SD card slot (optional)

A 3.3 V microSD module or breakout on the RP2040-Zero's SPI1, never on the Dreamcast's wires.

| SD module | RP2040-Zero |
|---|---|
| CLK / SCK | GP26 |
| CMD / DI / MOSI | GP15 (Pico layout: GP27) |
| DAT0 / DO / MISO | GP28 |
| DAT3 / CS | GP14 |
| VCC / GND | 3V3 or 5V, see [SD-SLOT.md](hardware/SD-SLOT.md#which-vcc) / GND |

No card-detect pin: the cable looks for a card every 2 s.

### ESP32-S3 extras (optional)

| ESP32-S3 SuperMini | Connects to |
|---|---|
| GPIO12 | W5500 module SCLK |
| GPIO11 | W5500 module MOSI |
| GPIO13 | W5500 module MISO |
| GPIO10 | W5500 module CS (SCS) |
| GPIO9 | W5500 module INT |
| 3V3, GND | W5500 module 3.3 V, GND |
| USB-C (modem build) | a USB host for one device (a hub works too): a USB voice modem through an OTG adapter (the Dreamcast's modem joins it by a phone cord, as with a DreamPi), or a DC-PicoLink cable: the link then runs over USB, no link wires needed |
| GPIO43 / GPIO44 | console TX / RX (UART0, 115200) |

The W5500 module is the Internet uplink when a router answers DHCP on it, otherwise the
socket for the Dreamcast's Broadband Adapter.

### Power and USB

* **Today:** 5V and GND are joined, so either board's USB powers both. **Plug in one USB at a
  time** (the Zero has no diode on its USB 5 V). The Zero's USB is for updates and for a
  computer (it then takes the serial and network traffic, as with DreamPi); the ESP32's USB-C
  is its console, or in the modem build the socket for a USB modem, a USB Ethernet adapter or a
  DC-PicoLink cable (then the link runs over USB and needs no link wires; the ESP32 powers it).
* **From the Dreamcast (serial port pin 1, +5 V):** not measured yet, so not recommended. Any
  such feed goes through a Schottky diode (e.g. SS14) so a USB cable plugged in later can't push
  power back into the console. Current budget and options: [hardware/ESP32.md](hardware/ESP32.md#power).
* Never join the Dreamcast's 3.3 V (port pin 10 / SD VDD) to a board's 3V3: it goes only to the
  sense pin, through 10 kΩ.

## IP plan

The same Dreamcast settings work on the ESP32 box and on a DreamPi with this add-on. 98: the
Dreamcast's launch year in Japan, and its usual last number on the home network.

| Network | The box (or Pi) | The Dreamcast |
|---|---|---|
| Setup Wi-Fi `DC-PicoLink-XXXX` (ESP32) | 192.168.4.1: the web page (also `http://dreampi.local`) | – |
| **The Dreamcast network**: the W5500 (cable), the BBA socket (W5500 module), a PPP call without its home address | 192.168.98.1: gateway and DNS | DHCP .2–.99 (a PPP call gets .2); a fixed address at **.100** or above, mask 255.255.255.0, gateway and DNS 192.168.98.1 |
| Modem / serial PPP with its home address (default) | home network, usually .99 | home network, usually .98 (no NAT; port forwarding points there). Picked as DreamPi does: counting down from .99, the first free address is the box's end of the link, the next the Dreamcast's |
| VPN (Dreamcast Live) | the address the server gives | that address + 1 (only the server's game networks go through the tunnel) |
| DreamPi's W5500 network (`dcusb0`) | 192.168.98.1 | DHCP .10–.50, fixed .100 or above |

The Dreamcast uses one way to connect at a time, so they share one network: the box answers on
the interface the Dreamcast was last heard on. On the W5500, gateway and DNS must be set to
192.168.98.1 (0.0.0.0 only works over the modem, where PPP fills them in).

Which address where (the ESP32 box):

| Dreamcast joined by | Internet over | The Dreamcast gets |
|---|---|---|
| The cable's emulated W5500 (RP2040) | Wi-Fi or the wired uplink | 192.168.98.x (DHCP), NAT to the uplink |
| BBA into the W5500 module (BBA role) | Wi-Fi only: the module's one port is the Dreamcast's | 192.168.98.x (DHCP), NAT to Wi-Fi |
| Modem (USB) or the cable's serial port | Wi-Fi or the wired uplink | home network .98 (or 192.168.98.2 when no home address is free) |

The W5500 module picks its role by itself: if a router answers DHCP on its cable it is the way to
the Internet (the router's address, ahead of Wi-Fi), if nothing answers in 10 s it is the BBA
socket. A USB Ethernet adapter is not supported yet.

## Quick start (DreamPi)

1. **Wire it** to the Dreamcast serial port: [hardware/WIRING.md](hardware/WIRING.md)
   (SD-adapter plug build; an internal install works the same way).
2. **Flash** a Raspberry Pi Pico: hold BOOTSEL, plug in, copy
   `dc-picolink-<version>-250mhz.uf2` from the [releases](../../releases).
3. **Install the DreamPi side**: copy `dc-picolink-dreampi-setup.sh` to the Pi and run
   `sudo bash dc-picolink-dreampi-setup.sh`.
4. **Serial PPP** (Netlink): in DreamPi's `netlink_config.ini` set `port = /dev/dccable`.
5. **W5500**: the Dreamcast gets an address by DHCP (192.168.98.x). Static settings:
   IP 192.168.98.100, mask 255.255.255.0, gateway 192.168.98.1, DNS 192.168.98.1 (DreamPi's DNS;
   46.101.91.123 without DreamPi). Settings: `/etc/dcusb0.conf` (an untouched 192.168.99.x one from
   an older version is replaced by the installer with 192.168.98.x).
6. **Check**: `sudo dreampi-snapshot` prints what the DreamPi and the add-on are doing.

Logs: `journalctl -fu dcsd` (the cable's own status lines), `journalctl -u dcusb0`,
`journalctl -u dcusb0-vpn`, `journalctl -u dcusb0-dcnow`, `journalctl -u dcusb0-masterfix`.

dcload test: build dc-tool-ser on the Pi (KallistiOS `dcload-serial`, `make -C host-src/tool`),
boot a dcload-serial CDI, then `dc-tool-ser -t /dev/dccable -b 1500000 -x program.elf`.

## Repository

| Folder | |
|---|---|
| [`firmware/`](firmware) | RP2040 firmware (Pico SDK, PIO) and its host-side tests (`firmware/tests`) |
| [`host/`](host) | software for the computer side: `dreampi/` today; `linux/`, `macos/`, `windows/` planned |
| [`esp32/`](esp32) | ESP32-S3 firmware: the cable's link to Wi-Fi without a computer (under test) |
| [`hardware/`](hardware) | wiring and build notes, [SD slot](hardware/SD-SLOT.md), [ESP32](hardware/ESP32.md) |
| [`docs/`](docs) | [architecture](docs/ARCHITECTURE.md), [technical reference](docs/REFERENCE.md), [roadmap](docs/ROADMAP.md), [testing](docs/TESTING.md), [development log](docs/DEVLOG.md) |
| [`tools/`](tools) | test runner (`run-tests.sh`) and the scripts it calls, DreamPi installer builder, `dctest/` (Dreamcast test program and host tool), network-namespace tests of the DreamPi add-on, `dreampi_vpn_extract.py` (VPN profiles out of a DreamPi image, plain Python, works in a-Shell on iOS) |

## Building

Cable (RP2040), Pico SDK 2.1.1 and `gcc-arm-none-eabi`:

```sh
# Test layout, single Pico / Pro Micro (Dreamcast on GPIO2-7, ESP32 link on UART GPIO12-15)
cmake -S firmware -B build -DPICO_BOARD=pico -DDC_SYS_KHZ=250000 && make -C build
# The stack: RP2040-Zero + ESP32-S3 SuperMini (Dreamcast on GP9-14, ESP32 link over SPI)
cmake -S firmware -B build-zero -DPICO_BOARD=pico -DDC_SYS_KHZ=250000 -DDC_BOARD=zero-stack && make -C build-zero
```

The UF2 is `build*/dc_usb_serial.uf2`: hold BOOT (BOOTSEL), plug in, copy it to the drive.
`-DPICO_BOARD=pico` is right for the RP2040-Zero and Pro Micro too (generic 2 MB flash).

ESP32-S3, ESP-IDF 5.4 (details in [esp32/README.md](esp32/README.md)):

```sh
cd esp32
idf.py set-target esp32s3 && idf.py build                                             # UART link (test layout)
idf.py -B build-zero -D SDKCONFIG=build-zero/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.zero-stack" build          # SPI link (stack)
idf.py -B build-modem -D SDKCONFIG=build-modem/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.zero-stack;sdkconfig.modem" build   # + USB modem port
```

Tests (host, no hardware): `tools/run-tests.sh` runs the C model tests, including the SPI
link's timing simulation; `tools/run-tests.sh build` adds the PIO simulations for both
Dreamcast pin layouts. More options: [docs/REFERENCE.md#build](docs/REFERENCE.md#build).
CI builds every push; tagged versions (`v*`) become releases with the UF2 files and the
DreamPi installer.

Part of a family of `DC-` Dreamcast projects.

`esp32/probe` is a test firmware for one question: can the ESP32-S3 do the RP2040's job too?
Measured: no (Wi-Fi pauses the other core for ~40 ns). See [hardware/ESP32-ONLY.md](hardware/ESP32-ONLY.md).

## Credits and licenses

MIT, see [LICENSE](LICENSE). Built on the work of others:

* [DreamPi](https://github.com/Kazade/dreampi) (Kazade and contributors, MIT): the reference for
  everything the ESP32 does for a modem Dreamcast.
* [Netlink](https://github.com/eaudunord/Netlink) (eaudunord): the Netlink tunnel ported to
  `esp32/main/netlink.c`.
* DCNet by flyinghead (`dcnet.rpi`, as used by DreamPi): the access-point protocol in `dcnet.c`.
* Dreamcast Live (Shuouma): the VPN servers and the client profiles in `esp32/vpn/`, used here
  with their okay.
* Espressif `mdns` and `usb_host_cdc_acm` components (Apache-2.0), vendored in `esp32/components/`.
* **Raspberry Pi Pico SDK 2.1.1** (BSD-3-Clause, Raspberry Pi (Trading) Ltd) and its **TinyUSB** (MIT): the
  firmware is built against them, not copied; `firmware/pico_sdk_import.cmake` is the SDK's own file and keeps its header.
* **KallistiOS**: the Dreamcast test program `tools/dctest` is built against it in CI (fetched at build time;
  BSD-style license, see its repository).

The documents in [docs/unified/](docs/unified/README.md) are covered by the same MIT license. For the licenses of
the other repositories in the family see the table there.
