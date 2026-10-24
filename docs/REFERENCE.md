# DC-PicoLink technical reference

How each part works today, in detail. For the big picture see [ARCHITECTURE.md](ARCHITECTURE.md);
for what changes next see [ROADMAP.md](ROADMAP.md).

> **SD card:** this describes the current implementation (blocks served from a disk image
> on the host by `dcsd`). It is planned to move to a physical SD slot on the board
> (ROADMAP.md); the Dreamcast side of the emulation stays the same.

One RP2040 on the Dreamcast serial port (through a DC SD Adapter V2), seen by the
PC/Pi as a composite USB device:

* **Serial port** (CDC-ACM interface 0, `/dev/dccable`) – the coder's cable: dcload-serial / dc-tool-ser,
  KOS debug output, and DreamPi's serial PPP (Netlink, see below)
* **SD card link** (vendor USB interface 2, no tty) – block link to `dcsd` on the Pi; the
  Dreamcast sees an SDHC card whose contents are a disk image on the Pi. It is deliberately
  *not* a serial port: DreamPi's modem detection (`wvdialconf`) opens every `/dev/ttyACM*`
  and would otherwise inject `AT` commands into it
* **Network interface** (CDC-NCM, `dcusb0`) – Ethernet frames for an emulated WIZnet W5500

The firmware switches automatically, depending on what the Dreamcast is doing:

| Dreamcast does…                                         | Mode   | Goes to                        |
|---------------------------------------------------------|--------|--------------------------------|
| normal SCIF UART (dcload-serial, KOS dbgio, serial PPP) | serial | `/dev/dccable`                 |
| KOS SCIF-SPI, SD driver (`sd_init`, `/sd`)              | SD     | image file via `dcsd`          |
| KOS SCIF-SPI, W5500 driver (`eth_init`)                 | W5500  | `dcusb0` network               |

Serial vs SPI: the 1k resistor from GPIO6 to GPIO2. In serial mode GPIO6 drives DC
CTS and GPIO2 reads it back. When the DC turns CTS into its SPI clock, GPIO2 stops
following GPIO6. After a burst of 16 clock edges within 1 ms, the firmware switches
to SPI. Games that drive CTS themselves (AFO does) don't trigger it. Once the DC
has released the line for about 200 ms, it switches back to serial mode.

SD vs W5500, per frame: an SD host always starts with 0xFF bytes followed by a command
with a valid CRC7. W5500 frames start with an address byte, which KOS never sends as
0xFF for register access.

## How this relates to DreamPi's serial support (Netlink)

