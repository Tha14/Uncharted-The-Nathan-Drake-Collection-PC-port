# Uncharted 2 PS4 1.02: what a new session layer must provide

Scope: the multiplayer menus that still exist on PS4 1.02 (PAGE_MULTIPLAYER, PAGE_LOBBY,
custom game, join/joining, post-game) and the match start. This file says which PS4 state
those menus read, who still writes it, what is stubbed, and which C++ functions a PC session
layer has to write or call. The transport that carries events between peers (send/receive
of net events) is covered separately in `TRANSPORT-INTERFACE.md` and is only referenced here.

Static analysis only. Nothing was run and no game file was changed. No game bytes are stored
here: only addresses, field offsets, ids and short mnemonics. All addresses are PS4 image
addresses (`boot-linked.bin` file offset - `0x183ce8`) unless marked PS3. No earlier
third-party Uncharted 2 multiplayer notes or code were used.

How it was checked:

| Tool (`u2mp/tools/`) | Used for |
|---|---|
| `u2_session_scan.py` (new) | Every instruction that reads or writes a global or one of its fields, grouped by function; store values; call sites with their argument set-up |
| `ps4_dis.py`, `ps4_cg.py` | Function bodies, callers, vtable slots (`--vt`) |
| `ps3_dis.py --annotate work/ps3-110` | PS3 1.10 counterparts (PAGE_LOBBY `0x443bc8`, NET_SM_UPDATE `0x444660`, IsHost `0x4d2394`) |
| `u2_menu_pages.py` (now also takes `PAGE_name` / `PAGE_prefix*`) | Controls of each MP page in the PS4 `menu.bin` |

`u2_session_scan.py` needs a full llvm-objdump listing of the text. Make it outside the repo
(it is derived from game code), as described in the script header, then for example:

```
python u2mp/tools/u2_session_scan.py <full.dis> writes 14ce97c      # every NET_SM store
python u2mp/tools/u2_session_scan.py <full.dis> refs 1065b08 105efb8 104f1c8
python u2mp/tools/u2_session_scan.py <full.dis> calls 63dc30         # SetNetGame call sites
```

Note on page callbacks: the first u32 of the argument is the page event. In every MP page
handler, `6` is the per-frame update (all state machines run there). `0` runs when the page
opens (PAGE_LOBBY picks a default net game on `0`, as PS3 does). PAGE_MULTIPLAYER also handles
`1` (it clears the MP mode).

---

## 1. What replaced NET_SM and IsOnline on PS4

### 1.1 The NET_SM state survived as a plain global: `0x14ce97c`

PS3 kept the state at `0xf11764`, behind SetNetState `0x4383cc`, GetNetState `0x4383e4`
and IsOnline `0x4384b0` (`state > 1`, 379 callers). PS4 keeps the same variable and the same
values at **`0x14ce97c` (int32)**:

- **SetNetState = `0x5e9d30`** (`mov [0x14ce97c], edi; ret`) and **GetNetState = `0x5e9d40`**
  still exist, but nothing calls them. The surviving code stores to the global directly.
- **IsOnline is inlined everywhere.** The global has 833 `cmp` sites: 448 `cmp 2 / jl`
  (offline), 217 `cmp 1 / jg` (online), 89 `cmp 1 / jle`, plus `setl`/`setg` forms. All of
  them test `state > 1`. With 927 references in 643 functions, this is the global that HUD,
  pause, NetGameManager, profile-save and menu code test instead of IsOnline. A typical site
  is the profile-save guard repeated in many pages (`[0x11474f8+0x6c0]==3 && state<=1` ->
  mark the save dirty), which means progress is saved locally only while offline.
- The other reads take the raw value (switch on the state): JOINING `609790`, the host/join
  page `6093f0`, Leave `5f00d0`, plus debug and HUD readers (`97a4xx..97bxxx`, `5f6xxx`).

Every PS4 store to `0x14ce97c` (all 26 sites):

| value | name (PS3 GOTO log) | PS4 writer | PS3 counterpart |
|---|---|---|---|
| 3 | CREATE_GAME_WAIT | `5eb830` @5eba21 (callback `e2707dbe`) | `444bb0` |
| 3 / 17 | IsHost(match) ? CREATE_GAME_WAIT : CLIENT_LOBBY | Leave::Enter `5f00d0` @5f0479 (custom game) | `445608` |
| 5 | CLIENT_GAME_LIST_WAIT | BUTTON_MULTIPLAYER_JOIN `6095d0` | `4655b0` |
| 7 | CLIENT_CONNECT_TO_HOST | BUTTON_MULTIPLAYER_SELECT_JOIN_GAME `609680` | `465808` |
| 10 | CLIENT_WAIT_FOR_PARTY_CONNECT_GAME_HOST | host/join page `6093f0`; JOINING `609790` | `465da0`, `4661e0` |
| 14 | CLIENT_JOIN_GAME_WAIT | JOINING `609790` (x2) | `4661e0` |
| 16 | SERVER_LOBBY | host/join page `6093f0` | `465da0` |
| 16 / 17 | IsHost(match) ? SERVER_LOBBY : CLIENT_LOBBY | Leave::Enter `5f00d0` (matchmaking) | `445608` |
| 17 | CLIENT_LOBBY | JOINING `609790` | `4661e0` |
| 18 | CHOOSE_HOST_JOIN | `6093f0`, PAGE_MULTIPLAYER_JOIN `609640`, `609680`, `609790`, LEAVING_GAME `5eb7d0` | several |
| 19 | START_MATCHMAKING | `5eba40` (cb `4055f45f`), POSTGAME `5ec4f0`, LEAVING_GAME `5eb7d0`, mode dispatcher `5ecb80` (BUTTON_MP_POSTGAME_BACK) | `43e99c`, `43fb38`, `43ea8c`, `443248` |
| 26 | UPDATE | **Playing::Enter `5f23e0`** | `444660` |
| 27 | RESET | PreResetting::Enter `5f13f0` | `43edc8` |
| 28 | REPLAY | cinema states `5e89d0`, `5e9630` | `4348f0`, `435be8` |
| 30 | ROUND_RESULTS | ShowRoundResults::Enter `5f0af0` | `4430d8` |
| 35 | LEAVE_GAME | Leave::Enter `5f00d0` | `445608` |
| any | SetNetState | `5e9d30` (dead) | `4383cc` |

