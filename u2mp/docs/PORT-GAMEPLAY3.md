# Port job "gameplay3": end of match, results, remaining match-path checks

Static analysis only (nothing built or run). PS3 = 1.10 EBOOT (NPUA80698), PS4 = Collection 1.02 image.
Code: `gow3_pc/src/u2mp/gameplay3.cpp`, `install_gameplay3(int &installed)` (not wired yet; order does
not matter relative to `install_cinema`, see 1.3).

## 1. ShowResults (end of match -> results screen)

### 1.1 State pairing

TaskManagerOnline state classes have the vtable layout [dtor, Enter, Exit, Update] on both builds and
appear in the same order, so they pair slot by slot:

| State | PS3 vtable: Enter / Exit / Update | PS4 vtable: Enter / Exit / Update |
|---|---|---|
| Leave | `d70438`: 445608 / 4385ec / 441b88 | `100a9c0`: 5f00d0 / 5f0050 / 5eff30 |
| ShowRoundResults | `d70458`: 4430d8 / 438488 / 4431a0 | `100aa00`: 5f0af0 / 5f0ae0 / 5f0a90 |
| **ShowResults** | `d70478`: **43ec08** / 438478 / 4421b8 | `100aa40`: **5f0ea0 (ret)** / 5f0e90 / 5f0b30 |
| ResettingReady | `d70498`: 43bc80 / base / 440130 | `100aa80`: 5f0fc0 / base / 5f0ec0 |
| Active | `d704b8` | `100aac0` |
| Resetting | `d704d8`: 443e40 / 440f48 / 440d50 | `100ab00`: 5f1240 / 5f1210 / 5f1000 |
| PreResetting | `d704f8`: 43edc8 / base / 4413a0 | `100ab40`: 5f13f0 / base / 5f1390 |
| Playing | `d70518`: 444660 / 43de50 / 442558 | `100ab80`: 5f23e0 / 5f2350 / 5f14d0 |
| WaitReady | `d70538`: 43bd28 / 443f00 / 4409c4 | `100abc0`: 5f2d90 / 5f2830 / 5f26d0 |
| LoadLevels | `d70558`: 4405c4 / 438448 / 440478 | `100ac00`: 5f2e50 / - / 5f2dc0 |
| EstablishConnection | `d70578`: 4416e0 / base / 440f90 | `100ac40`: 5f3050 / - / 5f2ea0 |

Go* virtual offsets: PS4 offset = 2 x PS3 offset (PS3 204 GoLeave = PS4 0x198, 184 = 0x170, 192 = 0x180).
The frontend TSV marks most of these PS3 functions `absent`; by the vtables they are present.
Correction to CINEMA.md: PS3 `0x443e40` is Resetting::Enter, not ShowResults::Enter. ShowResults::Enter
is `0x43ec08`, the "NET_SM -> RESULTS / Save Game Stats" row of the frontend map.

### 1.2 What PS3 ShowResults::Enter 0x43ec08 does, and what is ported

| Step | PS3 | PS4 / port |
|---|---|---|
| close an open in-game menu | `0x42a4bc(0xf0e800)` -> `0x4298b8(menu, 5)` | **ported**: the PS4 copy is inlined in Resetting::Enter `0x5f1264` and Leave::Enter `0x5f0130` (menu `0x1136640`, state `+0x1014`, event init `0xccf830`, `0x5cd5c0(menu, ev, 3/0)`) |
| "Save Game Stats" thread | `0x43bc30` -> `0x4e1590` ("Sending Stats to %s SQS") | not ported: Naughty Dog's stats server (Amazon SQS); no PC counterpart |
| NetPlayerData::Publish | `0x4c6298` (TUS) | not ported here: absent on PS4; progression.cpp publishes to the local store at Leave |
| matchmaking skill into room data | `0x4af240` -> `0x4d2ed4` (sceNpMatching2SetRoomMemberDataInternal) | not ported: no NP room on PC |
| results end time | `[0xf12504] = frames/300 + 10.0` | **ported** (see 1.3) |
| NET_SM = 29 (NET_SM_RESULTS) | `[0xf11764] = 29` | **ported**: `[0x14ce97c] = 29` (no PS4 writer of 29 existed) |
| voice volume back to 1.0 per remote tracker | `cellVoiceSetVolume` via `0x4e8848`/`0x4e6e64` | not ported: voice.cpp owns PC voice |

