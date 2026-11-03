# Pico2Maple (`PM-`): what the decompilation shows

Supplements `01-feature-inventory.md` (adds the `PM-` ids and amends X-3, X-8, X-14 and the channel table of `02-unified-language.md`).

## What was analysed

The owner supplied `pico2maple_decomp.zip` on 2026-11-03: a Ghidra decompilation (RP2040 and RP2350 images) of **`pico2maple_2026-06-14.uf2`, the stock USB-only build of cluoma/Pico2Maple-fw**, with reconstructed structs, a function map and a known-devices table. It is **not** the owner's modified firmware; that arrived afterwards and is described in the next section. Names in the decompilation are the decompiler author's interpretation; the zip's own README says field names are "educated guesses unless noted". Nothing below was run on hardware.

Pico2Maple is a commercial Dreamcast adapter by Luoma Lab (cluoma): a Pico 2 W based USB/Bluetooth controller adapter that also acts as a VMU with 10 slots, flash or microSD storage, and an OLED showing the VMU screen. **The license of the firmware repository was not found** (the zip does not state it); check it before copying any code. The zip also bundles third-party libraries (pico-sdk, TinyUSB, tusb_xinput, no-OS-FatFS) whose licenses apply separately. The compiled images and Ghidra projects from the zip are not copied into the family repositories.

## Features (PM-B = the stock build)

