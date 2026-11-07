# Uncharted 2 multiplayer: match, gameplay and progression functions (PS3 1.10 vs PS4 1.02)

Scope: the code that runs a multiplayer match once the session exists. NetGameManager
(rules, start, rounds, game over, teams, spawning), players and net events, objective modes,
weapons/loadouts/boosters, progression (money, rank, medals, unlocks, stats, leaderboards,
trophies, cinema) and the net script natives. Menus, session/room, transport and NetInit are
mapped separately in `functions-frontend-net.tsv` and are only referenced here.

Inputs: PS3 `NPUA80698/USRDIR/EBOOT.elf` (1.10, SHA-256 `fdac6628...fd1a`, see
`PS3-110-MAP.md`) and the PS4 Collection 1.02 executable (flat image `boot-linked.bin`, image
address = file offset - 0x183ce8; relocations come from the original `eboot.elf`).
Static evidence only. Nothing was run, no game file was changed, no game bytes are in the
repo. Server host names and keys that appear in the strings are not reproduced.

Machine-readable map: `functions-match-gameplay.tsv` (one row per PS3 function in scope,
1300 rows). Columns: area, role, PS3 address, PS4 address, PS4 state, evidence, porting
notes, source file (or range label), PS3 size, PS3 callers, PS3 callees, PS3 globals (absolute
data addresses the function touches). The tables below are the hand-checked subset.

## 1. How it was built

| Script (`u2mp/tools/`) | What it does |
|---|---|
| `ps4_cg.py` | PS4 call graph: disassembles the whole PS4 text once (llvm-objdump), indexes call/jmp/RIP-relative refs and immediates, and reads the R_X86_64_RELATIVE relocations of the PS4 ELF (the flat image has them unapplied) so vtable and table slots count as references. Function starts: call/reloc/lea targets plus `push rbp; mov rbp,rsp` prologues. Stub detection (`ret`, `xor eax,eax; ret`, `mov eax,imm; ret`). Cache `work/ps4-102/`. |
| `u2_func_match.py` | PS3 -> PS4 pairing. Passes: script natives (registration name), unique strings, unique 32-bit ids, string sets, menu callback ids, vtable alignment, callee alignment, unique caller, address order, an idf-weighted similarity over strings/ids/float constants/matched callees, and an "inlined" pass (PS3 function whose features sit inside the PS4 counterpart of a caller or callee). Liveness: a PS4 function is dead when nothing but dead functions reference it (fixpoint). Cache `work/match/`. |
| `u2_netevents.py` | Reads the two net event factories (`CreateNetEventByType`, PS3 0x47e550 / PS4 0x6180f0, both a switch over 80 types) and the PS3 debug name table (`kNpNetEvent*`, 0xd70c20) and pairs every event vtable slot. |
| `u2_match_report.py` | Builds the TSV and the tables below; holds the hand-checked roles and the manual pairings with their evidence. |
| `ps4_imports.py` | (frontend map) PS4 import names; used here for sceNpScore/sceNpTus/sceNpTrophy. |

```
set E="D:/2. PS Games/PS3/NPUA80698/USRDIR/EBOOT.elf"
set B="D:/1. PC Games/Uncharted-windows/analysis/big2-ps4_Shipping"
python u2mp/tools/ps4_cg.py %B%/boot-linked.bin %B%/eboot.elf build
python u2mp/tools/u2_func_match.py %E% %B%/boot-linked.bin %B%/eboot.elf build
python u2mp/tools/u2_netevents.py %E% %B%/boot-linked.bin %B%/eboot.elf
python u2mp/tools/u2_match_report.py %E% %B%/boot-linked.bin %B%/eboot.elf all u2mp/docs/functions-match-gameplay.tsv work/match/md
```

PS4 state used in the tables:

| State | Meaning |
|---|---|
| intact-alive | PS4 counterpart has code and is referenced by live code or a data slot |
| intact-dead | PS4 counterpart has code but nothing alive references it |
| stub | PS4 counterpart is `ret` / `ret 0` / `ret const` (16 of the 50 have a PS3 body of 0x40 bytes or less; the evidence says so) |
| partial | PS4 counterpart exists but lost a part: a net event whose Write/Read are stubs, or a body under 25 % of the PS3 size |
| inlined | no own PS4 function; the code is inside the PS4 function in the PS4 column |
| absent | no PS4 counterpart: its source file or identifying (assert/API) strings are gone from the PS4 image, or the PS4 executable does not import the API it needs |
| unresolved | not paired statically; the PS4 image still has the source file/strings around it. Most are small helpers (330 of them are <= 0x60 bytes and are probably inlined) or functions whose only strings were log texts, which the PS4 build compiles out |

## 2. Counts

| Area | Functions | intact-alive | intact-dead | stub | partial | inlined | absent | unresolved |
|---|---|---|---|---|---|---|---|---|
| match | 183 | 30 | 9 | 0 | 0 | 42 | 2 | 100 |
| players | 662 | 227 | 7 | 22 | 5 | 31 | 2 | 368 |
| modes | 129 | 72 | 1 | 13 | 5 | 0 | 0 | 38 |
| weapons | 91 | 75 | 0 | 2 | 1 | 1 | 0 | 12 |
| progression | 187 | 24 | 0 | 7 | 1 | 18 | 27 | 110 |
| natives | 48 | 37 | 0 | 6 | 1 | 0 | 0 | 4 |
| **all** | 1300 | 465 | 17 | 50 | 13 | 92 | 31 | 632 |

The PS4 build keeps nearly all of the match code: its source files (`net-game-manager.cpp`,
`net-event.cpp`, `net-player-tracker.cpp`, `net-snapshot.cpp`, `net-territory.cpp`,
`net-player-data.cpp`, `net-character*.cpp`, `playerstatmanager.cpp`, `unlockablemanager.cpp`,
`moneymanager.cpp`, `weapon-pickup.cpp`, `player-net-fight.cpp`) still have their asserts, the
event factory has all 80 types, and the net script natives are all registered. What is gone is
(a) every log text, so log-only functions cannot be paired by string, (b) `cinema.cpp` and the
flow/session/transport files, (c) the PS3 NP score/TUS/trophy code, and (d) a set of stubbed
net event serializers and script natives listed below. The PS4 compiler also merged many small
PS3 functions into their callers (NetGameManager::Update holds ClientUpdate, HostUpdate and the
game/round-over checks).

## 3. Top porting blockers

1. **Net event serializers stubbed on PS4.** Write (slot 2) and Read (slot 3) are `ret` for
   NetEventTerritoryOwn, TerritorySpawn, TerritorySetTimer (Turf War / King of the Hill),
   SpawnHiddenTreasure, EarnMedal, PlayVox, SimpleSnapshot, TurretSnapshot, StartNpcFight,
   NpcKill, NpcSpawn (Apply is a stub too), NpcDealDamage (Write) and NetEventEvent (the
   carrier of `net-send-event`, so script events do not cross the wire). Their Apply bodies
   mostly survive. Port the PS3 bit layouts (`0x46fc10/0x46f4e8`, `0x469e9c/0x469b80`,
   `0x46af48/0x46a4b8`, `0x46cb60/0x46c650`, `0x469ef8/0x469bd4`, `0x46ae78/0x46a408`,
   `0x46dda8/0x46d218`, `0x46dc98/0x46d124`, `0x46c198/0x46bd8c`, `0x46b1cc/0x46a6fc`,
   `0x46b200/0x46a730` + Apply `0x475578`, `0x46de30`, `0x4789c8/0x4792b0`), bitstream
   `0x42f8d8` (write) / `0x42fe78` (read). Also stubbed: NetEventKickGriefer Apply `0x4711f4`,
   NetEventStartConnection/ConnectionDone Apply (transport side).
