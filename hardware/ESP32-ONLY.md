# ESP32 only: can the RP2040 be left out?

**Status: measured on an ESP32-S3 SuperMini on 16 Nov 2026. Answer: no, not for the W5500
and SD card over the serial port. The RP2040 stays. An ESP32 alone is still enough for
serial games, a USB modem and the broadband adapter port, which don't need this timing.**

## Result

| | Wi-Fi off | Wi-Fi on |
|---|---|---|
| Pauses of the slave's core (5 s, a pause of 25 ns or more) | 0 | 4 idle, 5 while sending and listening, 39 while scanning; each about 40 ns |
| 8 MHz clock with a 37 ns high phase, 200 000 writes | 0 bad | 53 bad (no scan) |
| 13.3 MHz even clock, 200 000 writes | 0 bad | 1085 bad (no scan) |
| Fastest clean clock | 13.3 MHz (16 MHz fails) | none |

* With Wi-Fi off the bit-banged slave is clean up to 13.3 MHz, just above the Dreamcast's
  fastest clock (40 ns phases, like 12.5 MHz). There is no margin beyond that.
* With Wi-Fi on, the core that runs the slave is paused for about 40 ns now and then, also
  when the radio only sends beacons. Masking interrupt levels 1-5 doesn't change it, so it
  isn't an interrupt. A 40 ns pause is as long as the Dreamcast's shortest clock phase:
  when it lands on one, the edge is never seen. No code can fix that.
* In use that would be a corrupted W5500 register or data byte every few seconds.

Found on the way and worth keeping: a slave must check chip select in every clock wait
(frames are whole bytes, so one missed edge otherwise leaves it a bit off for ever), and
nothing may write to flash while one core is held with interrupts off (the first Wi-Fi
start saves calibration data and hung).

The rest of this page is the question and the method, as written before the measurement.

The RP2040 is in the design for one job: when a Dreamcast program uses the W5500 or SD card
over the serial port, the Dreamcast is the SPI master and bit-bangs the clock with phases
down to about 40 ns, and a read has to be answered with data that depends on the header it
has just sent. The RP2040 does that with PIO plus a core that runs with interrupts off.

The ESP32-S3 has no PIO, and its SPI slave peripheral needs the reply loaded before the
transfer starts, so the peripheral can't do it. What the ESP32-S3 does have is *dedicated
GPIO*: CPU instructions that read and write pins in one cycle, without a bus access. With
one of the two cores kept for the Dreamcast (interrupts off, code and data in RAM, Wi-Fi and
everything else on the other core), a bit-banged slave is possible on paper:

| | Dreamcast | bit-banged slave at 240 MHz (from the disassembly) |
|---|---|---|
| shortest clock phase | ~40 ns | the pins are polled every ~17 ns (4 cycles) |
| shortest bit | ~80 ns | one bit costs ~33 ns (8 cycles) |
| longest stretch without looking at the clock | must stay under 40 ns | ~33 ns (the answer lookup after bit 21) |
| first reply bit after the header | the cable today: ~15 ns after the edge | ~35 ns plus the pad delay |

That is a margin of about two, where the RP2040's PIO has much more. What the numbers can't
show: the real cost of the pin instructions and of taken branches, the delay and jitter of
the input synchroniser, and whether Wi-Fi activity disturbs the other core at all.

## The probe

`esp32/probe` is a firmware that measures it. It needs a bare ESP32-S3 board, no wiring and
no Dreamcast: the SPI master peripheral plays the Dreamcast on GPIO4-7 at 1 to 20 MHz, and
the bit-banged slave answers on the same pads from the other core. It runs 20 000 frames
per speed, first with Wi-Fi off and then twice with Wi-Fi on, and prints a table.

    esptool.py --chip esp32s3 write_flash 0 dc-picolink-esp32-probe-<version>-merged.bin

(or `cd esp32/probe && idf.py flash monitor`). Leave GPIO4-7 unconnected. Read the board's
USB serial port; the table takes about a minute.

How to read it:

* The Dreamcast's fastest clock looks like 12.5 MHz. **No FAIL up to the 13333 kHz line is
  the minimum** to go on; a clean 16000 kHz line means there is margin.
* "bad first byte" is the reply deadline, "bad writes" and "bad later byte" mean clock edges
  were missed, "lost frames" means the slave got out of step.
* Wi-Fi on should give the same result as Wi-Fi off. If it doesn't, the cores aren't
  isolated well enough and the approach is out.

What the probe can't tell: how the real Dreamcast behaves (uneven clock, its late sampling
of RXD). A pass means the next step is worth it: the W5500 model on the ESP32 and a test on
a console. A fail means the RP2040 stays for W5500 and SD, and "ESP32 only" is a
serial/modem/BBA adapter.

The slave's logic (frames, bit order, aborted frames) is tested on the host:
`esp32/tests/dcspi_bb`, run by `tools/run-tests.sh`.

## What ESP32 only would cost in the firmware

* One core belongs to the Dreamcast whenever SPI mode is on. Flash writes (settings) wait
  for the other core, so they are only allowed while the Dreamcast's bus is idle and the
  slave has let go.
* Wi-Fi, lwIP, PPP, VPN and the web page all run on the remaining core.
* The UART side needs the Dreamcast's baud rates (baud_model.h) on the ESP32's UART.
