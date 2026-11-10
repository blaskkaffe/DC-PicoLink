# DC-PicoLink ESP32 side

Firmware for an **ESP32-S3** (ESP32-S3-Zero) connected to the cable over UART, so a
Dreamcast can go online without a Raspberry Pi. Design and plan: the project's
`07-esp32-link.md` (steps E-1 to E-11). Wiring: [../hardware/ESP32.md](../hardware/ESP32.md).

**Status: step E-4 (written, builds, rules host-tested; not yet tried on hardware).**

| Step | What | State |
|---|---|---|
| E-1 | link to the cable (shared framing `../firmware/link_model.h`) | done in the cable firmware |
| E-2 | Wi-Fi setup network + web page: link state, the cable's hello, status lines, serial monitor, dc1 box, home Wi-Fi | done |
| E-3 | Internet and DCNow! over serial PPP: Hayes stub, PPP server, DNS, NAT, TTL 64 and PowerSmash SYN rule, DCNow! reports | done |
| E-4 | Internet and DCNow! over the cable's W5500 network: second interface 192.168.99.1, DHCP, DNS, NAT | **this** |
| E-5 | NAT rewrite list, UPnP + STUN, DreamPi's helpers | |
| E-6 | DCNET | |

## Build and flash

ESP-IDF 5.4:

```
cd esp32
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor      # the board's own USB port
```

CI builds it too (Actions → build → artifact `dc-picolink-esp32`): one merged image, flashed with
`esptool.py --chip esp32s3 write_flash 0x0 dc-picolink-esp32-<version>-merged.bin`
(hold BOOT while plugging in the board if it isn't found).

## Use

Join **DC-PicoLink-XXXX** (password `dreamcast`, change it in menuconfig) and open
http://192.168.4.1. Save your home network under *Wi-Fi*; the page is then also on the
address shown there. The setup network stays on.

## Internet for the Dreamcast over serial (E-3)

Dial anything from a serial-PPP program on the Dreamcast: the ESP32 answers like a modem
(`AT` → `OK`, `ATD…` → `CONNECT 33600`), then runs the PPP server. The Dreamcast gets
10.64.0.2, the ESP32 (10.64.0.1) is its gateway and DNS, and everything else is NAT'd out
through your Wi-Fi. What a DreamPi does around that, and how it is done here:

| DreamPi | ESP32 |
|---|---|
| fake Hayes modem (Python) | `../firmware/hayes_model.h`, `main/ppp_srv.c` |
| pppd, no login needed | lwIP PPP server (`main/ppp_srv.c`) |
| dnsmasq: revival servers 46.101.91.123 / 209.50.50.129, `dreampi_dns.conf`, domain-needed, bogus-priv | `main/dnsd.c`, `../firmware/dns_model.h`; the file is fetched from dreamcast.online when Wi-Fi is up and every 6 h |
| iptables MASQUERADE | lwIP NAPT on the PPP interface |
| iptables TTL 64; drop double SYN from ports 3200-3205 (PowerSmash) | `../firmware/hdlc_model.h`, applied to the PPP frames before lwIP sees them |
| `dcnow.py` every 15 s | `main/dcnow.c`, `../firmware/dcnow_model.h`; can be switched off on the page |

Not yet: the `dcnatrules` list, UPnP and the game helpers (E-5), DCNET (E-6), the VPN (E-7), the
modem (E-9). Games that need an incoming connection will not work until E-5/E-7.

Tests: `tools/run-tests.sh` (Hayes, frame filter, DNS and DCNow! rules) and
`esp32/tests/run-ppp-test.sh` (ESP-IDF's lwIP PPP server on the PC, talked to like a
Dreamcast: LCP, IPCP with address and DNS, ping, hang up). The real Dreamcast is still to be tried.

## Internet over the cable's network (E-4)

With the cable in W5500 mode (the Dreamcast uses its network adapter driver) and **no computer
mounting the cable on USB**, the cable's Ethernet frames arrive here. They are the wire of a second
interface, `192.168.99.1/24` (the address a DreamPi uses on the cable too): a DHCP server hands
the Dreamcast `192.168.99.2` and up, this box is its gateway and DNS, and NAT sends it out
through your Wi-Fi. The same TTL / PowerSmash rules and the same DNS and DCNow! code as the serial
side apply (`../firmware/ipfilter_model.h`). Difference to a Pi: the Pi gives out the revival DNS
server directly, here the DHCP offer names this box, so DCNow! can see which game is looking up
its server. Static settings also work: IP 192.168.99.60, mask 255.255.255.0, gateway and DNS 192.168.99.1.

Both sides can be used in the same session; DCNow! reports run while either is active.
Not yet: the `dcnatrules` list, UPnP and the game helpers (E-5), DCNET (E-6), a wired uplink (W5500 on the ESP32).

## Files

| | |
|---|---|
| `main/link.c` | UART1 link: reader task, packet queue, asks the cable `get hello` every second |
| `main/wifi.c` | setup network + home network (NVS) |
| `main/ppp_srv.c` | Hayes stub, PPP server, NAT, call control |
| `main/dc_eth.c` | the cable's W5500 network as an interface: DHCP, NAT, filter |
| `main/dnsd.c` | DNS server for the Dreamcast, downloaded DNS file |
| `main/dcnow.c` | DCNow! reports |
| `main/web.c` | the page and `/api/*` |
| `main/main.c` | status-line ring, serial monitor ring, start-up |
| `../firmware/link_model.h` | framing, shared with the cable (tested in `firmware/tests/test_link.c`) |
| `../firmware/hayes_model.h`, `hdlc_model.h`, `ipfilter_model.h`, `dns_model.h`, `dcnow_model.h` | the E-3/E-4 rules, host-compilable (tests `test_hayes.c`, `test_hdlc.c`, `test_ipfilter.c`, `test_dns.c`) |