2. **Co-op script natives stubbed.** `net-coop-host-script-id`, `net-coop-script-id`,
   `net-player-coop-id`, `net-get-capture-zone`, `net-apply-npc-fight-damage`,
   `net-get-player-by-net-id` return 0 on PS4; `net-get-net-id-from-player` is cut to 0xd0
   bytes (PS3 0x530). Co-op objective scripts and NPC fights rely on them.
3. **Cinema/theater recording and playback.** `cinema.cpp` is gone (replay start `0x4348f0`,
   replay state machine `0x435be8`, cinema list/file store `0x434324..0x435468`,
   `0x4631d8..0x464464`); only the `TaskManagerReplay` task vtable (`0x433b98` -> `0x5e3980`)
   and a stubbed cinema menu callback remain.
4. **Online progression back end.** Leaderboards (`sceNpScoreInit`, `RecordScore`,
   `GetRanking*` are not imported on PS4), NetPlayerData publish/fetch over TUS
   (`0x4c6298`, `0x4c65f8`, `0x4c6e18`, encrypt/decrypt `0x4c4df0/0x4c4cd8`) and the PS3
   TrophyManager (`0x1735b8`, replaced by a PS4 trophy manager at `b894a8`) are absent. The
   match-side stat code (NetStatGame, money tallies, skill rating, medals) is present but merged
   into a few large PS4 functions (`66d8c0`, `66cb70`), and its log-only PS3 entry points
   (`AddMoneyToTally 0x4dcf88`, `Stop 0x4dfc08`, `CaptureTallies 0x4dce9c`,
   `RestoreCapturedTallies 0x4e1b24`) are unresolved: port them from the PS3 bodies.
5. **Rules code is present but merged.** Game over (score/time/round limit, last team),
   round over, HostUpdate and ClientUpdate exist only inside PS4 `NetGameManager::Update`
   (`0x63e6a0`). Hooking them individually on PS4 is not possible; port them as separate C++
   functions from PS3 `0x4a49e8`, `0x49f114`, `0x4a8af8`, `0x494930` (field map in section 5).
6. **Log-only functions.** 632 PS3 functions are unresolved. Treat them as "present on PS4
   in some form" only where their source file survives; the C++ port should follow the PS3
   bodies.

## 4. Call graph: match start -> play -> round/game over (PS3 1.10, PS4 1.02)

Flow/TaskManagerOnline are in the frontend map; their addresses are given for orientation.

```
TaskManagerOnline task updates (flow.cpp, PS3 vtables 0xd70498..0xd70578)
 |-- host "Connections done" task 0x440f90
 |     |-- NetGameManager::AssignTeams 0x4a2490 ............ PS4 6400d0 (alive)
 |     |     `-- build/send NetEventAssignTeam 0x469314 -> Apply 0x4747b0 / PS4 632110
 |     `-- build/send NetEventLoadLevel 0x489d4c -> Apply 0x474bd8 / PS4 63b6b0
 |-- wait-ready tasks 0x440478 / 0x4405c4
 |     |-- build/send NetEventReadyToStart 0x48a26c -> Apply 0x474abc / PS4 63b500
 |     `-- AssignTeams (small) 0x4a1be8 .................... PS4 640fc0
 |-- start tasks 0x440130 / 0x4409c4
 |     `-- build/send NetEventStartGame 0x48a13c -> Apply 0x4759b0 / PS4 63b300
NET_SM_UPDATE 0x444660 (flow) / cinema 0x4348f0
 `-- StartNetGame 0x4a85a0 ................................. PS4 647a50 (alive)
       |-- net-games record setup 0x49eb80 (inlined in 647a50)
       |-- 0x49ed20 -> PS4 6471a0, 0x49fc70 -> PS4 646bf0
       `-- initial spawn 0x4a1eb0 ........................... PS4 644220 (called by 647a50)
             `-- coop spawner helper 0x49f618 ............... PS4 642b60
TaskManagerOnlineUpdate 0x442558 / replay 0x436220 (every frame)
 `-- NetGameManager::Update 0x4a9410 ...................... PS4 63e6a0 (alive; callers 5e5c80, 5f14d0)
       |-- ClientUpdate 0x494930 + UpdatePlayerTrackerVisibility 0x494608   (inlined in 63e6a0)
       |-- per-slot tracker update 0x4c9088 (x10) ........... PS4 6658d0 (x10)
       |-- NetPlayerTracker update 0x4cac50 .................. PS4 664bc0
       |     `-- spawn remote NetCharacter 0x4c9880 -> 6636b0; set model 0x4c9b58 -> 663280
       |           `-- spawn at spawner 0x4c8568 -> 662fa0
       |-- respawn/spawn players 0x4a6008 (+0x493d30) ....... PS4 642d20
       `-- HostUpdate 0x4a8af8 (host only) .................. inlined in 63e6a0
             |-- CheckGameOver 0x4a49e8 ...................... inlined in 63e6a0 (@63f31d)
             |     `-- reason -> build/send NetEventEndGame 0x489ff8 (PS4 alive)
             |           -> Apply 0x47a680 / PS4 62c9f0 -> results (flow NET_SM_RESULTS)
             |-- CheckRoundOver 0x49f114 ..................... inlined in 63e6a0 (probable)
             |     `-- 0x486f4c -> build/send NetEventEndRound 0x469848
             |           -> Apply 0x47b168 / PS4 61a950 -> round results task
             |-- SpawnNetFlag 0x4a03f8 ........................ PS4 63e1b0
             |-- per-slot host update 0x4caa28 (x10) .......... PS4 6640a0 (x10)
             |-- net task events 0x487a90 (CoopTeamFailed), 0x487cd0 (CompleteTask),
             |   0x487bb0 -> 0x46b7c8 (StartNetTask), 0x48d628 (PickupPickedUpClient) -> PS4 611160
             `-- 0x49d448 (objective helper, consts 33364361/5bfae217; unresolved)
Script side (DC scripts of each mode): net natives 0x4cc8e4..0x4cfc28 -> PS4 0x667d40..0x669c80
```

## 5. NetGameManager fields and constants confirmed on both builds

From CheckGameOver (PS3 `0x4a49e8`) and its inlined copy in PS4 `0x63e6a0` (`63f31d..63f3e5`):

| Meaning | PS3 1.10 | PS4 1.02 |
|---|---|---|
| g_netGameManager | object `0xf1dc80` (this) | `r12` in 63e6a0 |
| current net game index (s16) | `this+0x18ec` | `this+0x1ca0` |
| net-games table | `*net-games*` symbol, sid `0x3b9a067d` via `0x151d4c`; {count, records}, record stride 0x80 | same sid via `a15e70`; record stride 0x90 |
| record: game type (10 = no time limit check) | `rec+0x0c` | `rec+0x0c` |
| record: sub-record pointer, compared with `this+0x19b0` | `rec+0x48`, `this+0x19b0` | `rec+0x48`, `this+0x1d6c` |
| match start frame / round start frame | `this+0x19a4` / `this+0x19a0` | `this+0x1d60` / `this+0x1d5c` |
| frame counter (global) | `[0xe60a00+0x1cc]` | `[0x1116180+0x204]` |
| minimum time before game over (frames) | 1500 | 1500 |
| time limit (float) x 300.0 | `this+0x1c38` | `this+0x21b8` |
| score limit (int) | `this+0x1c34` | `this+0x21b4` |
| end reason "time expired" | 6 | 6 |