Corrections to `FUNCTIONS-FRONTEND-NET.md`: `0x444660`, `0x445608`, `0x4430d8`, `0x43edc8`
are not absent. They became the Enter methods of the TaskManagerOnline states (section 3).
`0x465da0` is the page handler `6093f0`. `0x443248` is the mode dispatcher `5ecb80`.
RESULTS (29) has no writer on PS4.

**Nothing on PS4 moves the state above 1 when you enter Multiplayer.** On PS3 this was done
by NetInit `0x447a5c`, which stores 18 at `0x4494a8`. The session layer must store 18.

### 1.2 Session objects: g_matchSession `0x1065b08`, party session `0x105efb8`

Both are instances of one class (the PS3 Session class: g_matchSession `0xf20c48`, party
`0xf2c8a0`). PS3 IsHost `0x4d2394` is `this+0x10 == 0 || this[0x1124] == this[0x1128]`.
PS4 inlines exactly this test about 120 times:

```
IsHost(s) = (*(u64*)(s+0x10) == 0) || (*(u32*)(s+0xb00) == *(u32*)(s+0xb04))
```

| field | meaning (PS4) | evidence |
|---|---|---|
| `+0x10` u64 | session context / room handle, nonzero = in a session | IsHost; copied to netInfo+0x60 by `6093f0`; POSTGAME saves it |
| `+0xb00` u32 | my member id | IsHost; `66a3d0` compares member ids with it |
| `+0xb04` u32 | host (room owner) member id | IsHost; JOINING looks up the member with this id |
| `+0x228 + i*0xd0`, i<10 | member records: `+0x9` valid byte (`s+0x231`), `+0x10` member id (`s+0x238`) | `66a3d0`, JOINING @60996x |
| `+0x99` u8, `+0x9c` s32 | match only: create-room reply arrived, result (<0 = error) | `6093f0` state 3 |
| `+0xa8` u8 + `+0xac` s32 | match only: search reply arrived, result (<0 = error) | PAGE_MULTIPLAYER_JOIN `609640` |
| `+0xb8` u8 + `+0xbc` s32, `+0xc0` u64 | match only: join reply arrived, result, joined-room handle | JOINING state 14 |
| `+0xc8` u8 | match only: left/kicked, read by LEAVING_GAME `5eb7d0` | |
| `+0xc`, `+0xf8/+0xfc`, `+0x100`, `+0x104` | match only: create-room parameters (attribute from table sid `e5350103`, netInfo+0, 8 slots, two floats) | written by `5eb830` |
| `+0xb50` | match: peer index into the connection table `0x1152a10`; party: u8 "leave party" request | `6093f0`, `609790`, `5eb080`, `5eb1a0` |
| `+0xb54` u8 | match only: **NetEventLoadLevel received**. Set by LoadLevel Apply `63b6b0`, consumed by EstablishConnection `5f2ea0` (host) and the client wait page `609d00` | |

**Nothing on PS4 writes `+0x10`, `+0xb00`, `+0xb04` or the member records** (no direct or
`lea`-based store anywhere in the text). With the Matching2 code gone, both sessions always
look like "not in a session, I am host". Their methods (create, join, leave, member events)
are the main thing the session layer must supply. Two small leftovers, `5e9d90`/`5e9db0`,
clear `this+8`, `this+0xb54` and the byte `0x113a270`.

### 1.3 Net info object `0x104f1c8` (PS3 `0xf1fa48`)

PAGE_LOBBY pairs it with PS3 `0xf1fa48`, the object that holds the NP id at `+0x288`
(PS3 field `+0x2b4` = PS4 `+0x2c4`).

| field | meaning | written by (PS4) | read by |
|---|---|---|---|
| `+0x0` u64 | own id / room key copied into the create-room request | nobody (session layer) | `5eb830`, `6095d0` |
| `+0x8` u8 | 1 = this console created the custom game, 0 = joined | `5eb830`=1, `6095d0`=0 | |
| `+0x1c` u8 | **network error / disconnected**. Pages react with the error box (text `61a3662e`) and LEAVING_GAME | nobody (session layer) | CONNECTING, PAGE_MULTIPLAYER, PAGE_LOBBY, client wait `609d00` |
| `+0x24/+0x28` f32 | create-room parameters | nobody | `5eb830` |
| `+0x30` u8 | flag used by the Resetting states and roster | `5e5b00`, `5f1240` | `65b1c0`, `65cc80` |
| `+0x34` s32 | 1 = returning from a matchmaking match (Leave sets it); chooses 19 vs 18 on leave | `5f00d0`, cleared by `5eb830`/`6095d0` | `5eb7d0`, `5f00d0`, rosters |
| `+0x60` u64 | current room handle (from match `+0x10` or the selected list entry `+0x20`) | `6093f0`, `609680` | `609d00` (compared with the 0x103 payload) |
| **`+0x2c4` s32** | **MP menu mode**: 0 none, 1 matchmaking, 2 custom, 3 cinema, 4 leaderboard, 5 net trophies | dispatcher `5ecb80` (1..5), PAGE_MULTIPLAYER on event 1 (=0), PAGE_LOBBY from message 0x110 (party members) | 59 readers: lobby queries (1 or 2), roster (2/3), NetGameManager `63e6a0`, AssignTeams `6400d0`, stats `66c600`/`66cb70` |
| `+0x2d8/+0x2dc` | roster bookkeeping | `5ec010`, `65e3b0`, PAGE_MULTIPLAYER | |

