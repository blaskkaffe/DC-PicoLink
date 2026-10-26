# dctest: serial and W5500 test tool

A Dreamcast program (`dctest.c`, KOS) and a Python host tool (`dctest.py`, runs on macOS,
Linux, the DreamPi). The host drives everything over the cable and writes one timestamped log
(`dctest-<time>.log`, plus `.jsonl` with one result per test) that can be shared as is.
Every run prints its `--seed`; pass it again to repeat the exact traffic.

## What it tests

| Command | Test |
|---|---|
| `serial` | for each baud: SETBAUD handshake, duplex echo with 3 (or 8 with `--full`) data patterns and frame sizes 1..2048, host→DC sink (the DC checks every byte), DC→host source, first frame after idle gaps |
| `ramp` | one baud, offered load 5→100→5 % of the line rate: duplex echo, sink, source |
| `hop` | baud rate changed back and forth N times, an echo check at each |
| `net` | W5500: UDP round-trip times, echo by frame size (1..1400), sink/source load ramp up and down (250 kbit/s..8 Mbit/s), TCP echo and 8 MB sink/source |
| `switch` | N cycles of serial at baud X → W5500 session → back to serial, with echo checks on each side and the time the cable needs to come back |
| `soak` | random mix of all of the above for `--minutes` |
| `detect` / `auto` | hardware report / report + every test that fits |
| `selftest` | the host tool against a host build of the DC program (no hardware); also checks that injected bit errors are reported |

Data patterns: random, all 0x00, all 0xFF, 0x55/0xAA, ramp, walking bit, PPP-like (lots of
0x7E/0x7D/0x11/0x13), and edge values. Every frame has a CRC32 and a sequence number, so
lost, corrupted, duplicated and skipped bytes are told apart; the DC reports its own counts
(`dc_crc_bad`, `dc_skipped`, `dc_pat_bad`, `dc_gap`) so you can see which direction broke.
The Dreamcast side programs the port with KOS's `scif_init`, whose divider rounds: 115200 really is
120192 (+4.3 %), 38400 is 39062, 500000 is 520833, 1600000 wraps to 6103. The log shows `dc_rate` when it
differs from the request, and **the host port is set to the Dreamcast's real rate** (not the nominal
one), so cable and Dreamcast agree from the first byte. `--nominal-baud` sets the requested rate instead,
like dc-tool does, to see what such a setup would suffer.

The Dreamcast screen is a fixed 48 x 18 status panel (serial rate, host link, frame/byte counters, errors, W5500 / SD
probe, network address) with a six-line event log at the bottom; rows are rewritten in place, nothing wraps or scrolls.

The host tool is silent at start: it listens (up to `--wait`, 60 s) and sends nothing until the Dreamcast
program announces itself, so it can be started before the console is switched on. `--send-anyway` skips that.

The Dreamcast announces itself again after 3 s without any host frame, so the tool can be started again
against a program that is already running.

Until a host frame arrives at a new rate, the Dreamcast sends a small announce frame every 300 ms; the
DC-PicoLink only learns the Dreamcast's real rate from what the Dreamcast transmits. In network mode the
Dreamcast returns to serial by itself after 30 s without a frame (or Start); 1200 baud is skipped on a
PicoLink (firmware from before the "no 1200-baud touch" change reboots into BOOTSEL at that rate).

## Hardware check first

Like the W5500 tester, `dctest` looks at what is there before running anything. On start the
Dreamcast program probes the SPI pins (W5500 version register, SD `CMD0`) and shows the result
on screen; the host adds what it knows about the adapter. `python3 dctest.py detect` prints:

    host adapter : /dev/cu.usbmodem1101  [DIY Dreamcast Coder's Cable + W5500 (RP2040), USB cafe:4dc5]
    adapter type : DC-PicoLink  - the cable follows the Dreamcast's real rate
    Dreamcast    : dctest 1 v0.8-3-gabc, serial link works at 115200 baud
    W5500        : found (version register 04)
    SD card      : no answer (CMD0 answer FF)
    tests that fit: serial sweep up to 1562500 baud, ramp, hop, W5500 network, serial<->W5500 switching

Every command does this first and only runs what fits (skipped groups show as `INFO`);
`--force` runs W5500/switching anyway, `--no-limits` tries rates above the adapter's limit,
`--no-detect` skips the report. `dctest.py auto` = detect + everything that fits.
`--port` can be left out when only one USB serial adapter is plugged in.

**Other coder's cables work too** (FTDI, CP210x, CH340/CH341/CH9102, PL2303, unknown ones): the
serial tests need nothing from the firmware. They get the adapter chip's rate limit
(unknown adapters: 921600; `--no-limits` overrides) and a note when the Dreamcast's real rate
differs from the requested one by more than 2.5 % (e.g. `-b 500000` really is 520833 on the
Dreamcast, which a plain FTDI cannot follow; the DC-PicoLink measures and follows it).
W5500 tests need a W5500 found by the probe; serial↔W5500 switching needs the DC-PicoLink.

## Seeing the cable's own log (DC-PicoLink)

The cable reports what it thinks is going on (`mode: serial -> offline`, `DC transmits at ~120192 baud`, framing
errors, firmware version) as status lines on its vendor USB interface; on a DreamPi `dcsd` reads them into
`journalctl`. On a Mac or PC `dctest.py` reads them itself and merges them into the log as `cable| ...` lines
(on by default for a DC-PicoLink, `--no-cable-log` to skip):

    brew install libusb && pip3 install pyusb       # macOS; Linux: libusb + pyusb from your package manager
    python3 dctest.py cablelog                      # just the cable's lines for 10 s

While it runs the cable treats the computer like `dcsd` with no card image (the Dreamcast sees no SD card). Do not run
it on a machine where the `dcsd` service is running (it owns that interface): read `journalctl -fu dcsd` there.

**Bus mode.** With the cable log on, the tool pins a DC-PicoLink to *serial-only* mode (what `dcsd-mode serial` does on
the Pi) for the serial tests (`serial`, `ramp`, `hop`, `soak`, and the serial part of `auto`), so a stray SPI session can never
take the serial path away; it puts the cable back to *automatic* when it exits (also on Ctrl-C). If the tool is killed hard the
cable stays serial-only until the tool runs again, `dcsd` connects, or the cable is replugged. The SPI probe (W5500 / SD) only
runs for `detect`, `net`, `switch` and the network part of `auto`; if the cable does not come back from it, the tool forces it to
serial and says so.

The W5500 / SD probe is **not** run at start-up (it makes the cable switch to SPI mode and back): the Dreamcast
announces itself on serial first, and probes when `detect` asks for it.

## Reading a failed echo test

A failing `echo` line says what happened to the missing frames: `late` = the reply did arrive, but only after the
test gave up (we send more traffic and see whether it shows up); `lost` = never; `dc_got` = how many of the requests
the Dreamcast counted (if it is lower than `n`, they were lost on the way to it; if equal, the replies were lost on the
way back); `dc_scif_err` / `dc_ovr` = receive errors and overruns the Dreamcast's serial chip reported; `dc_flow` = times
the Dreamcast's serial chip had hardware flow control switched on (KOS does that after any error; dctest switches it off
again). The same counters are in the `Errors` row of the Dreamcast screen.

