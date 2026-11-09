# Cinema (theater): PS3 1.10 map, PS4 1.02 remnants, C++ restoration

Sources: PS3 `NPUA80698` EBOOT 1.10 (`ps3_dis.py --annotate work/ps3-110`, `ps3_map.py`) and the PS4
1.02 image `boot-linked.bin` (`ps4_dis.py`, `ps4_cg.py`). Addresses: PS3 = ELF vaddr, PS4 = image
offset. Code: `gow3_pc/src/u2mp/cinema.cpp` (guest hooks), `cinema_store.cpp` (files, testable),
test `gow3_pc/tests/test_u2mp_cinema.cpp` (`ninja -C out/gpu u2mp-cinema-test`).

This corrects `FUNCTIONS-MATCH-GAMEPLAY.md` section 3 blocker 3: most of cinema survives on PS4.
What is gone is the file store, the recorder, the stream buffer and parts of the menu logic.

## 1. What is recorded

The replay is the **net event stream**, not snapshots of world state.

- Recorder PS3 `0x440c38(NetEvent* ev, float dt)` (absent on PS4). Active while the byte
  `0xf119ac` is set (set by open-for-write, cleared by finish). Skips when the stream holds more
  than `0x1fc00` bytes. Writes `u32 time = clock + int(dt * k)` (clock = `[0xf116d4]+12`, the net
  frame counter; `dt` is 0 at every call site), `u8 ev->type` (PS3 `ev+4`), then `ev->Write(bs)`
  (vtable slot 3) into the stream `0xf119b4`.
- Callers (every event this console sends or receives):
  - `SendNetEvent 0x493728`, `SendNetEventToPlayer 0x493a48`, `NetSession::SendNetEvent
    0x4d3754` and `0x4d3060` (sends; recorded before serialisation),
  - the message layer's receive hook `M+0x150` (PS3 C+292): `"%s joined match" 0x43dad8` stores the
    recorder's OPD `0xdef008` there, so every received event is recorded right after its `Read`,
    before relay and Apply (`0x467e88`).
- PS4: the receive hook is still called by `0x60a110` (`vxorps xmm0; mov rdi, ev; call [M+0x150]`
  at `0x60a465`, `0x60aae6`, `0x60abb6`), but the function the game installs there is the stub
  `0x5e9d80` (`ret`) - the stripped recorder. The send entries `0x644ab0` / `0x66a3d0` no longer
  call anything. `ev+8` is the type on PS4.

Stream start (written when the WaitReady state exits, PS3 `0x443f00`, PS4 **`0x5f2830` intact**):
`u32 5, u32 net game index (ngm+0x1ca0), u32 [0x113a034] (float bits), u32 [0x113a038] (clock
start)`; then records; the finish writes `u32 0`.

## 2. File format and store

PS3: `0x464334` (init, from NetInit) builds the directory `"%s/cinema%d/%s"` (HDD path, 5, user id)
at cinema+164 and the server prefix `"cinema%d/%s/"` at +292; magic `+152 = 0x04b00005`
(`(1200 << 16) | 5`). File = **1200-byte header + stream bytes**. cellFs is used directly.

| PS3 | role |
|---|---|
| `0x4631d8` | refresh: opendir, up to 20 files, stat, header read (1200 bytes, must start with the magic, else **unlinked**), RFC2822 date, sort newest first |
| `0x4635c8` | make room: refresh; at 20 files unlink the oldest with mode `0x180` (unprotected) |
| `0x463738` | load(index): +420 = index, read header + 256 stream bytes, SetNetGame(game index) |
| `0x4638cc` | open(cinema, read?): write: make room, header magic/game/overrides/time, recording flag, async create; read: async open of entries[+420] |
| `0x463ab8` | delete selected (+420 decremented), refresh |
| `0x463ba4` | protect: chmod `0x100` / unprotect `0x180` |
| `0x463ce8` | pump: completes async ops; writes 32 KB chunks when the cursor passes 32 KB; reads 64 KB chunks while <= 64 KB are buffered, drops consumed 16 KB chunks |
| `0x464148` | abort (close, unlink) |
| `0x464464` | finish: flag off; write: `u32 0`, duration into header+16, flush, header at offset 0, close |
| `0x4630a0` | control flags +156..+159 (protect / delete / unprotect / upload enable) |