`+0x2c4` is the "net mode" value the remaining code tests besides `state > 1`. Many
gameplay and stat paths take the co-op branch on `==2` / `==3` and the competitive branch on
`==1`. Values 1 and 2 come only from the PAGE_MULTIPLAYER buttons. The session layer must send
the value to party members (message 0x110, see 1.5) so their copy is the same.

### 1.4 NetGameManager `0x113b760` (PS3 `0xf1dc80`)

The fields the frontend uses (game-rule fields are in `FUNCTIONS-MATCH-GAMEPLAY.md` section 5):

| field | meaning |
|---|---|
| `+0x10 + i*0x2c0`, i<10 | net player slots. `+0x20` used, `+0x120` peer/connection set, `+0x26c` local, `+0x275`, `+0x148` (see 3.3). Cleared by ClearNetPlayers `63db90` (calls `662a30` ten times) |
| `+0x1ca0` s16 | current net game index into `*net-games*` (-1 = none) |
| `+0x1e88` | restart parameter (TaskManagerOnline event `15f1cf05`) |
| `+0x20a0[32]`, `+0x21a0` | playlist pointers and count (PLAYLIST query `6562d0`; the save keeps the chosen id at `0x11474f8+0x104`) |
| `+0x21b4..+0x21c7` | rule overrides set by SetNetGame: `+0x21b4` score limit, `+0x21b8` time limit (f32), `+0x21bc` byte flags (friendly fire), `+0x21c0` rounds, `+0x21c4` byte difficulty |

### 1.5 Message mailbox `0x14d0e18` (NetEventMessage, type 46)

Peers exchange small menu/session messages as NetEventMessage (type 46, Apply `62dc50`,
Write `62de90`, Read `62dd40`). Apply copies the message into a mailbox:

- count `0x14d0e18` (u32, max 64). Entries start at `0x14d0e20`, stride 0x40: `+0x24` sender
  member id (from the receiving connection `+0x160`), `+0x28` code, `+0x30` heap copy of the
  payload, `+0x38` size.
- Readers scan `0x14d0e48` (= the code field) for a code, then call
  `660e70(index, u32* outSender, void* outBuf, u32* inOutSize)`, which copies the payload out,
  removes the entry and returns 0 on success. `660db0` removes by sender. `660da0` (the PS3
  "post request" slot) is `ret 0`.
- Sending is `614960(dest, code, data, size)`, used by Leave::Enter (`0x105`) and JOINING
  (`0x10d`). The transport layer has to carry it.

| code | consumed by | payload | effect |
|---|---|---|---|
| `0x101` | JOINING state 9 | 0x30 bytes, `buf[8]` != 0 = slots reserved | -> state 10, otherwise fail |
| `0x103` | client wait page `609d00` | u64 room handle | equal to netInfo+0x60 -> pop + LEAVING_GAME (room closed) |
| `0x105` | ShowResults::Update `5f0b30` (client) | | host is leaving the match -> GoLeave |
| `0x10d` | `661310` (now a stub that always reports "no error, ready") | 1 byte | |
| `0x10e` | JOINING state 12 | 1 byte, != 0 = party host joined | ClearNetPlayers, -> state 14 |
| `0x110` | PAGE_LOBBY update | s32 MP mode | -> netInfo+0x2c4 |
| `0x117` | Playing::Update `5f14d0` | | in-match notice |
| `0x118` | POSTGAME `5ec4f0` | u64 room handle | party-up accepted -> state 19, PAGE_MULTIPLAYER_MATCHMAKING |

Nothing on PS4 posts local messages here any more, so this is the session layer's inbox.
The session layer can inject a message by building a NetEventMessage
(`+0xc` code, `+0x10` data, `+0x18` size) and calling `62dc50(evt, conn)` with `conn+0x160`
= sender id. That uses the game's allocator. Writing the entry directly is also possible, but
the payload must then come from the game allocator (`b333f0`).

### 1.6 Other globals on the path

| address | what |
|---|---|
| `0x1136640` | menu manager. ShowPage `5cb990(menu, id, flags)`, PopPage `5cc6f0(menu)`, close/reset `5cd1e0(menu)`. Message box data at `*(menu+0x17d0)` (`+8` text id, `+0x54` button cb id), then ShowPage(`0x719e9fec`). Dynamic page handlers: count `menu+0x24f0`, entries `menu+0x24f8` (0x18 each: u32 page id, fn, arg), max 32 (assert `m_numPageEventHandlers < kMaxPageEventHandlers`, menuhost.cpp). Pending-page stack after a level change: count `menu+0x1160`, ids `menu+0x1164` (max 200) |
| `0x1152a10` | connection table, 32 x 0x220 (signaling peers; `+0x8` id, `+0xc` active, `+0x40` refcount). Released through `60ba10` |
| `0x113a048` | 10 x 0x34 slot table, count `+0x208` (reset by `6093f0`) |
| `0x113ddb0` | custom-game choice list (section 4) |
| `0x113db48` | create/join game settings block (two floats + settings copied from the list entry `+0x78`) |
| `0x14cee70` | selected custom-game list entry (pointer) |
| `0x113a270` | u8 "abort match" flag -> GoLeave / LEAVING_GAME |
| `0x104f1c1` | u8 "net package load failed" (CONNECTING) |
| `0x11474f8` | profile save (`+0x6c0` save state, `+0x104` playlist id, `+0x114` team pref) |

---

## 2. Surviving MP pages and callbacks

