# Development log

The working notes kept while the firmware was brought up (Oct 2026), unchanged apart from
this header. Build names (net5, net7, net8c…) refer to test builds before the repository
existed; file paths refer to the old layout (`source/` = today's `firmware/` + `host/`).

---

# RP2040 Dreamcast cable — handover summary

Attach the whole handover zip (this file, `source/`, `firmware/`, the Pi setup script) to a new chat.
README.md describes the design and WIRING.md the wiring. This file is the state of
the work.

## Current state (23 Oct 2026)

| Part | State |
|------|-------|
| Serial coder's cable (dcload, KOS console, DreamPi/Netlink PPP, AFO) | works (last regression-checked before the W5500 work; recheck pending) |
| W5500 emulation (KOS `w5500_adapter.c` → USB CDC-NCM `dcusb0`) | **works**: JaderFox tester, ping, RuneScape connects and logs in (net6, 250 MHz, 0 late / 0 underruns / 0 bad RECV) |
| SD card emulation (dcsd image on the Pi) | never tested on hardware since the MISO timing change |
| Pi side (`dreampi/`, `dc-cable-pi-setup.sh`) | works; dcusb0 192.168.99.1/24, DHCP .10–.50, DNS 46.101.91.123 |

**Latest firmware: `net7`** (23 Oct, 10:07), with the "safe updates" from the code review:
* GPIO4 (MISO / UART TX to DC RXD): fast slew rate + 8 mA drive (was slow / 4 mA).
* W5500 header hot path: only the four candidate answers are computed between the
  address word and the block-select word. The diagnostics (SysTick timestamps,
  FIFO level, loop counters) and the frame-state stores now run after the answer.
* `uart_tx`: stop bit exactly 1 bit (was 9/8: `pull side 1 [6]` + the CTS wait).
* Dead code removed: the `DC_FLOW_CONTROL` option (always on; the nop-patch path was
  unused) and the `uart_pio->irq = fe_bit` write (no PIO program raises that IRQ).
* Files: `dc_cable_net7.uf2` (200 MHz), `dc_cable_net7_250mhz.uf2`,
  `dc_cable_net7_sense.uf2`, `dc_cable_net7_sense_250mhz.uf2` (GPIO7 3.3 V sense).
* Host tests pass: test_w5500, test_sd, test_w5500_recv, pio_sim (56 checks), kos_replay.

**Pi side, 24 Oct: `dcusb0-vpn` (new; install with `sudo bash dc-cable-pi-setup.sh`).**
DreamPi gives a modem Dreamcast its own Dreamcast Live VPN address (tun0 + 1), so
peers/servers reach it on every port. dcusb0 was only masqueraded behind tun0's own
address, where DreamPi's INPUT rules drop new connections: that is the likely reason
AFO, Worms World Party, Quake 3 and UT online don't work over the W5500 (the cable
itself showed no errors in those sessions). `dcusb0-vpn.service` maps tun0+1 1:1 to the
Dreamcast's dcusb0 address (DNAT/SNAT in own chains DCUSB0_PRE/POST, FORWARD accept,
TCP MSS clamp in mangle POSTROUTING because DreamPi RETURNs tun0 traffic at the top of
mangle FORWARD), follows VPN reconnects and new DHCP leases (each game has its own
MAC), and steps aside while ppp0 is up. Dry-run tested with mocked ip/iptables only;
not yet on the real Pi. Check: `journalctl -u dcusb0-vpn` shows
`Dreamcast 192.168.99.x on dcusb0 = VPN address …`; `sudo iptables -t nat -L DCUSB0_PRE -n`.
Firmware net8f (23:38): net8e + `RX resync N:` detail lines. First one seen (DreamKarts):
a 69-byte frame's length header 0x0045 read as 0x0044 (last bit of the byte): a real
1-bit misread on the wire, recovered by the resync. KOS samples RXD after its own
CLK-high write (scif_spi_rw_byte: write CLK high, then read SCSPTR2), so the margin
after our "MISO right after the rising edge" is thin; widening it is the next PIO item.

