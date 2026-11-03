# Decisions

Decisions made by the project owner. Newest first. Each one says what it changes. Anything not listed here is still an open question (see the end).

## 2026-11-03

| # | Decision | What it changes |
|---|---|---|
| D-1 | **DC-BlueRetro is the firmware for the BlueRetro-style ESP32 wiring**, and it may simply be **another build of the DC-iBlueControlMod repo** (tentative: "would make more sense possibly"). | DC-iBlueControlMod already has `configs/hw1/dreamcast` and `configs/hw2/dreamcast` next to `dreamcast_iblue`. One codebase, several build targets. DC-BlueRetro stays an untouched mirror of upstream until the owner decides to retire it. Open: the final repo name and whether it is archived. |
| D-2 | **The Dreamcast ↔ adapter control channel is a new design.** The existing ones (`brctl`, the VMU-block channel) were experimental, never tested and not what the owner wanted. | `brctl` and the VMU-block channel are treated as **ideas to mine**, not as the base. A new channel gets designed in `docs/unified/04-control-channel.md` (not written yet) on top of the shared vocabulary. Anything that waits for the channel (profiles per port from openMenu, game id, hotkey actions, network colours) stays "blocked". |
| D-3 | **No telephone numbers at all.** The test numbers for switching the network were a first experiment before it was clear that it can be done over the data channel. All telephone-number features are removed in the unified branch. | Done in DC-DreamDash (`unified`): numbers module, number matching, hang-up numbers, busy tone and the `switcher` actions are gone; the network is selected over the page, the GPIO buttons and `POST /dcnow` / `POST /dcnet`. openMenu `unified` has no switching numbers (it still dials its fixed connect number, because it has to place the call). The unmerged openMenu `dreamy-planck` ideas that use dialing (Switch buttons, Custom Dial pad) are dropped. |
| D-4 | **Docs live in `unified` in every repo.** All READMEs on `unified` link each other, and every license is stated properly in each. | `docs/unified/` in each repo, a cross-link block and a License section in each README. |

## Consequences found while doing D-3 (need a decision)

- **openMenu connect while DCNET is selected.** Before, openMenu's fixed dial number selected DCNow! by itself. Now nothing does. If DCNET is selected, openMenu's call goes to DCNET and its login can fail. openMenu can only `POST /dcnow` after it is connected. Until a solution exists, DCNow! has to be selected from the page or a GPIO button first. Options: openMenu tries DCNET's login as well; DreamPi side accepts openMenu's login on both; the Pi answers "who is calling" some other way. **Not decided.**
- **DC-DreamDash has no license file.** Only third-party notices exist. See `README.md` of that repo.

## Still open

1. Pico2maple firmware: to be pasted by the owner (a `PM-` section follows).
2. The new control channel's design (D-2).
3. License for DC-DreamDash.
4. The open-menu connect problem above.
5. Final names: DC-BlueRetro's fate (D-1) and the openMenu repository, which is currently `DC-openMenu-Virtual-Folder-Bundle` while the project notes call the fork "DC-openMenu-Wave".
