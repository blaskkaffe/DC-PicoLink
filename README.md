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
| DCNet | `*69` before the number, or every call | yes |
| Netlink | direct dial, matchmaking, game server codes, XBAND, Capcom matching (USB modem) | yes |
| VPN | Dreamcast Live, profiles built in (okayed by Dreamcast Live); the Dreamcast gets its own VPN address; auto mode pings every server and takes the fastest | yes |
| Own home address | as DreamPi: the Dreamcast gets .98 on your network (this box .99, answers ARP for it, no NAT); 1:1 with the VPN up | yes |
| Port forwarding | DreamPi's game ports by UPnP straight to the Dreamcast's .98, nothing else; says when the provider shares your IPv4 (then the VPN is switched on) | yes (optional there) |
| Game packet rules | TTL 64, PowerSmash double SYN, Planet Ring / Daytona public address, DNAT address swaps (dnat.txt) | yes |
| Serial line queue | game (UDP) packets go ahead of downloads on the slow serial line | |
| Updates | both boards from the web page (ESP32 image; the Zero restarts into its USB drive) | |

Test before trusting it: [docs/TESTING.md](docs/TESTING.md).

## Wiring (full pinout)

Two builds share one firmware: the **Pico wiring** (a Raspberry Pi Pico or Pro Micro RP2040,
Dreamcast on GPIO2-7, for DreamPi or a computer over USB) and the **stack** (RP2040-Zero +
ESP32-S3 SuperMini, Dreamcast on GP9-14, no computer). Every signal below is 3.3 V logic.
More detail: [hardware/WIRING.md](hardware/WIRING.md), [hardware/ESP32.md](hardware/ESP32.md),
[hardware/SD-SLOT.md](hardware/SD-SLOT.md).

### Dreamcast side (both builds)

The usual way in is a **DC SD Adapter V2** in the serial port with an SD extender / sniffer
board in its slot (the adapter only maps the port's pins to SD contacts). An internal install
solders the same signals to the serial port's pins. Names are the Dreamcast's own (its TXD is
what it sends).

| Dreamcast signal | Serial port pin | SD contact: full-size / microSD | Pico wiring: GPIO (Pico pin) | Stack: RP2040-Zero | |
|---|---|---|---|---|---|
| CTS (SPI clock) | 7 | 5 CLK / 5 | GPIO2 (4) | GP9 | read by the cable |
| TXD (SPI MOSI) | 5 | 2 CMD / 3 | GPIO3 (5) | GP10 | Dreamcast → cable |
| RXD (SPI MISO) | 4 | 7 DAT0 / 7 | GPIO4 (6) | GP11 | cable → Dreamcast |
| RTS (SPI CS) | 6 | 1 DAT3 / 2 | GPIO5 (7) | GP12 | Dreamcast → cable |
| (drives CTS) | – | – | GPIO6 (9) **→ 1 kΩ →** GPIO2 | GP13 **→ 1 kΩ →** GP9 | the resistor is required |
| 3.3 V (sense, optional) | 10 | 4 VDD / 4 | GPIO7 (10) **via 10 kΩ** | GP14 **via 10 kΩ** | never to the board's 3V3 |
| GND | 3, 8 | 3 + 6 VSS / 6 | GND (3 or 8) | GND | |
| +5 V | 1 | – | not connected | not connected | powering from it is not measured yet |

Serial port pins 2 and 9 and SD contacts 8 and 9 (DAT1, DAT2) stay unconnected. The serial
port's numbers come from coder's cable guides: measure before soldering (pin 1 is +5 V,
pin 10 is +3.3 V with the console on). Why the 1 kΩ: the same CTS pin is an input in serial
mode and the SPI clock output in W5500/SD mode, so both sides may drive it
([WIRING.md](hardware/WIRING.md), "Why the 1 kΩ resistor").

### The stack: which pins are joined

RP2040-Zero on top, face up; ESP32-S3 SuperMini underneath, face down; both USB sockets at
the same end; positions count from the USB end. **Solder only what the table says**, and put
insulation (Kapton) between the two boards' backs.

| Pos | Zero left ↔ SuperMini | | Zero right ↔ SuperMini | |
|---|---|---|---|---|
| 1 | 5V ↔ 5V | **solder** | GP0 ↔ GPIO43 | optional (ESP32 console TX) |
| 2 | GND ↔ GND | **solder** | GP1 ↔ GPIO44 | optional (ESP32 console RX) |
| 3 | 3V3 ↔ 3V3 | **never** (two regulators) | GP2 ↔ GPIO1 | **solder**: DR (ESP32 has data) |
| 4 | GP29 ↔ GPIO13 | **never** | GP3 ↔ GPIO2 | **solder**: HS (ESP32 ready) |
| 5 | GP28 ↔ GPIO12 | **never** | GP4 ↔ GPIO3 | **solder**: SPI MISO (ESP32 → Zero) |
| 6 | GP27 ↔ GPIO11 | **never** | GP5 ↔ GPIO4 | **solder**: SPI CS |
| 7 | GP26 ↔ GPIO10 | **never** | GP6 ↔ GPIO5 | **solder**: SPI clock |
| 8 | GP15 ↔ GPIO9 | solder for strength (both unused) | GP7 ↔ GPIO6 | **solder**: SPI MOSI (Zero → ESP32) |
| 9 | GP14 ↔ GPIO8 | **never** (GP14 is the power sense) | GP8 ↔ GPIO7 | **not joined**: GPIO7 is the W5500 module's INT |

Left 4-7 are the Zero's SD slot pins over the ESP32's W5500 pins: both are used, each from the
outside (the Zero's edge pads, the SuperMini's holes from its outer face). The Zero's bottom
edge GP9-14 is the Dreamcast (table above). GP16 is the Zero's own RGB LED (red: no ESP32,
yellow: damaged link packets, green: link up, blue: link up and the Dreamcast on).

