# DC-PicoLink

An RP2040-based Dreamcast serial-port adapter: a full coder's cable (serial and serial
PPP), W5500 Ethernet emulation over USB networking, and SD card emulation (used by
openMenu for its settings and VMU backups). Works with DreamPi or a regular computer,
as a cheaper, faster alternative to separate coder's cables and W5500 adapters.

One small board on the Dreamcast's serial port shows up on the computer as one USB
device with three functions. It works out by itself which one the Dreamcast is using:

| The Dreamcast thinks it has… | Used for | On the computer |
|---|---|---|
| a **coder's cable** (UART) | dc-tool-ser / dcload-serial, the KOS console, serial PPP games (e.g. AFO through DreamPi's Netlink) | a USB serial port (`/dev/dccable` on DreamPi) |
| a **WIZnet W5500** Ethernet chip | KallistiOS homebrew and patched games with the W5500 driver | a USB network interface (`dcusb0`) the computer routes or bridges |
| an **SD card** | openMenu (settings, per-game VMU backups), other SCIF-SPI SD programs | today: a disk image on the host; planned: a physical SD slot on the board |

## Status

Working on hardware with DreamPi (Raspberry Pi, Linux):

* Serial: dc-tool-ser, the KOS console, Netlink serial PPP (AFO reaches PPP; online
  play is being tested together with the VPN mapping below). Exact Dreamcast baud rates
  and auto-baud.
* W5500: KOS networking, DreamKarts and RuneScape online, JaderFox tester, ping.
  Quake 3 / Unreal Tournament / AFO / Worms World Party online: being tested with
  `dcusb0-vpn` (the Dreamcast's own Dreamcast Live VPN address).
* SD card: works with KOS's SD driver; openMenu 1.7.1 doesn't find the card yet.

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
   IP 192.168.99.60, mask 255.255.255.0, gateway 192.168.99.1, DNS 46.101.91.123.

Logs: `journalctl -fu dcsd` (the cable's own status lines), `journalctl -u dcusb0`,
`journalctl -u dcusb0-vpn`.

## Repository

| Folder | |
|---|---|
| [`firmware/`](firmware) | RP2040 firmware (Pico SDK, PIO) and its host-side tests (`firmware/tests`) |
| [`host/`](host) | software for the computer side: `dreampi/` today; `linux/`, `macos/`, `windows/` planned |
| [`hardware/`](hardware) | wiring and build notes, [SD slot](hardware/SD-SLOT.md) |
| [`docs/`](docs) | [architecture](docs/ARCHITECTURE.md), [technical reference](docs/REFERENCE.md), [roadmap](docs/ROADMAP.md), [testing](docs/TESTING.md), [development log](docs/DEVLOG.md) |
| [`tools/`](tools) | test runner, DreamPi installer builder |

Build: see [docs/REFERENCE.md#build](docs/REFERENCE.md#build). CI builds every push;
tagged versions (`v*`) become releases with the UF2 files and the DreamPi installer.

Part of a family of `DC-` Dreamcast projects.

## License

MIT, see [LICENSE](LICENSE).