PS4 cinema manager `0x113a990` (PS3 `0xf1b590`, fields from +0x94 on are PS3 + 0x10):
`+0x00` file handle, `+0x04..+0x10` FIOS op handles, `+0x94` u8 reading, `+0x95` u8 eof,
`+0x9c..+0x9f` control flags, `+0x1a4` selected, `+0x1a8` local count, `+0x1b0` local entries,
`+0x1b8`/`+0x1c0` server count / entries, **`+0x1c8` header (0x4b0)**, `+0x69c`/`+0x6a0` replay
clock values, `+0x6a4..+0x7d0` render settings. Stream `0x113a258` = `{u32* data; u32 bits @8;
u32 maxBits @0xc; u32 bytes @0x10}`. Entry `0x6c8`: `+0` date, `+0x100` name, `+0x200` stat,
`+0x208` time, `+0x210` size, `+0x214` mode, `+0x218` header.

Header (verified against PS4 `0x5f2830`, `0x5e9630` and the list query `0x657af0`):
`+0` magic, `+4` game index, `+8` f32, `+0xc` clock start, `+0x10` duration (s; shown m:ss),
`+0x18` time_t, `+0x20` 20 bytes SetNetGame overrides (ngm+0x21b4, copied back by RePlay::Enter),
`+0x34` owner name, `+0x54` "%s - %s %s" title, `+0x94` "Description", `+0x114` player count,
`+0x118` 10 players x 0x5c (name 0x40, then tracker +0x148, +0x144, +0x130, +0x138, +0x2b0 (8),
+0x1a8).

PS4: the pump **`0x608c40` survives** (FIOS: `de6f20` read, `de6f30` write, same chunk sizes) and is
called by RePlay::Enter/Update, RePlaying::Update and **Playing::Update `0x5f14d0` (every frame of
an online match)**. Everything else in the table is absent. Nothing allocates the stream buffer:
the static initializer `0x5ef970` sets `data = NULL`, so **`0x5f2830` stores to address 0 at every
match start** unless a buffer is attached (cinema.cpp attaches one in its WaitReady::Exit hook).

**Our format** (`cinema_store.h`): `<user dir>/u2mp/cinema/yyyymmdd-hhmmss.u2cinema` = the same
1200-byte header + the stream as little-endian u32 words (whole words; PS3 words were big-endian,
so files are not byte-compatible with PS3). Protection = read-only file. 20 files; at 20 the oldest
unprotected one is deleted before recording; if all are protected the match is not recorded.
Unreadable files are skipped, not deleted (PS3 deleted them).

## 3. Menus

Pages: `PAGE_MULTIPLAYER_CINEMA_LIST 735c4dbe` (control `LIST_CINEMA`, cond 69, action 22),
`PAGE_CINEMA ca9509a0` (code page; start / protect / delete / upload), `PAGE_PROGRESS 1d84d11e`,
`PAGE_PROGRESS_CANCEL 354224e7`, `PAGE_PAUSE_CINEMA`, `PAGE_RENDER_SETTINGS_CINEMA*`.

Flow PS3: BUTTON_MULTIPLAYER_CINEMA -> dispatcher `0x443248` (mode 3, refresh, SetNetGame(-1),
load(0), `0x432d0c`: refresh thread "Refresh Cinema files", next page = LIST, ShowPage(PROGRESS))
-> PAGE_PROGRESS `0x4336e4` (event 6 = per-frame: when the thread is done show the next page or
pop; message `2760b1fd` if no files; events 2..4: `0x4630a0`) -> LIST (query `0x4bb170`: get
+420, max count-1, set -> load(index)) -> LIST_CINEMA `0x435248` (local: PAGE_CINEMA; server:
popup) -> STARTCINEMA `0x432ea4` (play the "replay" task).

