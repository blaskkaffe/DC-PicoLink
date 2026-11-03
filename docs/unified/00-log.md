# Unified Dreamcast Ecosystem: work log

Newest first. "Done" is what actually happened; "To do" is what is still open.

## 2026-11-03 (late night, 4): Pico2Maple answers

**Done**
- Recorded D-8: iBlue should answer game id like the other adapters; the patched UF2 is shipped in `DC-Pico2Maple` (`firmware/patched/`, checksum identical to the supplied file); the decompilation stays out of the repositories until the upstream author has been heard from.
- Checked who added the VMU-block game id: a Claude session on 2026-10-30 (iBlue commit `c6137bd`, openMenu branch `claude/gameid`). Checked the license question: no license file has ever been in the copy of the upstream repository.

**To do**
- Change the iBlue firmware to answer Maple `0x21` and drop the VMU-block game id (cannot be built here: needs ESP-IDF).
- Choose the patch's license; hear from the upstream author.

## 2026-11-03 (late night, 3): DC-Pico2Maple repo, and a correction

**Done**
- Looked at `blaskkaffe/DC-Pico2Maple`: a copy of the upstream firmware download repo (UF2 files, README, changelog; no source, no LICENSE file).
- **Correction:** I had written that openMenu does not use Maple `0x21`. It does: `vm2_set_id()` sends command 33 with the memory-card function, a 12-byte id and the game name, to VM2, VMU Pro, USB4Maple and Pico2Maple. iBlue is the one that does not follow it. Fixed in `05`, `01` and `02`; the earlier search had a filter that hid the result.
- Added the repo to the family links and put the patch there (see the next entry).

## 2026-11-03 (late night, 2): the owner's Pico2Maple patch

**Done**
- Read the modified firmware (`composite_toggle_patch.zip`): a binary patch of the stock build that toggles a combo receiver between Dreamcast keyboard and mouse with a 3 s hold of the front button. Compared the patched UF2 with the stock images: the differences are exactly the ones its README lists. Added it to `05-pico2maple.md` as `PM-1`.

**Limits**
- Not rebuilt (the build script needs an ARM toolchain at a fixed path), the emulator tests were not re-run, nothing tried on a console. The patched UF2 was not copied anywhere (it contains the upstream firmware).

**To do**
- Decide where the patch lives and whether to patch or fork; check the upstream license.

## 2026-11-03 (late night): Pico2Maple received

**Done**
- Read the decompilation the owner supplied (stock `pico2maple_2026-06-14.uf2`, RP2040 and RP2350) and wrote `05-pico2maple.md`: features (`PM-B1` to `PM-B8`), and how it touches the other repos.
- Main finding: Pico2Maple reads the game id from **Maple command 0x21**, which neither openMenu `unified` nor the iBlue firmware uses (iBlue reads a VMU block). Also: the game id is 12 characters there and in iBlue, and a second SD layout exists for VMU files.

**Limits**
- This is the stock upstream build, not the owner's modified firmware. Function names come from the decompiler's author; nothing was run on hardware. License of the firmware repository not found.

**To do**
- Decide on `0x21` (C-9); check the license.

## 2026-11-03 (night): DreamDash license and openMenu on both networks

**Done**
- D-6: `LICENSE` (MIT) added to DC-DreamDash `unified`, README license section and the shared license overview updated.
- D-5: the openMenu poll reply of DC-DreamDash has a new `CALL dcnow|dcnet` line (the network of the current call, from the hook's state file), beside `NET` (the selected network). Two new tests; the openMenu tests pass. Documented in `docs/openmenu.md` and `docs/hook.md`. **Not run with a real openMenu or call.**

**To do**
- The openMenu side (OM-P11): retry the login with the other network's, show/act when `NET` and `CALL` differ. Cannot be built here (no KallistiOS), so it needs a build environment.
- Confirm on hardware whether DCNET's login also works on DCNow! (openMenu's own header says DreamPi accepts any login on DCNow!).

## 2026-11-03 (evening): decisions applied

**Done**
- Recorded the owner's decisions in `03-decisions.md` (D-1 BlueRetro-wiring firmware as a build of DC-iBlueControlMod, tentative; D-2 new control channel; D-3 no telephone numbers; D-4 docs, cross-linked READMEs and licenses in every `unified`).
- **DC-DreamDash `unified`: telephone-number switching removed.** Deleted the `numbers` module, its tests and the switcher's number actions and hook file; the hook no longer matches digits, answers a busy tone or hangs up by number, it only routes a call to DCNET when DCNET is selected. Docs, README, CLAUDE.md and UI scripts updated, `install.sh` removes an old `numbers.json`. New `tests/test_hook_routing.py`. All 609 unit tests and the UI audit pass off-hardware. **Not run on a real call.**
- Kept on purpose: the Hang up button (restarts DreamPi), the debug log module (it decodes what the modem reports; its wording about "misheard numbers" was changed), and openMenu's fixed connect number (it places the call).
- Updated inventory, vocabulary and this log; copied them to `docs/unified/` in all five repos; added an ecosystem cross-link block and a License section to each README.

**Found while doing it**
- With DCNET selected, openMenu's connect is routed to DCNET and its login can fail (C-2). Needs a decision.
- BlueRetro's artwork in `static/` is CC BY-ND 4.0 (no derivatives) and its hardware is CERN-OHL-P-2.0; software Apache-2.0.
- The openMenu repository is `DC-openMenu-Virtual-Folder-Bundle`, not "DC-openMenu-Wave".

**To do**
1. Decide the open questions in `03-decisions.md`.
2. Design the new control channel (`04-control-channel.md`).
3. Fix the BLE `0x4B` collision (C-1) before more work on DC-DreamDash's iBlue module.
4. Add Pico2maple (`PM-`) when its firmware is pasted.
5. Start taking pieces from the old branches into `unified` one by one, after the channel and vocabulary are settled.

## 2026-11-03 (afternoon): branch survey of all five repos

**Done**
- Fetched every branch and full history (the clones were shallow and single-branch). Confirmed `unified` = baseline in all five repos. Compared every branch to its repo's baseline (commits, diffstat, own docs) and mapped which branches contain which.
- Wrote the feature inventory and the draft vocabulary.

**Limits**
- Pico2maple not covered. The GitHub CLI token in the environment is invalid, so only git history was read (no issues, pull requests or CI results).
- openMenu `dev` (307 commits) was sampled by commit subject and its roadmap/brief, not read diff by diff.
- Hardware claims are quoted from each repo's docs, not tested.
