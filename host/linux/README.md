# DC-PicoLink on any Linux computer

What the ESP32 box does, on a Linux computer instead (a stand-alone replacement for DreamPi's
`dreampi.py`; nothing from DreamPi is needed). **Tested against fake modems, fake access points and
fake pppd only; not yet with a real modem and a Dreamcast.**

```
sudo ./install.sh              # USB modem server + the RP2040 cable's side
sudo ./install.sh --no-cable   # only the modem server
sudo ./install.sh --cable-ppp  # Dreamcast serial PPP on the RP2040 cable instead of a modem
sudo ./install.sh --web        # the modem (or cable) is used from Chrome: http://127.0.0.1:8099/
sudo ./install.sh --no-modem   # only the cable's side (same as host/dreampi/install.sh)
journalctl -fu dc-modemd       # (dc-cabled / dc-web for the other two)
```

Needs systemd, python3, pppd (`ppp`), iptables (for NAT), and for the cable's side apt
(dnsmasq-base, dosfstools; see `../dreampi/install.sh`). Options: `/etc/dc-modemd.conf`
(`DC_MODEMD_ARGS`, e.g. `--device /dev/ttyACM1`, `--dns 1.1.1.1`, `--dcnet always`; `dc-modemd --help`).
The three services share one modem/cable, so only one runs (`dc-modemd` default, `dc-cabled`, `dc-web`).

## In the browser

`--web` starts `dc-modemd --relay`: it serves one page on this computer only (`127.0.0.1:8099`).
In Chrome or Edge (desktop; Web Serial needs a secure context, and `localhost` is one) press
Connect, pick the modem's or cable's port, leave the tab open. The page does the modem part
(dial tone, hearing the number, ATA, or the Hayes stub for the cable) and passes the call's bytes
over a WebSocket to the relay, which runs pppd (NAT, DNS) or DCNet. A browser cannot open raw
network connections, so that half cannot move into the page. The relay refuses pages from other
sites (Origin check); to use it from another computer, listen with `--relay-host` and add that
page's address with `--relay-origin`: it then has no login, so keep it on a trusted network.

## What is covered, compared with the ESP32

| Feature | Here |
|---|---|
| USB voice modem: dial tone, hear the number, ATA, PPP, NAT (192.168.98.1 / .2) | yes (`dc-modemd`) |
| Normal Internet call, `*70`, Capcom number | yes |
| DCNet (`*69` before the number, or `--dcnet always`; access point by ping; port 7656 numbers) | yes |
| DNS for the Dreamcast: dreampi_dns.conf overrides, revival servers, plain names and private reverse lookups refused | yes (UDP only; no TCP DNS) |
| RP2040 cable: serial port `/dev/dccable`, W5500 network `dcusb0` with DHCP/NAT, SD image, DC Now!, VPN | yes (`../dreampi`, installed by `install.sh`) |
| Serial-cable Hayes/PPP call (Dreamcast on the RP2040's serial port) | yes, opt in (`--cable-ppp`; takes `/dev/dccable`, so not with dc-tool-ser) |
| The same from Chrome (Web Serial) | yes, `--web` |
| DC Now! for modem and serial calls (the DNS log), DCNet/DCNow selection page | no |
| Netlink, XBAND, game-server numbers | recognised and logged, hung up on; not carried out |
| Wi-Fi setup, firmware update from the page, status JSON | no (the ESP32's own page) |

Not carried out yet means the dial is logged ("... not carried out by this host program yet") and
the call ends, so a log line shows what to build next.

## Tests

`tools/run-tests.sh` runs `tests/test_modemd.py`: the parsers (same rules as
`firmware/usbmodem_model.h`), a whole modem call against a fake modem on a pty with a fake pppd,
the serial-cable call, the DNS server against a fake upstream, DCNet against a fake access point,
the relay over a real WebSocket, and the page's own parsers in node (`tests/test_page.js`, when node is installed).
