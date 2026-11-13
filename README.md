# DC-PicoLink

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
| an **SD card** | openMenu (settings, per-game VMU backups), other SCIF-SPI SD programs | today: a disk image on the host; planned: a physical SD slot on the board |

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
| [`tools/`](tools) | test runner, DreamPi installer builder |

Build: see [docs/REFERENCE.md#build](docs/REFERENCE.md#build). CI builds every push;
tagged versions (`v*`) become releases with the UF2 files and the DreamPi installer.

Part of a family of `DC-` Dreamcast projects.

## License

MIT, see [LICENSE](LICENSE).
