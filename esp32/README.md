# DC-PicoLink ESP32 side

> ⚠️ **Under development.** Tested with a real Dreamcast on a breadboard build (the cable and
> the ESP32 joined by wires), over the serial cable on home Wi-Fi and a phone hotspot: PPP, DNS,
> DNAT swaps, VPN, UPnP, DCNow!; Quake, Quake III (also the 8-player mod), Unreal Tournament and
> Alien Front Online. Written and host-tested but not tried on hardware: the soldered stack,
> the cable's W5500 emulation as a network, the SD slot, the wired port, the USB modem, DCNet,
> Netlink. No guarantee that it works. Test list: [../docs/TESTING.md](../docs/TESTING.md).

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
(always with the modem build), hold BOOT while plugging it in. The merged image **erases the
saved settings** (Wi-Fi, DCNow! ID, switches), so the box starts as new; the uploaded VPN
profiles stay (own partition, `partitions.csv`). Updating from the page (*Firmware update*, the
app `.bin` or the merged one) and `idf.py flash` keep everything.

## Use

Join **DC-PicoLink-XXXX** (password `dreamcast`, change it in menuconfig) and open
http://192.168.98.1 (the setup network is a /24 of its own, 192.168.98.0; a home network with
that range would clash with it). Save your home network under *Wi-Fi*; the page is then also on the address
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
| BBA | a W5500 module on GPIO9-13 (`wired.c`): with a cable from the Dreamcast's BBA it serves 192.168.97.x; with a cable to a router it is the uplink instead |

### As DreamPi

| DreamPi | here |
|---|---|
| dial tone; number ends after 2 s without a key; PPP answered 8 s later (ATZ0, AT&F0, ATE0W2, ATA) | same (`usbmodem.c`) |
| pppd, no login | lwIP PPP server (`ppp_srv.c`) |
| dnsmasq with `dreampi_dns.conf`, revival DNS 46.101.91.123 / 209.50.50.129 | `dnsd.c`; the file is read while it downloads; if the router intercepts DNS, questions go over TCP |
| TTL 64, PowerSmash double-SYN drop, Planet Ring / Daytona public address | `../firmware/hdlc_model.h`, `ipfilter_model.h`, `pubip.c` |
| `dcnow.py`, `config_server.py` (port 1998) | `dcnow.c`; also announced as `dreampi.local` for dreamcast.online/now/configure. DCNow! knows a device by the SHA-256 of its MAC address (see *DCNow! ID* below) |
| DCNet (`*69`, `dcnet.rpi`) | `dcnet.c`: closest access point, TCP 7654/7656, the `~` fix. Also over the serial cable |
| Netlink (eaudunord/Netlink): direct dial, matchmaking, game server codes, XBAND, Capcom | `netlink.c` (USB modem only); no SIP ring on XBAND calls |
| OpenVPN to Dreamcast Live, Dreamcast on tunnel address + 1, input rules on tun0 | `ovpn.c`, `vpn.c`; routes pushed by the server go into the tunnel (`../hooks/dcl_lwip_hooks.h`) |
| UPnP port forwarding (optional) | `portfwd.c`: as DreamPi's port_forwarding.py, each mapping deleted, then DreamPi's game ports straight to the Dreamcast's home address; nothing else (not the Netlink ports); on by default |

### SD card files

The page's *SD card* section copies files (VMU saves, openMenu's folders) between the card in the
cable's slot and a computer or phone over Wi-Fi: browse, download, upload, new folder, delete.
The cable lends the card block by block (`firmware/sdlink_model.h`); FatFs runs on the ESP32
(`main/sdfs.c`). While the page has the card the Dreamcast sees none; details in
[../hardware/SD-SLOT.md](../hardware/SD-SLOT.md).

### DCNow! ID

DCNow! knows a device only by the SHA-256 of its Wi-Fi MAC address, and cannot remove entries.
So the ID cannot be made up freely:

* **This box** (the default): the ID of this box's own Wi-Fi MAC. The ESP32-S3 has one base MAC;
  Wi-Fi station = base, setup network = base+1, the W5500 wired port and the cable's Ethernet
  side = base+3. DCNow! and the VPN use the Wi-Fi station one, whichever way the traffic goes.
* **Clone DreamPi** (page: *Clone a DreamPi on this network*): reads the ID from a DreamPi's
  settings service (port 1998) on the same network, found as `dreampi.local` or by its address.
  Both devices then count as one player at DCNow!, with the stats together. The page's *DCNow! ID*
  menu has three choices: this box; the other device always; or (the default after a clone) the
  other device **only while it is away**. In that mode the box looks for the cloned device every
  30 s (its port 1998, at the address it was cloned from, or as `dreampi.local`): while it is on
  the network this box reports with its own ID, so two devices never report as one at the same
  time; 3 misses in a row (about 90 s) and it takes the cloned ID, and the cloned device's
  return switches it back. The ID changes only while no session is running. One limit: the
  look-up by name finds only one `dreampi.local`, so with several DreamPis on the network the
  remembered address is what counts.
* **Typing** a MAC or ID is possible only until the box has been on a Wi-Fi network once
  (the field disappears after that). The way to unlock it again is flashing the merged image.

### VPN

The Dreamcast Live profiles from DreamPi 2.1 are built in (`vpn/`, used with Dreamcast Live's
okay); the VPN is on by default. Newer profiles can be uploaded on the page without a rebuild:
the `.conf` files and `server.list` from a DreamPi's `/etc/openvpn/client/`, or the
`dreampi-vpn.txt` that `../tools/dreampi_vpn_extract.py` makes from a DreamPi image.

Like DreamPi, the tunnel stays up the whole time the box is on, not only during calls. Which
server: "auto" on the page does as DreamPi: one ping to every server (1 s to answer), the
fastest first (status line `VPN: ping ...`). Servers that do not answer follow in `server.list`
order (shumania first), so with ping blocked everywhere it is the list order. After a lost
tunnel it pings again; on a slower fallback server it pings again and moves to the fastest every
15 minutes while no Dreamcast is online. The page can also pin one server.

### The serial line's queue

The serial line is the slowest part (115200 baud = 11.5 KB/s). Everything for the Dreamcast
used to wait in one line of bytes (up to ~12 KB on the ESP32 and the cable), so during a
download a game update could wait over a second. Now the cable holds only ~20 ms; the rest waits
on the ESP32 as whole packets in two queues, and the line gets one packet at a time:

* **UDP and ICMP first** (game updates, DNS, ping), then **TCP** (pages, downloads, lobbies).
  TCP stays in one queue in arrival order, so its segments never overtake each other.
* Full queues drop as a modem's full buffer would: UDP its oldest packet (a game wants the
  newest), TCP its newest (the server sends it again). `queue_full` in `/api/status` counts them.
* The Dreamcast connects exactly as before. A game alone sees no difference (nothing waits).

`firmware/tests/test_serq_sim.c` runs the real rules: a 20 Hz game update during a download
with an 8 KB TCP window waited about 1 s at 115200 (4 s at 57600) before, about 0.12 s on
average and 0.19 s at worst now (0.22 / 0.35 s at 57600), with the line just as busy. What is
left is mostly the TCP segment the update has to wait behind (1500 bytes = 0.13 s at 115200).

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