**net8e (23 Oct 23:03, current; not yet on hardware as a whole):** everything below
(net8c) plus three changes from tonight's tests:
* **GPIO4 back to the default pad (slow slew, 4 mA) and the stop bit back to 9/8 bit**,
  i.e. net7's two cable→DC changes undone. On net7, DreamShell 4 crashed at boot
  (EXC_DATA_ADDRESS_WRITE, a bad pointer: corrupted SD data) and RuneScape wedged
  (22k bad RECVs). The A/B build net7r (= net7 with both reverted, banner 22:34:02) ran
  RuneScape 8 min clean (1829 out / 1814 in, 0 bad RECV, 0 underruns). net7p (pad only)
  was not tested separately. DreamShell on net7r/net8e: still to test.
* **SD is for the loader (openMenu) only** (`sd_locked` in spi_slave.c): once a program
  opens the W5500 socket, SD detection is off until the next SPI session or power-off.
  The status line ends in `SD allowed (loader)` / `SD off (a game opened the W5500 socket)`.
  `dcsd-mode sd` still forces SD. (net7r's RuneScape run still had 5 false SD switches:
  this removes them for good.)
* **RX resync on a bad RECV** (`w5_command`, w5500_model.h): an RX_RD beyond the data, or
  one that doesn't land on a sane 2-byte length header, drops the buffered frames and
  restarts at rx_wr (always a frame boundary). net7 put the old read position back; if
  that was itself inside a frame, KOS re-read the same garbage length forever. Status
  line: `RX resyncs`. `test_w5500_recv.c` reproduces the endless loop on the net7 model.
* Banners: `dc_cable_net8e.uf2` 23:03:06 (200 MHz), `dc_cable_net8e_250mhz.uf2` 23:03:47,
  `dc_cable_net8e_250mhz_nodiag.uf2` 23:04:28.
* Open: AFO over Netlink PPP: the link comes up (≈40 good PPP frames from the DC, no
  wire errors), then the DC goes quiet and redials. Same on net7 and net7r, so not the
  cable's TX path; look at the DreamPi/Netlink log and the AFO server side. Quake 3:
  master server never answers (8 out / 7 in, then silence): network side. openMenu
  "can't find SD card": reads 2 blocks and stops; check the image isn't mounted on the
  Pi and what openMenu expects.

**net8 candidate (`net8c`, 23 Oct, built 21:51–21:52, not yet on hardware):** the cleanup
below, done ahead of the net7 results so it is ready. **Test net7 first.** net8c only
replaces net7 once net7 is confirmed; then net8c gets the same checks.
* With `DC_DIAG=1` (the default) the core1 hot path (`w5_header`, `w5_frames`) is the
  same machine code as net7, instruction for instruction; only variable addresses
  differ, plus the underrun counter increment (2 instructions shorter). The PIO machine
  code is byte-identical (only labels were removed).
* **Sense wire found automatically (no more `_sense` builds):** GPIO7 (pull-down) high
  for 100 ms → the DC's 3.3 V decides power on/off (700 ms online delay, 20 ms
  offline) until the RP2040 loses power. Never high (no wire) → TXD/RTS sensing as
  before. Wire loose (DC drives TXD/RTS high for 2 s while GPIO7 is low) → back to
  TXD/RTS, re-found when it's high again. Runs on core0 (`sense_model.h`,
  `power_sense_task`); core1 only reads `dc_sense` in `dc_alive()` and the two
  timeouts (not in the header path). Host test: `test/test_sense.c`. Kept in RAM on
  purpose: a flash note would need a ~50 ms core0 interrupt-off erase, and the cable
  is powered from the Pi anyway. `-DDC_POWER_PIN=-1` turns it off. To check on
  hardware: banner ends `power sense wire not seen (auto)`; with the wire, DC on →
  `power sense GPIO7: found ...`; DC off → mode offline within ~20 ms.
* `DC_DIAG=0` saves 44.6 KB RAM (bss 135 → 90.6 KB) and 5.4 KB code. Its hot path is
  shorter (no SysTick timestamps, FIFO level or frame-list stores), so it needs its own
  hardware run before shipping.
* **Baud matching = dcload's formula exactly** (`baud_model.h`): above 115200 the wire
  rate is `1562500 / ((uint8_t)(1562500/req - 1) + 1)`, like dcload's
  `SCBRR = 50000000/(32*bps) - 1` in the 8-bit register. Requests above 1562500 wrap
  to SCBRR 255 = 6103 baud on the DC (e.g. `-b 1600000`), so the cable follows that
  and logs a hint to use 1500000/1562500. net7 passed >1562500 through unchanged.
  Host test `test/test_baud.c` checks every request 115201..4000000 against dcload.
  To check on hardware: `-b 1500000` → log `wire 1562500`; `-b 1600000` → the hint line.
