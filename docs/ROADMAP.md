# Roadmap

> Under development: the ESP32 side has been tested with a Dreamcast on a breadboard build (serial PPP, DNS, VPN, UPnP, DCNow!; Quake, Quake III, Unreal Tournament, Alien Front Online). The soldered stack and the parts listed in 1b are not tested yet (docs/TESTING.md).

## 1. Validate the current firmware (on a DreamPi)

* Run `tools/dctest` (serial baud sweep, ramps, switching, W5500 net/soak) on hardware first.
  CI builds `dctest.elf` and a CDI image against KallistiOS (the `dctest` artifact).

* Test the current `main` build on hardware with a DreamPi (the zero-stack checklist is in TESTING.md):
  DreamShell boot, openMenu, long W5500 sessions (RX resyncs should stay rare), serial PPP.
* Test `dcusb0-vpn` with Quake 3, Unreal Tournament, AFO, Worms World Party.
* Validate the release build (diagnostics off) on hardware: all testing so far used
  diagnostics on.

## 1b. RP2040-Zero + ESP32-S3 SuperMini stack (hardware/ESP32.md)

* Works on a breadboard build with a Dreamcast: SPI link, serial PPP over Wi-Fi, DNS, VPN with
  built-in profiles and pushed routes, UPnP port forwarding, DCNow! + port 1998, web page,
  firmware update.
* Written since the last hardware test (host-tested): UDP-first queue for the serial line,
  VPN auto picks the fastest ping, DCNow! ID clone/lock, the slot card's files on the web page.
* Written and host-tested, **not on hardware yet**: the soldered stack, W5500 emulation as a
  network, SD slot on SPI1 (GP26-29), the wired uplink / BBA port, USB modem, DCNet,
  Netlink/XBAND/Capcom, shared-IPv4 check.
* Next: run docs/TESTING.md on hardware and fix what it finds.
* Then: Netlink/XBAND over the serial cable; the ESP32 takes over the computer side of the
  cable's USB (all Dreamcast traffic over the link, a log port in every mode); per-game
  connection choice from the game information openMenu sends over serial.

## 2. SD card on a physical slot

* Written (sdcard_model.h, not on hardware yet): a card on SPI1 (GP26/27/28, CS GP9, or GP29 on
  the zero-stack) is the Dreamcast's SD card when the Pi's dcsd isn't connected; multi-block
  streams and a CRC check on every block.
* Written (sdlink_model.h + esp32/main/sdfs.c, not on hardware yet): the card's files on the
  ESP32's web page (download, upload, folders, delete).
* Next: USB mass storage (TinyUSB MSC) of the card while the Dreamcast is off; fix openMenu
  1.7.1 "no SD card" (needs a debug log of the openMenu boot).

## 3. Hosts

* Linux PC: udev + network sharing setup script (NetworkManager or systemd-networkd).
* macOS, Windows: network sharing instructions/scripts, serial port names.
* Android: investigate.
* A PPP server for serial games without DreamPi (like Netlink), per platform.
* **ESP32-S3 instead of a computer** (`esp32/`): see 1b; everything DreamPi does is written.
* Own USB VID/PID (pid.codes for open-source hardware) instead of the TinyUSB test ID
  (`cafe:4dc5`).

## 4. Robustness

* Widen the W5500 MISO timing margin: KOS samples RXD right after raising the clock,
  and the answer changes right after that edge. One 1-bit misread per ~2600 packets
  was seen (recovered by the RX resync).
* W5500 command register polls right after a command write are answered late (harmless,
  KOS polls again): move the command's slow part out of that window.

## 5. Modem tiering (internal mod)

* Internal Raspberry Pi Zero 2W running DreamPi, with a regular modem (small USB modem
  + line voltage) inside the console.
* PicoLink ↔ Pi Zero over GPIO instead of USB, freeing the Zero's single USB port for the
  modem: serial on the Pi's UART (with RTS/CTS), the network as framed packets over SPI
  with a small Pi-side program presenting a TUN interface (it can carry the status log
  too). The RP2040 stays: the Pi can't answer the Dreamcast's SPI within ~250 ns.
* A GPIO-switched line switch to turn the modem "off" (no dial tone) so games that try
  the modem first (AFO) fall back to serial; priority W5500 → serial → modem.
* Per-game choice from the game information openMenu sends over serial before a game starts
  (not every build has an SD card); everything stays on when nothing arrives.
* Experimental, later: an I2S audio codec + software modem on the Pi Zero instead of a
  USB modem (realistically V.32bis-class speeds).