| control / page | PS3 | PS4 | PS4 state | C++ |
|---|---|---|---|---|
| mode dispatcher CINEMA | `0x443248` | `5ecb80` | refresh + load(0) gone | done in our PAGE_PROGRESS |
| PAGE_PROGRESS | `0x4336e4` | `5e3f60` | only events 2..4 left | replaced |
| PAGE_CINEMA | `0x43367c` | `5e4060` | pops every frame (NP-online test folded to false) | replaced (no-op) |
| list query cond 69 | `0x4bb170` | `657af0` | alive; local "set" dropped | op 9 patched -> load |
| LIST_CINEMA `6c3c0690` | `0x435248` | `5e4080` | alive | replaced (row -> load, PAGE_CINEMA) |
| `bf6960e8`/`c9f003a8` refresh | `0x432c8c` | `5e4490` | ShowPage(PROGRESS) only | replaced |
| `fb637366` local/server | `0x432c00` | `5e45d0` | toggle + PROGRESS | replaced (local only) |
| PROTECT `f35f2353` / UNPROTECT `9ddc5658` | `0x433644`/`0x43360c` | `5e41d0`/`5e41e0` | ret | replaced |
| DELETE `dd990755` | `0x433300` | `5e4270` | confirm popup, alive | kept |
| delete confirmed `7bb3c875` | `0x433588` | `5e41f0` | pop + message only | replaced |
| UPLOAD `140271ff` | `0x433240` | `5e42d0` | PROGRESS_CANCEL | replaced (no server) |
| STARTCINEMA `dba0d3e6` | `0x432ea4` | `5e45a0` | PlayTask only, no file open | replaced |
| send to friend `fe0e7de7`, `6b2970e2` | `0x43402c`, `0x433554` | `5e4510`, `5e4520` | ret | left |

## 4. Playback: TaskManagerReplay

DC task "replay" (`ad76460e`, task set `9f2aeafb`) names `TaskManagerReplay` (`85ef6592`). PS4
vtable `0x100a550` (Start `5e5420` at +0x30 calls GoRePlay); Go methods: Active `5e3980`,
RePlay `5e3ac0`, RePlaying `5e3c00`, Resetting `5e3d40`, Leave `5e3e80` (PS3 `0x433b98`,
`0x433f70`, `0x433eb4`, `0x433df8`, `0x433d3c`). States [dtor, Enter, Exit, Update]:

| state | PS4 vtable | Enter | Exit | Update | PS3 |
|---|---|---|---|---|---|
| Active | `100a750` | base | base | `5e5c70` | `d702c8` |
| RePlay | `100a7d0` | **`5e9630`** | `5e95a0` | `5e9420` | `d70308`: `0x435be8` / `0x433acc` / `0x4350ac` |
| Resetting | `100a710` | `5e5b00` | `5e5ad0` | **`5e56a0`** | `d702a8`: `0x434608` / `0x4346cc` / `0x437fb0` |
| RePlaying | `100a790` | **`5e89d0`** | `5e8740` | **`5e5c80`** | `d702e8`: `0x4348f0` / `0x436028` / `0x436220` |
| Leave | `100a6d0` | `5e5520` | `5e5510` (ret) | `5e5490` | `d70288`: `0x434848` / `0x432840` / `0x433c54` |

- RePlay::Enter `5e9630` (= `0x435be8`): NET_SM = 28 (REPLAY), ClearStats, stream reset, **loops
  `pump(cinema); usleep(1000)` until `stream.bytes != 0`** (PS3 opened the file first with
  `0x4638cc(cinema, 1)`; PS4 dropped that call, so without an open file it spins forever), reads
  tag (not 5 -> GoLeave via vtable +0x168), game index -> SetNetGame, clock values -> `+0x69c`,
  `+0x6a0`, ClearNetPlayers, rebuilds the 10 trackers from the header players, copies the header
  overrides to ngm+0x21b4, AssignTeams `640fc0`, resets the net clock to the recorded start.
