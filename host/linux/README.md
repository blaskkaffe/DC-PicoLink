# DC-PicoLink on any Linux computer

What the ESP32 box does, on a Linux computer instead (a stand-alone replacement for DreamPi's
`dreampi.py`; nothing from DreamPi is needed). **Stage 1; tested against a fake modem on a pty,
not yet with a real modem and Dreamcast.**

```
sudo ./install.sh              # modem server + the RP2040 cable's side
sudo ./install.sh --no-cable   # only the modem server
sudo ./install.sh --cable-ppp   # Dreamcast serial PPP on the cable instead of a modem
sudo ./install.sh --no-modem   # only the cable's side (same as host/dreampi/install.sh)
journalctl -fu dc-modemd
```

Needs systemd, python3, pppd (`ppp`), iptables (for NAT), and for the cable's side apt
(dnsmasq-base, dosfstools; see `../dreampi/install.sh`). Options: `/etc/dc-modemd.conf`
(`DC_MODEMD_ARGS`, e.g. `--device /dev/ttyACM1`, `--dns 1.1.1.1`).

## What is covered, compared with the ESP32

| Feature | Here |
|---|---|
| USB voice modem: dial tone, hear the number, ATA, PPP, NAT (192.168.98.1 / .2) | yes (`dc-modemd`) |
| Normal Internet call, `*70`, Capcom number | yes |
| RP2040 cable: serial port `/dev/dccable`, W5500 network `dcusb0` with DHCP/NAT, SD image, DC Now!, VPN | yes (`../dreampi`, installed by `install.sh`) |
| DNS overrides from dreampi_dns.conf, DNS log for DC Now! on the modem call | no: the Dreamcast gets the revival DNS servers directly |
| Serial-cable Hayes/PPP call (Dreamcast on the RP2040's serial port) | yes, opt in: `sudo ./install.sh --cable-ppp` (`dc-cabled`, takes `/dev/dccable` and replaces `dc-modemd`, so not with dc-tool-ser) |
| DCNet (`*69`), Netlink, XBAND, game-server numbers | recognised and logged, hung up on; not carried out |
| Web page, Wi-Fi setup, firmware update from the page, status JSON | no (the ESP32's own page) |

Not carried out yet means the dial is logged ("... not carried out by this host program yet") and
the call ends, so a log line shows what to build next.

## Tests

`tools/run-tests.sh` runs `tests/test_modemd.py`: the parsers (same rules as
`firmware/usbmodem_model.h`) and a whole call against a fake modem on a pty with a fake pppd.
