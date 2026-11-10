# Wiring the ESP32-S3 (no DreamPi needed)

The cable (RP2040) talks to an **ESP32-S3** over one UART with hardware flow control. The
ESP32 gives the Dreamcast its internet connection over Wi-Fi (later also a W5500 on the
ESP32). The same cable firmware still works with a Raspberry Pi or a PC over USB; see
"Who gets the traffic" below.

Tested boards: RP2040-Zero + ESP32-S3-Zero (Waveshare); any RP2040 and ESP32-S3 board works
with the same GPIO numbers. **Status: written, not yet tried on hardware.**

## Connections

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
2. Join the Wi-Fi network **DC-PicoLink-XXXX** (password `dreamcast`), open http://192.168.4.1.
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
3. **Expect** in *Status lines*: `Ethernet: the Dreamcast is sending`, then
   `Ethernet: the Dreamcast got 192.168.99.x by DHCP`. In the card: *cable network (W5500)* counts
   frames in and out, `DNS:` counts questions, `DCNow!: ... sent` goes up every 15 s.
4. Try a web page or a game server. If frames come in but nothing goes out (or reverse), send the card's text.
