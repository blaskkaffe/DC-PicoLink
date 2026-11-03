# DC ecosystem: feature inventory (all branches vs each repo's baseline)

Survey of 2026-11-03, **updated the same evening with the owner's decisions** (see `03-decisions.md`). The survey itself changed nothing; the one change made afterwards is the removal of telephone-number switching in DC-DreamDash `unified` (D-3).
Baseline = each repo's `main` (`master` for DC-BlueRetro). `unified` was **identical to the baseline in all five repos** when the survey was made, so every difference below is relative to what `unified` was then.

How to read IDs: `PL-` DC-PicoLink, `DD-` DC-DreamDash, `IB-` DC-iBlueControlMod, `OM-` DC-openMenu-Virtual-Folder-Bundle (the fork), `BR-` DC-BlueRetro, `X-` same idea in several repos, `C-` conflict or collision. In a number: `B` = baseline, `P` = planned in the repo's own docs, `R` = roadmap idea.
"Proof" words are the draft unified ones from `02-unified-language.md`: idea, written, built, host-tested, hw-seen, hw-verified.

Sources: branch commits, diffs, and each repo's own docs (FEATURES.md, ROADMAP.md, TODO.md, hardware-status.md, PROJECT_BRIEF.md). Where a doc says "seen on hardware" I repeat it as the doc's claim; none of it was verified on hardware here.

Branch names below are the branches of the **old (pre-unified) work**; they are on each repo's remote and are not merged into `unified`.

---

## 1. Branch map

The local clones were shallow, so the first ahead/behind counts (up to 1,536) were wrong. These are from full history.

