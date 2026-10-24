# DC-PicoLink: notes for working on this repo

Read docs/ARCHITECTURE.md first (goal and design), docs/ROADMAP.md (what's next),
docs/DEVLOG.md (how the hard bugs were found; read before touching timing code).

## Build and test
- Pico SDK 2.1.1 + Ubuntu `gcc-arm-none-eabi`. `cmake -S firmware -B build -DPICO_BOARD=pico -DDC_SYS_KHZ=250000 [-DDC_DIAG=0|1] && make -C build`
- `tools/run-tests.sh [build-dir]`: C model tests; with a build dir also the PIO simulations.
- `tools/make-dreampi-setup.sh`: the one-file DreamPi installer (never commit the generated file).
- CI (.github/workflows/build.yml) builds release (250 MHz, diag off) and debug UF2s; tags `v*` make a GitHub release.
- The firmware's first status line carries `git describe`; always check it in a user's log before analysing.

## Rules learned the hard way
- core1's W5500 header path (`w5_header`, `w5_frames` in spi_slave.c) is timing-critical: after a change, compare its disassembly with the previous build and run pio_sim. Diagnostics must never run before the answer.
- KOS samples RXD right after raising the clock: MISO changes right after the rising edge, and GPIO4 stays at the default pad (slow slew, 4 mA). Fast slew/8 mA broke cable→DC data (DEVLOG, net7).
- SD detection: only until a program opens the W5500 socket (`sd_locked`); ≥ 2 leading 0xFF; armed only after the SD shifter realign.
- W5500 RECV with a bad RX_RD resyncs to rx_wr (drop buffered frames); never put back a position that may be inside a frame.
- Baud above 115200 = dcload's divider exactly (baud_model.h), including the 8-bit wrap above 1562500.
- Host side for DreamPi stays an add-on: own files, services, iptables chains; never edit DreamPi's scripts.

## Conventions
- Status lines are plain English, read by users in `journalctl`; keep them short and specific.
- Models (`*_model.h`) stay host-compilable and get a test in firmware/tests for every fix.