### 1.3 The folded results timer (PS3 0xf12504)

PS3 ShowResults::Update `0x4421b8`: `time_left = [0xf12504] - frames/300` (`fnmsubs`), then
time_left < 1.0 -> owner+228 = 1.0 (fade); host: time_left < 0 -> GoLeave; client: waits for
message 0x105 (and checks the trackers).

PS4 ShowResults::Update `0x5f0b30` is the same code, but `0xf12504` has no PS4 writer (its only PS3
writer is the stripped Enter), so the compiler folded it to 0.0: `0x5f0b5a: vcvtsi2ss [clock+0xc];
0x5f0b5f: vmulss -1/300 (0xe62288)` -> time_left = -frames/300 < 0 on the first frame. The host
left the results screen at once.

Fix: `0x5f0b50..0x5f0b67` (0x17 bytes: clock load, convert, scale; byte-checked) jumps to a stub
`xmm1 = frames * (-1/300) + end; jmp 0x5f0b67`. `end` is written by our Enter. Without our Enter
(end = 0.0) the result is bit-identical to the original.

The Enter is installed in the vtable slot `0x100aa48` (checked: it holds `0x5f0ea0`). It calls
`0x5f0ea0` at the end, so cinema.cpp's recording finish (which replaces the body of `0x5f0ea0`)
still runs, whichever install runs first.

## 2. Folded / never-written globals (scan)

Method (scratch script `g3/folded.py`): with the PS3 analysis of `ps3_elf.py` (TOC-resolved memory
accesses), every data address in 0xe00000-0x1100000 is split into writer and reader functions
(store opcodes stw/stb/sth/stfs/stfd and update forms). Reported: addresses whose writers are all
absent/stub on PS4 (from both TSVs; the TaskManagerOnline state functions of 1.1 overridden to
present, `0x43ec08` to stub) and that have at least one reader present on PS4.

| PS3 global | writers (all stripped) | live readers | status |
|---|---|---|---|
| `0xf12504` results end time | ShowResults::Enter `43ec08` | ShowResults::Update `4421b8` = `5f0b30` | **fixed here** |
| `0xf116d4` net clock | NetInit `438bc8` | many | fixed before (session.cpp creates `0x113a040`) |
| `0xf119c4` match abort | `438424`, `43d610`, `43d660` | Resetting/Playing/WaitReady updates (`0x113a270`) | session.cpp sets `0x113a270` on abort |
| `0xe15650/54`, `0xf10e14`, `0xf11a64` | NetInit / matchmaking | PAGE_MULTIPLAYER_CONNECTING `5ed6a0` | netinit.cpp / session.cpp (NETINIT.md) |
| `0xe1d734`, `0xf1fa90/94`, `0xf1fd18`, `0xf1db98`, `0xf2c8a0` | NetInit / matchmaking / friends | stubbed or replaced menu callbacks (`5eb780`, `5ecb80`, `5ed8c0`, `64e910`, `5ebde0`, `5ed410`) | menu side, outside the match path |

No other address on the match path qualifies.

Vtable pass (scratch `g3/vtpair.py`, follow-up of PORT-NULLSITES "What is left"): every PS4 vtable
with mostly MP slots was aligned to its PS3 vtable by voting over the TSV pairs, and each `ret` /
`ret 0` slot was compared with the PS3 slot body. Results: the net event vtables (all known; slot 0
destructors only reset the vptr on PS3), NetAnimCmdGenerator `0x103fab0` slot 4 (section 3), and
`0x100b570` (one vote only; its stub slots are the identical-code-folded `ret` bodies shared by many
classes, not a real pairing). `0x100af70` (102 slots, 34 votes): 14 stub slots, all
also empty on PS3. `0x100b430`, `0x100b510`, `0x100bc30`, `0x103fa50`, `0x103fb10`, `0x1041a00`,
`0x1041a50`: no PS3 pairing (menu/session classes; `0x103fa50` is the match session, covered by
SESSION-INTERFACE.md).