- Resetting::Update `5e56a0` / RePlaying::Update `5e5c80` (= `0x437fb0` / `0x436220`): read
  `u32 time`; 0 -> end; `time > clock` -> rewind 32 bits and wait; else `u8 type`,
  `CreateNetEventByType 0x6180f0(type)`, `ev->Read(stream)`, `ev->Apply(ev, NULL)`, release.
  So recorded events are fed back through the normal event code as local events.
- RePlaying::Enter `5e89d0` (= `0x4348f0`): "multi-player-cinema", StartNetGame `647a50`,
  CameraControlCinema. Exit `5e8740`: PS3 `0x436028` also called finish `0x464464` (dropped).
- NET_SM 28 writers: `5e9630` (and the dead `5e9d30`).

## 5. What cinema.cpp installs

Checked before patching: byte prologues (`replace_function` targets), `is_ret_stub`, vtable slot
values (= image base + expected function), and the 20 bytes of the query case.

| target | how | does |
|---|---|---|
| `608c40` pump | replace | host file I/O with the PS4 chunk rules (`Writer::flush`, `Reader::fill`) |
| `5e9d80` receive-hook stub | replace (ret stub) | recorder for received events |
| `644ab0`, `66a3d0` sends | detour (17-byte PIC prologue moved to a trampoline) | recorder for sent events (covers the tail jumps `644e30`/`644e80`) |
| slot `100abd0` WaitReady::Exit `5f2830` | vtable slot | attach the 128 KB buffer, run the original, open the file (PS3 `0x4638cc(…,0)`) |
| `5f0ea0` ShowResults::Enter (ret) | replace | finish (PS3 `0x443e40` -> `0x464464`) |
| slot `100a9c8` online Leave::Enter `5f00d0` | vtable slot | finish, then original (PS3 `0x445608` -> `0x464464`) |
| slot `100a7a0` RePlaying::Exit, slot `100a6d8` replay Leave::Enter | vtable slots | close the replay file |
| menus | see section 3 | refresh / load / protect / delete / start (opens the file before PlayTask) |

Recording is on unless `UNCHARTED_U2MP_CINEMA=0`.

## 6. Unverified (static analysis only; nothing was run)

- Event 6 = per-frame update for page handlers (inferred from PAGE_MULTIPLAYER_CONNECTING and
  PAGE_PROGRESS); if it were "back", the PAGE_CINEMA no-op would trap the page.
- ShowPage pushes; our PAGE_PROGRESS pops itself before showing the list.
- Whether the list control moves its cursor through query op 9 (we also load on LIST_CINEMA).
- PS3 `0x443e40` = ShowResults::Enter (matched by role: it finishes the recording and PS4's
  ShowResults::Enter is a ret stub).
- The replay clock: timestamps use `[0x113a040]+0xc` like the PS4 readers; the start value comes
  from the stream header the game writes.
- Events whose Read/Write are still stubs record and replay empty (see blocker 1).
- Duration is wall-clock seconds between WaitReady::Exit and finish (PS3 used a game timer).
- `0x104f1b9` (local/server list flag) is forced to 1; the server list stays empty.

## 7. Theater HUD (PS3 0x8af620): traced, not ported (2026-11-09)

