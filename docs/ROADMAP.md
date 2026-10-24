# Roadmap

## 1. Validate the current firmware (v0.8)

* Test the net8f state on hardware (see TESTING.md): DreamShell boot, openMenu, long
  W5500 sessions (RX resyncs should stay rare), serial PPP.
* Test `dcusb0-vpn` with Quake 3, Unreal Tournament, AFO, Worms World Party.
* Validate the release build (diagnostics off) on hardware: all testing so far used
  diagnostics on.

## 2. SD card on a physical slot

* Wire an SD/microSD slot to free GPIOs on the RP2040's own SPI (SPI1), card powered
  from the board's 3.3 V.
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
* A GPIO-switched line switch to turn the modem "off" (no dial tone) so games that try
  the modem first (AFO) fall back to serial; priority W5500 → serial → modem.
* Per-game choice from openMenu's "last played game" file on the SD card.
* Experimental, later: an I2S audio codec + software modem on the Pi Zero instead of a
  USB modem (realistically V.32bis-class speeds).

## Repository

* Pick a license.
* Rename the GitHub repository to DC-PicoLink.