## Setup


    pip install pyserial                 # macOS / Linux / DreamPi

`dctest.elf` is built by GitHub Actions (workflow `build`, job `dctest`; no KOS install needed):
Actions tab → build → latest run → Artifacts → `dctest`, or the GitHub release of a `v*` tag.
Run it manually on any branch with *Run workflow*. The job uses the toolchain image the KOS project
publishes for its own CI (`pcercuei/dreamcast-toolchain:stable`) and a pinned KOS commit; the
same script works locally: `tools/dctest/ci-build.sh <KallistiOS checkout> <out dir> <version>`
inside that image, or `make` in your own KOS environment. The firmware UF2s and the DreamPi
installer come from the same workflow (artifact `dc-picolink`).

Load it with `dc-tool-ser -t <port> -b 115200 -x dctest.elf`, then **quit dc-tool** (the
program takes over the serial port; only one program may hold the port). The screen shows
progress; Start quits. On the Pi stop Netlink monitoring first (see docs/REFERENCE.md).

    python3 dctest.py ports
    python3 dctest.py hello  --port /dev/cu.usbmodemXXXX
    python3 dctest.py serial --port ... --bauds common            # or uncommon | all | 9600,115200
    python3 dctest.py ramp   --port ... --baud 1500000
    python3 dctest.py net    --port ...                           # needs the Pi/Mac to hand out DHCP on dcusb0
    python3 dctest.py switch --port ... --cycles 20
    python3 dctest.py soak   --port ... --minutes 60

**Firmware logs in the same file:** `--fw-log-cmd "journalctl -fu dcsd -n0"` on the Pi, or
`--fw-log-cmd "ssh pi@dreampi 'journalctl -fu dcsd -n0'"` from the Mac. The log shows
them as `fw| ...` lines next to the test results and counts lines with `resync`, `underrun`,
`late`, `error`. Use the debug UF2 for the extra counters.

**W5500 notes:** `net` and `switch` start the Dreamcast's network side over the serial link
(`NETSTART`); the DC announces itself by broadcast (UDP port+1) and answers on UDP 7777 (TCP
7778). If the beacon does not reach the host (different subnet, no DHCP yet) give `--dc-ip`.
With the cable's mode forced (`dcsd-mode`) the switch back to serial will not happen: use `auto`.
`net --dc-ip IP` without `--port` tests a DC that is already in network mode.

## Protocol

`D3 7C | type | flags | seq u32 | len u16 | payload | crc32` (little endian; crc over
type..payload); replies set bit 7 of the type. The same frames run over the serial line, UDP
(one frame per datagram) and TCP. `proto.h` (C) and `dctest.py` (Python) are mirrors; the
selftest runs both against each other.

## Status

Selftest passes (88 tests clean; injected errors reported; cable identification and the
skip logic for missing hardware are checked too). The Dreamcast program compiles cleanly against
the headers of current KallistiOS master (checked here). It has **not been linked or run**
yet: the CI job builds it, but no `dctest.elf` has been tried on a console. If the first run shows a KOS
difference, send me the log. Things I could not check without hardware: the W5500/SD probe
bit-banging, and how the cable reacts to the probe at start.

**Flow control.** At about 1.5 Mbaud the Dreamcast's 16-byte serial FIFO overruns now and then in long back-to-back bursts
(one lost frame per ~110 KB, `dc_ovr` in the failing line). `--flow on|off|auto` switches the Dreamcast's hardware flow control
(RTS/CTS; the cable stops sending while the Dreamcast's RTS is high); `auto` (default) turns it on from 500000 baud up, and the
screen shows `FC` next to the rate when it is on.

Known limit on the Dreamcast side: KOS receives on the SCIF through a 1 KB interrupt ring with no hardware flow
control. `dctest` drains it into its own 16 KB queue between 16-byte chunks of every transmission, and the
Dreamcast screen reports "receive queue overflowed" if even that is not enough, so a lost byte can be told
apart from a cable problem (compare `lost` with `dc_crc_bad`/`dc_gap`).
