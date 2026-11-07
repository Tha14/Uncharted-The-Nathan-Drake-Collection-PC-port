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

## Status (8 November 2026)

Every multiplayer function the PS4 build left as a stub is restored in C++ (77 of 85; the other 8
are empty on PS3 too, or were the PSN connection handshake, which the session layer replaces), and
the removed layers (transport, packet layer, sessions, NetInit) are rebuilt. **None of it has run in
the game yet**; the host unit tests pass.

| Part | Where | State |
|---|---|---|
| Multiplayer in the main menu (with and without saves) | `patcher/u2mp_overlay.py` | checked in game (layout) |
| Module: build check, hooks | `gow3_pc/src/u2mp/install.cpp` | built |
| Network core: UDP, acks, reliable/unreliable, 10 peers | `net.cpp` | unit test passes |
| Game packet layer (PS3 header/acks, the game's own pack/unpack) | `gamenet.cpp` | built |
| Sessions: party, lobby roster, start match, follow, leave, kick, NET_SM | `session.cpp` | built |
| 14 net event types PS4 emptied (Read/Write, NpcSpawn Apply) | `events.cpp` | unit test passes |
| Co-op / net script natives | `natives.cpp` | built |
| Progression (money, rank, stats, leaderboards) kept on the PC | `progression*.cpp` | unit test passes |
| Cinema (recording, list, playback) | `cinema*.cpp` | unit test passes |
| NPC damage in co-op, player/party options, PSN-only buttons | `remaining.cpp` | built |
| Asset import from the PS3 copy (skins, the Fort, DLC maps) | not started | [docs/ASSET-INVENTORY.md](docs/ASSET-INVENTORY.md) |
| Voice chat | not started | the PS4 build imports no voice library |

## How it runs

- Launcher, Multiplayer tab while Uncharted 2 is chosen: "Multiplayer in the Uncharted 2 menu" (on by
  default), "Your name", "Join a host" (empty: this PC hosts on UDP port 27210). `u2mp_env` makes the
  menu overlay into `analysis/uncharted-user/u2mp/overlay` and sets `UNCHARTED_U2MP_OVERLAY`,
  `UNCHARTED_U2MP=1`, `UNCHARTED_U2MP_NAME`, `UNCHARTED_U2MP_JOIN`.
- `tools/probe_uncharted.py` lays the overlay over /app0 for Uncharted 2; the runtime calls
  `u2mp_install` after linking the image, before protecting it.
- In game: Multiplayer starts the party (host, or join the address). The party leader picks
  Custom Game or Matchmaking (a random game of the chosen type), then Start Match: the members
  follow the leader into the match. Results return everyone to the lobby.
- Files on the PC: `<user>/u2mp/progression.bin`, `<user>/u2mp/cinema/*.u2cinema`.
- Switches: `UNCHARTED_U2MP_PORT`, `UNCHARTED_U2MP_CINEMA=0`, `UNCHARTED_U2MP_PROGRESSION=<file>`.
- The release package ships `u2mp/patcher` and this README (`tools/package_windows.sh`); the C++ is in
  the runtime executable.

## Docs

| File | What |
|---|---|
| `docs/PS3-110-MAP.md` | The PS3 1.10 multiplayer code |
| `docs/FUNCTIONS-FRONTEND-NET.md`, `docs/FUNCTIONS-MATCH-GAMEPLAY.md` (+ .tsv) | Every function, PS3 1.10 against PS4 1.02 |
| `docs/TRANSPORT-INTERFACE.md` | Where the transport plugs in (message layers, events, BitStream) |
| `docs/SESSION-INTERFACE.md` | Sessions, NET_SM, pages, match start |
| `docs/CINEMA.md` | Cinema |
| `docs/ASSET-INVENTORY.md` | Multiplayer assets on PS4 and PS3 |

## Layout

| Folder | What |
|---|---|
| `patcher/` | What the launcher runs on the player's PC (`u2mp_overlay.py`). |
| `tools/` | Research scripts (PS3/PS4 disassembly, function matching, inventories). |
| `docs/` | Findings. |
| `../gow3_pc/src/u2mp/` | The C++ module; tests in `../gow3_pc/tests/test_u2mp_*.cpp` (`ninja -C out/gpu u2mp-net-test u2mp-events-test u2mp-progression-test u2mp-cinema-test`). |

Code that runs inside the game is **C++**, built into the runtime (`gow3_pc`); the C runtime calls it
through `extern "C"` entry points. Python only for the launcher-side steps before the game starts.

## Next

1. First in-game tests: the frame hook rate (log "frame hook: N calls in 10 s"), the Multiplayer
   button, two PCs (or two instances) in one party, a custom match start.
2. Asset import from the player's PS3 copy (skins, the Fort's pieces, DLC maps).
3. Voice chat (a PC implementation), LAN game browser, host migration.
