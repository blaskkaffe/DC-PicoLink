# DC-PicoLink ESP32 side

> ⚠️ **Under development, not tested on hardware yet.** Everything below is written, builds
> and passes the host tests; none of it has been tried with a real Dreamcast, modem or router.
> No guarantee that it works. Test list: [../docs/TESTING.md](../docs/TESTING.md).

Firmware for an **ESP32-S3** next to the cable, so a Dreamcast can go online without a
Raspberry Pi: it does what DreamPi does, plus Wi-Fi, a web page, a wired port and a USB modem.
Two links to the cable (menuconfig → *DC-PicoLink ESP32* → *Link to the cable*): UART (Pico
wiring, the default) or SPI (the RP2040-Zero + ESP32-S3 SuperMini stack, `sdkconfig.zero-stack`;
this board is the SPI slave). Wiring: [../hardware/ESP32.md](../hardware/ESP32.md).

## Build and flash

ESP-IDF 5.4. Everything it needs is in the repository (no component registry).

```
cd esp32
idf.py set-target esp32s3
# the stack (SPI link)
idf.py -B build-zero -D SDKCONFIG=build-zero/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.zero-stack" build
# the stack with the USB modem port (the USB socket is then the modem's, console on UART0)
idf.py -B build-modem -D SDKCONFIG=build-modem/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.zero-stack;sdkconfig.modem" build
idf.py -B build-zero -p /dev/ttyACM0 flash monitor
```

CI builds merged images (Actions → build → artifact `dc-picolink-esp32`), flashed with
`esptool.py --chip esp32s3 write_flash 0x0 <image>-merged.bin`. If the board isn't found
(always with the modem build), hold BOOT while plugging it in. Flashing keeps the settings and
uploaded VPN profiles (own partitions, `partitions.csv`).

## Use

Join **DC-PicoLink-XXXX** (password `dreamcast`, change it in menuconfig) and open
http://192.168.4.1. Save your home network under *Wi-Fi*; the page is then also on the address
shown there. The page shows the link to the cable, status lines from both boards, the serial
monitor, and the settings: Internet over serial, DCNow!, VPN, DCNet, port forwarding, VPN
profile upload.

## What it does

### Internet for the Dreamcast

| Path | How |
|---|---|
| serial cable | modem emulation (`../firmware/hayes_model.h`), lwIP's PPP server. As on DreamPi: the Dreamcast gets its own home address (usually .98, this box .99 = its DNS and gateway), this box answers ARP for it, no NAT (`dclan.c`); tun+1 with the VPN up, mapped 1:1 to the home address. Without a free home address: 10.64.0.2 behind NAT |
| the cable's W5500 emulation | second interface 192.168.99.1/24, DHCP, DNS (`dc_eth.c`); mapped 1:1 to the Dreamcast's home address (NAT only without one) |
| USB modem | a USB voice modem on this board's USB socket; the Dreamcast's own modem calls it over a phone cord (`usbmodem.c`) |
| BBA | a W5500 module on GPIO9-13 (`wired.c`): with a cable from the Dreamcast's BBA it serves 192.168.98.x; with a cable to a router it is the uplink instead |

### As DreamPi

| DreamPi | here |
|---|---|
| dial tone; number ends after 2 s without a key; PPP answered 8 s later (ATZ0, AT&F0, ATE0W2, ATA) | same (`usbmodem.c`) |
| pppd, no login | lwIP PPP server (`ppp_srv.c`) |
| dnsmasq with `dreampi_dns.conf`, revival DNS 46.101.91.123 / 209.50.50.129 | `dnsd.c`; the file is read while it downloads; if the router intercepts DNS, questions go over TCP |
| TTL 64, PowerSmash double-SYN drop, Planet Ring / Daytona public address | `../firmware/hdlc_model.h`, `ipfilter_model.h`, `pubip.c` |
| `dcnow.py`, `config_server.py` (port 1998) | `dcnow.c`; also announced as `dreampi.local` for dreamcast.online/now/configure. DCNow! knows a device by the SHA-256 of its MAC address; the page can set a DreamPi's MAC (or the ID it logs as "MAC address: ...") so both count as one |
| DCNet (`*69`, `dcnet.rpi`) | `dcnet.c`: closest access point, TCP 7654/7656, the `~` fix. Also over the serial cable |
| Netlink (eaudunord/Netlink): direct dial, matchmaking, game server codes, XBAND, Capcom | `netlink.c` (USB modem only); no SIP ring on XBAND calls |
| OpenVPN to Dreamcast Live, Dreamcast on tunnel address + 1, input rules on tun0 | `ovpn.c`, `vpn.c`; routes pushed by the server go into the tunnel (`../hooks/dcl_lwip_hooks.h`) |
| UPnP port forwarding (optional) | `portfwd.c`: as DreamPi's port_forwarding.py, each mapping deleted, then DreamPi's game ports straight to the Dreamcast's home address; nothing else (not the Netlink ports); on by default |

