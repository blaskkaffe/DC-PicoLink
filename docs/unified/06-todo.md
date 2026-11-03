# To do

The owner's list across all repos. Newest additions are marked with their date. Details and reasons are in `00-log.md` (what was done), `03-decisions.md` (what was decided) and `01-feature-inventory.md` (ideas to take from old branches). "Written" means the code exists but has not been built or tried on hardware.

## Next step (added 2026-11-03; the order of the two is not decided)

- [ ] **T-1 Design the new Dreamcast ↔ adapter control channel** (`04-control-channel.md`, decision D-2). `brctl` and the VMU-block channel are only ideas to mine. Game id is already solved (Maple `0x21`), so the channel carries config, profiles, pairing, LED/network colours, per-port setup. Must say what happens when no controller is connected (C-4) and use the shared vocabulary (`02`).
- [ ] **T-2 Fix the BLE command `0x4B` collision (C-1).** DreamDash's iBlue module expects `0x4B` = port status; the iBlue firmware already uses `0x4B`-`0x4F` for Joy-Con motion. Make one reserved-ID table and move the port-status command; port status itself is not implemented anywhere yet.

## Build and try (needs a toolchain or hardware)

- [ ] **iBlue firmware with the `0x21` game id** (written, host-tested): build with ESP-IDF in CI, flash, start a game from openMenu, check the game id in blueretro.io, and check on a console that the BIOS and games still see the VMU (the extended-info reply is longer than before).
- [ ] **openMenu fork with the `iBlueControlMod ` marker** (written): build with KallistiOS.
- [ ] **Patched Pico2Maple UF2** (keyboard/mouse toggle): try on a console.
- [ ] **DreamDash:** network switching over the data channel on a real call; the new `CALL` line with a real openMenu.
- [ ] **Does DCNET's login also work on DCNow!?** If yes, openMenu can use one login for both networks.

## Write

- [ ] **openMenu side of "works on both networks" (OM-P11, D-5):** retry the login with the other network's, read `NET` and `CALL`, say so when they differ, offer `POST /dcnow` / `POST /dcnet`. Until then: select DCNow! on the page or a GPIO button before connecting from openMenu.
- [ ] *(optional)* **A "leave game" message:** openMenu sends the game id only when a game launches (and `DCBIOS` only to real VM2 devices), so iBlue keeps the last game's configuration until the next game or a restart. Could send `DCBIOS` to iBlue too and let iBlue treat it as "no game". Decide first whether you want that.
- [ ] **Take pieces from the old branches into `unified`, one at a time**, after T-1 and T-2: e.g. iBlue `integration` (web config redesign, Racing and Maracas modes, boot animation settings), the profile hotkey and keep-per-game-config-through-reset, PicoLink cable diagnostics (PL-1 to PL-6), DreamDash Serial PPP / KOSnet rows and the noautoupdates switch. List in `01`.
- [ ] **DC-BlueRetro as a build target of DC-iBlueControlMod** (D-1, tentative): decide the final name and what happens to the mirror repository.

## People and decisions

- [ ] **Colin Luoma (Pico2Maple author):** ask about source access or permission, and about the decompilation. Until then the decompilation stays out of every repo (D-8c).
- [ ] **Names:** the openMenu repository is `DC-openMenu-Virtual-Folder-Bundle` while the project notes say "DC-openMenu-Wave".
- [ ] **Docs size:** these shared documents are copied into six repositories and the Project; decide whether the Project stays the master copy.