DreamPi 2 already ships eaudunord's
[Netlink tunnel](https://github.com/eaudunord/Netlink). Its `netlink.py` watches a
USB-serial adapter (default `/dev/ttyUSB0`, 115200). When the Dreamcast "dials" over
the serial cable, it answers `CONNECT` and runs `pppd` with hardware flow control.
That gives the DC internet over the coder's cable. This firmware's **serial mode is
exactly that kind of adapter**, so Netlink works with it unchanged. Point it at this
cable in `netlink_config.ini`:

```ini
[Serial Port]
port = /dev/dccable
baud = 115200
```

Notes:
* Only one program can own the serial port: stop DreamPi's Netlink monitoring
  (`disabled = yes`) while using dc-tool-ser, and vice versa.
* 115200 and below are generated exactly (like FTDI/CP210x), because DC-side
  software rounds 115200 differently (111607 or 120192); the cable measures the DC
  and follows it (see auto-baud below). Rates above 115200 are set exactly the way
  dcload-serial programs the DC (`SCBRR = 50000000 / (32·bps) − 1`, integer, 8 bit),
  so there is no mismatch: `-b 1500000` and `-b 1562500` both give 1562500, the DC's
  fastest rate. Above 1562500 that divider wraps to 255 and the DC really runs at
  6103 baud (e.g. `-b 1600000` from CP2102N guides); the cable matches that too and
  logs a hint to use 1500000 or 1562500.
* Netlink's code comments mention keeping the adapter's TX low (a serial *break*)
  so the DC's RX pin isn't high during boot. This firmware handles that itself: its
  pins stay high-Z until the DC is up (see Boot safety). A break request from the
  host is ignored.
* The **W5500** and **SD card** parts are separate from Netlink. W5500 is a second way
  online, as Ethernet, used only by KOS homebrew/DreamShell built with the W5500 driver.
  Serial PPP through Netlink doesn't need it. If you only want Netlink plus the coder's
  cable, you can skip `dcusb0` (and `dcsd`) on the Pi.

## Wiring

Full instructions, with diagrams, SD/microSD contact numbers and the optional
3.3 V sense wire: **[hardware/WIRING.md](../hardware/WIRING.md)**.

| GPIO | SD contact | Dreamcast | serial mode   | SD / W5500 mode |
|------|------------|-----------|---------------|------------|
| 2    | 5 CLK      | CTS       | sense         | SCLK in    |
| 3    | 2 CMD      | TXD       | UART RX       | MOSI in    |
| 4    | 7 DAT0     | RXD       | UART TX       | MISO out   |
| 5    | 1 DAT3/CS  | RTS       | CTS in        | CS in      |
| 6    | –          | via 1k → GPIO2 | RTS out  | released (probe only) |
| GND  | 3/6 VSS    | GND       |               |            |
| 7    | 4 VDD via 10k | DC 3.3 V | optional power sense (found automatically) | |

Do not connect the Dreamcast's 3.3 V to the RP2040's 3V3.

GPIO4 (the only output to the Dreamcast) uses the default pad settings (slow slew, 4 mA).
net7 tried fast slew and 8 mA; that made cable-to-Dreamcast data unreliable on the
adapter wiring (corrupted SD reads and network data), so it was reverted.

### Why PIO and not the RP2040's SPI/UART blocks

The pins are fixed by the adapter's SD pinout, and the built-in peripherals can't
serve them. The SPI block as a slave receives on GPIO4 and sends on GPIO3, the
opposite of the adapter's wiring. Even with the wires swapped it can't act as a
W5500: in mode 0 it needs CS released between bytes, it changes MISO on the
falling edge (the Dreamcast then reads the previous bit), and it needs its own clock
≥ 12× the SPI clock. No UART can receive on GPIO3, and UART1's transmit pin
(GPIO4) can't use GPIO5 as CTS, so hardware flow control would be lost. The PIO
programs do all of this. The current pin order also helps them: CLK directly below
MOSI lets the W5500 program read the clock and the first data bit in one snapshot,
and four pins in a row make the logic capture a single instruction.

## What works with W5500 mode

* KallistiOS programs built with a recent KOS (`w5500_adapter.c`, 2025+) and `INIT_NET`:
  `eth_init()` tries BBA, then LAN adapter, then W5500 (SCIF first) automatically.
* DreamShell (calls `w5500_adapter_init`).
* **Not** commercial games: they talk to the modem (use DreamPi normally) or the BBA.
* The W5500 path uses the serial port as its SPI bus. While it is active there is no
  serial console, and KOS refuses SCIF-SPI when a program was loaded through
  dcload-serial. Boot W5500 programs from disc, SD, or dcload-ip.

Emulation scope: socket 0 in MACRAW mode with 1/2/4/8/16 KB buffers, the MAC filter,
soft reset, PHY link status (up while USB is enumerated), and VERSIONR = 0x04. Other
sockets accept register writes but do nothing.
`firmware/tests/test_w5500.c` replays the KOS driver's SPI traffic against the model:
`gcc -O1 -I firmware firmware/tests/test_w5500.c -o t && ./t`.

## Serial receiver

Data from the Dreamcast is sampled FTDI-style:
* 16 clocks per bit;
* a start bit must still be low at its middle, so glitches don't start a false character;
* each data bit is sampled three times around its centre and decided by majority;
* the stop bit is checked, and characters with a bad one (framing errors) are
  counted and dropped;
* after each character the receiver waits for the line to go idle. A break (TX held
  low, which AFO does between dial attempts) counts as one framing error, not a
  stream of zero bytes.

A single noisy sample can't flip a bit. That matters on the Dreamcast's TX line
through the SD adapter: with single mid-bit sampling, about a quarter of AFO's PPP
frames arrived damaged.

## Auto-baud and serial diagnostics

Over USB the baud rate is only a label, so the cable can follow the Dreamcast. A PIO
state machine times every low pulse on the DC's TXD line (5 ns resolution) and
derives the DC's real bit rate from the shortest pulses. If the measured rate is
more than 2.5% away from the wire rate, the UART switches to it. With framing errors
present, two agreeing measurement windows are enough. Without them it takes four
clean windows, each with plenty of single-bit pulses, so one bad measurement can't
re-tune a working link. The new rate is snapped to a real SCIF rate (`50 MHz / (32·4ⁿ·(N+1))`) and used in both
directions. The host side (DreamPi/Netlink at 115200, a terminal, …) needs no
changes. KOS programs and Alien Front Online, for example, send at 120192 baud
when asked for 115200, because the SCIF divider truncates. At that 4% mismatch most
bytes still decode, so `AT` commands look fine, but about 1% are damaged. That's
enough to lose most large PPP frames, so following matters even without framing
errors. This applies only below 400 kbaud: dc-tool-ser's fast speeds are always
set explicitly. A host baud change cancels auto-follow, and auto-follow pauses for
1.5 s afterwards. `-DDC_AUTOBAUD=0` keeps measuring and reporting but never switches.

The firmware reports through the SD link, and `dcsd` logs it
(`journalctl -u dcsd -f`):
```
cable: serial: DC transmits at ~57712 baud (nearest SCIF rate 57870); wire 115200, host asked 115200
cable: serial: following the DC at 57870 baud (host asked 115200)
cable: serial: 12 framing errors from the DC so far (wire 57870 baud, host 115200)
cable: mode: serial -> SPI (SD/W5500)
cable: serial: PPP frames from the DC as received by the cable: 89 good, 0 bad checksum
```
The PPP line checks every frame's checksum inside the cable. Compare it with
`ip -s link show ppp0`: if the cable sees good frames but pppd counts RX errors,
the damage happens after the cable (USB / host side).
If framing errors keep rising *after* the DC's rate is followed, the DC isn't
sending 8N1 (e.g. 7-bit or parity), which is worth knowing when debugging a game.

## SD card emulation

The Dreamcast sees an SDHC card (SPI mode, CRC on, block addressing) in the SD
adapter. Every block is read from and written to a disk image on the Pi
(`/var/lib/dcsd/dcsd.img` by default; MBR + one FAT32 partition, created on the first
start). Works with KallistiOS `sd_init()` / `fs_fat` (`/sd`), DreamShell, and
anything else that uses KOS's SCIF-SPI SD driver.

* Reads: the RP2040 fetches up to 8 blocks ahead from the Pi during multi-block
  reads. The DC polls up to ~100 ms for each data token, and a Pi round trip is
  ~1–3 ms.
* Writes: each block is acknowledged to the DC only after the Pi has written it
  (the card shows "busy" meanwhile). The Pi syncs to disk every second.
* No Pi daemon or no image: the card answers CMD0 but rejects initialization, so
  KOS reports "no card" after a few dozen bytes instead of timing out.
* Speed is limited by the serial port's bit-banged SPI (roughly ≤0.5 MB/s reading),
  so it suits homebrew, saves and DreamShell files rather than whole-disc images.

Edit the card from the Pi (the DC sees "no card" while it's mounted; restart the DC
app afterwards so it re-reads the FAT):
```sh
sudo dcsd-mount            # mounts at /mnt/dcsd
cp mygame.elf /mnt/dcsd/
sudo dcsd-umount
```
Tests: `firmware/tests/test_sd.c` runs KOS's `sd.c` read/write/init code byte for byte against
the card model, with the PIO's timing. Pass `host/dreampi/dcsd.py image` to run it
end-to-end against the real daemon over a pty (`dcsd.py --device` uses a tty
instead of USB, for testing).

## Forcing a mode

Normally the cable works out by itself whether the Dreamcast uses the port as a
serial port or as an SPI bus (W5500 / SD card). To rule detection out, force it
on the Pi before starting a game (takes effect within a second, and is kept
until you change it, also across reboots):

    sudo dcsd-mode w5500    # SPI, W5500 only (W5500 games / test tools)
    sudo dcsd-mode sd       # SPI, SD card only
    sudo dcsd-mode spi      # SPI, W5500 and SD chosen per transfer
    sudo dcsd-mode serial   # coder's cable only, never switch to SPI (AFO, Quake 3 ...)
    sudo dcsd-mode auto     # back to automatic
    dcsd-mode               # show the current setting

The cable logs `mode setting from the Pi: ...` in `journalctl -u dcsd`. In a
forced SPI mode it switches to SPI as soon as the Dreamcast is on and never
falls back to serial; in `serial` it never switches to SPI.

## Online play over the Dreamcast Live VPN (dcusb0-vpn)

DreamPi gives a modem Dreamcast its own address on the Dreamcast Live VPN (the Pi's
`tun0` address + 1), so servers and other players reach it directly on every port.
`dcusb0-vpn.service` does the same for the W5500 Dreamcast: that VPN address is
mapped 1:1 to the Dreamcast on `dcusb0` (all ports, both directions), and TCP MSS is
clamped to the VPN's smaller MTU. Without it the Dreamcast sits behind the Pi's own
VPN address, and new incoming connections (hosting, peers joining: AFO, Worms World
Party, Quake 3, Unreal Tournament) are dropped by DreamPi's firewall.

It follows changes by itself (VPN reconnect, new DHCP lease per game) and steps aside
while a modem Dreamcast is online through DreamPi (`ppp0`), whose address it is then.
Log: `journalctl -u dcusb0-vpn`. Off: `VPN_MAP=no` in `/etc/dcusb0.conf`, then
`sudo systemctl restart dcusb0-vpn`. Without the VPN nothing is mapped (outbound NAT only).

## Logic capture (diagnostics)

The cable can record its four bus pins (CLK, MOSI, MISO, CS) like a small logic
analyzer: a 1.3 ms ring at 50 MHz. Once the driver is past its probe (5+ common-register
accesses), it stops 300 µs after the bus goes quiet. Otherwise it stops after 60 s.
Only in builds with the diagnostics (`DC_DIAG=1`, the default); a `DC_DIAG=0` build
logs that the capture isn't available.

    sudo dcsd-capture      # then start the program on the Dreamcast
    journalctl -fu dcsd    # decoded SPI frames appear here

Each frame is logged with its length, clock count, gap to the previous frame and
the MOSI/MISO bytes, plus the narrowest CLK and CS pulses seen (glitch check). The
raw capture is saved as /var/lib/dcsd/capture-<time>.bin, .txt and .vcd (open the
.vcd in PulseView/GTKWave).

## Boot safety

Some Dreamcasts hang on a black screen if their serial pins see voltage while they
reset or boot. The firmware therefore starts with every Dreamcast-facing pin high-Z
(input, no pull-ups, nothing driven). It only goes live once the Dreamcast is up, and
releases them again after the Dreamcast powers off.

* Without the sense wire, the Dreamcast counts as up once it drives its own TXD or RTS high.
  That only happens after software (dcload, KOS) has set up the serial port. It
  counts as off after both have been low for 30 s. Some games, Alien Front Online
  among them, park the port low for 10–20 s between dial attempts.
* With the optional sense wire (the SD adapter's VDD pin = DC 3.3 V, through 10k to
  GPIO7, see WIRING.md) the same firmware switches to it by itself: once GPIO7 has been
  high for 100 ms, the Dreamcast's 3.3 V decides. Pins go live 700 ms after 3.3 V appears
  (GRESN is released ≥400 ms after power-up) and drop within 20 ms of power-off. This lasts
  until the RP2040 loses power. Without the wire GPIO7's pull-down keeps it low and
  nothing changes. If the wire comes loose (the DC drives TXD/RTS high for 2 s with
  GPIO7 low), the cable goes back to TXD/RTS sensing. Until then it treats the
  Dreamcast as off (pins high-Z), so a loose wire costs a 2 s dropout. `-DDC_POWER_PIN=-1` turns the feature off.

## Speed

The Dreamcast is the SPI master and the RP2040 can't stretch the clock. So on the
Dreamcast side this runs exactly as fast as a real W5500: DC-SWAT measured SCIF-SPI at
~1000 KB/s for TX and ~550 KB/s for RX. The other limit is USB full-speed. CDC-NCM on an
RP2040 tops out around 1 MB/s, so DC→Pi bursts may be held at roughly that level.

At ~1 MB/s one SPI bit is ~120 ns. The tightest point is answering the first byte of
a read: the block-select bits arrive ~3.5 bit-times before the Dreamcast samples MISO,
which leaves ~0.43 µs. The PIO program pushes address and block select as one word
after the 21st bit and pulls the answer only once SCLK has gone low, at the last
moment; core1 polls with interrupts off and answers in well under that.

Frame boundaries are found by the PIO program itself, like the real chip: between
two bytes (while SCLK is low) it checks CS, and CS high for more than ~20 ns ends
the frame (an end-of-frame marker word goes into the RX FIFO). Back-to-back frames
with a very short CS-high gap are therefore never merged, and the CPU never has to
restart the shifter in time. Anything that doesn't end on a byte boundary (frames
shorter than the header, the floating CS line after the DC releases the port) is
cleaned up by a watchdog in core1's loop. `firmware/tests/pio_sim.py` runs the assembled
program cycle by cycle against KOS-like waveforms:
`python3 firmware/tests/pio_sim.py build/w5500_spi.pio.h`.