| Repo | Branch | Last commit | Ahead of baseline | What it is |
|---|---|---|---|---|
| DC-BlueRetro | master | 2025-12-14 | 0 | Upstream BlueRetro, final commit "Farewell". Pure mirror |
| | unified | | 0 | = master |
| DC-PicoLink | main | 2026-10-27 | 0 | v0.8-era firmware, docs, dctest |
| | claude/vibrant-meitner-9g03mc | 2026-10-27 | 6 | Cable diagnostics + dctest. PR #1 merged an earlier state; these 6 came after |
| DC-DreamDash | main | 2026-10-12 | 0 | Module-based DreamPi dashboard |
| | wifi | 2026-09-30 | 0 | Already merged into main, nothing to take |
| | development | 2026-10-16 | 10 | DC Bridge module (added, then removed), autoupdates switch, Serial PPP / KOSnet rows |
| | claude/iblue-ports | 2026-11-03 | 17 | `development` + 7 commits: iBlueControlMod module |
| DC-iBlueControlMod | main | 2026-10-30 | 0 | Fork of BlueRetro: iBlue hardware, RGB/port LEDs, boot animation, taps, profiles, game id, web config |
| | claude/integration | 2026-11-03 | 63 | Superset of `web-config` and `test-fw`. Newest and richest |
| | claude/test-fw | 2026-10-31 | 45 | Test firmware builds. Fully inside `integration` |
| | claude/web-config | 2026-10-31 | 3 | Dreamcast-only setting, presets, LED panel. Fully inside `integration` |
| | development | 2026-10-30 | 7 | `brctl` (Maple-bus control channel), VMU LCD capture. **Not in integration** |
| | claude/restructure | 2026-10-30 | 9 | `development` + `main/iblue/` layout, brctl v3, profiles, hotkey. **Not in integration** (only its layout idea was redone there) |
| | claude/openmenu-remap | 2026-10-30 | 7 | Second control channel over VMU blocks 200-207. **Not in integration**; restructure's doc says brctl supersedes it |
| | claude/led-behaviour | 2026-10-30 | 4 | Early boot animation. Reimplemented on test-fw/integration (compare, don't merge) |
| | gh-pages | 2026-10-30 | 1 | Published web config |
| DC-openMenu-Virtual-Folder-Bundle | main | 2026-09-25 | 0 | openMenu Virtual Folder Bundle + Card Manager |
| | dev | 2026-10-31 | 307 | "NewOpenMenu": BIOS-style menu rewrite. Contains 5 other branches |
| | NewOpenMenu, -phase2, -phase5, sketch, sketch2, claude/stoic-goldberg | | 0 vs dev | Ancestors of dev. Nothing unique |
| | PS2 | 2026-10-29 | 8 | PS2-style menu skin, built from PCSX2 measurements |
| | animated-theme | 2026-10-07 | 8 | Animated 3D backdrop themes (waves, synthwave) for Folders |
| | claude/dreamy-planck-uq69cz | 2026-10-01 | 17 | Dial-based network switching, Custom Dial numpad, merged player list (telephone parts dropped, D-3) |
| | claude/iblue-controllers | 2026-10-30 | 5 | Controllers screen on brctl |
| | claude/gameid | 2026-10-30 | 1 | Game id writer (VMU block) |
| | claude/serial-vmu-safety-spec | 2026-10-30 | 2 | Design doc only, no code |
| | claude/block-46-font-size-wo6pri | 2026-09-24 | 3 | Serial VMU progress font size |

---

## 2. Per-repo: baseline, then what only the branches have

### DC-PicoLink (RP2040 Dreamcast serial-port adapter)

**Baseline (main):** serial coder's cable with dcload-exact baud dividers, auto-baud, FTDI-style sampling, RTS/CTS (PL-B1). W5500 emulation as a USB network interface (PL-B2). SD card emulation with the image served by the `dcsd` host daemon (PL-B3). Serial/SPI auto-detect via GPIO6/GPIO2 (PL-B4). SD vs W5500 sharing one chip select, with SD detection locked once a program opens the W5500 socket (PL-B5). DreamPi add-on and one-file installer (PL-B6). `dctest` hardware test tool (PL-B7). Plain-English status lines starting with `git describe`, diagnostics build switch (PL-B8). Host dirs for linux, macos, windows, dreampi.

**Only on `claude/vibrant-meitner` (6 commits):**
- PL-1 RTS-held-high watch: status line when the Dreamcast's RTS pad is high in >=2% of samples over 5 s. *written; no test in the diff*
- PL-2 Pad-level report while the Dreamcast is off: the six pads are read out so a cable can be tested without a meter. *written; no test in the diff*
- PL-3 Auto-baud guard (`ab_fraction`): follow a 1/n fraction of the wire rate only after 16 agreeing windows. Has a model test in `test_baud.c`. *host-tested*
- PL-4 dctest: leave the Dreamcast at the start rate after each run, auto-return after 20 s without a host.
- PL-5 dctest: 64 KB Dreamcast receive queue, queue high-water mark and main-loop gap in STATS. The commit message cites a 30-minute soak on hardware: 577 of 582 tests passed, the 5 failures being Dreamcast-program stalls.
- PL-6 dctest: USB write timeout fails one test instead of ending the run; soak re-finds the Dreamcast.

**Ideas in PicoLink's own ROADMAP (not on any branch):**
- PL-R1 Physical SD slot on the RP2040's SPI1 (GPIO26/27/28, CS GPIO9); card becomes the block source; drop the host image link; status on a second small USB serial.
- PL-R2 USB mass storage of the card while the Dreamcast is off.
- PL-R3 Fix openMenu 1.7.1 "no SD card" (it reads two sectors and stops). **Ties to OM Serial VMU.**
- PL-R4 Host support: Linux udev + sharing script, macOS/Windows instructions, Android investigation, a PPP server without DreamPi, own USB VID/PID.
- PL-R5 W5500 MISO timing margin; slow command-register polls.
- PL-R6 **Internal modem tiering:** Pi Zero 2W with DreamPi inside the console, PicoLink to the Zero over GPIO (UART + framed packets over SPI with a TUN program), GPIO line switch so games that try the modem first fall back to serial, priority W5500 → serial → modem, per-game choice from openMenu's last-played file, later an I2S software modem.
- PL-R7 Validation backlog: dctest on hardware, Quake 3 / UT / AFO / Worms over `dcusb0-vpn`, release (diag off) build on hardware.

### DC-DreamDash (DreamPi dashboard add-on)

**Baseline (main):** base + modules: switcher (DCNow!/DCNET buttons, GPIO buttons, hook inside DreamPi, `/tag`), system, clock, events (DC99), players, openmenu link, led, debuglog, wifi, rebootupdate, background, imagebg, snow. Page kit with palette of 15 colours, module picker order = priority. State in files, no sockets between services. Python 2.7 hook rule. *(The baseline also had a `numbers` module for phone numbers; removed in `unified`, D-3.)*

**Only on `development` (net of the add-then-remove):**
- DD-1 Allow / Restore `/boot/noautoupdates.txt` (Settings > System, PIN-gated; moves the file via systemd-run). *host-tested*
- DD-2 Network box rows **Serial PPP** and **KOSnet** (the PicoLink cable's two sides). Doc: serial PPP seen working on a real Pi, rows themselves not checked on a call. *host-tested, hardware partial*
- DD-3 **DC Bridge module** (added 2026-10-15, removed 2026-10-16). It was the predecessor of DC-PicoLink: RP2040 firmware + Pi side. Not in any current repo, worth mining for ideas:
  - Pi **save manager**: FAT image, per-save history, queued edits, web page, JSON API, CLI, zip of all saves, served through the dashboard on `/dcbridge/`.
  - Routing scripts for PPP (`ppp10`) and the W5500 link (`dcnet0`): DHCP, DNS, NAT, port forwards; udev rules; systemd units.
  - A module that installs/uninstalls from its own Settings box; "say why the board is not connected"; last Dreamcast requests kept for diagnosis; link to the SD card trace.
  - All of it described as never run on real hardware.

**Only on `claude/iblue-ports` (7 commits on top of development):**
- DD-4 **iBlueControlMod / BlueRetro module** (off by default): box with four port tiles A-D (BT / Wired / Empty, coloured per port, console name under it), pop-up per port with Controller type, Accessory (None / VMU / Rumble / Both) and Profile (loads a whole saved configuration). Own service `dreampi-netswitch-iblue` talks BLE with `bleak`, connecting briefly per page view. Proto module is pure functions, Python 2+3. *host-tested with a simulated adapter; never tried on an adapter.*
  - Depends on a firmware command `0x4B` "port status" that **does not exist in any remote iBlue branch** (see C-1).
  - Open hardware questions it lists itself: can the Pi connect outside the 60 s pairing window, and does a config connection take a Bluetooth slot and shift ports.
- DD-5 Later idea: read what a wired port holds from openMenu over the modem/serial link, since the adapter cannot see it.

**Done on `unified` (D-5):** the openMenu poll reply carries `CALL dcnow|dcnet`, the network of the current call, beside `NET`, the selected one. **Done on `unified` (D-6):** MIT `LICENSE`.

**Done on `unified` (D-3):** the numbers module, number matching, hang-up numbers and busy tone, and the `switcher` actions are removed; the hook only routes a call to DCNET when DCNET is selected. `tests/test_hook_routing.py` replaces the old number tests (609 tests pass). The Hang up button (restarts DreamPi) and the debug log stay.

### DC-iBlueControlMod (ESP32 BlueRetro fork for the internal Dreamcast adapter)

**Baseline (main):** vendor-firmware hardware port (pin map, button timings, hotplug), RGB power LED with orange/blue/custom colours via taps (IB-B1), port LEDs that never blink without a controller (IB-B2), boot animation "drops on water" (IB-B3), tap actions (IB-B4), four whole-configuration profiles (IB-B5), Dreamcast game id through one VMU block written by openMenu (IB-B6), BLE config service with `iBLU` info (command 0x40), hosted web config on GitHub Pages.

**Only on `claude/integration` (63 commits, includes web-config and test-fw):**
- IB-1 Restructure: fork code in `main/iblue/`, one Kconfig menu (`Kconfig.iblue`), one-line hooks in upstream files.
- IB-2 Web config redesigned: one page, one BLE connection, menu of Device / LEDs / Buttons / Controllers / Games / Update / Advanced, light and dark; BlueRetro's own pages shown inside.
- IB-3 "Dreamcast only" setting (default on): hides non-Dreamcast systems/labels/N64 tools.
- IB-4 Dreamcast controller presets page: standard, 6 button + dual stick, racing, keyboard, mouse; per-game save.
- IB-5 **Racing Controller** output mode (`0x000700FE`). *built, not tried on a console*
- IB-6 **Game id on/off** tap action (LED dips once = on, twice = off).
- IB-7 **Fireworks boot intro** (16 blinks, boom) replacing the scanner intro; the scanner version was seen per FEATURES.md. Controller LED brightness setting (20%).
- IB-8 **Animation settings over BLE** (0x47-0x4A, NVS): length, LED distance, density, tank length, group position, depth, level curve; "Save and play" preview.
- IB-9 Start-up order fixes (LED takeover before the firmware starts, error LED leaves GPIO14).
- IB-10 **Maracas output mode** (Samba de Amigo phase 1), from Sega's Maracas spec rev 0.99: capability `0x003C090F`, analog X/Y per maraca, shake as button, lost flag, 4-7 s start-up. Preset on the web page.
- IB-11 **Joy-Con motion as maracas:** IMU parsing, tilt/heading/shake maths (host-tested), calibration poses, BLE 0x4B-0x4F, preset "Samba de Amigo (Joy-Con motion)". *not tried on hardware*
- IB-12 CI builds a numbered test `.bin` into `firmware/test` (its TODO says test builds should not be committed to `main`).
- IB-13 Docs: FEATURES.md with seen / built / branch / planned / dropped.

**Not in integration, only on older branches (ideas to mine, D-2):**
- IB-14 **`brctl` control channel** (development, restructure): private Maple-bus function code; items INFO, STATUS, CONFIG (the whole BlueRetro config struct, 12,356 bytes), PROFILES, GAME; LED colour control, pairing control, restart; KOS client library `dc/brctl/` and example; protocol version 3 on restructure. *firmware side host-tested; library only compile-checked. Owner: experimental, never tested, not what was wanted.*
- IB-15 **Remappable button/touch gestures reported to Dreamcast software** (brctl v2, development) — a second gesture table beside integration's tap actions (see X-4).
- IB-16 **VMU LCD capture:** keep the picture the Dreamcast writes to the VMU screen (development / restructure `vmu_lcd.c`).
- IB-17 **Home + face button hotkey** loads profile 1-4 (restructure). The TODO replaces it with customizable Home combos (IB-P1).
- IB-18 Plain **HW1 / HW2 builds with the openMenu channel** (`dreamcast_openmenu` configs) so non-iBlue adapters get the features. TODO: "keep for now". Related to D-1.
- IB-19 **VMU-block settings channel** (openmenu-remap): eight blocks 200-207 of the emulated VMU as a request/answer window with `iBLU` magic, CRC32, sequence number, commands HELLO, PORT_INFO, GET/SET_MAP, SET_TYPE, RESET_MAP, COMMIT, REVERT, LIVE (press-to-learn), GET/SET_GLOBAL, PROFILE_*, GAME_*. 211 host checks. *superseded by brctl per restructure's doc; both are now ideas only*
- IB-20 Per-game configurations kept through a factory reset (`prof_*`, `DC_*`) (restructure).
- IB-21 Early boot animation implementation (led-behaviour; `iblue_boot_anim.c`, `docs/led-behaviour.md`) — see X-6.

**Planned in iBlue's own TODO (no code):**
- IB-P1 **Customizable Home + key combos:** hold Home, press a second button to run a user-chosen action on that controller; second button hidden from the game; buttons incl. d-pad, face, shoulders, sticks, DS4/DS5 touchpad halves; actions: rumble, next/chosen type, profile 1-4, game id, colour, disconnect, later network switch. Table on web page.
- IB-P2 **LED colours by network** (DCNow! / DCNET) and a button to switch network. Needs the control channel. **Core cross-repo feature.**
- IB-P3 Tap-action list changes; better factory reset (10 s hold too easy); measure real BIOS boot animation length; Panther mode `0x003FFFFF` and full device info reply.
- IB-P4 Check before `main`: every config builds (hw1, hw2, iblue), host + web tests pass.

### DC-BlueRetro
- BR-1 Nothing unique. `master` is upstream at its last commit. `unified` = master.
- BR-2 **iBlueControlMod's `main` is this code plus 158 changed files** (iBlue hardware port, fork features, web config): one codebase today. **Decision D-1:** DC-BlueRetro means "the firmware for BlueRetro-style ESP32 wiring" and is probably best a **build target of DC-iBlueControlMod** (it already has `configs/hw1/dreamcast` and `configs/hw2/dreamcast`). Tentative; repo name and fate open.

### DC-openMenu-Virtual-Folder-Bundle (the openMenu fork, "Wave")
**Baseline (main):** openMenu Virtual Folder Bundle (virtual folders/subfolders, per-game virtual VMUs, modified GD MENU Card Manager).

**Only on `dev` (307 commits, includes five branches):** a rewrite as a BIOS-look menu ("NewOpenMenu"):
- OM-1 Engine: Ninja model parser, script VM, sounds from the boot ROM driver, TV safe area, DMA vertex path, perf tooling. Needs the user's own `dc_boot.bin`.
- OM-2 Main menu Play / File / Internet / Settings; Music replaced by Internet; header clock formats incl. Swatch .beat; **online lamp + "DCNow! n DCNet n"**; header logo switches to **Now!** or **DCNET** from the Pi's `/tag`.
- OM-3 Play: 7-row list, spinning GD disc, 3D jewel case, X = options popup, Y = case viewer, favourites (own `EXTENDED.SYS`), Recent / Folders / Sort / Group bottom row, disc grouping, release-year sort by IP.BIN date. Genre grouping is off (data unreliable).
- OM-4 File: BIOS file screen, controller pills per port A-D with type labels, VMU colour/logo.
- OM-5 **Serial VMU** (restore before launch, backup at next start, five slots) with 1.7.1's SD layout; keeps `OPENMENU.SYS` unchanged so Derek's 1.7.1 still works.
- OM-6 Internet: Players, Games (A starts the game if on the card), Events (4 per page), News (read directly from dc.dreamcastlive.net / dc.segaonline.net, parser host-tested, never run against real pages).
- OM-7 **DreamPi link:** polls `GET /openmenu/poll` every 3 s, uploads the game list, receives LAUNCH / NET / PLY / EVENT lines, `POST /dcnow` `POST /dcnet`. Serial PPP path (no adapter) with `ATDT`.
- OM-8 Settings: scrollable BIOS-style list, background colour choice, dark mode, settings read from stock Card Manager `DEFAULTS.INI [BIOSMENU]`.
- OM-9 Folders themes in the DreamPi page's style (orange/blue).
- OM-10 Hardware checklist is the user's own notes (`TEST_CHECKLIST.md`): one screen at ~19 fps, "never connects" online (modem/serial/W5500 all tried, undiagnosed), 4:3 vs 16:9 text issue.

**Not in dev:**
- OM-11 `PS2`: PS2-style menu skin (memory-card grid list, glass cubes, prism settings scene, start-up intro), measured from PCSX2 video. Experiment.
- OM-12 `animated-theme`: animated 3D backdrops (waves, synthwave) and glass panels for Folders themes; theme builder tool improvements.
- OM-13 `dreamy-planck`: quick network-switch buttons that dial and hang up, Custom Dial numpad, "Auto DCNet / Auto DCNow!" boot modes, merged DCNow!+DCNET player list from dc99.net, "Options" menu with "Change DreamPi to DCNET / DCNow!". **D-3: the dialing parts (Switch by dialing, Custom Dial pad, auto-switch by dialing) are dropped.** Worth keeping as ideas: the merged player list, the "Options" menu, and a network switch done over the data channel instead.
- OM-14 `iblue-controllers`: Controllers screen (type, rumble, VMU, dual stick, pairing mode, per-button remap, profiles, per-game settings with Y in the game list). Written for brctl, first for the VMU channel; it has to be redone for the new channel (D-2). The screen design is the reusable part.
- OM-15 `gameid`: tells the adapter which game starts through one VMU block.
- OM-16 `serial-vmu-safety-spec`: design only. Tag in root block 0x60 (`OMSV`, version, slot, revision, lineage id, launch nonce, game id, CRC32) so Serial VMU never overwrites a non-Serial VMU or older save. Stated reason: other devices may sit on that port (BlueRetro, Pico2Maple, USB4Maple).
- OM-17 `block-46-font-size`: fixed small font for the Serial VMU progress line.

**Planned in openMenu's brief/roadmap (idea list):**
- OM-P1 Per-game network (DCNow!/DCNET) remembered per game, global default, precedence per-game > global.
- OM-P2 Y screen: description/genre/credits from META.DAT, scroll, A launches, Y snap-then-flip.
- OM-P3 Version organisation (PAL/NTSC/translation/serial-patch) and version picker; META2.DAT for developer/publisher/year.
- OM-P4 Group by developer, publisher, local/online multiplayer, modem, serial, W5500, broadband.
- OM-P5 Star player/game/event and notification banner (needs Pi-side storage).
- OM-P6 Online game catalogue prepared on the Pi.
- OM-P7 ISP settings editor; DreamPi Ethernet/Wi-Fi settings; interface tests (modem, BBA, serial, W5500; speed, port forwarding).
- OM-P8 Flash partition compaction before the ISP editor.
- OM-P9 File menu hardware: VMU slot selection and GameID box (VM2, VMU Pro, pico2maple), controller type per port, **Bluetooth pairing per port**, cheats in the X popup.
- OM-P10 No-DreamPi network (a todo noted in CLAUDE.md).
- OM-P11 **(D-5)** Work equally well on DCNow! and DCNET: try DCNET's login as well as its own, read `NET` (selected) and `CALL` (connected) from the poll reply, say so when they differ, and offer `POST /dcnow` / `POST /dcnet`. Pi side done (`CALL` line).

---

## 3. Same idea, implemented differently (the unification targets)

| ID | Idea | Implementations found | Problem |
|---|---|---|---|
| X-1 | **Dreamcast ↔ device control channel** | (a) brctl: private Maple function code, config struct, 512 B transfers. (b) VMU blocks 200-207 request window with `iBLU`, CRC32, seq. (c) Game id in one VMU block (IB main, OM gameid). (d) openMenu → Pi: plain HTTP over PPP (`/openmenu/poll`, `/tag`, `/players`). (e) ~~openMenu → Pi: dialing special numbers~~ removed (D-3). (f) PicoLink: USB vendor interface for the SD link, STATS/TRACE via dctest. | Separate channels, each with its own byte format and its own way to detect the other side. **D-2: a new design replaces (a), (b), (c).** |
| X-2 | **Profiles** | IB main: 4 slots, each the **whole** config, saved from the web page or a tap. brctl/restructure: 4 slots per **port** (type + map). VMU channel: 4 slots per port. DD module: "load profile" button (whole config). | "Profile" means two different things. IB's TODO already notes two formats exist |
| X-3 | **Game id / per-game settings** | IB `iblue_gameid` VMU block (main); brctl `GAME` item; VMU channel `GAME_SET` (≤16 chars A-Za-z0-9-_); openMenu already sends the id (and the game's name) to VM2/VMU Pro/USB4Maple/Pico2Maple as Maple command 0x21 (see `05-pico2maple.md`, C-9); PicoLink roadmap "per-game choice from last-played file"; OM per-game network (OM-P1); Serial VMU keyed by game id. | Game id length differs (12 vs 16 chars); iBlue is the only adapter that does not answer 0x21. One id should key controller config, network choice, VMU slot and PicoLink link choice |
| X-4 | **Button gestures / hotkeys** | IB `iblue_gesture` tap table (main/integration); brctl v2 gesture table reported to Dreamcast (development); `button_map.c` (restructure); Home+face hotkey (restructure); planned customizable Home combos (IB-P1). | Two tables, two profile-hotkey routes, one planned replacement |
| X-5 | **Network selection (DCNow! / DCNET)** | DD: web buttons, GPIO buttons, `POST /dcnow` `/dcnet`, `/tag` (DCNET, DCNOW, DCNET_OFF, INACTIVE). OM: `POST /dcnow` `/dcnet`, OM-P1 per-game. IB-P2: LED colour + button. PicoLink: priority W5500 → serial → modem. (Dial numbers: removed, D-3.) | "Network" (service) and "link" (physical path) are mixed up in the wording. Each repo wants to both show and set it. See C-2 |
| X-6 | **Status/LED language** | DD LED module: NeoPixel rows by colour/animation/priority. IB: RGB power LED, port LEDs, boot animation. OM: online lamp, header logo, panel border follows network. PicoLink: status lines in journalctl. | DD has 15-colour palette ids and message priorities; IB hard-codes orange/blue/custom |
| X-7 | **Web configuration surface** | DD page kit (layout.json, palette, module picker). IB hosted web config (panels, one BLE connection, light/dark). OM Folders themes copy the DD page style. DD-4 shows IB ports as tiles. | Separate UIs with separate looks. PicoLink has none |
| X-8 | **Virtual SD / save storage** | PicoLink `dcsd` image on host (+PL-R1 physical slot). DD-3 DC Bridge save manager (history, queued edits). OM Serial VMU (`/sd/OPENMENU/SAVES/<id>/SLOTn.VMU`) and safety tag (OM-16). PL-R3 openMenu "no SD card" bug. | The SD layout must be shared by PicoLink, openMenu and any save manager |
| X-9 | **Capability discovery / hello** | IB: `iBLU` info with feature bits (0x40). VMU channel: HELLO with versions and limits. DD: `launcher` in module.json, `openmenu 1` poll header, ignore-unknown-lines rule. PicoLink: first status line with `git describe`. OM: ROADMAP "Blocked" rows because no protocol. | No shared "what can you do / what is your version" |
| X-10 | **Online data (players, events, news)** | DD players + events modules (Pi-side). OM Internet screen fetches news directly and also reads `/players`, `/api/events/upcoming`. OM-13 reads dc99.net. | OM's own brief says the Pi should collect, but it implements direct fetches. DreamPi already has the data |
| X-11 | **Serial/cable link status on the Pi** | DD-2 Serial PPP + KOSnet rows. PicoLink status lines. DD-3 "say why not connected". | The Pi dashboard does not read PicoLink's status |
| X-12 | **Status words in docs** | IB: seen / built / branch / planned / dropped. DD: verified on hardware vs not. OM: Done / Partial / Missing / Blocked / Conflict plus checklist. PicoLink: validated/soaked. | Four vocabularies. DD's rule: never claim hardware behaviour that was only simulated |
| X-13 | **Pairing** | IB 60 s window, tap, web; DD-4 "Search again"; OM-P9 per-port pairing; IB-P1 combos. | Several places want to start pairing |
| X-14 | **Controller type vocabulary** | IB: Normal / Dual stick (`GamePadAlt`) / Keyboard / Mouse / Racing / Maracas. DD-4: first four only. OM: pills per port from maple function codes. | New output modes not known to DD or OM; "Dual stick" label unverified |
| X-15 | **Install / update** | DD installer + `.pth`; PicoLink one-file DreamPi installer; IB OTA + CI test builds; OM Card Manager. | Four update stories |

---

## 4. Conflicts and collisions

- **C-1 BLE command 0x4B.** DD-4 assumes `0x4B` = port status (firmware branch `claude/port-status`, which is not on the remote). IB integration already uses `0x4B` = get motion settings, and 0x4C-0x4F for motion. Port status itself is not implemented anywhere. Needs a reserved command table. *Open.*
- **C-2 Network-select numbers.** ~~Three different schemes.~~ **Resolved by D-3:** no numbers at all. **New consequence:** with DCNET selected, openMenu's connect is routed to DCNET and its login can fail. **Decided (D-5):** openMenu tries DCNET's login too and reads the connected and selected network from the API (Pi side done, openMenu side OM-P11). *Open until the openMenu side exists.*
- **C-3 brctl vs VMU channel.** *Resolved by D-2:* both are ideas, a new channel will be designed. The openMenu Controllers screen (OM-14) and IB-P1/IB-P2 wait for it.
- **C-4 Reachability.** brctl and the VMU channel only exist on a port with a connected Bluetooth controller (hotplug on), so openMenu cannot configure an empty adapter. DD-4 has the mirror problem over BLE (pairing window). *The new channel must say what it does here.*
- **C-5 Two branch lineages in iBlue.** `integration` holds test-fw + web-config + restructure's *layout*, but not restructure's brctl/hotkey/profile code. With D-2 the brctl-based parts are not ported; the profile hotkey (IB-17) and per-game keep-through-reset (IB-20) are the pieces still worth porting by hand.
- **C-6 `OPENMENU.SYS` compatibility.** OM must not change it (Derek's 1.7.1 reads it). Anything that wants per-game network or controller data needs its own file (`EXTENDED.SYS`).
- **C-7 Name spelling.** DD: UI says **DCNow!** and **DCNET**, code keeps `dcnow`/`dcnet`, DreamPi's own string is `DCNet`. OM and IB docs mix `DCNet`, `DCNET`, `DC Now!`.
- **C-8 "Dual stick" is a guess.** DD-4's docs say to check `GamePadAlt`'s label against the adapter.
- **C-9 Game id: iBlue does not follow the existing protocol.** openMenu (`main`) already sends the game id and name to VM2, VMU Pro, USB4Maple and Pico2Maple as Maple command `0x21`; iBlue reads a VMU block instead (IB-B6, OM-15). Proposal: iBlue answers `0x21` and the VMU-block game id goes. Details in `05-pico2maple.md`. *Open: owner's yes/no.*

---

## 5. Decisions and what is still open

Decided on 2026-11-03 (details in `03-decisions.md`): D-1 BlueRetro-wiring firmware = another build of DC-iBlueControlMod (tentative); D-2 a new control channel; D-3 no telephone numbers (done in DC-DreamDash); D-4 docs and cross-linked READMEs and licenses in every `unified`; D-5 openMenu works on both networks and learns both from the API; D-6 DC-DreamDash is MIT.

Still open:
1. The new control channel's design.
2. The openMenu side of D-5 (OM-P11), and whether DCNET's login also works on DCNow!.
3. Pico2Maple (see `05-pico2maple.md`, ids `PM-`): the patch's license, whether to ask the author for source, and whether iBlue answers Maple `0x21` for the game id (C-9).
4. DC-BlueRetro's final name/fate, and the openMenu repository name.