Control/condition data comes from the PS4 `menu.bin`. Page controls with action 22 call a
query function chosen by the control's condition number, from the **query table `0x105e370`**
(0x18 per entry: u32 cond, fn at `+8`, u64 arg at `+0x10`; the arg reaches the function at
`query+0x48`). Query object: `+0` op (1 = text, 7 = enabled, 2..9 value ops), `+4` value
in/out, `+0x40` out text, `+0x50`/`+0x54` sub-id/column.

### 2.1 BUTTON_MULTIPLAYER `5eb160` (stub, replaced by us)

PS3 `0x43b150` ran the content check, the bootstrap `0x438bc8` (shows
PAGE_MULTIPLAYER_CONNECTING, starts NetInit) and finally SetNetState(18). On PS4 the session
layer has to do this (section 5, step 1).

### 2.2 PAGE_MULTIPLAYER_CONNECTING `5ed6a0` (partial, works as a loader)

Runs on event 6 only. If netInfo+0x1c is set it pops itself and shows the error box. If not,
it closes a pending popup (`menu+0x9b8`) and looks up the package `"net"` (sid `4240ef2e`) in
the package table `0x1130390`. If the package is not there it requests it
(`56ad90(0x1130390,"net",0)`) and **pops itself** once the package reaches state 7 with flag
0x80. The NetCtl, NAT and EULA checks are gone. Use: push it on top of PAGE_MULTIPLAYER so the
`net` package loads.

### 2.3 PAGE_MULTIPLAYER `5eb1a0` (partial)

- event 1: netInfo+0x2c4 = 0.
- event 0: if not IsHost(party), sets party+0xb50 = 1 (leave the party). Then reseeds the RNG,
  calls `[0x1060888]` and `64e810` (list refresh).
- event 6: profile/rank widgets from the save `0x11474f8` and DC tables
  (`d34ac18e`, `33d2252a`, `be2ca34b`, `c85e199d`). Then:
  - if netInfo+0x1c: ShowPage(PAGE_ROOT_MAIN `37656388`, 1) + error box `61a3662e`;
  - else if **not IsHost(party)**: ShowPage(PAGE_LOBBY). **A party member is sent to the
    lobby automatically.**
- Controls: BUTTON_MULTIPLAYER_MATCHMAKING (cond 51), _CUSTOM (52), _CINEMA (53),
  BUTTON_LEADERBOARD (131), BUTTON_NETTROPHIES (136), party roster PARTY_PLAYER0..9 (cond
  16..25), INVITES (48).
- The buttons go to the intact **mode dispatcher `5ecb80`** (PS3 `0x443248`):
  - CUSTOM `7d593924`: mode 2, ShowPage(PAGE_LOBBY).
  - MATCHMAKING `649e809a`: mode 1, ShowPage(PAGE_LOBBY).
  - CINEMA `b85cb12a`: mode 3, SetNetGame(-1), PAGE_PROGRESS.
  - LEADERBOARD: mode 4, PAGE_MENU_LEADERBOARD.
  - NETTROPHIES: mode 5, PAGE_NET_TROPHIES.
  - BUTTON_MP_POSTGAME_BACK `a0448333`: if IsHost(party), state 19 and
    PAGE_MULTIPLAYER_MATCHMAKING.
  - `ab58f9da`: PAGE_MULTIPLAYER.
- Roster query `659bb0` (cond 26..35 = 0x1a..0x23, arg 0..9) is **`ret`**, so the party list is
  empty. On PS3 this was the roster query `0x4bc728` from table `0xe15d30` (absent).

What it needs: party session valid and host (`+0x10 != 0`, `+0xb00 == +0xb04`) for the
leader, or `+0xb00 != +0xb04` for a member; NET_SM >= 2; netInfo+0x1c = 0.

### 2.4 PAGE_LOBBY `5ebac0` (intact apart from the stripped PS3 calls)

- event 0: if ngm+0x1ca0 < 0, **SetNetGame `63dc30`(ngm, 0, NULL, 10)**. Then picks a random
  team preference into the save `+0x114`. Gone compared with PS3: party `0x4d5338(party,0)` and
  the NetPlayerData init.
- event 6: takes message 0x110 (mode) into netInfo+0x2c4. Then **pops the page if
  IsHost(party) and mode == 0**, or if netInfo+0x1c is set. Gone compared with PS3: the call to
  `0x43a444` (lobby status -> START_MATCHMAKING / CLIENT_CONNECT_TO_HOST) and `0x4a9d34`.
- Controls and query functions (all in table `0x105e370`):

| control | cond | query fn | state |
|---|---|---|---|
| BUTTON_MULTIPLAYER_STARTMATCH | 50 | `655c40`: enabled = IsHost(party); label from table `0xdef4e0[mode-1]`. Callback `5eb780` = **`ret`** | needs a callback |
| PLAYLIST (mode 1) | 55 | `6562d0`: ngm `+0x20a0[]/+0x21a0`, save `+0x104` | intact; playlists must be loaded |
| CUSTOMGAMETYPE (mode 2) | 57 | `656550`: list `0x113ddb0`, game types sid `f985c7b6` | intact |
| CUSTOMMAP | 58 | `6568c0` | intact |
| CUSTOMVARIATION | 59 | `656b20` | intact |
| CUSTOMSCORELIMIT | 60 | `656d70` | intact |
| CUSTOMTIMELIMIT | 61 | `656fe0` | intact |
| CUSTOMFRIENDLYFIRE | 62 | `657270` | intact |
| CUSTOMROUND | 63 | `657490` | intact |
| CUSTOMDIFFICULTY (co-op) | 65 | `6576d0` | intact |
| BUTTON_MULTIPLAYER_CUSTOMTEAM | 66 | `657900` | intact |
| PARTY_PLAYER0..9 | 26..35 | `659bb0` = **`ret`** | write it |
| BUTTON_MULTIPLAYER_INVITES | 48 | `659b50` | intact |