## 3. NetAnimList::AddAnim 0x45212c / generator slot 0x453c10 (open item of PORT-GAMEPLAY2)

- PS3 NetAnimCmdGenerator vtable `0xd705a8`: 0 `452d80`, 1 `4520f0`, 2 `451e38` (return 7),
  3 `45319c` CreateAnimCmds, 4 `452fd4`, 5 **`453c10`**, 6 `452e24`, 7 `452a6c`.
  PS4 `0x103fab0`: 0 `5f53c0`, 1 `5f53a0`, 2 `5f3ca0`, 3 `5f3f70`, 4 **`5f48d0` (ret)**, 5 `5f38e0`,
  6 `5f3bd0`, 7 `5f5390`, 8 `5f3c90` (return 7). `5f48d0` is referenced only by the vtable.
- The generator layout is the same on PS4 with 16 more bytes per snapshot: PS3 snapshots +64 stride
  128 (14), time +2124; PS4 +0x70 stride 0x90, count +0x68, time +0x960. The AddAnim list (PS3 +1856,
  count +2112) therefore sits at PS4 ~+0x850..+0x950. The PS4 constructor (NetCharacter ctor
  `0x60671d`, generator at NetCharacter+0x310) no longer initialises it: nothing reads it on PS4.
- **The per-frame remote-animation path is intact**: PS3 NetCharacter update `0x45ed60` calls the
  generator directly with `bl 0x4533f8` (0x600 bytes, snapshot pair by time +2124, then anim
  commands). PS4 `0x605070` calls `0x5f48e0` at `0x6050ac` (same code: `+0x68 >= 3`, time `+0x960`,
  stride 0x90; it also holds the inlined `0x4539f8`). So remote players animate through `5f48e0`.
- `0x453c10` is the virtual alternative that keeps a NetAnimList (16 entries, fade values) and
  restarts anims through `0x7ab88`. No direct PS3 caller; it is reached only through the vtable.
  Porting it would need the PS4 equivalent of `0x7ab88` and the anim-controller layout, and its only
  effect would be on that virtual path. Not ported; revisit only if a two-player run shows remote
  characters with wrong or frozen animations that `5f48e0` does not explain.

## 4. Unresolved rows on the match path, checked

The TSV's "unresolved" are mostly log-only PS3 functions. The large ones were checked through their
PS3 callers' PS4 counterparts (the PS4 caller must call or contain an equivalent):

| PS3 | role | PS4 evidence |
|---|---|---|
| `0x4976e0` (0x568) | co-op arena wave complete | Apply `0x476338` = `0x61afe0` calls `0x647650`: same 10-slot unrolled tally update at `0x1147bd0` (PS3 `0xf2efc4`); TSV updated |
| `0x495ad0` (0x1d0) | native helper | native `0x4ccd48` = `0x669800` calls `0x64c330` (0x4b0) |
| `0x4963d4` (0x504) | native helper | native `0x4cd530` = `0x669500` (0x1a0) keeps the virtual calls and `0x99af90` |
| `0x49a12c` CanPickUp, `0x49a608` AbortPickUp | pickup arbitration | `0x630c00` calls `0x645790`; `0x630f20` (0x4c0, PS3 0x258) contains the rest |
| `0x498850`, `0x497f90` (m_pDeadList) | npc trackers | `0x644fa0` calls `0x64d910`; `0x6453d0` calls `0x64dae0` |
| `0x4ca4a8` | tracker helper | `0x632d50` calls `0x663b70` / `0x645c50` |
| `0x49d448`, `0x4a49e8`, `0x49f114`, `0x494930` | objective / game-over / round-over / client update | inlined in NetGameManager::Update `0x63e6a0` (FUNCTIONS-MATCH-GAMEPLAY.md 5) |
| `0x4886b0`, `0x4887b4` | TerritorySetTimer / TerritorySpawn senders | vptrs `0x1040350` / `0x10403a0` are stored at `0x6156cb` / `0x615601`, `0x64c2b5` (senders present) |
| `0x4e1590` | "Sending Stats to %s SQS" | server upload; not needed (1.2) |
| `0x4dcf88`, `0x4dfc08`, `0x4dce9c`, `0x4e1b24` | stat tallies (add, stop, capture, restore) | probably the copy loops over `0x1147bd0` (+0x405c <- +0x42cc stride 0x34, memcpy 0xa28) in Playing::Update `0x5f1d00..0x5f2110` |