Net events: same 80 type ids on both builds (table in section 7). Event object: vtable at
+0, type id byte at +8 (PS4 factory writes it explicitly), 7 virtual slots: 0 dtor, 1 deleting
dtor, 2 Write(BitStream), 3 Read(BitStream), 4 Apply, 5 shared base method, 6 size (ret 0).

## 6. Function tables (hand-checked subset; the TSV has every function)

Columns: role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | notes for porting.

### 6.1 NetGameManager, rules, match flow

| Role | PS3 1.10 | PS4 1.02 | State | Evidence | Porting notes |
|---|---|---|---|---|---|
| NetGameManager::SendNetEventToHost | 0x493ca8 | - | unresolved | no static pairing (log-only on PS3) |  |
| respawn helper (sid 332fb3f6) | 0x493d30 | 0x642d20 | inlined | inlined (unique sid 332fb3f6 sits in 642d20); host alive | inlined on PS4 |
| NetGameManager::ClearNetPlayers | 0x4942ac | 0x63db90 | intact-alive | score:0.58 |  |
| NetGameManager::UpdatePlayerTrackerVisibility | 0x494608 | 0x63e6a0 | inlined | inlined (unique sid c032f430 is inside 63e6a0); host alive | inlined on PS4 |
| NetGameManager::ClientUpdate (pickup counters, tracker visibility) | 0x494930 | 0x63e6a0 | inlined | inlined (manual: its callee UpdatePlayerTrackerVisibility (sid c032f430) sits in 63e6a0); host alive | inlined on PS4 |
| NetGameManager::KickVisibilityJob | 0x494c38 | - | unresolved | no static pairing (log-only on PS3) |  |
| NetGameManager::IsHost (this = g_netGameManager 0xf1dc80) | 0x494ea8 | - | unresolved | no static pairing (small; probably inlined) | tiny getter; PS4 inlines it everywhere |
| NetGameManager::ClearStatsNetPlayers | 0x495790 | - | unresolved | no static pairing (log-only on PS3) |  |
| NetGameManager team accessor (assert team < kMaxNetTeams) | 0x495cf0 | 0x64aeb0 | intact-alive | str:team < kMaxNetTeams |  |
| NetGameManager::AddPickup (net pickup table) | 0x4983c8 | 0x6453d0 | intact-alive | str:NetGameManager - too many pickups! total |  |
| NetGameManager::AddNpcTracker | 0x498b28 | 0x644fa0 | intact-alive | str:NetGameManager - too many npcs for a net |  |
| NetGameManager::RequestPickUp | 0x499fe0 | - | unresolved | no static pairing (log-only on PS3) |  |
| NetGameManager::CanPickUp (host arbitration) | 0x49a12c | - | unresolved | no static pairing (log-only on PS3) |  |
| NetGameManager::AbortPickUp | 0x49a608 | - | unresolved | no static pairing (log-only on PS3) |  |
| NetGameManager::GetRandomNetGameIndex (map/mode pick from *net-games*) | 0x49bb00 | - | unresolved | no static pairing (log-only on PS3) | log-only on PS3 |
| StartNetGame helper (net-games record setup, sid ad66fbf6) | 0x49eb80 | 0x647a50 | inlined | inlined (unique sid ad66fbf6 sits in 647a50); host alive | inlined into StartNetGame on PS4 |
| CheckRoundOver: round time expired / only one team alive | 0x49f114 | 0x63e6a0 | inlined | inlined (probable: HostUpdate is merged into 63e6a0; log-only on PS3); host alive | no own PS4 function found |
| NPC art-group / anim set for net NPCs (anim-npc-multiplayer) | 0x4a12e8 | 0x641690 | intact-alive | str:anim-npc-multiplayer |  |
| NetGameManager::AssignTeams (small variant) | 0x4a1be8 | 0x640fc0 | intact-alive | manual: both PS3 and PS4 have two functions with the "AssignTeams - num teams is zero" assert; 0x4a2490 pairs with 6400d0 by "pTrackers[0]" |  |
| initial player spawn at game start | 0x4a1eb0 | 0x644220 | intact-alive | manual: consts 09b2fb53/33364361/3b9a067d/5bfae217/d50bc34b, calls 642b60 (coop spawner); PS4 caller StartNetGame 647a50 | no static PS3 caller |
| NetGameManager::AssignTeams / team balancing (teams, models, spawners) | 0x4a2490 | 0x6400d0 | intact-alive | str:pTrackers[0] | sends NetEventAssignTeam |
| Net Menu "Play Task" (start from debug/net menu) | 0x4a46b0 | 0x648c30 | intact-alive | caller |  |
| CheckGameOver: time limit, score limit, round limit, last team standing; returns end reason (6 = time) | 0x4a49e8 | 0x63e6a0 | inlined | inlined (manual: 63e6a0 @63f31d..63f3e5 = same sequence (net-games lookup, now-start >= 1500, record+0xc != 10, time-limit float * const, cvttss2si, reason 6)); host alive | reads net-games record (*net-games* sid 3b9a067d, 0x80 stride) and NGM fields; see field map |
| spawn/respawn players every frame (spawner choice incl. coop spawners) | 0x4a6008 | 0x642d20 | intact-alive | manual: same consts 09b2fb53/20bea032/33364361/3b9a067d/5bfae217/d50bc34b, both call the coop-spawner helper 0x49f618 = 642b60; PS4 caller Update 63e6a0 | no static PS3 caller (reached through a pointer) |
| StartNetGame: picks net game, sets m_startTime, starts Play Task, spawns players | 0x4a85a0 | 0x647a50 | intact-alive | manual: same callees 49ed20->6471a0 and 49fc70->646bf0, "Net Menu"/"Play Task" strings; 0x49eb80 is inlined into it | callers: NET_SM_UPDATE 0x444660 (flow), cinema 0x4348f0 |
| NetGameManager::HostUpdate (host-only rules: game/round over, flags, pickups, net tasks) | 0x4a8af8 | 0x63e6a0 | inlined | inlined (manual: 63e6a0 calls 63e1b0 (= SpawnNetFlag 0x4a03f8) and 611160 (= 0x48d628) which only HostUpdate calls on PS3); host alive | inlined on PS4 |
| NetGameManager::Update (per frame; "net-task: %s"; calls ClientUpdate, HostUpdate) | 0x4a9410 | 0x63e6a0 | intact-alive | manual: shares consts dec97a4/2743c4b0/9f2aeafb/d3b86d60, callees 63e1b0 (SpawnNetFlag) and a572d0; PS4 callers 5e5c80/5f14d0 (online/replay task update) | callers: TaskManagerOnlineUpdate 0x442558, replay 0x436220. PS4 merged Update+ClientUpdate+HostUpdate+game/round-over checks into one function |
| debug Net menu (Force Rank, Disable Min Player Limit, give medals...) | 0x4b4244 | 0x64ec40 | intact-alive | str:Allow ProgPause During Multiplayer |  |
| per-slot host update (called 10x by HostUpdate) | 0x4caa28 | 0x6640a0 | intact-alive | manual: PS4 Update 63e6a0 calls 6640a0 ten times |  |

