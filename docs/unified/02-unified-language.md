# Unified language: draft vocabulary and rules (proposal)

Built from what the five repos already say. Where a repo already has a rule, it is quoted as the starting point. Items marked **needs yes/no** are not decided. See `01-feature-inventory.md` for the evidence (X-, C- ids) and `03-decisions.md` for what the owner has decided.

## 1. Proof words (replaces four status vocabularies, X-12) — needs yes/no

One scale for "how far did it get", used in every repo's docs and in any capability report:

| Word | Meaning | Maps from |
|---|---|---|
| `idea` | written down only | IB planned, OM Missing |
| `written` | code exists, not compiled in CI | IB branch |
| `built` | compiles in CI | IB built |
| `host-tested` | tests on a PC or emulator pass | DD tests, OM host tests, PL model tests |
| `hw-seen` | someone saw it work on real hardware (video counts) | IB seen |
| `hw-verified` | measured or repeated on hardware with a logged result | DD "Verified on hardware", PL soak |
| `blocked` | needs something else first (name it) | OM Blocked |
| `superseded` / `dropped` | replaced / decided against | IB dropped |

Rule taken from DC-DreamDash and kept for all: **never claim hardware behaviour that was only simulated.** A separate "where" column says baseline / branch name / planned.

## 2. Words with two meanings today — needs yes/no