### SD card slot (optional)

A 3.3 V microSD module or breakout on the RP2040's SPI1, never on the Dreamcast's wires.

| SD module | Pico wiring | Stack: RP2040-Zero |
|---|---|---|
| CLK / SCK | GPIO26 (pin 31) | GP26 |
| CMD / DI / MOSI | GPIO27 (pin 32) | GP27 |
| DAT0 / DO / MISO | GPIO28 (pin 34) | GP28 |
| DAT3 / CS | GPIO9 (pin 12) | GP29 |
| VCC / GND | 3V3 (pin 36) or 5 V, see [SD-SLOT.md](hardware/SD-SLOT.md#which-vcc) / GND | 3V3 or 5V / GND |

No card-detect pin: the cable looks for a card every 2 s.

### ESP32-S3 extras (optional)

| ESP32-S3 SuperMini | Connects to |
|---|---|
| GPIO12 | W5500 module SCLK |
| GPIO11 | W5500 module MOSI |
| GPIO13 | W5500 module MISO |
| GPIO10 | W5500 module CS (SCS) |
| GPIO7 (stack) / GPIO9 (other builds) | W5500 module INT |
| 3V3, GND | W5500 module 3.3 V, GND |
| USB-C (modem build) | a USB voice modem through an OTG adapter; the Dreamcast's modem joins it by a phone cord, as with a DreamPi |
| GPIO43 / GPIO44 | console TX / RX (UART0, 115200) |

The W5500 module is the Internet uplink when a router answers DHCP on it, otherwise the
socket for the Dreamcast's Broadband Adapter.

### Pico wiring + any ESP32-S3 (UART link)

| RP2040 | | ESP32-S3 | |
|---|---|---|---|
| GPIO12 (pin 16) | → | GPIO6 | cable TX → ESP32 RX |
| GPIO13 (pin 17) | ← | GPIO5 | ESP32 TX → cable RX |
| GPIO14 (pin 19) | ← | GPIO7 | ESP32 RTS → cable CTS |
| GPIO15 (pin 20) | → | GPIO8 | cable RTS → ESP32 CTS |
| GND | — | GND | |

3 Mbaud with RTS/CTS, wires under ~15 cm.

### Power and USB

* **Pico wiring:** powered by its USB (the Pi or computer). Only GND is shared with the Dreamcast.
* **Stack:** 5V and GND are joined, so either board's USB powers both. **Plug in one USB at a
  time** (the Zero has no diode on its USB 5 V). The Zero's USB is for updates and for a
  computer (it then takes the serial and network traffic, as with DreamPi); the ESP32's USB-C
  is its console, or the USB modem's socket in the modem build.
* Never join the Dreamcast's 3.3 V (port pin 10 / SD VDD) to a board's 3V3: it goes only to the
  sense pin, through 10 kΩ.

## Quick start (DreamPi)

1. **Wire it** to the Dreamcast serial port: [hardware/WIRING.md](hardware/WIRING.md)
   (SD-adapter plug build; an internal install works the same way).
2. **Flash** a Raspberry Pi Pico: hold BOOTSEL, plug in, copy
   `dc-picolink-<version>-250mhz.uf2` from the [releases](../../releases).
3. **Install the DreamPi side**: copy `dc-picolink-dreampi-setup.sh` to the Pi and run
   `sudo bash dc-picolink-dreampi-setup.sh`.
4. **Serial PPP** (Netlink): in DreamPi's `netlink_config.ini` set `port = /dev/dccable`.
5. **W5500**: the Dreamcast gets an address by DHCP (192.168.99.x). Static settings:
   IP 192.168.99.60, mask 255.255.255.0, gateway 192.168.99.1, DNS 192.168.99.1 (DreamPi's DNS;
   46.101.91.123 without DreamPi). Settings: `/etc/dcusb0.conf`.
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
# Pico / Pro Micro wiring (Dreamcast on GPIO2-7, ESP32 link on UART GPIO12-15)
cmake -S firmware -B build -DPICO_BOARD=pico -DDC_SYS_KHZ=250000 && make -C build
# RP2040-Zero + ESP32-S3 SuperMini stack (Dreamcast on GPIO9-14, ESP32 link over SPI)
cmake -S firmware -B build-zero -DPICO_BOARD=pico -DDC_SYS_KHZ=250000 -DDC_BOARD=zero-stack && make -C build-zero
# the same with the computer's USB network wired to the ESP32 (tests the stack without a Dreamcast)
cmake -S firmware -B build-zero-bridge -DPICO_BOARD=pico -DDC_SYS_KHZ=250000 -DDC_BOARD=zero-stack -DDC_USB_BRIDGE=1 && make -C build-zero-bridge
```

The UF2 is `build*/dc_usb_serial.uf2`: hold BOOT (BOOTSEL), plug in, copy it to the drive.
`-DPICO_BOARD=pico` is right for the RP2040-Zero and Pro Micro too (generic 2 MB flash).

ESP32-S3, ESP-IDF 5.4 (details in [esp32/README.md](esp32/README.md)):

```sh
cd esp32
idf.py set-target esp32s3 && idf.py build                                             # UART link (Pico wiring)
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