The option writers are enabled only for the party host (op 7 -> IsHost(party)).

### 2.5 Host side of a custom game: `5eb830` (cb `e2707dbe`) and page handler `6093f0`

`5eb830` (PS3 `0x444bb0`, intact) does the following:

- netInfo+8 = 1, ClearNetPlayers, netInfo+0x34 = 0.
- Fills the create-room request in g_matchSession (`+0xc`, `+0xf8..+0x108`).
- Resets the menu (`5cd1e0`) and sets the wait text `69deab50`.
- Registers the page handler `6093f0` for page `0x471007a1`, shows that page and sets
  **state 3**.

The control `e2707dbe` is not on any page in the PS4 `menu.bin`, so the session layer has to
call `5eb830` itself (from STARTMATCH).

`6093f0` (PS3 `0x465da0`) runs on event 6 and switches on NET_SM:

- **3**: waits for match `+0x99`. If `+0x9c` < 0: pop, state 18. If `+0x9c` >= 0:
  netInfo+0x60 = match`+0x10`, reset `0x113a048`, **state 10** (any value >= 0 counts as success).
- **10**: `661310(&err,&ready)` is a stub (err=0, ready=1), so: `6144b0()`, **state 16**.
- **16**: **PlayTask(LookupTask(`9f2aeafb`, "deathmatch" `d3b86d60`))** =
  `a5be10(a572d0(0x9f2aeafb, 0xd3b86d60))`. This spawns TaskManagerOnline (section 3).
  It runs on every update while the state stays 16.

### 2.6 Join side: BUTTON_MULTIPLAYER_JOIN `6095d0`, PAGE_MULTIPLAYER_JOIN `609640`, SELECT_JOIN `609680`, JOINING `609790`, client wait `609d00`

- `6095d0`: netInfo+8 = 0, ClearNetPlayers, netInfo+0x34 = 0, copy netInfo+0 into
  match+0xf8. Shows PAGE_MULTIPLAYER_JOIN and sets **state 5**. The control is not on any PS4
  page, so it is only reachable from our code.
- `609640` (event 6): if match+0xa8 is set and +0xac < 0 -> pop, state 18. Its list
  BUTTON_MULTIPLAYER_SELECT_JOIN_GAME has cond 3 -> `659590` = **`ret`**: the game list is
  never filled.
- `609680` (SELECT_JOIN, broken): it reads the chosen entry from an **uninitialised stack
  array** (`[rbp + idx*0x50 - 0x608]`). It sets `0x14cee70` = entry and netInfo+0x60 =
  entry+0x20, copies the entry's settings (`+0x78`) to `0x113db48`, releases the old peer
  (match+0xb50), sets **state 7** and shows PAGE_MULTIPLAYER_JOINING. **Replace it.**
- `609790` JOINING (PS3 `0x4661e0`), event 6, switch on NET_SM 7..14:
  - **7**: the connect code is gone. It only releases the peer, clears match+0xb50 and fails
    (pop, state 18). **Never let the page see state 7.**
  - **9**: waits for message 0x101 (`buf[8]`): -> 10, otherwise fail. The PS3 timeout is gone
    (only an `rdtsc` remains), so the message must already be queued when 9 is set.
  - **10**: `661310` stub -> **14**.
  - **12**: message 0x10e (1 byte) -> ClearNetPlayers, **14**. A 0 payload fails.
  - **14**: waits for match+0xb8. If +0xbc < 0 it fails. Then it needs match+0xc0 != 0 and a
    member record with id == match+0xb04. Then it resets the menu, registers page handler
    `609d00` for `0x471007a1`, shows that page, calls `6144b0()` and sets **state 17**.
- `609d00` (client wait in the lobby, event 6):
  - netInfo+0x1c -> LEAVING_GAME `b808d03c`.
  - Message 0x103 with the current room -> pop + LEAVING_GAME. The same happens for ngm+8 ->
    `+0x26c` and for byte `0x113a270`.
  - **When match+0xb54 is set (NetEventLoadLevel arrived) it clears it and runs
    PlayTask("deathmatch")**.

### 2.7 LEAVING_GAME `5eb7d0`, POSTGAME `5ec4f0`, others

- `5eb7d0` (event 6): when match+0xc8 is set: ClearNetPlayers, pop, then state 19 if
  netInfo+0x34 else 18.
- `5ec4f0` POSTGAME (intact, PS3 `0x43fb38`): BUTTON_POSTGAME_PARTYUP `5eca50` shows the wait
  popup. The page saves match+0x10, registers handler `0x471007a1`, and on message 0x118 for
  that room pops and goes to state 19 + PAGE_MULTIPLAYER_MATCHMAKING. Its player rows
  BUTTON_MULTIPLAYER_PLAYER0..9 (cond 6..15) use `659bc0` = **`ret`**.
- PAGE_MULTIPLAYER_MATCHMAKING `64e910` = `ret` (the whole matchmaking state machine PS3
  `0x4b0a58` is gone). Its rows are the same `659bc0` stub. LIST_MATCHMAKING_VOTE `64e8e0` is
  partial.
- `5eb080` (cb `2e1a9a7b`): party member leave -> party+0xb50 = 1, pop, LEAVING_GAME.
  `5eb150` / `5eb180` (LEAVE_MULTIPLAYER): pop only. The PS3 teardown `0x4386d0` is gone.
- Stubs that only keep their slot: STARTMATCH `5eb780`, START `5eb7c0`, PLAYER0..9 `5ec460`,
  PARTY_PLAYER0..9 `5ec490`, INVITES `5ebea0`, LIST_FRIENDS `5ebde0`, PLAYER_OPTIONS
  `5ed8b0`, CLANTAG `5ed8a0`.

---

## 3. TaskManagerOnline on PS4

### 3.1 Who creates it

