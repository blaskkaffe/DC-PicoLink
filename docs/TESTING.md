# Hardware test checklist (zero-stack: RP2040-Zero + ESP32-S3 SuperMini)

Firmware: Zero `dc-picolink-zero-stack-250mhz.uf2` (release; the `-debug` one logs more),
ESP32 `dc-picolink-esp32-zero-stack-modem-merged.bin` (USB modem port) or `-nomodem-`
(USB socket stays the ESP32's console). Everything is read on the page (`http://192.168.98.1`
on the setup network, or the box's home-network address). Send the status lines of anything
that fails.

## 0. The stack itself

- [ ] Power from the ESP32's USB only (Zero fed through the 5V pin, no USB on the Zero): page
      shows `link: up`. If not: is the Zero's LED on? What does the CABLE box say?
- [ ] Power from the Zero's USB only: `link: up`.
- [ ] Power from the Dreamcast's serial port only (once wired): `link: up`.
- [ ] `esp32 link self-test: PASS, 2000 packets ... KB/s` after each power-up.
- [ ] `heap` in `/api/status`: free > 40 KB, min > 10 KB after an hour with a call.
- [ ] Log stays readable: no line repeating every few seconds while nothing happens.

## 1. Internet side: Wi-Fi and Ethernet (ESP32)

- [ ] Setup network `DC-PicoLink-XXXX` appears; page loads.
- [ ] Save home Wi-Fi: `Wi-Fi: connected to ..., address ...`; survives a restart.
- [ ] `DNS: N overrides and M extra servers from dreamcast.online` (or a reason it failed).
- [ ] Router intercepting DNS: `DNS: your router answers DNS questions itself ...; asking ... over TCP`.
- [ ] `public address a.b.c.d` line, or the "IPv4 is shared by your provider" line.
- [ ] VPN: `up to <server> for N s, tunnel address ..., N ms to the gateway`; `VPN: switched on
      by itself` if the IPv4 is shared; switching it off on the page sticks after a restart.
- [ ] Netlink: `N game server dial codes from netlink_config.ini`.
- [ ] `home network: the Dreamcast gets 192.168.x.98, this box is .99 ...` once Wi-Fi is up (as a DreamPi picks them).
- [ ] Port forwarding line: `router ... forwards 216 game ports to the Dreamcast's address 192.168.x.98` (public IPv4) or the shared-IPv4 text. The router's UPnP list shows .98, no Netlink ports, nothing for this box.
- [ ] If the router refuses mappings for another device's address (UPnP "secure mode"), the line says `refused`; DreamPi has the same limit.
- [ ] `http://dreampi.local:1998` shows `mac_address` / `is_enabled`; dreamcast.online/now/configure
      finds the box and the name can be set.
- [ ] Wired uplink (W5500 module on the ESP32, cable to the router): wired line shows `uplink`
      with a DHCP address; Internet still works with Wi-Fi switched off.

## 2. Serial cable (Dreamcast serial port -> Zero -> ESP32 modem emulation)

- [ ] Dreamcast on: CABLE box `link.mode serial`.
- [ ] A game or browser set to "modem" dials: `Dreamcast dialled <number>; starting PPP`,
      `PPP is up: Dreamcast 192.168.x.98, DNS and gateway 192.168.x.99` (or the tun+1 address with the VPN up), then
      `home network: the Dreamcast is 192.168.x.98 there; this box answers for it (proxy ARP), no NAT`.
- [ ] The router's device list shows .98 (with this box's MAC); from a computer at home, `ping 192.168.x.98` answers while the Dreamcast is online (if the game answers pings).
- [ ] Web browser loads pages; DNS line counts questions.
- [ ] During a big download over serial, `queue_full` in `/api/status` (ppp) may count up, but other traffic (the page, the VPN, a second game) keeps answering at normal speed.
- [ ] Serial queue: Quake III's lagometer and ping with and without a download in a web browser
      at the same time are close (before: the ping jumped by up to a second). A game alone: as before.
- [ ] VPN auto: the log has `VPN: ping <server> N ms, ...` and connects to the first one listed;
      with ICMP blocked to all of them, `no answer` everywhere and shumania is used.