* **False W5500 → SD switches fixed** (found in the net7 RuneScape log, 23 Oct 19:34–19:42:
  `switches to SD` went 3 → 7, `to W5500` 0 → 4, one SD frame each, during traffic
  bursts). Each one put the SD shifter on MISO for the rest of a W5500 frame (a corrupted
  RX packet; TCP retransmitted it, a UDP game would lose it). No SD image access happened.
  Two causes, both fixed in `sd_detect` / `w5_book`:
  - Detection was re-armed at every frame end, but the SD shifter's RX FIFO is only
    cleared by the realign in `w5_book` (bus idle ≥ 10 µs). In back-to-back frames the
    detector parsed stale bytes of earlier frames. Now it is armed only by that realign
    (and only if CS is still high after it), so it always sees a frame from byte 0.
  - One leading 0xFF was enough; KOS `sd_wait_ready()` always clocks ≥ 2. Now ≥ 2, which
    also rules out W5500 frames at address 0xFFxx. `test/test_sd_detect.c`: the old rule
    takes 20 W5500 frame shapes for SD commands, the new one none; every KOS SD command
    (14 commands, 1–4 extra 0xFF) is still detected.
  To check: in a long RuneScape/Quake 3 session `switches to SD` must not grow after the
  game has started.
* Same log, net7 otherwise clean over 8 min of RuneScape: 1767 frames out / 1774 in,
  0 underruns, 0 bad RECV, 0 buffer-full; 1 late read (RX buffer, after a 13 µs busy
  spell). The 644 short frames all came at the SD loader → game hand-over.
  Quake 3 (6p mod) log: 12 late reads, all the first Sn_CR poll right after a CR write
  (`w5_command` runs then, the DC polls ~4 µs later; it reads ff and polls again: harmless).
  Possible later optimisation: run the command's heavy part outside that window.
  Quake 3 master list: no reply reached the cable (8 out, 7 in, then silence) → network
  side; check with tcpdump on dcusb0/tun0, or connect by IP ("More").
* Files and banners: `dc_cable_net8c.uf2` (200 MHz, `21:51:23`),
  `dc_cable_net8c_250mhz.uf2` (`21:52:06`), `dc_cable_net8c_250mhz_nodiag.uf2`
  (`21:52:49`). Each works with or without the sense wire.
* Host tests pass on net8c: test_w5500, test_sd, test_w5500_recv, test_sense, test_baud (`-lm`), test_sd_detect, pio_sim (56/56 on
  200 MHz/250 MHz/nodiag), kos_replay.
* Build here: Pico SDK 2.1.1 + Ubuntu `gcc-arm-none-eabi` 13.2. A rebuild of net7 from
  this source gave UF2s the same size as the shipped ones.

**Next:**
1. Test net7 on both clocks: RuneScape (long session), Quake 3 (W5500 patch and 6p
   mod: master server list?), the JaderFox tester. Check the banner: `firmware Oct 23 2026 10:07:48` (200 MHz),
   `10:07:51` (250 MHz), `10:09:33` / `10:10:18` (sense builds); and that late / underrun / bad RECV stay at 0.
2. Serial regression: dcload-serial / dc-tool-ser, and DreamPi Netlink PPP or AFO
   (the new stop-bit length and pad drive affect serial too).
3. SD emulation: `sudo dcsd-mode sd`, then a KOS `/sd` program or DreamShell.
4. If net7 passes: run the same checks on `net8c_250mhz`, then on `net8c_250mhz_nodiag`.
   With net8c, also check power on/off with and without the sense wire.
5. Then the open items below (250 MHz default, `DC_DIAG` default off).

## Cleanup plan

Done in net8c:
* Old detectors behind `DC_FAST_DETECT=0` removed (the option is gone from CMake).
* Unused stats removed: `cs_glitches`, `common_t`. `book_lost` moved into `spi_stats`
  and reported (a status line, only when non-zero: the counts are then too low).