- The type "TaskManagerOnline" is registered by the static initializer `5ef970` (factory
  `5ea060`, which constructs the object with vtable `0x100a810`). No code refers to it by name.
- **The DC game task "deathmatch" names it as its task manager.** `dc1/ps4/tasks.bin` holds the
  sid `b7b3c967` (= "TaskManagerOnline") in the record of the task "deathmatch". The task
  "replay" holds `85ef6592` (= TaskManagerReplay).
- The task is started with `a5be10(a572d0(0x9f2aeafb, 0xd3b86d60))`. It is started only by:
  - the host page `6093f0` in state 16,
  - the client wait page `609d00` when match+0xb54 is set.

  (`5eb7b0` returns the task; `5e57eb`, `5f1120`, `63f9d8` look it up again for restarts.)

### 3.2 States (vtable `0x100a810`, Go* virtuals; state classes have [dtor, Enter, Exit, Update])

| Go method (vtable off) | state vtable | Enter | Update -> next |
|---|---|---|---|
| Active `5ea250` (+0x08) | `100aac0` | base `a630b0` | base `a63150` |
| EstablishConnection `5ea390` (+0x150) | `100ac40` | `5f3050` | `5f2ea0` -> LoadLevels / Leave |
| LoadLevels `5ea4d0` (+0x158) | `100ac00` | `5f2e50` | `5f2dc0` -> WaitReady / Leave |
| WaitReady `5ea610` (+0x160) | `100abc0` | `5f2d90` | `5f26d0` -> Playing / Leave |
| Playing `5ea750` (+0x168) | `100ab80` | **`5f23e0`** | `5f14d0` -> ShowResults / ShowRoundResults / PreResetting / Leave |
| PreResetting `5ea890` (+0x170) | `100ab40` | `5f13f0` (state 27) | `5f1390` -> Resetting |
| Resetting `5ea9d0` (+0x178) | `100ab00` | `5f1240` | `5f1000` -> ResettingReady / Leave |
| ResettingReady `5eab10` (+0x180) | `100aa80` | `5f0fc0` | `5f0ec0` -> Playing / Leave |
| ShowResults `5eac50` (+0x188) | `100aa40` | `5f0ea0` | `5f0b30` -> Leave (client: on message 0x105) |
| ShowRoundResults `5ead90` (+0x190) | `100aa00` | `5f0af0` (state 30) | `5f0a90` -> PreResetting |
| Leave `5eaed0` (+0x198) | `100a9c0` | **`5f00d0`** | `5eff30` (ends the task, `a5b430`) |

Each Go* sets a pending state: it initialises a static descriptor once (`0x14cea30..`, name,
factory), checks `this->vfunc[0xc0](desc)`, then stores `sm(+0x50)->+8 = desc` and sets
`+0x34 |= 0x100`. A C++ layer can call them as `(*(void(**)(void*))(*(u64*)tmo + off))(tmo)`.

**Start: `5efe00` (vtable +0x30)** resets the object and the NGM. Then, if IsHost(match), it
calls **GoEstablishConnection**, otherwise **GoLoadLevels**. Events (`5efd30`, +0x38):
`6ba8328a` -> +0x151, `bcc3b05b` -> +0x152, `15f1cf05` -> +0x150 = restart (ngm+0x1e88).

### 3.3 Match start on PS3 and on PS4

PS3: lobby -> NET_SM_UPDATE `0x444660` -> StartNetGame `0x4a85a0`. Disassembly shows that
`0x444660` is itself the Playing state Enter: `r3` is the state object, `+4` its owner, and
`+16` is set to !IsHost.

PS4: the same body is **Playing::Enter `5f23e0`**:

- NET_SM = 26.
- state+0x1c = !IsHost(match).
- Clears the HUD and player bits.
- **StartNetGame `647a50`(ngm `0x113b760`, TaskManagerOnline* = state+8, restart =
  tmo+0x150)**.
- Weather/lighting set-up from DC `c85e199d`.

StartNetGame reads the record at ngm+0x1ca0 (set by SetNetGame) and plays the "Net Menu" /
"Play Task" node. Its spawn helper is `644220`.

So on PS4 a match starts like this. **No new state object has to be made.** The session layer
must:

1. Have the custom game chosen: ngm+0x1ca0 and overrides via SetNetGame `63dc30` (section 4).
2. Make the sessions valid: match `+0x10`, member ids, `+0x99/+0x9c` (host) or
   `+0xb8/+0xbc/+0xc0` (client).
3. Fill the NGM player slots (ngm+0x10+i*0x2c0) for every peer, with the transport's peer
   handles. EstablishConnection::Update `5f2ea0` (host) only passes when every used slot with
   `+0x120 != 0` and `+0x26c == 0` is not in the state `+0x275 != 0 && +0x148 == 0`. Then it:
   - calls `6144b0()`,
   - calls **AssignTeams `6400d0`** (sends NetEventAssignTeam),
   - calls `610840` and `60c2e0` (they build and send the start events, among them LoadLevel,
     type 0x12 built by `610840`),
   - and goes to LoadLevels once its own copy of LoadLevel has set match+0xb54 (Apply `63b6b0`
     also calls SetNetGame from the event).
4. Let the host page reach state 16 (PlayTask). Clients reach the same task through `609d00`
   when the host's LoadLevel arrives.

WaitReady/Playing then run on NetEventReadyToStart / StartGame (transport). Every frame,
Playing::Update `5f14d0` runs `5ec010` and NetGameManager::Update `63e6a0`. Game over
(NetEventEndGame, Apply `62c9f0`) leads to ShowResults.

### 3.4 Back to the lobby

ShowResults -> **Leave::Enter `5f00d0`** (PS3 `0x445608`):