| Word | Today | Proposed |
|---|---|---|
| network | DCNow! / DCNET (a service) and also modem/serial/W5500 (a path) | **network** = service (DCNow!, DCNET). **link** = physical or logical path (modem, serial, W5500, BBA, Wi-Fi, USB-net) |
| channel | brctl, VMU-block channel, "openMenu link", SD link | **channel** = a way two devices exchange commands (over a link). Each channel has a name (`pi-http`, `ble-cfg`, `usb-sd`, and the new Dreamcast ↔ adapter one) |
| profile | whole-adapter config (IB main) vs per-port type+map (brctl) | **profile** = whole adapter configuration. Per-port piece = **port setup** |
| game id | 12 vs 16 chars, charset differs | **game id** = one string (suggest ≤16 of `A-Za-z0-9-_`), used as the key for port setup, network choice, VMU slot, link choice |
| module | DD modules vs IB Kconfig options vs OM settings rows | **module** stays DD's term. IB: **feature** (a Kconfig switch). OM: **setting** |
| port | A-D controller ports vs serial/USB ports | **port A-D** = controller port; **serial port** / **USB port** spelled out |
| type | "Controller type" Normal / Dual stick / Keyboard / Mouse / Racing / Maracas | **output mode** (BlueRetro's `dev_mode`), labels in one shared table |

## 3. Names and spelling

- UI text: **DCNow!** and **DCNET** (DC-DreamDash's rule), buttons "DCNow!" and "DCNET". Code and file names keep `dcnow` / `dcnet`. Upstream strings (DreamPi's `DCNet`, `[DCNet]` in netlink_config.ini) are not changed.
- Repos: `DC-` prefix, as already renamed: DC-DreamDash, DC-PicoLink, DC-iBlueControlMod, DC-BlueRetro, DC-openMenu-Virtual-Folder-Bundle (project notes call the fork "DC-openMenu-Wave"; the name is not final).
- Ports: letters **A-D** everywhere (openMenu, DD tiles, IB all already use them). Per-port colour: A orange, B green, C blue, D bright pink (DD-4's palette picks).
- Status lines: plain English, short and specific, read in `journalctl` (DC-PicoLink's rule); first line carries `git describe`.
- **No telephone numbers** anywhere as a control mechanism (D-3). The only dialed number left is the fixed one that places the call itself.

## 4. Capability and version report (X-9) — needs yes/no

Proposal: every device and service can answer one **hello**:

- `kind` (adapter, cable, pi-addon, menu, ...), `name`, `version` (git describe), `protocol` (integer),
- a list of **capabilities** by name (not bit numbers), e.g. `ports`, `profiles`, `game-id`, `led`, `network-switch`, `sd`, `w5500`, `serial`, `bt-pairing`, `vmu-slots`,
- `limits` it wants the client to respect.

Rules lifted from existing code: unknown capabilities and unknown lines are ignored (DD poll rule); old clients keep working (OM's `OPENMENU.SYS` rule); the answer is plain and cheap enough for a Dreamcast to read.
Existing material to reuse: IB `iBLU` info + feature bits, DD `launcher` announcement, DD `/tag` codes (DCNET / DCNOW / DCNET_OFF / INACTIVE).

## 5. Command and ID tables (C-1)

- One **reserved-ID table per channel**, kept in one shared doc. IB's BLE today: 0x40 info, 0x41-0x42 gestures, 0x43-0x46 profiles, 0x47-0x4A animation, 0x4B-0x4F motion. Port status must take a new number (not 0x4B), and DC-DreamDash's iBlue proto module must follow the table.
- Names of commands identical across channels where the meaning is identical (`PROFILE_SAVE`, `PROFILE_LOAD`, `GAME_SET`, `PORT_INFO`, `COMMIT`, `REVERT`).
- Framing: magic, protocol version, sequence number, CRC32 for anything carried over an unreliable path.

## 6. Channels (X-1)

| Channel (link) | Between | Carries today |
|---|---|---|
| `pi-http` (PPP over modem or serial) | openMenu ↔ DC-DreamDash | game list, launch, `/tag`, `/players`, events, `POST /dcnow` `/dcnet` |
| *new Dreamcast ↔ adapter channel* (D-2) | openMenu ↔ DC-iBlueControlMod | **to be designed**; brctl and the VMU-block channel are only ideas |
| `ble-cfg` (Bluetooth LE) | web page, DC-DreamDash ↔ DC-iBlueControlMod | config, animation, profiles, motion |
| `usb-sd`, `usb-net`, `usb-serial` | DC-PicoLink ↔ Pi/PC | SD image, W5500 frames, coder's cable, STATS |
| file state (`/opt/dreampi-netswitch/*`, `/tmp/…`) | DreamDash modules | cross-process state, no sockets |

Wanted but missing: DreamDash ↔ PicoLink status (X-11); Pi ↔ iBlue port status (C-1); Dreamcast ↔ Pico2maple (pending); any wifi/Bluetooth/ethernet channel for openMenu beyond `pi-http`.

## 7. Shared data

- One **game id** key and one **per-game settings** record: port setup, network, link order (PL-R6), VMU slot. Stored by whoever owns the hardware that applies it; openMenu is the source of the id.
- **Do not touch** `OPENMENU.SYS`; extra data in `EXTENDED.SYS` (OM rule C-6).
- SD layout for saves: `/sd/OPENMENU/SAVES/<GAMEID>/SLOT<n>.VMU` (+ the OM-16 tag when that is built). PicoLink and any save manager follow it.
- Palette: DreamDash's 15 palette ids are the only colour names (DD rule); the LED/RGB firmware gets a mapping from those ids rather than its own colour list.

## 8. Layering rules already in force (keep as global)

- DC-DreamDash: never edit DreamPi's own files; the hook stays Python 2+3; a module needs no HTML; modules never import each other, they read state files or announce through the base; state lives in files.
- DC-PicoLink: diagnostics must never run before the answer; the host side for DreamPi stays an add-on; every model fix gets a test.
- DC-iBlueControlMod: fork code in `main/iblue/`, one Kconfig menu, one-line hooks in upstream files; every config (hw1, hw2, iblue) must build before `main`.
- DC-openMenu: Dreamcast side shows, the Pi collects and prepares (its own brief); hardware-dependent features are marked as not working until firmware exists; button map A select, B back, X options, Y info.

## 9. Still to decide

1. Proof words (section 1), the network/link/channel wording (section 2), the hello (section 4).
2. One game id format and who owns the per-game record.
3. The new Dreamcast ↔ adapter channel (D-2) and what it does when no controller is connected (C-4).
4. How openMenu's retry works in detail (D-5): which login first, and what it shows when the selected and the connected network differ. The Pi side (`NET` and `CALL` in the poll reply) exists.