Status: KOS networking (JaderFox W5500 tester, RuneScape login, ping) works on the
250 MHz build. The 250 MHz build is the recommended one.

What the counters in `journalctl -u dcsd` mean:
* **reads answered late**: the first byte of a read went out as `ff` because core1
  answered too late. The rest of the frame stays aligned. Late `0001` (socket
  command register) polls are harmless, because KOS polls again.
* **burst underruns**: a byte in a long read went out as `ff` (core1 fell behind
  mid-burst).
* **RX resyncs**: the driver wrote an Sn_RX_RD beyond the received data or inside a
  frame (a misread length). The buffered frames are dropped and reading restarts at
  the next frame boundary, so the driver can't loop on the bad pointer. Each one costs
  one or a few packets; it should be rare (the first few per session are logged with
  details as `RX resync N: ...`).
* **short/aborted frames**: CS activity that isn't a whole frame. Most of it is the
  KOS console text on TXD/RTS while the SPI driver is shut down. Harmless unless it
  keeps rising while a program is using the network.

## Raspberry Pi / DreamPi side

Nothing switches on the Pi. The serial port and the network interface are always
there, and the RP2040 routes data to whichever one the Dreamcast is using.
Everything the Pi needs is in `host/dreampi/`:

| File | Installed to | Purpose |
|------|--------------|---------|
| `70-dc-cable.rules` | `/etc/udev/rules.d/` | names the network interface `dcusb0`, adds the `/dev/dccable` serial symlink, starts the service on plug-in |
| `dcusb0.service` | `/etc/systemd/system/` | runs while the cable is plugged in, stops when it's removed |
| `dcusb0-up.sh` | `/usr/local/sbin/` | gives `dcusb0` the address 192.168.99.1/24, enables IP forwarding, adds NAT (iptables, or nft if iptables is missing) |
| `dcusb0-dnsmasq.sh` | `/usr/local/sbin/` | DHCP-only dnsmasq for the Dreamcast (`--port=0`, so it never conflicts with another DNS server on port 53) |
| `dcusb0.conf` | `/etc/` | subnet, DHCP range and DNS server handed to the DC (kept on reinstall) |
| `dcsd.py` | `/usr/local/bin/` | SD card server: finds the cable by USB ID and serves the image over its vendor interface (Python 3 stdlib only, Linux usbfs) |
| `dcsd.service` | `/etc/systemd/system/` | runs `dcsd.py`; creates the image on first start |
| `dcsd.conf` | `/etc/` | image path, size for a new image (default 2G), `--readonly` |
| `dcsd-mount` / `dcsd-umount` | `/usr/local/sbin/` | mount the image's FAT32 partition at `/mnt/dcsd` to edit it on the Pi |
| `dcsd-mode` / `dcsd-capture` | `/usr/local/sbin/` | force a mode / arm the logic capture |
| `dcusb0-vpn` + `.service` | `/usr/local/sbin/`, `/etc/systemd/system/` | the Dreamcast's own Dreamcast Live VPN address (see above) |
| `install.sh` / `uninstall.sh` | – | sets up / removes all of the above |

