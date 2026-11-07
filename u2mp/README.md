# u2mp — Uncharted 2 multiplayer in the PC port

Bringing back Uncharted 2: Among Thieves multiplayer, which Bluepoint stripped from the PS4
Collection. Same approach as [u3mp](../u3mp/README.md): replicate the PS3 game, porting what the PS4
build left as stubs or dead code, with the PS3 executable as the map.

Started from scratch on 7 November 2026: earlier third-party Uncharted 2 multiplayer and co-op code
was removed from this repository (tag `backup/before-sparks-removal` keeps the state before).

> **No game files are included.** Everything here is source, scripts and notes. Files derived from
> the game are made on the player's PC from their own copies and stay out of git.

**Targets:** PS4 Collection **CUSA02320**, Uncharted 2 executable **1.02** (`big2-ps4_Shipping.elf`,
SHA-256 `5047b693…`). PS3 reference: **NPUA80698** (PSN, 1.10, EBOOT SHA-256 `fdac6628…`); the DLC map
packs come from a BCUS98123 install (see docs/ASSET-INVENTORY.md).

## Status

| Step | State |
|---|---|
| Multiplayer button in Uncharted 2's main menu | `patcher/u2mp_overlay.py` makes the menu from the player's game; the launcher lays it over Uncharted 2 (Multiplayer tab while Uncharted 2 is chosen, "Multiplayer in the Uncharted 2 menu", on by default). **Not yet checked in game.** |
| Asset inventory, PS4 vs PS3 | [docs/ASSET-INVENTORY.md](docs/ASSET-INVENTORY.md), every file in `docs/import-list.tsv` |
| PS3 1.10 function map | in progress |

## How the launcher uses it

- `launcher/uncharted_launcher.pyw` `u2mp_env`: builds the overlay into `analysis/uncharted-user/u2mp/overlay`
  (only when the game's menu changed) and sets `UNCHARTED_U2MP_OVERLAY`.
- `tools/probe_uncharted.py`: lays `UNCHARTED_U2MP_OVERLAY` over /app0 when Uncharted 2 starts.
- The release package ships `u2mp/patcher` and this README (`tools/package_windows.sh`).

## Layout

| Folder | What |
|---|---|
| `patcher/` | What the launcher runs on the player's PC (`u2mp_overlay.py`). |
| `tools/` | Research scripts: inventories and comparisons of the PS3 and PS4 files. |
| `docs/` | Findings. |

Code that runs inside the game (hooks, lobby, session, match logic) is written in **C++** and built into
the runtime (`gow3_pc`); the C runtime calls it through `extern "C"` entry points. Python is used only
for the launcher-side steps that run once before the game starts.

## Next

1. Check the Multiplayer button in game.
2. PS3 1.10 map of the multiplayer code (button callback, NetInit, session, lobby, match flow).
3. The Multiplayer button's callback, from the PS3 original.
4. Lobby and match flow, then the network transport.
5. The import of the missing assets from the player's PS3 copy (skins, the Fort's pieces, DLC maps).