- calls `649120`;
- the host sends message `0x105` to the clients;
- error -> push LEAVING_GAME on the pending-page stack, state 35;
- custom game -> state = IsHost(match) ? 3 : 17;
- matchmaking -> push PAGE_MULTIPLAYER_MATCHMAKING, netInfo+0x34 = 1, state =
  IsHost(match) ? 16 : 17.

Leave::Update `5eff30` ends the task. The menu comes back with the pending pages.
After a custom game the host is back in CREATE_GAME_WAIT (3), so `6093f0` must be the active
handler again with match `+0x99/+0x9c` set. Clients are in 17 with the `609d00` page.

---

## 4. Game type and map selection

- **`*net-games*`** (sid `3b9a067d`, via `a15e70`): `{s32 count; rec* records}`, record stride
  **0x90**. Fields used on PS4: `+0x4` level id (looked up in sid `dec97a4`, stride 0x38,
  package name at `+0x18`), `+0xc` game type (10 = no time-limit check; 4, 8, 9, 12 special
  cases), `+0x14` score limit, `+0x18`/`+0x1c` alternative score limits (by player-count
  class), `+0x20` time limit (f32), `+0x48` sub-record, `+0x60` friendly fire default,
  `+0x6c` team flag (LoadLevel Apply clears per-slot team values when it is set), `+0x88`
  rounds.
- Game type names: sid `f985c7b6`, stride 0x44 (`+0`/`+4` text ids).
- **Custom choice list `0x113ddb0`**: `{s32 count; entries stride 0x790 from +8}`, each entry
  is one game type (`+0` = type) with its maps and variations. It is built by `654c80` from
  `*net-games*` (grouped by `+0xc`, filtered by the level table). The setters are `655250`
  (type), `655390`, `655540`, and they end in **`655810` -> SetNetGame `63dc30`**.
- **SetNetGame `63dc30`(ngm, s16 index, const Overrides* o, int c)** (PS3 `0x49c458`):
  - stores ngm+0x1ca0 = index;
  - copies the overrides (`o+0` -> `+0x21b4`, `o+8` -> `+0x21bc`, `o+0x10` -> `+0x21c4`), or,
    when `o` is NULL, the record defaults;
  - preloads the level packages of `rec+0x4` (`568460` on `0x1130390`).

  Callers: PAGE_LOBBY open (`0,NULL,10`), cinema (`-1`), `655810` (x2), LoadLevel Apply
  `63b6b0` (`evt+0xc`, `evt+0x10`, 10).
- How the choice reaches StartNetGame: the lobby option queries change ngm+0x1ca0 and the
  overrides through SetNetGame. The host's LoadLevel event carries the index and overrides to
  the clients, whose Apply calls SetNetGame. Playing::Enter calls StartNetGame, which reads
  ngm+0x1ca0. The "random game" helper of PS3 (`0x49bb00`) has no PS4 pair, so matchmaking must
  pick the index itself.

---

## 5. C++ to write, in order

Notation: `R` = replace the PS4 function (hook its address or its menu-table slot),
`C` = call the existing PS4 function, `W` = new code with no PS4 counterpart.

### Step 0: session state helpers (W)

1. `NetSetState(int s)`: store to `0x14ce97c` (or C `5e9d30`). `NetGetState()`: `5e9d40`.
2. `SessionSet(Session* s, u64 ctx, u32 myId, u32 hostId)` for `0x1065b08` and `0x105efb8`:
   writes `+0x10`, `+0xb00`, `+0xb04`. `SessionSetMember(s, i, id, bool valid)`: writes
   `s+0x228+i*0xd0` (`+0x9` valid, `+0x10` id). `SessionClear(s)`: zeroes the same fields
   (C `5e9d90` for `+8`/`+0xb54`).
3. `NetMailboxPost(code, sender, data, size)`: C NetEventMessage Apply `62dc50` with a local
   event (`+0xc/+0x10/+0x18`) and a fake connection (`+0x160` = sender). The transport calls it
   for every received type-46 event.
4. `NetSetError(bool)`: netInfo `0x104f1c8+0x1c`. Set it on any transport loss. Every MP page
   then falls back to the error box / LEAVING_GAME.

### Step 1: enter Multiplayer (R `5eb160` BUTTON_MULTIPLAYER, already ours)

- Clear the error, netInfo+0x34 = 0, ngm+0x1ca0 = -1.
- Party session: `SessionSet(party, ctx, me, me)` (I am the party leader). Member 0 = me.
- `NetSetState(18)` (CHOOSE_HOST_JOIN; IsOnline is now true for the 833 inlined checks).
- C ShowPage(PAGE_MULTIPLAYER `53cbb5f2`, 0), then C ShowPage(PAGE_MULTIPLAYER_CONNECTING
  `9e1863b1`, 0). CONNECTING loads the `net` package and pops itself.

### Step 2: party / lobby roster (R query stubs)

- R `659bb0` (cond 26..35, party rows; arg at `query+0x48` = row 0..9) and R `659bc0`
  (cond 6..15, match/post-game rows). Op 1: name text into `query+0x40`. Op 7: row
  visible/enabled. Other ops: value. The data comes from the session layer's member list
  (names from the transport, member ids from the session).
- R `5ec490` (PARTY_PLAYERn button) and `5ec460` (PLAYERn): optional, player card or kick.
- A friend joins the leader's party: on the member side `SessionSet(party, ctx, me, leaderId)`.
  PAGE_MULTIPLAYER then sends that member to PAGE_LOBBY by itself. The leader sends message
  0x110 (s32 mode) each time netInfo+0x2c4 changes. PAGE_LOBBY copies it on the members.

### Step 3: mode and custom options (C, intact)

- MATCHMAKING/CUSTOM buttons -> dispatcher `5ecb80` (mode 1/2, PAGE_LOBBY).
- PAGE_LOBBY open -> SetNetGame(0). The option controls -> `656550`..`657900` ->
  `655810` -> SetNetGame.