- [ ] Phantasy Star Online connects; Quake III master list loads.
- [ ] DCNow!: the box shows up as online while the game is connected.
- [ ] DCNow! ID on a new box (before any Wi-Fi was saved): typing a MAC works; after the first
      Wi-Fi connection the field is gone, and typing through `/api/net` is refused with a status line.
- [ ] *Clone DreamPi* with a real DreamPi on the network (empty address, then its address):
      `cloned the DreamPi at ...`; *DCNow! ID* switches between this box and the clone; with this
      box's own `dreampi.local` answering only, the clone says `only this box answered`.
- [ ] Mode *only while it is away*: with the cloned DreamPi on, the box reports with its own ID
      (`reporting with this box's own ID`); power the DreamPi off: after ~90 s and no running
      session `reporting as ... (the other device's ID)`; power it on: back to the own ID.
- [ ] Peer-to-peer game (ChuChu Rocket!, Planet Ring): with the VPN on a shared IPv4.
- [ ] DCNet: ISP password `password`, dial with `*69` in front: `DCNet: connected to <AP>`.
- [ ] Hang up and dial again (no restart needed).
- [ ] Fast serial (dcload-serial 1.5 Mbaud from a computer on the Zero's USB).
- [ ] No Dreamcast, computer on the Zero's USB: `screen` the port, `AT` -> `OK` from the ESP32.

## 3. W5500 emulation and SD card on the Dreamcast's serial port (Zero)

- [ ] A W5500-capable program (KOS/dcload-ip build for the serial-port W5500): `mode: ... SPI`,
      DHCP lease (cable network line: `DHCP leases 1 (192.168.99.x)`), pings / page loads.
- [ ] Long transfer: no `spi: ... short/aborted frames` error line appears.
- [ ] DCNow! session starts with Ethernet traffic.
- [ ] SD card in the slot at power-up: `SD card: N MB SDHC/SDXC in the slot`.
- [ ] A loader (openMenu, DreamShell) lists the card; a game loads from it.
- [ ] Writing (a save, a file copy) and reading it back after a restart.
- [ ] Card pulled: `SD card: removed or not answering`; put back: found again within ~2 s.
- [ ] No card: one line `SD card slot: no card (no answer)`, nothing repeating.

## 4. USB modem (Dreamcast modem -> phone cord -> USB voice modem on the ESP32)

- [ ] Modem plugged in: `USB modem vvvv:pppp plugged in`, `dial tone on`.
- [ ] Dreamcast dials: `the Dreamcast dialled <number>; answering in 8 s`, `CONNECT ...`, PPP up.
- [ ] Browser, PSO, Quake III master list; DCNow! online.
- [ ] DCNet: `*69` before the number (or "every call"): `DCNet: connected`.
- [ ] Hang up on the Dreamcast: `the line dropped` / `ended the call`, dial tone back.
- [ ] Modem unplugged mid-call: `unplugged during the call`; plugged back: dial tone again.
- [ ] Netlink game server code (e.g. Sega Rally 2 `0352002700`): `<name>: connected to ...`.
- [ ] Netlink direct dial between two boxes (or a box and a DreamPi): `#a*b*c*d#` on one,
      `0`/`00` (waiting) on the other: `Netlink: connection established`, ping line.
- [ ] Netlink matchmaking: `#0xx#` waiting / `#xxx#` calling: `matched with ...`.
- [ ] XBAND server number from a Genesis/SNES XBAND (if you have one): `XBAND: connected to the server`.
- [ ] Capcom matching number `0642542154` (Japanese Capcom games): PPP up.

## 5. BBA over the W5500 module (real Broadband Adapter -> Ethernet -> W5500 on the ESP32)

- [ ] Cable from the Dreamcast's BBA: after ~10 s the wired line shows `BBA`, DHCP lease
      `192.168.97.x` to the Dreamcast.
- [ ] BBA games (PSO, Quake III, browser with BBA): Internet through Wi-Fi.
- [ ] DCNow! session with BBA traffic.
- [ ] `home network: the Dreamcast (192.168.97.x here) is 192.168.x.98 at home` while the BBA is in use; the router's UPnP list still points at .98.
- [ ] Swap the cable to the router: the role changes to `uplink` without a restart.
