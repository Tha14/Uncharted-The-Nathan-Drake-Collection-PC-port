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