* One build option `DC_DIAG` (dcmode.h, `cmake -DDC_DIAG=0`): the PPP frame checker,
  the logic capture (a `DC_DIAG=0` build answers `dcsd-capture` with "not in this
  build"), the per-frame list, the late-read detail. The counters stay in all builds.
* PIO: unused labels `byte`, `byte_pull`, `data_push` dropped; the comment re-wrapped.
* `dcmode.h`: the `core1_beat` / `rts_stamp` comments fixed.
* Status line: "(MISO byte went out as ff)" now follows "burst underruns", where it
  belongs (it was after "bad RECV pointers").
* `dcsd-capture` said "lasts 2.6 ms"; the ring is 1.3 ms. Fixed, and the setup script's
  payload regenerated (the only payload change).
* README: `DC_DIAG` documented.
* Mac/Windows Pi support and the WinUSB descriptor were already gone in net7.

Still open (needs the hardware results):
* Make 250 MHz the default if both clocks test clean, and ship one build.
* Flip `DC_DIAG` to default 0 once nodiag tests clean (one line in dcmode.h).
* Later, not cleanup: give `sd_spi.pio` the W5500 program's own end-of-frame
  detection (it is restarted by the CPU after every frame). Do this only after SD is tested.

## Code review findings (23 Oct): pinout, hardware SPI/UART, PIO

* **Pinout is right; keep it.** The built-in SPI0 slave on GPIO2–5 would receive on
  GPIO4 and send on GPIO3, the opposite of the adapter's wiring. Even with a swap,
  the PL022 slave can't be a W5500:
  * in mode 0 it needs CS released between bytes;
  * it changes MISO on the falling edge, which is the bug fixed by the MISO-after-rising-edge change;
  * it needs a clock ≥ 12× the SPI clock.

  No UART receives on GPIO3, and UART1 TX (GPIO4) can't use GPIO5 as CTS. The PIO
  relies on the pin order: CLK is just below MOSI for the one-snapshot read in the
  gap loop, and four consecutive pins make `la_capture` one instruction.
* **PIO memory:** pio0 holds 31/32 instructions (uart_tx 5, uart_rx 10, dc_tx_meter 7,
  sd_spi 9) and uses all 4 SMs. pio1 holds 32/32 (w5500_spi 31, la_capture 1), SM0 + SM1.
* `w5500_spi` is at its minimum: the gap loop (6 instructions) is the cost of watching
  SCLK and CS with only one `jmp pin`. The input-sync bypass (−2 cycles) isn't needed
  now that there are 0 late reads.

## Hardware

Wiring: see WIRING.md. GPIO2 CLK/DC CTS, GPIO3 CMD/DC TXD, GPIO4 DAT0/DC RXD,
GPIO5 DAT3/DC RTS, GPIO6 →1 kΩ→ GPIO2, GND. The GPIO7 →10 kΩ→ SD VDD sense wire
is optional and only read by sense builds. The USB side goes to a DreamPi at
192.168.86.17 (user `pi`) with the Dreamcast Live VPN (tun0).

## Firmware structure

Pico SDK 2.1.1, `copy_to_ram`. Build: `cmake -DPICO_BOARD=pico [-DDC_SYS_KHZ=250000]
[-DDC_DIAG=0] [-DDC_POWER_PIN=7]`. `source/` is a git repo: `net7 baseline`, then
`net8 candidate cleanup`, `handover update`, `auto-detect the sense wire`, `baud: match dcload's divider`, `sd detect: no false switches` (`git log -p`).
* **core0** (`main.c`): TinyUSB (CDC serial `/dev/dccable`, vendor link for
  dcsd, CDC-NCM `dcusb0`), the UART bridge with DMA, auto-baud, the PPP checker, the
  KOS console sniffer (UART RX keeps running in SPI mode at 111607), status reports,
  logic capture.
* **core1** (`spi_slave.c`): offline / serial / SPI watchers. SPI runs with interrupts
  off and bus priority. The `w5_frames()` frame engine handles whole frames back to
  back: `w5_header()` (asm answer path, `w5_cand[4]` for BSB 0–3, late detection =
  control word already in the RX FIFO), `fast_word()`, `w5_mark()` at the end marker;
  bookkeeping (`w5_book`) runs only when the bus is idle ≥ 10 µs or deep in a data burst.
* `w5500_model.h`: register model. `w5_command()` ends with `w5_refresh()`:
  RX_RSR/TX_FSR must follow RECV/SEND at once. An invalid RX_RD on RECV is reset (`n_recv_bad`).
* PIO: `w5500_spi.pio` (frame ends found by the program; MISO changes right after
  the rising edge; `pull noblock` so a late answer is `ff` and never stalls),
  `sd_spi.pio`, `uart.pio`.
* Tests in `test/`: `test_w5500.c`, `test_w5500_recv.c`, `test_sd.c`
  (`gcc -O1 -I. test/X.c`), `pio_sim.py` / `kos_replay.py <build>/w5500_spi.pio.h`.

## History: how W5500 got working (22–23 Oct)

1. **Frames merged:** the CPU missed short CS-high gaps. Fix: the PIO program ends
   frames itself (CS high at a byte boundary, spike filter) and pushes a marker word.
2. **Answers late:** the address (16 bits) and block select (21 bits) are pushed
   separately, the answers are precomputed, and the answer path is inline asm. Frame-end
   bookkeeping is deferred, and the DMA soft reset restores the 576 register bytes.
3. **Stalls lost clocks:** `pull noblock`, so a late answer goes out as `ff` and the
   shifter keeps step.
4. **The probe failed:** the DC read the previous MISO bit (KOS samples RXD late; 0x08
   read as 0x04, found with VERSIONR = 0x08/0x02 test builds). Fix: MISO changes right
   after the rising edge. The first MOSI bit of each byte comes from the same pin
   snapshot that saw SCLK high.
5. **Bad RX pointers (Quake 3):**
   - A late read shifted a whole burst. Fix: decide lateness just before the
     store (control word already pushed → skip byte 0), so a late read costs one byte.
   - The model kept an impossible RX_RD. Fix: reset it.
6. **Core1 a frame behind (Quake 3 6p mod, 0.2 µs CS gaps):** the main loop's
   ~190 cycles per word was too slow. Fix: the frame engine `w5_frames()` (net5).
7. **237 bad RECV pointers (RuneScape):** RX_RSR was stale after RECV because
   `w5_refresh()` was deferred. Fix: refresh inside `w5_command()` (net6, verified on
   hardware: 0 bad, RuneScape logs in).

Other notes:
* The KOS sources used are `kernel/arch/dreamcast/hardware/scif-spi.c` and
  `network/w5500_adapter.c`.
* `eth_init` probes at boot. A second probe means the first one failed.
* AFO over serial reaches the lobby, but its game-room UDP gets no reply. That's
  probably on the server or VPN side, not the cable.
* Always check the firmware banner time in a log: twice an older build was tested by mistake.

## Serial: the Dreamcast's RTS (10-12 Nov 2026)

Symptom: dctest serial failed at every rate (37 of 111) and replies reached the Dreamcast
late, while a build with the RTS wait replaced by a nop passed 111 of 111. Measured with
the cable's pad report: the RTS pad reads low while the Dreamcast boots, high at the menu,
in games and in dctest, and only toggles during dcload transfers; with the Dreamcast off
the cable reports nothing. The adapter is passive (micro-SD to SD, direct wires), so the
high level comes from the Dreamcast when nothing drives the pin (the SH4's RTS2 is also
the MD8 mode pin; current KallistiOS turns MCE off). Nothing was damaged and the wait in
uart.pio was there from the first import: what changed was the Dreamcast program.
dcload-serial 1.0.6 + dc-tool-ser 1.0.7 uploads clean at 1.5 Mbaud with the wait and
without it; 1.6 Mbaud stops after the first block on both (the SCIF divider wraps).
Fix: the wait is gone (a nop, same timing): the cable never waits for RTS, as DreamPi 2.1 does
(rtscts=False). A version that obeyed RTS only while it toggled was tried and removed: Quake 3
drives RTS but pauses it, which flipped flow control on and off, and DreamPi does not use it.


## Zero stack: Dreamcast pins moved, ESP32 link over SPI (16 Nov 2026, not on hardware yet)

`-DDC_BOARD=zero-stack`: the Dreamcast on GPIO9-14 and the ESP32 link on SPI0 (GPIO2-7). The
W5500/SD PIO programs keep their absolute `wait gpio` (the tightest loops can't afford
anything else) and are loaded with those indexes moved (dc_pio_reloc.h). pio_sim and
kos_replay run the moved program with the pins at GPIO9: same 56 checks, same output. The
hand-written CS test in `w5_header` takes its shift from PIN_CTS (#26 on the pico layout as
before, #19 on the stack); the pico build's `w5_header` disassembly is identical to before.

Why SPI: the UART link carries ~300 KB/s each way; the Dreamcast's W5500 path needs ~1000 KB/s
out and ~550 KB/s in. The SPI link keeps the packets and chops the byte stream into 1024-byte
full-duplex chunks padded with 0x00 (an empty packet to the receiver). The ESP32 can only take
part in a transaction it has loaded, so it signals HS (post_setup high, post_trans low) and DR.

The timing simulation (tests/test_link_spi.c: both sides' real code, ESP-IDF's slave queue
modelled, random latencies, stalls, resets) found one real bug before any hardware: the
"probe" for an ESP32 stuck with HS high fired whenever the link was down, including right after
a reboot before the first packet, and then clocked into the chunk the ESP32 had just finished
(its post_trans hadn't run yet). Fix: probe only after a full second without a falling edge
since our last transaction ended. Throughput in the simulation at 15.6 MHz: ~1.4-1.6 MB/s each
way with both directions saturated; at 20.8 MHz ~1.7-2.0 MB/s.

HS/DR first sat on ESP32 GPIO43/44 (over the Zero's GP0/1). Moved to GPIO1/2 (over GP2/3):
GPIO43 prints the ROM boot messages and GPIO44 has a pull-up during boot, so either could look
like "ready" while the ESP32 boots. GP0/1 ↔ 43/44 stay soldered as corner pins, both inputs.

Open on hardware: the ESP32-S3's slave output through the GPIO matrix at 15.6 MHz (half a clock
is 32 ns for MISO). Bit errors would show as bad checksums in the cable's link report; the
fallback is `-DLINK_SPI_HZ=10000000`.

## ESP32 only? A probe instead of a guess (16 Nov 2026, not on hardware yet)

Question: can an ESP32-S3 answer the Dreamcast's W5500 SPI itself, so the RP2040 can go?
The SPI slave peripheral can't (reply data must be loaded before the transfer). A bit-banged
slave on one core with the dedicated-GPIO instructions can on paper: 4 cycles per poll,
8 per bit, 8 for the answer lookup, against 40 ns clock phases (~10 cycles).
`esp32/probe` measures it on a bare board (master peripheral and slave on the same pads);
`hardware/ESP32-ONLY.md` says how to read the result. Found while writing it:
* GCC kept all 21 header snapshots in registers and combined them after bit 21 (~30
  instructions with no look at the clock: a missed edge at speed). Fixed with a barrier per
  bit; check the disassembly after every change to dcspi_bb.h, like w5_header.
* The host test caught a slave that polled chip select once more after a frame end and lost
  the next frame when the gap was short.

## ESP32 only: measured, and the answer is no (16 Nov 2026, SuperMini)

Seven probe runs (`esp32/probe`, results in hardware/ESP32-ONLY.md). Wi-Fi off: clean to
13.3 MHz, 16 MHz fails, so no margin over the Dreamcast's 40 ns phases. Wi-Fi on: the
slave's core is paused ~40 ns a few times a second even with beacons only (not an
interrupt: masking levels 1-5 changes nothing), and 53 of 200 000 writes go bad at a
Dreamcast-like clock without any scanning. The RP2040 stays for W5500 and SD.
Lessons: check chip select in every clock wait; no flash writes while a core is held;
`pdMS_TO_TICKS(2)` is 0 at a 10 ms tick.


## ESP32 RAM, Quake 3 and DCNet (Nov 2026)

- The ESP32-S3 SuperMini has no PSRAM. With the USB modem on, the heap fell to 5 KB minimum and
  secure connections failed (`alloc(4437 bytes) failed`), so dreampi_dns.conf never loaded
  ("0 overrides"). Static RAM 195 KB -> 157 KB: Wi-Fi fast paths out of IRAM (on the S3, IRAM
  is heap), -Os, log text packed (17.5 -> 8.5 KB), serial monitor 8 -> 4.5 KB, DNS table of
  pending questions 16 -> 8; the DNS file is parsed while it downloads (was a 16 KB copy);
  one HTTPS connection at a time (tls_lock).
- Quake 3's master list needs the revival DNS. The user's router answered OpenDNS itself
  (113-byte reply, 0 answers): it intercepts UDP 53, so 46.101.91.123 was never reached.
  pubip.c now repeats the question over TCP; when that works, dnsd forwards over TCP.
- DCNet as in DreamPi's netlink.py/dcnet.rpi (firmware/dcnet_model.h, tests/test_dcnet.c checks
  the '~' fix against a copy of flyinghead's loop).

## Netlink, XBAND, Capcom, port forwarding, VPN data path (Nov 2026)

- netlink.py (eaudunord/Netlink, Oct 2026) ported to esp32/main/netlink.c; the dial strings,
  packet format (with gap recovery), STUN, ini and PPP delimiter fix are in
  firmware/netlink_model.h with tests/test_netlink.c (checked against the Python logic).
- DreamPi answers only PPP calls 8 s after the number; DCNet, Netlink and Capcom right away.
  The USB modem now does the same.
- lwIP's NAT port-map table defaults to 32 entries; DreamPi's list is 215 (2300-2400 TCP and
  UDP), so IP_PORTMAP_MAX=232 is set for the whole build (esp32/CMakeLists.txt).
- VPN: marking the tunnel netif as a NAT "inside" interface (napt = 1) makes lwIP skip NAT on
  packets forwarded into it, so the Dreamcast keeps its tunnel address + 1 as on DreamPi. The
  newest netif is searched first, so the call's PPP link wins over the tunnel subnet for that
  address.
- VPN profiles live in their own partition (vpnprof, 0x190000), so flashing a new merged
  image keeps them.

## Link self-test never reported; SD slot (Nov 2026)

- The self-test logged "started" and then nothing. link_task() takes `now` before reading the
  link, and an answer read in the same pass is stamped later, so `now - t_last` wrapped to a
  huge number: the test "timed out" right after it started, and its FAIL line was sent while
  the queue was full of test packets, so it got lost. The timeout compare is signed now
  (tests/test_link_test.c checks it), and the result line waits for room on the link.
- Debug log: the spi/w5500 lines only when the Dreamcast did something on the SPI bus, the error
  line only with errors, USB network counters at most once a minute; "offline: waiting" once,
  then only when a pin changes or every 5 minutes.
- SD slot: sdcard_model.h drives a real card in SPI mode as a second block source for the SD
  emulation (same contract as sdh_poll). tests/test_sdcard.c runs it against a simulated card.
  core1's W5500 path: instructions identical to the previous build (only literal addresses moved).
- VPN routes pushed by the server go into the tunnel through lwIP's route hook
  (esp32/hooks/dcl_lwip_hooks.h wraps ESP-IDF's own ip4_route_src_hook).

## Serial line queue, SD files over Wi-Fi, debug build RAM (19 Nov 2026)

- Lag on serial games during any download: everything for the Dreamcast waited in one line of
  bytes (ESP32 ring + 4 KB on the cable, ~12 KB = ~1 s at 115200), and a full ring made the PPP
  output sleep inside lwIP's own thread in 10 ms ticks (FreeRTOS at 100 Hz), stalling every
  network on the box. tests/test_serq_sim.c runs the real rules: a 20 Hz game update during a
  download waited ~1 s (4 s at 57600) before, ~0.12 s average / 0.19 s worst now, the line just
  as busy. Fix: whole packets wait on the ESP32 (UDP first, TCP in order), the cable holds ~20 ms.
- Building every variant caught a break the main build hid: room_sem existed only in the SPI
  build, so the UART-link ESP32 image did not compile. Build all variants after link changes.
- The debug build (runs from RAM) stopped linking after ~2 KB more code: the 32 KB capture
  buffer, aligned to its size, sits first in .bss at the next 32 KB boundary after the code,
  so crossing a boundary costs 32 KB at once. It is allocated at start now (16 KB fits today).
- SD slot: CMD18 streams for the Dreamcast's runs, bulk SPI transfers, CRC checked per block.
  The ESP32 shows the card's files (sdlink_model.h + sdfs.c); tests/test_sdlink.c runs
  ESP-IDF's FatFs over the link protocol on the host, against a FAT32 image.
- Later the same night: the first version of the queue made the cable refuse the ESP32's serial
  pieces above ~20 ms. A refused piece stops the whole link (the cable runs no transaction while
  it holds one), so the Dreamcast's own packets to the server waited too: up to 129 ms during a
  download in the simulation, which did not model the upstream at all. Now the ESP32 paces by
  time (serq_pace_t, the line rate from the cable's hello) and the cable takes what fits; the
  simulation measures the upstream and fails if it ever waits.
