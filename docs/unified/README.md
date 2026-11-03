# DC ecosystem: shared documents

These files are the same in every repository of the family (branch `unified`). The planning copy lives in the project "Unified Dreamcast Ecosystem"; if the copies differ, the newest log entry decides.

| Document | What it is |
|---|---|
| [00-log.md](00-log.md) | What was done and what is left |
| [01-feature-inventory.md](01-feature-inventory.md) | Every feature found in every branch of the five repos, and where the same idea exists in several |
| [02-unified-language.md](02-unified-language.md) | Draft shared vocabulary, naming and rules |
| [03-decisions.md](03-decisions.md) | Decisions made by the owner and what is still open |

## The repositories (branch `unified`)

| Repository | What it is | Own license |
|---|---|---|
| [DC-DreamDash](https://github.com/blaskkaffe/DC-DreamDash/tree/unified) | Dashboard add-on for DreamPi (network switching, LEDs, players, events, openMenu link) | **none yet** (to be chosen) |
| [DC-PicoLink](https://github.com/blaskkaffe/DC-PicoLink/tree/unified) | RP2040 Dreamcast serial-port adapter: coder's cable, W5500, SD card | MIT |
| [DC-iBlueControlMod](https://github.com/blaskkaffe/DC-iBlueControlMod/tree/unified) | BlueRetro fork for the internal Bluetooth controller adapter, with the Dreamcast extras | Apache-2.0 |
| [DC-BlueRetro](https://github.com/blaskkaffe/DC-BlueRetro/tree/unified) | BlueRetro for BlueRetro-style ESP32 wiring (upstream, archived by its author) | Apache-2.0 (software) |
| [DC-openMenu-Virtual-Folder-Bundle](https://github.com/blaskkaffe/DC-openMenu-Virtual-Folder-Bundle/tree/unified) | openMenu fork with virtual folders and the GD MENU Card Manager | Modified BSD (openMenu), GPL-3.0 (Card Manager) |

Pico2maple (modified firmware) will be added when its firmware is supplied.

## Licenses at a glance

Each repository's README has the full License section. In short:

- **Each repo keeps its own license.** Mixing code between repos has to respect them (for example Apache-2.0 code in DC-iBlueControlMod, MIT in DC-PicoLink, GPL-3.0 in the Card Manager of the openMenu repo).
- **DC-DreamDash has no license file yet**, so nothing in it may be reused until one is chosen.
- **BlueRetro logos** in `static/` (DC-BlueRetro, DC-iBlueControlMod) are CC BY-ND 4.0 (© 2022 Humble Bazooka): they may be shared unchanged, not modified.
- BlueRetro's own hardware designs are CERN-OHL-P-2.0 upstream; no hardware files are in these repos.
- Third-party components are listed in each README.
- The documents in `docs/unified/` are project notes: in repositories with a single license (DC-PicoLink, DC-iBlueControlMod, DC-BlueRetro) that license covers them; in DC-DreamDash (no license yet) and DC-openMenu-Virtual-Folder-Bundle (two licenses by area) no separate license has been set for them.