Playing::Update `0x5f14d0` (PS3 `0x442558`), Playing::Exit `0x5f2350` (PS3 `0x43de50`), Resetting
`0x5f1240`/`0x5f1210`/`0x5f1000`, ShowRoundResults `0x5f0af0`/`0x5f0a90` and Leave::Enter `0x5f00d0`
were compared with PS3: same transitions (GoShowResults +0x188 on ngm+0x1e84 / +0x1e88 end states,
GoShowRoundResults +0x190, GoPreResetting +0x170, GoLeave +0x198), same globals except the ones in 2.

## 5. Not ported / known gaps

- Next match: after Leave the host must be back in CREATE_GAME_WAIT (3) with the host page `6093f0`
  and clients in 17 (SESSION-INTERFACE.md 3.4 / step 7). That is session.cpp's job; nothing in the
  PS4 state machine is missing for it.
- Server-side stats (SQS), TUS publish, NP room skill data, PS3 voice volume reset: no PC counterpart.
- NetAnimCmdGenerator slot `0x453c10` (section 3).
- Untested: the countdown restores the PS3 10 s results screen; the host then leaves through
  GoLeave and clients on message 0x105, both unchanged PS4 code.

## 6. In-match audit (after test run 41)

Run 41 (boot.log): three solo deathmatches (Gompa, Fort, Treasure, game type 0) reached Playing and
ran 51 s / 118 s / 39 s. Each time `leave flag 1` (the pause menu's Leave Game), so the limits were
never reached. ShowResults::Enter and the countdown were installed (cinema's finish chained after it).

### 6.1 Match end (new: `matchend.cpp`, `install_matchend`)

The game-over code is in NetGameManager::Update `0x63e6a0` from `0x63f31d` (field map in the
`matchend.cpp` header): it needs at least 1500 frames after ngm+0x1d60, then time limit (ngm+0x21b8
seconds x 300 against frames - ngm+0x1d5c, skipped for type 10 or when debug byte `0x104f4f8` is
set; it is 0 in the image), score limit (ngm+0x21b4 against the u16 team scores at
`[0x104ae70] + 0x3dee`), elimination rounds (ngm+0x21c0 / +0x1d74). The reason goes to the EndGame
sender `0x60c7f0`; its Apply `0x62c9f0` sets ngm+0x1e84/+0x1e88, which Playing::Update reads. All of
it is PS4 code that was never stripped; it was simply not reached.

`matchend.cpp` wraps the Playing::Update slot (after session.cpp's wrapper) and logs those values
every 10 s and whenever ngm+0x1e80/0x1e84/0x1e88 change. `UNCHARTED_U2MP_TIME_LIMIT=<10..3600>`
sets the time limit for test runs, so a solo match reaches the time-limit game over, then
ShowResults (10 s countdown) and Leave.

### 6.2 Scans

| Scan | Result |
|---|---|
| Every script native, the whole image (`u2_func_match.py natives`: 852 PS3 natives, all present on PS4) | 33 stubs: the 6 net natives natives.cpp ports, plus print/display/jetski/E3 debug natives (empty on purpose). HUD natives (`hud-set-timer`, `hud-set-counter`, `display-button-hud`, `net-hud-set-grapple-indicator`), booster natives (`net-get-booster-amount` 0x668820, `net-booster-enabled?` 0x668810), spawn/flag/territory natives: alive |
| PS4 ret stubs paired to a PS3 body across all 5565 matched pairs (`g3/stubpairs.txt`) | only destructors (PS4 dtors are `ret`), menu callbacks and natives already handled |
| Non-zero image-data globals read by MP code that no code ever sets back (`g3/initval.py` over full.dis) | `0x104f1c0` (fixed in session.cpp), `0x104f1c1` (PAGE_MULTIPLAYER_CONNECTING only; when 1 the page skips its own "net" request, which the Multiplayer button already makes), `0x104f1b9` (cinema), `0x1050638` (TSC frequency), `0x105e370` (menu). netInfo `0x104f1c8` keeps the PS3 constructor values (`+0x3c = 6`, `+0x58 = 12` = PS3 `0x4a9d64` +60/+80) |
| Allocator contexts used by MP code (calls to `0xb332a0` / GetAllocator `0xb2f280` with a constant context) | 0x14 (173 calls: netheap.cpp), 0x45 (alloc at `0x668e09` only; GetAllocator at `0x66d413`/`0x66d510` fixed in nullsites.cpp). No other context |
| PS3 IsMultiplayer callers (`0x4384b0`, 310) whose PS4 pair lost every NET_SM test | PostProcessUpdate (PS3 `0x21db8` = PS4 `0x3f5620`) and `0x19b710` = `0x293d90` only |

### 6.3 Found: NetGameManager::KickVisibilityJob (PS3 0x494c38) is not called on PS4 (ported: section 7)

PS3 PostProcessUpdate `0x21db8`, when multiplayer, calls NdiNetUpdate `0x449e88` (transport, voice,
party: replaced by session.cpp/voice.cpp) and **KickVisibilityJob `0x494c38`(g_netGameManager)**.
The job kicks, for every remote player with a character, an asynchronous line-of-sight probe from the
camera to the player into the tracker's probe object (PS3 tracker+408; collision kick `0x6c2d58`).
UpdatePlayerTrackerVisibility (PS3 `0x494608`, inlined in PS4 `0x63e6a0` at `0x63e78d`, sid `c032f430`)
reads the probe through `0x666160` (PS3 `0x4c9748`), keeps the per-tracker visibility state
(+0x230, PS3 +488) and sends the spotted event `0x611fa0` (PS3 `0x48cc58`). PS4 PostProcessUpdate has
no multiplayer branch, and no PS4 code writes the probe's frame (tracker+0x1c8) or handle (+0x1d4)
apart from the reset at `0x6626ec`. So the probe never runs on PS4.

Effect: only with remote players (enemy name tags / spotting). Solo runs cannot show it. Not ported
yet: it needs the PS4 equivalents of the PS3 collision-probe kick `0x6c2d58`, its setup `0x6c1a4c`,
and the camera/position helpers `0x6b2ba4` (= PS4 `0x9e8c50`), `0x4c7df8`, `0x8a2388`, `0x39d120` and
`0x93820`. `0x666160` reads the probe through a job table (`[0x14a6678]`) with a different layout
than PS3. Next step: find the PS4 kick that writes probe +0x18 (frame) and +0x24 (handle) in another
probe user. Then port `0x494c38` as a C++ call once per frame from the Playing::Update wrapper.

### 6.4 Checked, nothing missing on PS4

- Boosters: `net-booster-enabled?` = `0x5f6080`: compares the two equipped booster ids of NetPlayerData
  (`0x11474f8 + 0x4c/+0x50`) after the game-type gate. `net-get-booster-amount` = `0x5f6410` reads the
  booster list `0x113a788` (netinit.cpp/unlocks.cpp: 32 records) and the co-op table `b4bcce62`.
  Both are intact. Boosters apply once the loadout's ids are in `+0x4c/+0x50` (menus / progression).
- Respawn and spawn: `0x642d20` (PS3 `0x4a6008`), GetSpawnLocator `0x662fa0` (spawn.cpp), NetCharacter
  death and ragdoll (`player-death.cpp`, `net-character-death.cpp`): intact. No stub on that path.
- Modes: every territory, hidden-treasure and NPC event serializer is ported (events.cpp). The
  senders are present (TerritorySetTimer vptr store `0x6156cb`, TerritorySpawn `0x615601` /
  `0x64c2b5`) and the co-op natives are ported (natives.cpp). Co-op arena wave complete = `0x647650`.
- XP / cash: the match tallies (`0x1147bd0`, NetPlayerData `0x11474f8`) are still updated in Playing and Leave::Enter
  `0x5f00d0`. progression.cpp's Leave publish (call at `0x5f0952`) runs after them. matchend.cpp logs
  the cash every 10 s so the next run shows whether it moves.
- HUD: the multiplayer HUD is driven by the DC scripts through the natives above (all alive) and by
  the event Applies (kill info, medals, money) that events.cpp / remaining.cpp complete. The scans
  above found no stripped HUD update or draw function. Medal and XP pop-ups need kills, so a solo
  run will not show them.
- Taunts: an animation state (`kStateTaunt`) plus the unlock flags (unlocks.cpp). No net function.

## 7. KickVisibilityJob port (`visibility.cpp`, `install_visibility`)

The probe at tracker+0x1b0 is a sphere collision probe. The tracker constructor `0x662660` builds it
with vptr `0x1004180` (`0x662797`). Its vtable keeps the PS3 slot numbers:

| slot | PS4 | PS3 use in 0x494c38 / 0x6c2d58 |
|---|---|---|
| 1 Init(count, hits, flags, type, buffer, size) | `0x4f1dc0` | `0x39d120(probe, 1, 32, 0, 13, 0, 0)` |
| 7 SetRayDir(idx, start, dir, length) | `0x4f25b0` | slot 7, length 1.0 (`0xdc0f04`) |
| 32 SetRadius(idx, r) | `0x4f2600` | slot 32, 0.01 (`0xdb1e14`) |
| 9 SetFilter(idx, a, b, &obj) | `0x4f26c0` | slot 9, {26, 0}, default object `[0xf70c58]` = `[0x151f048]` |
| 11 | `ret` on PS4 | slot 11 with 0.0 |
| 2 Kick(site a, b, c, count, 0) | `0x4f2130` (writes +0x18 frame, +0x24 handle) | slot 2 with `0xf71140..48` = `0x151f058/60/68` |
| 12 IsActive / 5 Release | `0x4f2850` / `0x4f2490` | (Init releases the previous job itself) |

PS4 `0x97ec30` (= PS3 `0x6c3990`) has the same Init/SetRay/SetFilter/Kick sequence inlined for a
stack probe, which confirms the order and the arguments. The other helpers come from the intact reader
`0x666160`: player position `0x665f70(out, tracker)` (PS3 `0x4c7df8`), view test
`0x5566b0(out, 0.0f, &pos)` with the reject flag at out+0x2c (PS3 `0x8a2388`, flag at +44), and the camera
position `0x104e820` (PS3 `0xee0170`). PS3 `0x6b2ba4` (local player check) is replaced by the local tracker
ngm+0, which `0x63e782` checks before the read.

Per frame (`visibility_kick()`, called by matchend.cpp right after Playing::Update, the place where PS3
PostProcessUpdate ran it), only when NET_SM >= 2, the collision world exists and ngm+0 is set: for each
used, not excluded, non-local tracker whose probe has the sphere-probe vptr, and whose view test passes,
the port runs Init, SetRayDir(camera, player - camera, 1.0), SetRadius 0.01, SetFilter {26, 0} and Kick.
The next frame's NetGameManager::Update reads the probe through `0x666160` (frame check: kick frame =
current - 1). With no remote player nothing runs. A tracker that is freed or excluded gets its probe
released. Playing::Exit (slot `0x100ab90` -> `0x5f2350`, wrapped) releases every kicked probe, and no
probe is kicked again in the frame of that Exit, so the tracker reset in LoadLevels (`0x662660`
zeroes the probe without releasing it) never drops a running job. Install checks: the seven vtable
slots, the prologues of `0x665f70` / `0x5566b0`, the reader's probe offsets at `0x66618c`, the
constructor's vptr store at `0x662797` and the read call at `0x63e813`.

Unverified: the filter values {26, 0} are the PS3 collision layers. If PS4 renumbered the layers, the
probe hits too much (remote players never visible) or too little (always visible). It needs a
two-player run: look at the enemy name tags in line of sight and behind walls.

### 7.1 0x293d90 (the second IsMultiplayer mismatch): nothing to port

The pairing PS3 `0x19b710` -> PS4 `0x293d90` rests on one string id (`16a66ef3`). `0x293d90` is only the
type switch of the PS3 function (the type ids into +0x40d0). The multiplayer part of `0x19b710` (an
NPC set-up that builds a NetAnimCmdGenerator with `0x19a130` when IsMultiplayer) is in PS4
`0x2168d0`. It has its own NET_SM test at `0x216d24` and stores the generator vptr at `0x216e3c`. It is
intact.