Install (copy the `host/dreampi` folder to the Pi first, e.g. with `scp -r host/dreampi pi@dreampi.local:`):
```sh
cd dreampi
sudo ./install.sh
```
The installer:
* installs `dnsmasq-base` and `dosfstools` if they're missing. That's just the binary; no system-wide dnsmasq service gets enabled.
* tells dhcpcd (`denyinterfaces dcusb0`) or NetworkManager (`unmanaged-devices`) to leave `dcusb0` alone, whichever one is present.
* uses NAT and finds the uplink by itself (eth0 or wlan0 both work), so DreamPi's own network setup and the modem/PPP path are untouched.

Check it:
```sh
ip addr show dcusb0              # 192.168.99.1/24
systemctl status dcusb0          # dnsmasq running
journalctl -u dcusb0 -f          # DHCP lease when the Dreamcast app starts networking
ls -l /dev/dccable               # serial port (dc-tool-ser -t /dev/dccable)
journalctl -u dcsd -f            # SD card server
```
To change the subnet or DNS, edit `/etc/dcusb0.conf`, then run `sudo systemctl restart dcusb0`.
The DNS server handed out by default is 46.101.91.123 (the Dreamcast community DNS, which
resolves the games' master and login servers). Set `DNS=` empty to use the Pi's first
upstream nameserver from `/etc/resolv.conf` instead.

Static settings on the Dreamcast (games with their own broadband settings, e.g. Quake 3):
IP 192.168.99.60, mask 255.255.255.0, gateway 192.168.99.1, DNS 46.101.91.123.

Or use the single file `dc-picolink-dreampi-setup.sh` from a release (all of
`host/dreampi/` in one self-extracting script, made by `tools/make-dreampi-setup.sh`):
`sudo bash dc-picolink-dreampi-setup.sh`.
Remove everything with `sudo ./uninstall.sh`.

The Dreamcast side uses DHCP unless a static ISP config is stored in its flash.

## Build

Pico SDK 2.1.1. Runs from RAM at 200 MHz by default (`PICO_USE_FASTEST_SUPPORTED_CLOCK`);
the 250 MHz build (core voltage 1.20 V) gives the W5500 answer path more margin and is
the recommended one.
```sh
cmake -S firmware -B build -DPICO_BOARD=pico -DDC_SYS_KHZ=250000 && make -C build   # 250 MHz
cmake -S firmware -B build200 -DPICO_BOARD=pico && make -C build200                 # 200 MHz
```
Options: `-DDC_POWER_PIN=-1` turns off the sense-wire detection (default GPIO7).
`-DDC_BAUD_MATCH=0` turns off
Dreamcast baud rounding. `-DDC_AUTOBAUD=0` measures but never follows the DC's rate.
`-DW5_VERSIONR_VALUE=0x..` (diagnostics) changes the W5500 version register.
`-DDC_DIAG=0` leaves out the heavy diagnostics (−44 KB RAM): the PPP frame checker,
the logic capture, the per-frame list and the late-read detail. The counters in the
status lines (frames, late reads, underruns, bad RECV pointers, framing errors) stay.

Release files (GitHub releases, built by CI): `dc-picolink-<version>-250mhz.uf2`
(release: diagnostics off), `-250mhz-debug.uf2` and `-200mhz-debug.uf2` (diagnostics on).
All work with or without the sense wire. The first log line after connecting
(`cable: DC-PicoLink firmware <version> (<date> <time>) ready; ...; power sense ...`)
shows which build is running and whether the wire is in use.
Setting the serial port to 1200 baud and closing it reboots the board into BOOTSEL.