### 6.2 Players: tracker, NetCharacter, snapshots, events

| Role | PS3 1.10 | PS4 1.02 | State | Evidence | Porting notes |
|---|---|---|---|---|---|
| NetCharacter attach/props (gun_holster_mp_male...) | 0x45dad0 | 0x600230 | intact-alive | vtable |  |
| NetCharacter::Init (health, anim layers) | 0x46055c | 0x5fb7b0 | intact-alive | sid:6ce7d1bd |  |
| net event debug dump (kNpNetEvent* names table 0xd70c20) | 0x468ed8 | - | unresolved | no static pairing (log-only on PS3) |  |
| CreateNetEventByType (factory, 80 types) | 0x47e550 | 0x6180f0 | intact-alive | manual: same switch over 0..79 (cmp 0x4f) storing one vtable per type; "CreateNetEventByType" assert | factory and type table identical on PS4 |
| FillAnimCmdBitStream (snapshot animation stream writer) | 0x480698 | 0x634970 | intact-alive | str:(animIndex & (-1<<NetSnapshot::kAnimIdBi |  |
| NetPlayerTracker spawn at spawner ("Missing spawner") | 0x4c8568 | 0x662fa0 | intact-alive | str:Missing spawner '%s' trying to spawn pla |  |
| per-slot tracker update (called 10x by Update) | 0x4c9088 | 0x6658d0 | intact-alive | manual: PS4 Update 63e6a0 calls 6658d0 ten times |  |
| spawn remote NetCharacter (hero-default, factory 0x7069c8, sid f2c86c22) | 0x4c9880 | 0x6636b0 | intact-alive | strset |  |
| NetPlayerTracker set model ("Model %s") | 0x4c9b58 | 0x663280 | intact-alive | sid:42a5be7f |  |
| NetPlayerTracker per-frame update (create/update remote characters) | 0x4cac50 | 0x664bc0 | intact-alive | probable: PS4 callee of Update that calls 63ffb0 (= 0x492a20) and the tracker spawn helpers 6636b0 (= 0x4c9880) / 663280 (= 0x4c9b58) |  |
| NetSimpleSnapshot interpolation | 0x4d93e8 | 0x66ab90 | intact-alive | str:index < m_snapshotCount - 1 |  |
| NetSnapshot parse (anim commands) | 0x4da608 | 0x66b430 | intact-alive | str:Parse |  |
| NetSnapshot anim art-group check | 0x4db060 | 0x66b070 | intact-alive | str:artGroup < kNumAnimArtGroups |  |

All 80 event types with Write/Read/Apply are in section 7. Kill/death/assist accounting is
in NetEventKillInfo (17) / KillInfoClient (16) / PlayerDeath (64) Apply and the NetStatGame
tallies (section 6.5). Medals: NetEventEarnMedal (61, serializers stubbed on PS4),
MedalManager `0xc2030`.

### 6.3 Objective modes (Plunder, King of the Hill, Turf War, Elimination, Chain Reaction, co-op)

| Role | PS3 1.10 | PS4 1.02 | State | Evidence | Porting notes |
|---|---|---|---|---|---|
| HiddenTreasure::Init (treasure pickups) | 0xa0790 | 0x54c430 | intact-alive | str:Treasure does not have a name  |  |
| NetSkill (net NPC skill, co-op AI) | 0x2814e0 | 0x33b840 | intact-alive | str:!pNpc->IsDead() |  |
| NetEventTerritorySetTimer::Apply | 0x475d38 | 0x61e610 | partial | evtable: same type/slot in the factory vtables; Write/Read of this event are stubs on PS4 | Write/Read stubs on PS4 |
| NetEventRevivePlayer::Apply (co-op revive) | 0x475ec8 | 0x61f700 | intact-alive | evtable: same type/slot in the factory vtables |  |
| NetEventEndWave::Apply (arena wave end) | 0x476338 | 0x61afe0 | intact-alive | evtable: same type/slot in the factory vtables |  |
| NetEventCaptureFlag::Apply (Plunder/CTF capture) | 0x476cd4 | 0x623b80 | intact-alive | evtable: same type/slot in the factory vtables |  |
| NetEventCoopTeamFailed::Apply (co-op fail) | 0x47780c | 0x61b660 | intact-alive | evtable: same type/slot in the factory vtables |  |
| NetEventSetJuggernaut::Apply | 0x477f40 | 0x62f250 | intact-alive | sid:dd2d35e7 |  |
| NetEventSpawnHiddenTreasure::Apply | 0x47ae08 | 0x61d280 | partial | sid:f0c80b36; Write/Read of this event are stubs on PS4 | Write/Read stubs on PS4 |
| NetEventTerritorySpawn::Apply (zone moves) | 0x47af00 | 0x61e710 | partial | sid:4bd2a224; Write/Read of this event are stubs on PS4 | Write/Read stubs on PS4 |
| NetEventTerritoryScore::Apply | 0x47b3fc | 0x61e8a0 | intact-alive | evtable: same type/slot in the factory vtables |  |
| NetEventTerritoryOwn::Apply (zone captured) | 0x47b468 | 0x61eb90 | partial | sid:289d4d29; Write/Read of this event are stubs on PS4 | Write/Read of the event are stubs on PS4 |
| coop arena: wave complete (duration, bonus) | 0x4976e0 | - | unresolved | no static pairing (log-only on PS3) |  |
| NetGameManager::ShuffleNetTerritorySpawners | 0x49d880 | - | unresolved | no static pairing (log-only on PS3) |  |
| coop: spawn players at coop spawners | 0x49f618 | 0x642b60 | intact-alive | str:Missing coop spawner for player '%s'! |  |
| NetGameManager::SpawnNetTerritory (Turf War / King of the Hill zones) | 0x4a00c0 | 0x64bf20 | intact-alive | sid:b388cbe8 |  |
| NetGameManager::SpawnNetFlag (CTF flags; Plunder treasure carrier) | 0x4a03f8 | 0x63e1b0 | intact-alive | callee |  |
| coop: map player -> coop id / spawner index | 0x4a0d88 | 0x642840 | intact-alive | str:NetGameManager - invalid coop id %s  |  |
| NetTerritory object (Turf War / King of the Hill zone) | 0x4e43b8 | 0x673f40 | intact-alive | strfuzzy |  |
| NetTerritory owned state ("Owned") | 0x4e450c | 0x673de0 | intact-alive | str:Owned |  |
| NetTerritory update | 0x4e45c8 | 0x673ca0 | intact-alive | strfuzzy |  |

Mode logic is split between the net-games record (game type at `rec+0x0c`, read by
CheckGameOver and `net-is-game-type?`), the DC scripts of each mode (they call the natives in
section 8: `net-spawn-flag`, `net-capture-flag`, `net-spawn-territory`, `net-kill-territory`,
`net-end-wave`, `net-coop-*`), and the events above. Elimination is the "only team left/alive"
branch of CheckGameOver/CheckRoundOver. Chain Reaction and King of the Hill use the territory
objects (`net-territory.cpp`, PS4 `670338..`), whose state events cannot be serialized on PS4.

### 6.4 Weapons, loadouts, pickups, boosters

| Role | PS3 1.10 | PS4 1.02 | State | Evidence | Porting notes |
|---|---|---|---|---|---|
| NetBoosterManager: booster id lookup ("Invalid booster id") | 0x4552b8 | - | unresolved | no static pairing (log-only on PS3) |  |
| Net Boosters debug menu | 0x4555d0 | 0x5f72d0 | intact-alive | str:Boosters... |  |
| NetBoosterManager::GetAmount (boosted scale) | 0x455900 | 0x5f6410 | intact-alive | sid:b4bcce62 |  |
| DcPlayerSaveWeaponSet (player-save-weapon-set) | 0x6617c0 | 0x90b830 | intact-alive | native:DcPlayerSaveWeaponSet | loadout save |
| DcWeaponSetReset (weapon-set-reset) | 0x661cec | 0x90ad00 | intact-alive | native:DcWeaponSetReset |  |
| DcGivePlayerWeapon | 0x669a88 | 0x906b20 | intact-alive | native:DcGivePlayerWeapon |  |
| WeaponPickup draw control / pickup object | 0x89d5e0 | 0xb23310 | intact-alive | str:pPickupDrawControl->GetNumMeshes() == pS |  |

Pickup arbitration (RequestPickUp `0x499fe0`, CanPickUp `0x49a12c`, AbortPickUp `0x49a608`)
is log-only on PS3 and unresolved; the pickup events (types 19..25, 75) are intact on PS4.

### 6.5 Progression: money, rank, medals, unlocks, stats, leaderboards, trophies, cinema

| Role | PS3 1.10 | PS4 1.02 | State | Evidence | Porting notes |
|---|---|---|---|---|---|
| MedalManager: award medal | 0xc2030 | 0x5779a0 | inlined | inlined (features 67% inside); host alive |  |
| MoneyManager: NPC points from net-npc-info | 0xcfd94 | 0x5decf0 | intact-alive | str:%s archetype isn't in the net-npc-info l |  |
| TrophyManager thread (create context, register, unlock) | 0x1735b8 | absent | absent | identifying strings missing on PS4 ("sceNpTrophyUnlockTrophy(m_npContext, m_n") | PS4 has its own trophy manager at b894a8 (new SDK API) |
| UnlockableManager skin swap | 0x174a68 | 0xa76460 | intact-alive | str:"Unknown unlockable manager skin swap st |  |
| UnlockableManager weapon swap | 0x175090 | - | unresolved | no static pairing (log-only on PS3) |  |
| TaskManagerReplay (vtable) | 0x433b98 | 0x5e3980 | intact-alive | vtable |  |
| cinema: start replay (NET_SM_REPLAY, multi-player-cinema) | 0x4348f0 | absent | absent | source file cinema.cpp has no trace in the PS4 image | cinema.cpp is gone on PS4 |
| cinema: replay state machine | 0x435be8 | absent | absent | source file cinema.cpp has no trace in the PS4 image |  |
| cinema file paths (cinema%d/...) | 0x464334 | - | unresolved | no static pairing (log-only on PS3) |  |
| leaderboards poll/destroy transaction | 0x4a9f78 | - | unresolved | no static pairing | PS4 still imports sceNpScorePollAsync/DeleteRequest (one caller near 64dae0) |
| leaderboards get rankings | 0x4aa170 | absent | absent | needed PS4 API not imported | sceNpScoreGetRanking* not imported on PS4 |
| leaderboards record score (sceNpScoreRecordScore) | 0x4ab6d0 | absent | absent | needed PS4 API not imported | sceNpScoreRecordScore is not imported by the PS4 executable |
| leaderboards init (sceNpScoreInit, title ctx) | 0x4ab854 | absent | absent | needed PS4 API not imported | sceNpScoreInit/CreateTitleCtx are not imported on PS4 (it imports only sceNpScorePollAsync/DeleteRequest) |
| NetPlayerData::Data::Decrypt | 0x4c4cd8 | - | unresolved | no static pairing (log-only on PS3) |  |
| NetPlayerData::Data::Encrypt | 0x4c4df0 | - | unresolved | no static pairing (log-only on PS3) |  |
| NetPlayerData trophy stat counter (kNpdTrophyStatCount*) | 0x4c5268 | 0x661a50 | inlined | inlined (features 67% inside); host alive |  |
| NetPlayerData::Publish (Save Game Stats thread, TUS) | 0x4c6298 | absent | absent | identifying strings missing on PS4 ("NetPlayerData") | PS4 TUS code lives in d5bb28 (collection-wide) |
| NetPlayerData::FetchTusData | 0x4c65f8 | absent | absent | identifying strings missing on PS4 ("sceNpTusGetData( transactionId, &npId, s") |  |
| NetPlayerData::FetchHddData | 0x4c699c | - | unresolved | no static pairing (log-only on PS3) |  |
| NetPlayerData::RunConnectThread | 0x4c6ab8 | - | unresolved | no static pairing (log-only on PS3) |  |
| NetStatGame::CaptureTallies | 0x4dce9c | - | unresolved | no static pairing (log-only on PS3) | log-only |
| NetStatGame::AddMoneyToTally (cash per event, multiplier) | 0x4dcf88 | - | unresolved | no static pairing (log-only on PS3) | log-only |
| NetStatGame::AddCoopTreasurePickedUp | 0x4df644 | 0x66f7b0 | intact-alive | sid:fc04df84 |  |
| skill rating update ("Player %s skill %f ->") | 0x4df898 | 0x66cb70 | inlined | inlined (features 60% inside); host alive |  |
| NetStatGame::Stop (total money of the match) | 0x4dfc08 | - | unresolved | no static pairing (log-only on PS3) | log-only |
| NetStatGame::RestoreCapturedTallies | 0x4e1b24 | - | unresolved | no static pairing (log-only on PS3) | log-only |
| NPC kill points (coop/arena score) | 0x4e2630 | 0x66d8c0 | inlined | inlined (features 84% inside); host alive |  |

## 7. Net event types (factory id, PS3 -> PS4 per slot)

Write = slot 2, Read = slot 3, Apply = slot 4; "STUB" = `ret` on PS4. The last column is the
PS3 function(s) that build and send the event (store its vtable outside the factory).
Type 62 (`kNpEventCoopTeamFail`) has no case of its own on PS3.

| Id | Event | Write PS3 -> PS4 | Read PS3 -> PS4 | Apply PS3 -> PS4 | PS3 sender |
|---|---|---|---|---|---|
| 0 | NetEventStartConnection | 468c18 -> 63c100 STUB | 468c1c -> 63c0f0 STUB | 474e50 -> 63c0e0 STUB | 489c50 |
| 1 | NetEventConnectionDone | 4697d4 -> 63bf70 alive | 469768 -> 63bdb0 alive | 474d90 -> 63bda0 STUB | 489ec8 |
| 2 | NetEventLoadLevel | 46a12c -> 63bad0 alive | 469dd0 -> 63b760 alive | 474bd8 -> 63b6b0 alive | 489d4c |
| 3 | NetEventReadyToStart | 46b72c -> 63b620 alive | 46abdc -> 63b580 alive | 474abc -> 63b500 alive | 48a26c |
| 4 | NetEventStartGame | 469ff4 -> 63b470 alive | 469cbc -> 63b3d0 alive | 4759b0 -> 63b300 alive | 48a13c |
| 5 | NetEventResetGame | 46a0ec -> 62d790 alive | 469da0 -> 62d6f0 alive | 476d44 -> 62d620 alive |  |
| 6 | NetEventEndGame | 46ba4c -> 62d500 alive | 46b9f0 -> 62d3c0 alive | 47a680 -> 62c9f0 alive | 489ff8 |
| 7 | NetEventEmitProjectileBullet | 46c428 -> 62c6e0 alive | 46bfe0 -> 62c340 alive | 47c968 -> 62b9d0 alive | 474028 |
| 8 | NetEventEmitGrenade | 46cfa8 -> 62b650 alive | 46ca58 -> 62b250 alive | 47c330 -> 62ada0 alive | 471d80 |
| 9 | NetEventGrenadeExplode | 483090 -> 62a780 alive | 4804b0 -> 62a020 alive | 47b840 -> 629460 alive | 46957c 4718c8 |
| 10 | NetEventGrenadeStartFuse | 46b6b8 -> 6292d0 alive | 46ab74 -> 629110 alive | 475ad8 -> 629040 alive | 470270 |
| 11 | NetEventEmitRocket | 46ee4c -> 63b050 alive | 46ea34 -> 63ad40 alive | 478dc8 -> 63a7c0 alive | 48ab58 |
| 12 | NetEventCharacterMove | 46fd80 -> 6398a0 alive | 46f638 -> 6387f0 alive | 485060 -> 6382d0 alive | 4694e0 |
| 13 | NetEventAttackProjectile | 46ecc4 -> 628be0 alive | 46e8cc -> 628670 alive | 477100 -> 627da0 alive | 4832e0 |
| 14 | NetEventAttackExplosion | 482f4c -> 6279d0 alive | 480388 -> 6274f0 alive | 47db78 -> 627060 alive | 4716b8 |
| 15 | NetEventAttackCloseCombat | 46e4b8 -> 626dc0 alive | 46db74 -> 626a30 alive | 47a448 -> 6268b0 alive | 471528 |
| 16 | NetEventKillInfoClient | 46cdc0 -> 634490 alive | 46c888 -> 633ef0 alive | 48ef0c -> 633cb0 alive | 4801bc 48a3c0 |
| 17 | NetEventKillInfo | 46cbc0 -> 6335b0 alive | 46c6a8 -> 632f80 alive | 475f94 -> 632d50 alive | 48ed90 |
| 18 | NetEventAssignTeam | 470830 -> 6328a0 alive | 4705e0 -> 632340 alive | 4747b0 -> 632110 alive | 469314 |
| 19 | NetEventRequestPickup | 46b588 -> 631520 alive | 46aa68 -> 6313e0 alive | 48e360 -> 630f20 alive | 48dc6c |
| 20 | NetEventApprovedPickup | 469fc4 -> 632080 alive | 469c8c -> 631fe0 alive | 475328 -> 631e40 alive | 48e11c |
| 21 | NetEventDeniedPickup | 46b5e8 -> 631b10 alive | 46aac0 -> 631730 alive | 4743e8 -> 631630 alive | 48df24 |
| 22 | NetEventPickupPickedUpClient | 46b4b4 -> 626720 alive | 46a9a8 -> 626560 alive | 48d5fc -> 6262a0 alive | 48d628 |
| 23 | NetEventPickupPickedUp | 46b418 -> 626030 alive | 46a920 -> 625d50 alive | 47d750 -> 625890 alive | 472238 |
| 24 | NetEventAddDroppedPickupClient | 46e3c8 -> 6255a0 alive | 46da9c -> 6251a0 alive | 48d244 -> 624fb0 alive | 484af8 |
| 25 | NetEventAddDroppedPickup | 46e2d8 -> 624cc0 alive | 46d9c4 -> 6248c0 alive | 4843c0 -> 6242f0 alive | 484a50 |
| 26 | NetEventStartCloseCombat | 46c34c -> 630890 alive | 46bf18 -> 630490 alive | 47e090 -> 630180 alive | 48cd9c |
| 27 | NetEventSetCanSee | 46b3b4 -> 630090 alive | 46a8c4 -> 62ff70 alive | 4751e4 -> 62ff20 alive | 48cc58 |
| 28 | NetEventCheating | 46b380 -> 62fe90 alive | 46a890 -> 62fdf0 alive | 4709c8 -> 62fcb0 alive |  |
| 29 | NetEventRespawnPlayer | 46e238 -> 6240e0 alive | 46d938 -> 623df0 alive | 476390 -> 623ce0 alive | 48ca28 |
| 30 | NetEventCaptureFlag | 46b34c -> 623c50 alive | 46a85c -> 623bb0 alive | 476cd4 -> 623b80 alive | 48c8a8 |
| 31 | NetEventSpawnCarryObject | 46b2ec -> 623a70 alive | 46a804 -> 623930 alive | 47d478 -> 6236b0 alive | 48c770 |
| 32 | NetEventCarryObjectStartMove | 46e100 -> 623500 alive | 46d810 -> 623290 alive | 47cfb0 -> 622d60 alive | 48c210 |
| 33 | NetEventCarryObjectClientStartMove | 46e19c -> 622bb0 alive | 46d8a4 -> 622940 alive | 48c3a0 -> 622760 alive | 4842d0 |
| 34 | NetEventCarryObjectStopMove | 46c2f0 -> 62fc10 alive | 46bebc -> 62fb50 alive | 479f90 -> 62f9c0 alive | 48c090 |
| 35 | NetEventSetJuggernaut | 46b288 -> 62f8b0 alive | 46a7a8 -> 62f770 alive | 477f40 -> 62f250 alive |  |
| 36 | NetEventDrawSniperLaser | 46a024 -> 622500 alive | 469cec -> 622240 alive | 477d88 -> 622090 alive | 46b968 |
| 37 | NetEventNpcSpawn | 46b200 -> 62f240 STUB | 46a730 -> 62f230 STUB | 475578 -> 62f220 STUB | 48bd9c |
| 38 | NetEventNpcKill | 46b1cc -> 622080 STUB | 46a6fc -> 622070 STUB | 4767e4 -> 621f80 alive | 48bb7c |
| 39 | NetEventBreakableAttackProjectile | 46dfb8 -> 621c20 alive | 46d4b0 -> 621670 alive | 47a0b8 -> 621180 alive | 469244 483f50 |
| 40 | NetEventBreakableAttackExplosion | 46ded4 -> 620e50 alive | 46d328 -> 6209c0 alive | 477608 -> 6207e0 alive | 471440 |
| 41 | NetEventBreakableBreakPiece | 46ec28 -> 62f070 alive | 46e7d0 -> 62ee50 alive | 479568 -> 62ebe0 alive | 48b670 |
| 42 | NetEventBreakableCollapse | 46eb30 -> 620570 alive | 46e598 -> 6201f0 alive | 476988 -> 620100 alive | 470db0 |
| 43 | NetEventNpcChangeOwnership | 46b16c -> 620080 alive | 46a6a4 -> 61ff60 alive | 476be0 -> 61fe10 alive |  |
| 44 | NetEventNpcDealDamage | 46de30 -> 61fe00 STUB | 46d294 -> 61fb10 alive | 48a620 -> 61fa70 alive | 471290 |
| 45 | NetEventPlayerInfo | 470740 -> 62e780 alive | 470508 -> 62e260 alive | 475014 -> 62e090 alive | 4895b8 |
| 46 | NetEventMessage | 470f20 -> 62de90 alive | 46f320 -> 62dd40 alive | 470ee0 -> 62dc50 alive | 468e88 486ef0 4893dc |
| 47 | NetEventEvent | 4789c8 -> 61fa60 STUB | 4792b0 -> 61fa50 STUB | 470fc8 -> 61fa10 alive | 471034 |
| 48 | NetEventRevivePlayer | 469f68 -> 61f900 alive | 469c38 -> 61f7d0 alive | 475ec8 -> 61f700 alive | 489210 |
| 49 | NetEventTerritoryOwn | 46fc10 -> 61f030 STUB | 46f4e8 -> 61f020 STUB | 47b468 -> 61eb90 alive | 471fc4 |
| 50 | NetEventTerritoryScore | 46afe4 -> 61ea80 alive | 46a53c -> 61e940 alive | 47b3fc -> 61e8a0 alive | 4888c8 |
| 51 | NetEventTerritorySpawn | 469e9c -> 61e890 STUB | 469b80 -> 61e880 STUB | 47af00 -> 61e710 alive | 4887b4 |
| 52 | NetEventTerritorySetTimer | 46af48 -> 61e700 STUB | 46a4b8 -> 61e6f0 STUB | 475d38 -> 61e610 alive | 4886b0 |
| 53 | NetEventPlayVox | 46ae78 -> 61e600 STUB | 46a408 -> 61e5f0 STUB | 4785d0 -> 61e2b0 alive | 470c0c |
| 54 | NetEventSimpleSnapshot | 46dda8 -> 61e2a0 STUB | 46d218 -> 61e290 STUB | 476fac -> 61e1c0 alive | 483e60 |
| 55 | NetEventStartNpcFight | 46c198 -> 61e1b0 STUB | 46bd8c -> 61e1a0 STUB | 482718 -> 61d6d0 alive | 483890 |
| 56 | NetEventNpcFightAlign | 46dd34 -> 61d620 alive | 46d1ac -> 61d4c0 alive | 479e38 -> 61d3b0 alive | 487f3c |
| 57 | NetEventSpawnHiddenTreasure | 46cb60 -> 61d3a0 STUB | 46c650 -> 61d390 STUB | 47ae08 -> 61d280 alive | 487de0 |
| 58 | NetEventReviveCreditClient | 46b108 -> 61f5f0 alive | 46a648 -> 61f4b0 alive | 489014 -> 61f390 alive | 4890c0 |
| 59 | NetEventReviveCredit | 46b0a4 -> 61f280 alive | 46a5ec -> 61f140 alive | 4766b8 -> 61f040 alive | 488edc |
| 60 | NetEventEarnMedalClient | 468dc0 -> 61d270 STUB | 468dc4 -> 61d260 STUB | 468dc8 -> 61d250 STUB |  |
| 61 | NetEventEarnMedal | 469ef8 -> 61d240 STUB | 469bd4 -> 61d230 STUB | 46b760 -> 61d1e0 alive |  |
| 62 | NetEventCoopTeamFail | - | - | - |  |
| 63 | NetEventNpcArmorDestroyed | 46b044 -> 62db40 alive | 46a594 -> 62da00 alive | 476b34 -> 62d910 alive | 488ddc |
| 64 | NetEventPlayerDeath | 46c0ec -> 61cfd0 alive | 46bcd0 -> 61cd60 alive | 479878 -> 61cb50 alive | 483610 |
| 65 | NetEventCompleteTask | 468dcc -> 62d900 STUB | 468dd0 -> 62d8f0 STUB | 4711c0 -> 62d820 alive | 487cd0 |
| 66 | NetEventStartNetTask | 46fb04 -> 61c700 alive | 46f404 -> 61c160 alive | 4778d8 -> 61bef0 alive | 46b7c8 |
| 67 | NetEventTurretSnapshot | 46dc98 -> 61bee0 STUB | 46d124 -> 61bed0 STUB | 476e70 -> 61be10 alive | 487904 |
| 68 | NetEventPlayerHealth | 46fa90 -> 61bce0 alive | 46f398 -> 61bb00 alive | 476500 -> 61ba40 alive | 469060 |
| 69 | NetEventKickGriefer | 46ae44 -> 61b9b0 alive | 46a3d4 -> 61b910 alive | 4711f4 -> 61b900 STUB | 4877b0 |
| 70 | NetEventGrieferKicked | 46ae10 -> 61b870 alive | 46a3a0 -> 61b7d0 alive | 471258 -> 61b7c0 alive | 487688 |
| 71 | NetEventCoopTeamFailed | 46a0bc -> 61b730 alive | 469d70 -> 61b690 alive | 47780c -> 61b660 alive | 487a90 |
| 72 | NetEventKnockedDownClient | 46addc -> 61b5d0 alive | 46a36c -> 61b530 alive | 487388 -> 61b410 alive | 487434 |
| 73 | NetEventKnockedDown | 46ada8 -> 61b380 alive | 46a338 -> 61b2e0 alive | 47b370 -> 61b230 alive | 487258 |
| 74 | NetEventEndWave | 46dc3c -> 61b190 alive | 46d0cc -> 61b040 alive | 476338 -> 61afe0 alive | 48710c |
| 75 | NetEventAbortPickup | 46b528 -> 630e10 alive | 46aa10 -> 630cd0 alive | 4742a8 -> 630c00 alive | 48da20 |
| 76 | NetEventKillEntity | 469e6c -> 61afd0 STUB | 469b50 -> 61afc0 STUB | 470aa4 -> 61afa0 alive | 487038 |
| 77 | NetEventEndRound | 46ad14 -> 61ad80 alive | 46a2bc -> 61ab30 alive | 47b168 -> 61a950 alive | 469848 |
| 78 | NetEventShieldDamage | 46ac44 -> 61a660 alive | 46a20c -> 61a300 alive | 479b90 -> 619f50 alive | 489ac8 |
| 79 | NetEventPlayerLeft | 46ac10 -> 619ec0 alive | 46a1d8 -> 619e20 alive | 474ef0 -> 619db0 alive | 489968 |

## 8. Net script natives (DC)

The 29 `DcNet*` C names of the PS3 build plus the 7 `DCNet*` ones and the other net-related
natives in the same table. PS3 registration routine `0x14d470` (called from `0x4ce820`); PS4
registers natives as static nodes in `0x666d50`. All are registered on PS4.

| Role | PS3 1.10 | PS4 1.02 | State | Evidence | Porting notes |
|---|---|---|---|---|---|
| script native DcNetSendEvent (net-send-event) | 0x8196c | 0x4c3b60 | intact-alive | native:DcNetSendEvent |  |
| script native DcNetIsCinema (net-is-cinema?) | 0x4cc8e4 | 0x669c50 | intact-alive | native:DcNetIsCinema |  |
| script native DcNetGetBoosterAmount (net-get-booster-amount) | 0x4cc950 | 0x668820 | intact-alive | native:DcNetGetBoosterAmount |  |
| script native DcNetKillEntity (net-kill-entity) | 0x4cc99c | 0x6696a0 | intact-alive | native:DcNetKillEntity |  |
| script native DcNetCurrentNetTask (net-current-net-task) | 0x4cca70 | 0x669780 | intact-alive | native:DcNetCurrentNetTask |  |
| script native DcNetCoopHostScriptId (net-coop-host-script-id) | 0x4ccab8 | 0x669c40 | stub | native:DcNetCoopHostScriptId; PS4 stub(ret0) |  |
| script native DcNetIsHost (net-is-host) | 0x4ccb00 | 0x669c80 | intact-alive | native:DcNetIsHost |  |
| script native DcSetForceTension (net-set-force-tension) | 0x4ccb74 | 0x667ee0 | intact-alive | native:DcSetForceTension |  |
| script native DcAlivePlayerCount (net-alive-player-count) | 0x4ccc30 | 0x667f20 | intact-alive | native:DcAlivePlayerCount |  |
| script native DcActivePlayerCount (net-active-player-count) | 0x4ccc78 | 0x667f40 | intact-alive | native:DcActivePlayerCount |  |
| script native DcPauseCoopCombo (net-pause-coop-combo) | 0x4cccc0 | 0x667f60 | intact-alive | native:DcPauseCoopCombo |  |
| script native DcNetEndWave (net-end-wave) | 0x4ccd0c | 0x6687e0 | intact-alive | native:DcNetEndWave |  |
| script native DcNetKillTerritory (net-kill-territory) | 0x4ccd48 | 0x669800 | intact-alive | native:DcNetKillTerritory |  |
| script native DcNetSpawnTerritory (net-spawn-territory) | 0x4ccdf4 | 0x669840 | intact-alive | native:DcNetSpawnTerritory |  |
| script native DcNetSpawnFlag (net-spawn-flag) | 0x4cce60 | 0x6698c0 | intact-alive | native:DcNetSpawnFlag |  |
| script native DcNetIsGameTypeP (net-is-game-type?) | 0x4cceb4 | 0x667e60 | intact-alive | native:DcNetIsGameTypeP |  |
| script native DcNetGetBoosterEnabled (net-booster-enabled?) | 0x4ccf94 | 0x668810 | intact-alive | native:DcNetGetBoosterEnabled |  |
| script native DcNetHideWaveScreen (net-hide-wave-screen) | 0x4ccfe4 | 0x668850 | intact-alive | native:DcNetHideWaveScreen |  |
| script native DcNetShowWaveScreen (net-show-wave-screen) | 0x4cd08c | 0x668890 | intact-alive | native:DcNetShowWaveScreen |  |
| script native DcNetHudSetGrappleIndicator (net-hud-set-grapple-indicator) | 0x4cd174 | 0x668980 | intact-alive | native:DcNetHudSetGrappleIndicator |  |
| script native DCNetClearNpcFightOwner (net-clear-npc-fight-owner) | 0x4cd2b8 | 0x668a10 | intact-alive | native:DCNetClearNpcFightOwner |  |
| script native DCNetCharacterKilledP (net-character-killed?) | 0x4cd3a4 | 0x668b10 | intact-alive | native:DCNetCharacterKilledP |  |
| script native DcNetCoopTeamFailedP (net-coop-team-failed?) | 0x4cd4b4 | 0x669c20 | intact-alive | native:DcNetCoopTeamFailedP |  |
| script native DcNetAllPlayersDeadP (net-all-players-dead?) | 0x4cd4d8 | 0x6694b0 | intact-alive | native:DcNetAllPlayersDeadP |  |
| script native DcNetAllPlayersInRegionP (net-all-players-in-region?) | 0x4cd530 | 0x669500 | intact-alive | native:DcNetAllPlayersInRegionP |  |
| script native DcGetCaptureZone (net-get-capture-zone) | 0x4cd66c | 0x6687d0 | stub | native:DcGetCaptureZone; PS4 stub(ret0) |  |
| script native DcNetPlyaerCoopId (net-player-coop-id) | 0x4cd6e8 | 0x669240 | stub | native:DcNetPlyaerCoopId; PS4 stub(ret0) |  |
| script native DcNetCoopTask (net-coop-task) | 0x4cd850 | 0x6697e0 | intact-alive | native:DcNetCoopTask |  |
| script native DcNetCoopScriptId (net-coop-script-id) | 0x4cd8ac | 0x669c30 | stub | native:DcNetCoopScriptId; PS4 stub(ret0) |  |
| script native DcNetRevivePlayer (net-revive-player) | 0x4cd8f4 | 0x667d40 | intact-alive | native:DcNetRevivePlayer |  |
| script native DcNetCaptureFlag (net-capture-flag) | 0x4cda00 | 0x669960 | intact-alive | native:DcNetCaptureFlag |  |
| script native DCNetApplyNpcFightDamage (net-apply-npc-fight-damage) | 0x4cdc90 | 0x668db0 | stub | native:DCNetApplyNpcFightDamage; PS4 stub(ret0) |  |
| script native DCNetGetNetIdFromPlayer (net-get-net-id-from-player) | 0x4ce030 | 0x668fe0 | partial | native:DCNetGetNetIdFromPlayer; PS4 size d0 vs PS3 530 |  |
| script native DcNetRespawnFlag (net-respawn-flag) | 0x4ce560 | 0x6698e0 | intact-alive | native:DcNetRespawnFlag |  |
| script native DcNetGetGrappleTime (net-get-grapple-time) | 0x4ce69c | 0x6687f0 | intact-alive | native:DcNetGetGrappleTime |  |
| script native DCNetGetPlayerByNetId (net-get-player-by-net-id) | 0x4ce6f0 | 0x6690b0 | stub | native:DCNetGetPlayerByNetId; PS4 stub(ret0) |  |
| prints "ScriptFn" | 0x4ce820 | 0x666d50 | intact-alive | str:DCNetApplyNpcFightDamage |  |
| - | 0x4ceeec | - | unresolved | no static pairing (small; probably inlined) |  |
| script native DCNetGetNpcFightLocator (net-get-npc-fight-locator) | 0x4cef00 | 0x668dc0 | intact-alive | native:DCNetGetNpcFightLocator |  |
| script native DCNetSafetyNpcFightDamage (net-safety-npc-fight-damage) | 0x4cf168 | 0x668bd0 | intact-alive | native:DCNetSafetyNpcFightDamage |  |
| script native DcNetDrawObjectiveIcon (net-draw-objective-icon) | 0x4cf3a8 | 0x669250 | intact-alive | native:DcNetDrawObjectiveIcon |  |
| script native DcUpdateFightRef (net-update-fight-ref) | 0x4cf64c | 0x667f80 | intact-alive | native:DcUpdateFightRef |  |
| script native DcNetNpcConfirmCoopPlayersContact (net-npc-confirm-coop-players-contact) | 0x4cfc28 | 0x6690c0 | intact-alive | native:DcNetNpcConfirmCoopPlayersContact |  |
| - | 0x4cfe20 | - | unresolved | no static pairing (small; probably inlined) |  |
| - | 0x4cfe38 | - | unresolved | no static pairing (small; probably inlined) |  |
| - | 0x4cfe88 | - | unresolved | no static pairing |  |
| script native DcWaitUntilNetTime (wait-until-net-time) | 0x7d56c8 | 0xa40870 | intact-alive | native:DcWaitUntilNetTime |  |
| script native DcNetHeliMoveOnSpline (net-heli-move-on-spline) | 0x7ffdc0 | 0xa7fcc0 | intact-alive | native:DcNetHeliMoveOnSpline |  |

## 9. Limits

- PS3 function bounds come from OPD entries and `bl` targets; PS4 starts from call/reloc/lea
  targets and frame-pointer prologues, so a few PS4 leaf functions may be merged with their
  neighbour. "alive" means referenced by live code or a data slot; reachability from the entry
  point is not computed.
- Automatic pairings carry their method in the evidence column (`str`, `sid`, `native`,
  `vtable`, `evtable`, `evsender`, `callee`, `caller`, `score:x`, `strfuzzy`, `inlined`).
  `score`, `strfuzzy`, `caller` and `inlined` pairings are probable, not proven; `manual`
  rows were checked in the disassembly.
- Semantics come from strings, call structure and the factory tables, not from execution.
