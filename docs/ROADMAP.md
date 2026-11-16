# Roadmap

## 1. Validate the current firmware (v0.8)

* Run `tools/dctest` (serial baud sweep, ramps, switching, W5500 net/soak) on hardware first;
  compile `dctest.c` in a real KOS environment (never built against KOS yet).

* Test the net8f state on hardware (see TESTING.md): DreamShell boot, openMenu, long
  W5500 sessions (RX resyncs should stay rare), serial PPP.
* Test `dcusb0-vpn` with Quake 3, Unreal Tournament, AFO, Worms World Party.
* Validate the release build (diagnostics off) on hardware: all testing so far used
  diagnostics on.

## 1b. RP2040-Zero + ESP32-S3 SuperMini stack (hardware/ESP32.md)

* Done in firmware, not on hardware yet: `-DDC_BOARD=zero-stack` (Dreamcast on GPIO9-14, the
  W5500/SD PIO programs loaded moved there), the ESP32 link over SPI (cable master, ESP32
  slave, HS/DR handshake, ~1.4 MB/s each way in the timing simulation), the USB bridge test
  build. First hardware test: the bridge build (no Dreamcast needed), watch the link's
  checksum counters at 15.6 MHz.
* Next: the ESP32 takes over the computer side when it is present (all Dreamcast traffic
  over the link; the ESP32's USB presents the cable's serial port, network and status
  interfaces to DreamPi, plus a log port that is there in every mode); the RP2040 falls back
  to its own USB only when the ESP32 is gone.
* Then: the physical W5500 on the ESP32 (fast SPI pins 10-13) as uplink or BBA port, the SD
  slot on the Zero's SPI1 (GP26-29), the USB modem on the Zero's USB-C (RP2040 as USB host)
  with a line-voltage board, per-game connection choice from the game information openMenu
  sends over serial.

## 2. SD card on a physical slot

* Wire an SD/microSD slot to the RP2040's own SPI (SPI1): GPIO26/27/28 + CS GPIO9,
  the same pins on Pro Micro RP2040, RP2040-Zero and Pico (hardware/SD-SLOT.md). On the
  zero-stack layout GPIO9 is the Dreamcast's: CS on GPIO29 there.
* Firmware: the SD emulation's block source becomes the card (it already handles a slow
  source); remove the host image link (`dcsd`, USB vendor interface) and move the status
  lines to a small second USB serial port.
* USB mass storage (TinyUSB MSC) of the card while the Dreamcast is off; "no medium"
  while it's on.
* Fix openMenu 1.7.1 "no SD card" (it reads two sectors and stops: needs a debug log
  of the openMenu boot).

## 3. Hosts

* Linux PC: udev + network sharing setup script (NetworkManager or systemd-networkd).
* macOS, Windows: network sharing instructions/scripts, serial port names.
* Android: investigate.
* A PPP server for serial games without DreamPi (like Netlink), per platform.
* **ESP32-S3 instead of a computer** (`esp32/`, plan E-1 to E-11 in the project's
  `07-esp32-link.md`): E-1 link, E-2 web page, E-3 Internet/DCNow! over serial PPP (Hayes stub, PPP server,
  DNS to the revival servers, NAT, DCNow! reports) E-4 the same over the W5500 frames and E-7 an OpenVPN client for Dreamcast Live (connects, keeps the tunnel up;
  no traffic through it yet) are written, not yet tried on hardware; next
  NAT rewrites/UPnP/DreamPi's helpers (and traffic through the VPN), DCNET, power from the serial port, the modem.
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