- For mode 1 the playlists (ngm `+0x20a0/+0x21a0`) must be filled (W: build from
  `*net-games*`, or route mode 1 to the custom flow with a random index).

### Step 4: start (R `5eb780` BUTTON_MULTIPLAYER_STARTMATCH)

Leader only (the query already disables the button for others).

- mode 2 (custom): C `5eb830` (state 3, host page `6093f0`). Then W `HostCreateGame()`:
  ask the transport/server for a game, and on reply set match `+0x10` = ctx,
  `SessionSet(match, ctx, me, me)`, member 0 = me, `+0x9c` = 0 (or <0 on failure), then
  `+0x99` = 1. `6093f0` then runs 3 -> 10 -> 16 -> PlayTask("deathmatch").
- Tell the party members to follow (transport message of our own). On each member,
  W `ClientFollowParty()`:
  1. netInfo+8 = 0, C ClearNetPlayers `63db90`.
  2. Connect the transport to the host.
  3. `SessionSet(match, ctx, me, hostId)`, with members for host and me valid.
  4. match `+0xbc` = 0, `+0xc0` = ctx, `+0xb8` = 1.
  5. C ShowPage(PAGE_MULTIPLAYER_JOINING `1af4bdd2`), `NetSetState(12)`.
  6. `NetMailboxPost(0x10e, hostId, {1}, 1)`.

  JOINING then runs 12 -> 14 -> 17 and shows the client wait page `609d00`.
- mode 1 (matchmaking): `64e910` and the PS3 matchmaking machine are gone. W: either the
  server picks the host and everyone follows the custom path, or rebuild PAGE_MULTIPLAYER_MATCHMAKING
  `64e910` from PS3 `0x4b0a58` (vote list `64e8e0`).

### Step 5: join a listed game (optional browser)

- R `659590` (cond 3: join list rows from the server's game list).
- R `609680` SELECT_JOIN:
  - set `0x14cee70`, netInfo+0x60 and `0x113db48` from our list entry;
  - connect;
  - fill match as in step 4;
  - **NetSetState(9)** (not 7), then `NetMailboxPost(0x101, hostId, buf{[8]=1}, 0x30)`;
  - ShowPage(JOINING).

  JOINING runs 9 -> 10 -> 14 -> 17.
- R or C `6095d0` to open PAGE_MULTIPLAYER_JOIN (state 5). Keep match `+0xa8/+0xac` = 0
  unless the search failed.

### Step 6: players into the match (W, with the transport)

- For each connected peer, add a NetGameManager player slot (ngm+0x10+i*0x2c0, ten slots) the
  way PS3 did on "%s joined match" (`0x43dad8`, absent): `+0x20` used, `+0x120` the
  transport's peer handle. Clear it with `662a30` on leave. EstablishConnection
  `5f2ea0` waits for these.
- Host: nothing else. EstablishConnection calls AssignTeams and sends LoadLevel. The local
  Apply sets match+0xb54 -> LoadLevels -> WaitReady -> Playing::Enter -> StartNetGame.
- Client: the received LoadLevel (Apply `63b6b0`) sets +0xb54 -> `609d00` plays the task ->
  TaskManagerOnline Start -> GoLoadLevels.

### Step 7: results and back to the lobby (C, intact)

- NetEventEndGame -> ShowResults -> Leave::Enter `5f00d0`: host sends 0x105, state 3/17
  (custom) or 16/17 (matchmaking), pending pages.
- W `OnReturnToLobby()` after the menu is back:
  - host: `6093f0` must be the handler again, so C `5eb830` again or re-register it. Keep
    match +0x99=1/+0x9c=0 so it goes 3 -> 10 -> 16 at the next STARTMATCH. **Do not leave
    state 16 set while sitting in the lobby: 16 plays the task immediately.**
  - clients stay in 17 on `609d00`.
- POSTGAME party-up: host sends 0x118 with the room handle. Members get state 19 +
  PAGE_MULTIPLAYER_MATCHMAKING.

### Step 8: leave

- W `LeaveGame()`:
  - set match+0xc8 = 1 (LEAVING_GAME `5eb7d0` then ClearNetPlayers, pops and sets 18 or 19);
  - or post 0x103 with the room handle to the clients when the host closes the game;
  - SessionClear(match); release transport peers.
- W `LeaveMultiplayer()` (R `5eb180` LEAVE_MULTIPLAYER, which now only pops): this replaces
  the PS3 teardown `0x4386d0`:
  - SessionClear(party and match);
  - netInfo+0x2c4 = 0;
  - ngm+0x1ca0 = -1, C ClearNetPlayers;
  - `NetSetState(0)`;
  - close the transport;
  - C PopPage.

### Hooks summary

| address | action |
|---|---|
| `5eb160` BUTTON_MULTIPLAYER | R (step 1) |
| `659bb0`, `659bc0` roster queries | R (step 2) |
| `5eb780` STARTMATCH | R (step 4) |
| `659590` join list, `609680` SELECT_JOIN | R (step 5) |
| `5eb180` LEAVE_MULTIPLAYER | R (step 8) |
| `64e910` MATCHMAKING page | R only if real matchmaking is wanted |
| `5e9d30`/`5e9d40`, `63dc30`, `63db90`, `5eb830`, `5cb990`, `5cc6f0`, `62dc50`, `a572d0`+`a5be10` | C |
| sessions `0x1065b08`/`0x105efb8`, netInfo `0x104f1c8`, mailbox `0x14d0e18`, NGM slots | W (data the layer owns) |

Open points (not checked here): the exact event codes 0..6 dispatched by the menu manager
(`5cd5c0`), what `6144b0` sends (it reads the title ids "NPUA70051"/"BCET70015"), the layout of
NGM player slots beyond the fields above, and what `610840`/`60c2e0` send (transport side).