PS3 Draw `0x8aef68` (PS4 slot 1 of vtable `0x103f550`, now hud.cpp's `net_hud_draw`) calls, while a film
plays (ngm+7076 = PS4 ngm+0x209c), with HUD flag 0x100000 and `[[0xe1eb1c]+16] == 0`:
`0x8a8ed8(element+6372)` (camera selector update) and `0x8ac3f0(element+6372)` (draw). The PS4 sub-object
is element+0x1928 (ctor `0x55d690` = PS3 `0x8a7c64`, Reset `0x55abb0`).

What is known (PS3 -> PS4):

| PS3 | PS4 | note |
|---|---|---|
| camera table sub+12, 24-byte entries, count sub+252 | sub+0xc, 0x18-byte entries `{+0 callback id, +4 release id, +8 image {sid, table, index}, +0x14 name text}`, count sub+0xfc | 10 cameras: c1f65aeb 682c6868 a65c61ba 43348860 953b8f2a e8184ce8 (release 9eb9632d) 3cf5deed ff502160 806c9824 887c7d6e |
| shown list sub+256 (10 ptrs), count +296, fade +300, hide time +304, handle +308 | sub+0x100 (10 x 8), count +0x150, fade +0x154 (Reset 1.0), time +0x158, handle +0x15c | sub+0 selected index (Reset 3), sub+4 smoothed index (3.0), sub+8 pending release id |
| filter by cinema mode `[0xf1b590+1640]` | `[0x113a990+0x678]` | 3cf5deed always, e8184ce8 never, ff502160 mode 1, 806c9824 mode 2, 887c7d6e mode 0 |
| frame counter `[0xe60a00+236]` | `[0x1116180+0x104]` | |
| pad pressed `[0xe60a00+2068]` (pad object `0xe61180` = +1920) | `[0x1116180+0x864]` | RePlaying::Update clears it (PS3 `0x436678`, PS4 `0x5e6d95`) |
| capture gate `[0xfc3aa8]+17` | probably `[0x144e6e0]+0x13` (RePlaying::Update `0x5e6b1b`) | unverified pairing |
| camera switch `0x426dbc(menu, id, 0)` | DoCallback `0x5c8840` | the ids are handled on PS4 (`0x5e9049`, `0x5ecf8d`) |
| HUD flag 0x100000 | set by RePlaying::Update `0x5e6dca` (task+0x16c, toggled by pad 0x8000) | the gate is reachable |

Why it is not ported:

1. **Draw primitives.** `0x8ac3f0` (0xc28 bytes) draws the camera carousel, names, progress bar and
   "x%2.1f" with PS3 low-level 2D calls: quad `0x89ff3c`, material `0x12c070`, prim batch `0x8b0220` /
   `0x8b1600` / `0x8b0de8` / `0x8b12b8`, DrawText `0x73cf20`, colour `0x3cf18`, layout `0x420f58` /
   `0x421018` / `0x4156cc`. None has a PS4 counterpart (PORT-HUD.md 4.1 found the same for DrawText and the
   quad); hud.cpp draws with copied widgets through `0x5555a0` / `0x5559f0`, whose helpers are private to
   hud.cpp. A port needs those helpers exported (text at a position with a font/colour, solid and textured
   quads) - that is hud.cpp's owner's change - and the replay position / length (`0x462fbc`, cinema +456).
2. **Buttons.** The selector uses pad repeat `0xaac38(pad, 4 / 8, 0.2, 0.1)` and confirm 0x80 (0x20 when
   `[0xe61ed8+8664] == 1`). The PS4 game word is in scePad bit order (menu mapper `0x5d0be0` = PS3 `0x4227c8`:
   PS3 0x20 -> PS4 0x2000, 0x40 -> 0x8000, 0x10 -> 0x1000), PS4 has no `0xaac38` (the repeat is inlined, table
   `[0x1116180+0x88c]`), and PS4 RePlaying::Update already binds 0x8000 (the PS3 square) to the theater HUD
   toggle. PS3 bits 4 / 8 have no verified PS4 bit; a guess would break the cinema toggle.
3. Sound `0x30fb28` / `0x7069c8` (f57d53ce on camera change): PS4 pair not found.

Without it a film plays from the camera the replay picks; the camera ids are still reachable from any
PS4 control that sends them.