### VPN

The Dreamcast Live profiles from DreamPi 2.1 are built in (`vpn/`, used with Dreamcast Live's
okay); the VPN is on by default. Newer profiles can be uploaded on the page without a rebuild:
the `.conf` files and `server.list` from a DreamPi's `/etc/openvpn/client/`, or the
`dreampi-vpn.txt` that `../tools/dreampi_vpn_extract.py` makes from a DreamPi image.

Like DreamPi, the tunnel stays up the whole time the box is on, not only during calls. Which
server: "auto" on the page tries them in `server.list` order (shumania first), starts from the
top again after a lost tunnel, and on a fallback server goes back to the first one every 15
minutes while no Dreamcast is online. DreamPi pings every server once at start and takes the
fastest; the page can also pin one server.

### Shared IPv4 (carrier-grade NAT)

The box asks the router for its outside address. If the provider shares the IPv4 address
(DS-Lite, 100.64/10, IPv6-only with shared IPv4), it says so in the log, skips UPnP (useless
then) and switches the VPN on, unless you switched it off: peer-to-peer games and Netlink
waiting need the VPN on such a connection.

## Files

| | |
|---|---|
| `main/link.c` | link to the cable (UART or SPI slave), packets, `get hello` every second, self-test echo |
| `main/wifi.c` | setup network + home network (NVS) |
| `main/web.c` | the page and `/api/*` |
| `main/ppp_srv.c` | modem emulation, PPP server, NAT, call control (serial cable and USB modem) |
| `main/dc_eth.c` | the cable's W5500 network as an interface |
| `main/wired.c` | the W5500 module: uplink or BBA port |
| `main/usbmodem.c` | USB host, CDC-ACM modem, dial tone and answering |
| `main/dcnet.c`, `main/netlink.c` | DCNet; Netlink, game servers, XBAND |
| `main/vpn.c`, `main/ovpn.c` | VPN task, profiles, the tunnel interface; the OpenVPN client |
| `main/portfwd.c` | UPnP, NAT port maps (only without a home address), shared-IPv4 check |
| `main/dclan.c` | the Dreamcast's own home address as on DreamPi: the .99/.98 pick, proxy ARP, routing, the VPN's 1:1 mapping (`../firmware/dclan_model.h`) |
| `main/dnsd.c`, `main/dcnow.c`, `main/pubip.c` | DNS server, DCNow! + port 1998 service, public address |
| `main/main.c` | status-line ring, serial monitor ring, start-up |
| `hooks/dcl_lwip_hooks.h` | lwIP route hook (VPN routes) |
| `components/` | vendored `mdns`, `usb_host_cdc_acm` (Apache-2.0) |
| `../firmware/*_model.h` | host-compilable rules shared with the cable, each with a test in `../firmware/tests` |

Tests: `../tools/run-tests.sh` (all models), `tests/run-ppp-test.sh` (lwIP's PPP server on the
PC, talked to like a Dreamcast), `tests/ovpn/run-ovpn-test.sh` (the OpenVPN client against a
real OpenVPN 2.6 server).

## Not done yet

- Netlink and XBAND over the serial cable (USB modem only).
- SIP ring on XBAND calls; "everything through the VPN" (redirect-gateway) is not followed.
- Fetching newer VPN profiles from Dreamcast Live by itself.
- The ESP32 taking over the computer side of the cable's USB.