- **PM-B1 Maple peripheral side.** One Pico plays the *peripheral* on the Maple bus (the other repos' firmware or openMenu play the console or the host side). Two PIO programs (`maple_rx`, `maple_tx`) are the same as in the open dreamwave sources apart from an RP2040 variant of the receiver. Core 1 runs the bus loop; core 0 runs USB host.
- **PM-B2 Functions it presents:** controller (`Dreamcast Controller`), Racing Controller (G29 wheel), keyboard (func 0x40), mouse (func 0x200), VMU (storage, LCD, clock/buzzer), rumble. Extended product name string `Pico2Maple USBBT`.
- **PM-B3 USB host drivers:** about 14 driver kinds (Steam Controller and 2026 model, three 8BitDo, XInput, PS3 and ~55 third-party PS3-mode pads, DS4, DualSense and Edge, Switch Pro, Logitech G29, a generic report-descriptor driver, a 105-entry VID/PID table). The 68-entry known-device table is in the zip's `known_devices.csv`. Bluetooth is a separate build (the string `USBBT` and the Pico 2 W suggest it; not in this image).
- **PM-B4 Button mapping:** 28 Dreamcast targets × up to 4 sources from `pico2maple_controller_map.cfg` on the SD card (text file `TARGET = SRC[,SRC…]`; a sample is written if missing). Normalised input of 25 bytes. A hotkey modifier (select) exists; holding A+B+X+Y with D-pad left/right switches two stick-deadzone presets (XInput).
- **PM-B5 VMU emulation:** 10 internal flash slots (128 KB images) or SD files `vmuNN.bin`; only dirty 4 KB blocks are written back; short press of the front button = next slot (3 s and 8 s holds are detected but unused). A 0x55 header check formats a blank slot.
- **PM-B6 Per-game VMU ("GameID", slot 10):** on SD only, `gameid/<ID>/<ID>-N.bin`. The game id comes from **Maple command `0x21`** (the decompiler's comment: "GDEMU/openMenu GameID extension"): the first 12 bytes are the id, further words (up to 128 bytes) are stored too, meaning not determined. An id that reads as `DCBIOS` is ignored. A settings flag ("gameid autoload") and a mapping flag `autoload` exist; what the second one switches is not clear from the decompilation.
- **PM-B7 Settings:** 256 bytes at flash 0x3FC000 or `pico2maple_settings.bin` on SD (XOR checksum); fields: VMU slot, mapping, game id, game-id autoload.
- **PM-B8 Hardware:** SSD1306 OLED (I²C 0x3C) shows the VMU LCD; buzzer; rumble; front button; SD on SPI1; alternative pin layout when GPIO15 is grounded; one UF2 holds the RP2040 and the RP2350 image.

## The owner's modified firmware: composite keyboard/mouse toggle (`PM-1`)

Supplied as `composite_toggle_patch.zip`: `pico2maple_2026-06-14_composite-toggle.uf2` plus the patch source and build scripts (`composite_toggle.c` about 100 lines, `chips.py`, `build.py`, `patch.ld`, `emu_test.py`).

- **What it is:** a **binary patch of the stock 2026-06-14 UF2**, not a source fork. New code is placed at `0x10030000`; three stock functions are hooked (HID mount, HID unmount, front-button poll); the heap limit is lowered by 2 KB to hold the patch's state. Addresses are hard-coded per chip in `chips.py`.
- **What it does (per its README):** holding the GPIO8 front button for 3 seconds switches a USB receiver that exposes a boot keyboard and a boot mouse as two interfaces between a Dreamcast keyboard and a Dreamcast mouse. The OLED shows the keyboard or mouse icon and the device is re-announced to the Dreamcast (about 300 ms). A short press still selects the next VMU slot (the stock firmware detects the 3 s hold and ignores it). Gamepads and single-interface devices behave as before. Flashing the stock UF2 reverts it; VMU data and settings are not touched.
- **Limitations it lists:** games may only notice the change at boot or on certain screens; receivers that put the mouse on a non-boot interface with report IDs are not covered; Logitech Lightspeed receivers keep the stock mouse-only behaviour; keys held during a toggle can look stuck.
- **Checked here:** a byte comparison of the patched UF2 with the stock images found exactly what the README says and nothing else: RP2350, 11 patched call sites (2 mount, 8 unmount, 1 button poll), the 2-byte heap limit and the new code; RP2040, 10 call sites, the heap limit and the new code. **Not checked:** that `build.py` reproduces the UF2 (it needs an ARM GCC at a fixed path and the stock images next to it), the 14 emulator scenarios, and any behaviour on a console.

What it means for the family:

- It is purely local. There is still **no hello, no API and no network channel** in Pico2Maple; the patch adds only a button gesture. Everything the unified plan wants from it (the `0x21` game-id decision, a hello, VMU slot selection from openMenu, status to DreamDash) is new firmware work.
- A binary patch ties the work to one stock build. When upstream releases a new version every address in `chips.py` has to be found again, and each new feature costs RAM and hooks. A source fork (if the upstream license allows it) would suit the family better; the decision is the owner's.
- **License:** the patched UF2 contains the whole stock firmware, so it can only be shared under the upstream license (not found). The patch source and scripts are separate and can live in a repository of their own; the UF2 should not be committed. The patch's own license has not been set.
- **X-4 (button gestures):** a 3 s hold is a button gesture like the 3/6/10 s holds on the iBlue adapter and the DreamDash GPIO buttons; one table of hold lengths and actions per device would make them comparable.
- **X-14 (output kinds):** Keyboard and Mouse are chosen here by a hold at run time, in iBlue by the configured output mode. The shared vocabulary calls both an *output mode*.

## What it means for the family (stock build)

- **C-9 A game-id carrier already exists, and it is not the VMU block.** iBlueControlMod (main) takes the id from VMU block 200 (written by an openMenu branch, OM-15), the abandoned brctl/VMU-channel designs carried it differently, and Pico2Maple takes it from **Maple command 0x21**. Neither openMenu `unified` nor the iBlue firmware uses `0x21` today (searched). If openMenu is to work with several adapters, the game id should be sent the way the devices already read it. Proposal for the new channel (D-2): **keep 0x21 as the game-id message** and let adapters that cannot answer it use their own fallback. Needs the owner's decision, and a check of what openMenu 1.7.x and GDEMU really send.
- **X-3 amended:** the de-facto game-id length is **12 characters** (Pico2Maple `gameid[12]`, iBlue `DC_<id>` 12 characters); the 16 of the abandoned VMU channel is the outlier. The shared record (`02`, section 7) should be 12.
- **X-8 amended:** a second SD layout exists: `/pico2maple/vmuNN.bin`, `/pico2maple/gameid/<ID>/<ID>-N.bin`, `/pico2maple/internal/vmu_internalNN.bin`. openMenu Serial VMU (`/sd/OPENMENU/SAVES/<GAMEID>/SLOTn.VMU`) and its safety tag (OM-16) must not clash with a Pico2Maple VMU on the same port, which is the very case OM-16 names. Both are 128 KB images, so they could be made interchangeable.
- **X-14 amended:** output kinds Pico2Maple presents (controller, Racing, keyboard, mouse) overlap with iBlue's (Normal, Dual stick, Keyboard, Mouse, Racing, Maracas). Maracas and dual stick are not in the Pico2Maple list.
- **X-2 / mapping:** a third mapping format (28×4 text file) next to BlueRetro's `map_cfg` and the removed brctl one; per-game mapping may exist (`autoload`, unclear).
- **X-9 hello:** the Dreamcast reads the usual Maple device info (name strings above) and nothing else was found; there is no Pico2Maple API for slot selection from the Dreamcast apart from `0x21`. OM-P9 (VMU slot selection from openMenu) therefore needs new firmware work; the owner's composite-toggle patch (`PM-1`) does not add it.
- **Same device, same port:** Pico2Maple, the iBlue adapter and a real VMU may sit on one controller port; the new channel must say how a client tells them apart (hello, section 4 of `02`).

## Open

1. Where the patch lives: a repository of its own in the family (for example DC-Pico2Maple, patch source and scripts only) or inside an existing one; and whether to keep patching binaries or fork the upstream source.
2. License of cluoma/Pico2Maple-fw and its permission for the family to include, mirror or fork code; and a license for the patch itself.
3. Whether to adopt Maple `0x21` for the game id in the new channel (C-9).
4. What openMenu and GDEMU actually send as GameID, and whether the Bluetooth build differs.
