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
| `selftest` | the host tool against a host build of the DC program (no hardware); also checks that injected bit errors are reported |

Data patterns: random, all 0x00, all 0xFF, 0x55/0xAA, ramp, walking bit, PPP-like (lots of
0x7E/0x7D/0x11/0x13), and edge values. Every frame has a CRC32 and a sequence number, so
lost, corrupted, duplicated and skipped bytes are told apart; the DC reports its own counts
(`dc_crc_bad`, `dc_skipped`, `dc_pat_bad`, `dc_gap`) so you can see which direction broke.
Rates above 115200 follow dcload's divider (`baud_model.h`): the log shows `dc_rate` when the
Dreamcast really runs a different rate than requested (e.g. 500000 → 520833, 1600000 → 6103).

## Setup

    pip install pyserial                 # macOS / Linux / DreamPi
    cd tools/dctest && make              # in a KOS environment (source environ.sh): dctest.elf

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

Selftest passes (87 tests clean; injected errors reported). The Dreamcast build has **not been
compiled against a real KOS yet**: `tools/run-tests.sh` only syntax-checks it against
stub headers (`stub/kos.h`, written from memory of the KOS 2.x API). Expect to fix a name or two
(`scif_*`, `net_init`, controller read) on the first `make`.
