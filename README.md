# DC-PicoLink

> ⚠️ **Under development. No guarantee that anything here works.**
> Use it at your own risk, on hardware you can afford to lose: wiring mistakes or firmware
> bugs can damage a Dreamcast, a modem or the boards. Only the RP2040 cable with DreamPi has
> been tried on hardware (see *Status*). Everything on the **RP2040-Zero + ESP32-S3 SuperMini
> stack** (Wi-Fi, USB modem, SD slot, DCNet, Netlink, VPN, port forwarding, the BBA port) is
> written, builds and passes the host tests, but **has not been tested on hardware yet**:
> expect bugs. The checklist for that is [docs/TESTING.md](docs/TESTING.md). When you report
> a problem, include the first status line (firmware version) and the log.

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
| an **SD card** | openMenu (settings, per-game VMU backups), other SCIF-SPI SD programs | a disk image on the host (DreamPi), or a microSD in the board's own slot (written, not tried on hardware) |

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
[hardware/ESP32.md](hardware/ESP32.md) and the project's DreamPi spec. Not tried on hardware yet.

Other hosts (Linux PC, macOS, Windows, Android) are planned: see [docs/ROADMAP.md](docs/ROADMAP.md).

## The stack: RP2040-Zero + ESP32-S3 SuperMini (no computer needed)

Not tried on hardware yet. The two boards sit back to back; the RP2040 does everything towards
the Dreamcast, the ESP32 the Internet side. Wiring: [hardware/ESP32.md](hardware/ESP32.md);
ESP32 details: [esp32/README.md](esp32/README.md).

| | What it does | Like DreamPi |
|---|---|---|
| Internet | home Wi-Fi, or a W5500 Ethernet module on the ESP32 as uplink | |
| Setup | own Wi-Fi network `DC-PicoLink-XXXX`, web page with status, log, settings | |
| Serial cable | modem emulation (`AT`, `ATD`), PPP server, NAT | serial PPP |
| W5500 emulation | the Dreamcast's W5500 network: DHCP, DNS, NAT | `dcusb0` |
| SD card | a microSD on the Zero's SPI1 is the Dreamcast's SD card | (dcsd image) |
| USB modem | a USB voice modem on the ESP32: dial tone, number, answer, PPP | yes |
| BBA | the Dreamcast's Broadband Adapter on the ESP32's W5500 module | |
| DNS | DreamPi's DNS list from dreamcast.online, revival DNS; over TCP when the router intercepts DNS | yes |
| DCNow! | reports, and `dreampi.local:1998` for dreamcast.online/now/configure | yes |
| DCNet | `*69` before the number, or every call | yes |
| Netlink | direct dial, matchmaking, game server codes, XBAND, Capcom matching (USB modem) | yes |
| VPN | Dreamcast Live, profiles built in (okayed by Dreamcast Live); the Dreamcast gets its own VPN address | yes |
| Own home address | as DreamPi: the Dreamcast gets .98 on your network (this box .99, answers ARP for it, no NAT); 1:1 with the VPN up | yes |
| Port forwarding | DreamPi's game ports by UPnP straight to the Dreamcast's .98, nothing else; says when the provider shares your IPv4 (then the VPN is switched on) | yes (optional there) |
| Game packet rules | TTL 64, PowerSmash double SYN, Planet Ring / Daytona public address | yes |

Test before trusting it: [docs/TESTING.md](docs/TESTING.md).

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
| [`esp32/`](esp32) | ESP32-S3 firmware: the cable's link to Wi-Fi without a computer (being built) |
| [`hardware/`](hardware) | wiring and build notes, [SD slot](hardware/SD-SLOT.md), [ESP32](hardware/ESP32.md) |
| [`docs/`](docs) | [architecture](docs/ARCHITECTURE.md), [technical reference](docs/REFERENCE.md), [roadmap](docs/ROADMAP.md), [testing](docs/TESTING.md), [development log](docs/DEVLOG.md) |
| [`tools/`](tools) | test runner, DreamPi installer builder, `dreampi_vpn_extract.py` (VPN profiles out of a DreamPi image, plain Python, works in a-Shell on iOS) |

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
