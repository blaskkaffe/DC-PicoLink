# Testing

## Host-side tests (no hardware)

`tools/run-tests.sh [build-dir]` compiles and runs the model tests; with a firmware build
directory it also runs the PIO simulations on that build's assembled W5500 program.

| Test | What it checks |
|---|---|
| `test_w5500.c` | KOS `w5500_adapter.c` SPI traffic replayed against the W5500 model |
| `test_w5500_recv.c` | RX_RSR follows RECV at once; a misread length/pointer resyncs in one poll instead of looping |
| `test_sd.c` | KOS `sd.c` init/read/write byte for byte against the SD model, with host latency |
| `test_sd_detect.c` | no W5500 frame is taken for an SD command; every KOS SD command is |
| `test_sense.c` | sense-wire detection (found, wire loose, glitches, timer wrap) |
| `test_baud.c` | the wire rate equals the rate dcload really sets on the Dreamcast, for every request 115201–4000000 |
| `pio_sim.py` | the W5500 PIO program cycle by cycle against KOS-like waveforms (56 checks) |
| `kos_replay.py` | the KOS W5500 probe sequence through the PIO program |

## On hardware

Keep `journalctl -fu dcsd` open on the Pi; the cable's status lines appear there. Check
the first line after flashing: `cable: DC-PicoLink firmware <version> ... ready`.

**Power and boot safety**
- [ ] Dreamcast off: `offline: waiting for the DC ...` every 10 s, `core1 running`.
- [ ] Three cold boots with the board connected: never a black-screen hang.
- [ ] With the sense wire: `power sense GPIO7: found`, offline within ~1 s of power-off.

**W5500**
- [ ] JaderFox tester passes; `eth_init` probes once (`dc console:` lines).
- [ ] DHCP address 192.168.99.x; ping both ways.
- [ ] Long session (RuneScape / DreamKarts, 30+ min): 0 burst underruns, RX resyncs rare.
- [ ] `SD off (a game opened the W5500 socket)` once the game is online; `switches to SD` stays put.
- [ ] With `dcusb0-vpn`: `journalctl -u dcusb0-vpn` shows the mapping; Quake 3 master
      list, UT, AFO, Worms World Party online.

**Serial**
- [ ] dc-tool-ser at the default rate and at `-b 1500000`: upload, console output, no framing errors.
- [ ] `-b 1600000` logs the divider-wrap hint.
- [ ] Netlink PPP (AFO): `PPP frames ... N good, 0 bad` after the rate is followed.

**SD**
- [ ] openMenu 1.7.1 finds the card and writes its settings / VMU backups.
- [ ] DreamShell 4 boots and reads `/sd`.
- [ ] A KOS `/sd` program: read and write, files intact on the host.

**Modes**
- [ ] dcload-serial → W5500 program → back to dcload in one session.
- [ ] `dcsd-mode` overrides take effect and are logged.
