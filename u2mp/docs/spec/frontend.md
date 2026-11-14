# Spec: frontend (frontend.cpp, frontend2.cpp, frontend3.cpp, frontend6.cpp)

This is the clean-room description of four files of `gow3_pc/src/u2mp/`. It says what the code must do,
as seen from outside: guest calls, session and online calls, game memory, log lines. It does not say how
the code is organised. Names of internal helpers are free.

Conventions used in this spec:

- Addresses are image offsets of the PS4 Uncharted 2 1.02 executable. "base" is the loaded image base.
  A guest pointer value is base + offset.
- "menu" means the menu manager object at offset `0x1136640`.
- "netInfo" means the object at `0x104f1c8`. "ngm" means the net game manager at `0x113b760`.
- "NET_SM" means the s32 at `0x14ce97c` (the network state machine state).
- All game memory is little-endian. Reads and writes are unaligned-safe.
- `s16`, `s32`, `u8`, `u32`, `u64` are sizes and signedness of the stored value.
- Every guest function is called with the System V x86-64 convention (`U2MP_GUEST`). Every function the
  guest calls (replaced functions, callbacks, stubs) must also use it.
- `log(...)` means the module's `u2mp::log` (prints `u2mp: ` + text + newline). Log texts are given as
  printf format strings in backticks. They must match exactly.
- "send(slot, bytes, except)" means `session::send_message(slot, body, except)` with `body` the listed
  bytes. `except` is -1 unless said otherwise.
- "the clock" means `session::net_ticks()` (300 ticks per second). All time arithmetic on it is 32-bit
  unsigned with wrap; "X has passed" tests are done as a signed 32-bit difference, as stated per item.
- "rand()" means the C runtime `std::rand()`. Where rand() is used, the number of calls and their order
  are part of the contract (a tester may seed it).

Shared mechanisms from `u2mp_internal.h` (not part of these files, used as they are):
`image.matches`, `is_ret_stub` (bytes `c3 66 66 66 66`), `replace_function(offset, 5, host, name)`
(writes `e9 rel32` to a 16-byte near stub `48 b8 imm64 ff e0`, logs `%s: 0x%llx -> C++` on success, or
its own failure lines), `near_stub(16)`, `show_page`, `addr::menu_manager`, `addr::show_page`, `sid`.

Install order (install.cpp, unchanged): `install_frontend` first; later `install_frontend2`, then
`install_frontend3`, then `install_frontend6`.

---

## 1. frontend.cpp

### 1.1 Purpose

Holds the guest page opener used by many files, and the frontend install entry point.

### 1.2 Interface

- `void u2mp::show_page(uint32_t page);` (declared in `u2mp_internal.h`)
- `void u2mp::install_frontend(int &installed);` (declared in `u2mp_internal.h`)

Callers of `show_page`: cinema.cpp, remaining.cpp, session.cpp, frontend3, frontend6.
Caller of `install_frontend`: install.cpp.

### 1.3 Game facts

| fact | value | meaning |
|---|---|---|
| ShowPage | `0x5cb990` | guest `ShowPage(menu*, u32 page id, u32 flags)` |
| menu manager | `0x1136640` | first argument of ShowPage |

### 1.4 Behaviour

- `show_page(page)`: one guest call `ShowPage(base + 0x1136640, page, 0)`. Nothing else. No log.
- `install_frontend(installed)`: calls `session::install(installed)` and nothing else. It installs nothing
  by itself and logs nothing by itself.

### 1.5 Tests

- show_page(0x12345678): exactly one call to `0x5cb990` with (menu, 0x12345678, 0).
- install_frontend: exactly one call to `session::install` with the same counter reference.

---

## 2. frontend2.cpp

### 2.1 Purpose

The PS4 callback BUTTON_MULTIPLAYER_SELECT_JOIN_GAME reads a join entry from an uninitialised stack
array and dereferences it. The PC joins by address, or online through `online.cpp`. The replacement hands
the row to the online layer, and otherwise only logs.

### 2.2 Interface

- `void u2mp::install_frontend2(int &installed);` (declared in `u2mp_internal.h`, called by install.cpp).
- Entry point the game reaches: the function at `0x609680` is replaced (5 bytes). The game calls it as a
  menu button callback with three pointer arguments `(arg, x, y)`; only `arg` is used.
- Uses: `bool online::select_join(int row)` (online.h).

### 2.3 Game facts

| fact | value | meaning |
|---|---|---|
| SELECT_JOIN_GAME | `0x609680` | button callback BUTTON_MULTIPLAYER_SELECT_JOIN_GAME |
| prologue check | at `0x609680`, 15 bytes: `55 48 89 e5 53 48 81 ec 58 06 00 00 48 8b 1d` | push rbp; mov rbp,rsp; push rbx; sub rsp,0x658; mov rbx,[rip+..] |
| array read check | at `0x609680 + 0x1a`, 19 bytes: `8b 47 04 48 8d 04 80 48 c1 e0 04 48 8b 84 05 f8 f9 ff ff` | reads row at arg+4, then the stack array |
| callback arg | `u32` at arg+4 | the selected row |

### 2.4 Behaviour

Replaced callback `(arg, _, _)`:

1. row = the u32 at arg+4 when arg is not NULL, else 0.
2. Call `online::select_join(int(row))`. If it returns true: done, no log.
3. Else log `join game list: row %u selected; the PC joins by address (UNCHARTED_U2MP_JOIN), nothing to join`
   with row (unsigned). No game memory is written.

### 2.5 Install

1. Check both byte runs (prologue, then array read). If either differs: log
   `BUTTON_MULTIPLAYER_SELECT_JOIN_GAME at 0x%llx: not the function this port expects, left alone`
   with 0x609680; nothing patched, counter unchanged.
2. Else `replace_function(0x609680, 5, callback, "BUTTON_MULTIPLAYER_SELECT_JOIN_GAME")`. On success
   `installed += 1`.

### 2.6 Tests

- arg NULL, online declines: select_join(0) called, log with row 0.
- arg with row 3, online declines: select_join(3), log with 3.
- online accepts: select_join called, no log.
- Install with the bytes intact / prologue changed / array read changed (one byte at 0x609680+0x1a+5).

---

## 3. frontend3.cpp: matchmaking, vote, leader back-out

### 3.1 Purpose

PS4 removed the matchmaking page handler (PAGE_MULTIPLAYER_MATCHMAKING is a bare `ret`), the vote
broadcast, and the party mailboxes. The page widgets (status line, vote rows, player rows) are intact
and read game memory. This file is the matchmaking state machine on the PC session layer: the party
leader starts it, the host gathers the players, runs a vote between two net games, sends the winner,
counts down and creates the match. Members follow by session messages. Optionally (online), the host
first finds or makes a room with strangers through `online.cpp`.

Hooks: replaced page handler `0x64e910`, replaced vote row callback `0x64e8e0`, replaced leader
back-out callbacks `0x5eba80` and `0x5eba30`. It calls START_MATCHMAKING `0x5eba40` (not replaced).
It is driven by session.cpp (messages, peer join, migration, Start Match) and frontend6 (popup done).

### 3.2 Interface

Declared in `frontend3.h`, namespace `u2mp::matchmaking`:

- `bool installed();` true only when the page handler was installed and START_MATCHMAKING passed its
  check (see 3.5).
- `void leader_start();` called by session.cpp `start_match` (mode 1) and by frontend6's done callback.
- `void on_message(uint8_t from, const uint8_t *data, size_t size);` called by session.cpp for every
  session message whose type byte is >= 0x10 (`session::kFrontendMessages`). `from` is the transport
  slot of the sender (a client only ever hears from slot 0, the host).
- `void on_peer_joined(uint8_t slot);` called by session.cpp on the host when a peer joins while online.
- `void on_migrated(bool host);` called by session.cpp `online_migrated` when matchmaking goes on under a
  new room host.

Declared in `u2mp_internal.h`: `void install_frontend3(int &installed);`.

Uses from other files: `session::is_host()`, `session::member_count()`, `session::net_ticks()`,
`session::send_message()`, `session::create_match()`; `online::mm_online()`, `mm_step()`, `mm_begin(u32)`,
`mm_reset()`, `mm_research()`, `mm_leave()` (constants `kMmIdle 0`, `kMmBusy 1`, `kMmHosting 2`,
`kMmJoined 3`, `kMmFailed 4`); `playlist_game_types(std::vector<int32_t>&)` (menus.cpp);
`dlc_net_game_available(int32_t)` (dlc.cpp; it makes its own guest lookups); `frontend6::preload_net_game`,
`frontend6::popup_up`, `frontend6::member_starting`, `frontend6::kStartingMessage` (0x17); `show_page`.

Entry points the game reaches (all replaced with `replace_function(addr, 5, ...)`):

| address | name used in logs | game calls it as | arguments |
|---|---|---|---|
| `0x64e910` | `PAGE_MULTIPLAYER_MATCHMAKING` | page event handler | (event record pointer) |
| `0x64e8e0` | `LIST_MATCHMAKING_VOTE` | list row callback | (arg pointer; u32 row at arg+4) |
| `0x5eba80` | `leader back (bbb649ef)` | menu callback | three pointers, unused |
| `0x5eba30` | `leader back (bf6ce4d5)` | menu callback | three pointers, unused |

One more entry point is created at run time: the "Leave Matchmaking" yes callback, registered in the
menu callback table (see 3.4.3). The game calls it with one pointer argument (unused).

### 3.3 Game facts

Memory:

| fact | value | meaning |
|---|---|---|
| NET_SM | s32 at `0x14ce97c` | network state |
| netInfo | `0x104f1c8` | net info block |
| netInfo+0x08 | u8 | "created by me": 1 on the host, 0 on a member (status line picks host text / tips) |
| netInfo+0x1c | u8 | error flag (host gone / kicked); nonzero = error |
| netInfo+0x20 | u32 | deadline in clock ticks (status line countdown) |
| netInfo+0x34 | s32 | phase: 0 none, 1 searching/gathering, 2 voting, 3 game starting |
| ngm | `0x113b760` | net game manager |
| ngm+0x1ca0 | s16 | current net game index (-1 none) |
| vote block | `0x113db50`, 0x18 bytes | read by the vote rows (cond 90, `0x65ae50`) and `0x64e8e0` |
| vote+0x00 | u8 | active |
| vote+0x01 | u8 | closed |
| vote+0x02..0x03 | 2 bytes | always 0 |
| vote+0x04 | s32 | my vote: -1 not voted, 0 or 1 |
| vote+0x08, +0x0c | s32 each | net game index of option 0 / 1 (stored sign-extended from s16) |
| vote+0x10, +0x14 | s32 each | vote count of option 0 / 1 |
| tip index | s32 at `0x113ddac` | which tip the status line shows |
| NetPlayerData | `0x11474f8` | |
| NetPlayerData+0x104 | u32 | chosen playlist id |
| menu+0x1010 | s32 | top page index |
| menu+0x9d0 | u64[...] | page stack (pointer per index) |
| page+0x10 | u32 | page id |
| menu+0x17d0 | u64 | message box descriptor pointer |
| menu+0x18ec | u32 | menu callback table count |
| menu+0x18f0 | entries of 0x18 bytes | {u32 id at +0, (+4..+7 untouched), u64 fn at +8, u64 arg at +0x10}; at most 128 used |
| page event record | +0x00 u32 event; +0x10 u8 "back held" | event 0 closed, 1 shown, 2 and 3 back, 6 update |

Message box descriptor (as the game's two-button box reads it):

| offset | value written |
|---|---|
| box+0x08 | u32 `0xcca090f2` (text "Leave Matchmaking?") |
| box+0x40 | u32 1 |
| box+0x44 | u32 `0x0260a263` (text "Yes") |
| box+0x48 | u32 `0x11d8e78a` |
| box+0x4c | u32 `0x075bc82d` (text "No") |
| box+0x50 | u32 `0x1519fa3d` |
| box+0x54 | u32 yes callback id = `sid("U2MP_MATCHMAKING_LEAVE")` = `0x7d1ecad7` |
| box+0x58 | u32 no callback id `0x2a2d147e` (registered nowhere; "no" does nothing) |

Guest functions called:

| address | signature | meaning |
|---|---|---|
| `0x5cc6f0` | (menu*) | PopPage |
| `0x63dc30` | (ngm*, s16 index, const void* = NULL, s32 = 10) | SetNetGame |
| `0xa15e70` | (u32 id) -> table* | DC lookup; table = {s32 count, s32 pad, u8* records} |
| `0x5eba40` | (p, p, p) | START_MATCHMAKING (menu reset, NET_SM 19, shows the matchmaking page) |
| `0x64e810` | () | publish player info (progression.cpp hooks it) |
| `0x5cb990` | via show_page | ShowPage |

Ids and tables:

| id | meaning |
|---|---|
| `0xe35f0f1a` | PAGE_MULTIPLAYER_MATCHMAKING (= sid of that name) |
| `0x719e9fec` | message box page |
| `0x3b9a067d` | DC table `*net-games*`: records of 0x90 bytes, game type s32 at record+0x0c |
| `0xb25ab071` | DC table of tips (its count is used) |

NET_SM values: 16 SERVER_LOBBY, 17 CLIENT_LOBBY, 18 CHOOSE_HOST_JOIN, 19 START_MATCHMAKING,
20 SERVER_VOTE_SCREEN, 21 CLIENT_VOTE_SCREEN.

Times (clock ticks, 300 per second):

| name | ticks | use |
|---|---|---|
| gather min (LAN) | 900 | 3 s |
| gather max (LAN) | 3000 | 10 s |
| vote | 4500 | 15 s |
| start countdown | 3000 | 10 s |
| create at | 600 | 2 s left |
| tip period | 2400 | 8 s |
| online gather | 6000 | 20 s |
| online gather max | 12000 | 40 s |
| research min | 13500 | 45 s |
| retry after failure | 900 | 3 s |

Session messages (type byte first; multi-byte fields little-endian, unaligned):

| type | name | body after the type | direction |
|---|---|---|---|
| 0x10 | start | none | leader -> all; host -> joining slot |
| 0x11 | ready | none | member -> host (slot 0) |
| 0x12 | vote options | s16 game a, s16 game b, u32 ticks left (9 bytes total) | host -> all / slot |
| 0x13 | vote | u8 choice (0 or 1) | voter -> host; host -> others |
| 0x14 | result | s16 game, u32 ticks left (7 bytes total) | host -> all / slot |
| 0x15 | leave | none | anyone |
| 0x16 | leader back | none | host -> all |
| 0x17 | starting | none (frontend6) | host -> all |

### 3.4 Behaviour

#### 3.4.1 Machine state (host memory, not game memory)

The file keeps, per process: running, leave requested, ready sent, created, gather start time,
ready-slot bit set (32 bits), next tip time, last logged NET_SM (initially -1), last seen phase,
online room flag, research time, retry time. "Reset the machine" means: all of these to false/0, except
the last logged NET_SM to -1. The machine starts in the reset state. Every reset also forgets the last
logged NET_SM, so the `NET_SM` log line (3.4.6) prints again on the next update.

Common operations (named here only to describe them once):

- **top page**: read s32 at menu+0x1010. If < 0 or > 0x40, the top page is 0. Else read the u64 at
  menu+0x9d0 + 8*top; NULL -> 0; else the u32 at pointer+0x10.
- **pop**: guest PopPage(base+0x1136640).
- **ticks left**: (s32)(u32 netInfo+0x20 - clock); if > 0 that value, else 0.
- **set deadline(t)**: u32 netInfo+0x20 = clock + t.
- **clear vote block**: write 0x18 zero bytes at `0x113db50`.
- **set net game(i)**: guest SetNetGame(base+0x113b760, i, NULL, 10).
- **ensure page**: if the top page is not `0xe35f0f1a`, guest START_MATCHMAKING(NULL, NULL, NULL).
- **begin(host)**: reset the machine; then running = true, gather start = clock, ready bits = 1 (bit 0),
  next tip = clock + 2400; clear vote block; netInfo+0x34 = 1; u8 netInfo+0x08 = 1 if host else 0;
  last seen phase = 1; log `matchmaking: started (%s)` with "host" or "member".
- **open vote(a, b, t)**: vote block = active 1, closed 0, pad 0, mine -1, game0 a, game1 b, counts 0;
  netInfo+0x34 = 2; set deadline(t); then `frontend6::preload_net_game(true, true, true)`.
- **apply result(g, t)**: vote+0x01 = 1; set net game(g); netInfo+0x34 = 3; set deadline(t); then
  `frontend6::preload_net_game(true, true, false)`. Other vote fields unchanged.
- **reply target**: slot -1 on the host, slot 0 on a member (`session::is_host()` decides).

#### 3.4.2 Vote pick, vote start, vote end (host)

**Pick two games.**
1. Guest DC lookup(`0x3b9a067d`). Fail when the table pointer is NULL, its records pointer is NULL, or
   its count <= 0. On fail nothing else in this step is called.
2. Call `playlist_game_types(types)`. If it returns false, every game type is accepted.
3. For each record index i from 0 up to count-1: the record's game type is the s32 at
   records + 0x90*i + 0x0c. If the type is accepted (any, or equal to one of the playlist types), call
   `dlc_net_game_available(i)`; if true, i joins the pool. Records whose type is not accepted do not
   call `dlc_net_game_available`.
4. Empty pool: fail.
5. a = pool[rand() % pool size]. If the pool has more than one entry, draw b = pool[rand() % size]
   repeatedly until b != a. With one entry, b = a and rand() is called once only.

**Start the vote.**
1. Pick two games. On fail: a = b = the s16 at ngm+0x1ca0 if it is >= 0, else 0; log
   `matchmaking: no game of the playlist found, voting on net game %d` with a.
2. open vote(a, b, 4500) (includes the preload).
3. NET_SM = 20.
4. send(-1, [0x12, a s16, b s16, 4500 u32]).
5. log `matchmaking: vote between net games %d and %d (%u players)` with a, b,
   `session::member_count()` as unsigned.

**End the vote.**
1. Winner = option 0 if count0 > count1; option 1 if count1 > count0; on a tie rand() % 2 (rand() is
   only called on a tie).
2. g = the winner's game (low 16 bits, signed).
3. apply result(g, 3000) (includes SetNetGame and the preload).
4. send(-1, [0x14, g s16, 3000 u32]).
5. log `matchmaking: votes %d / %d, net game %d wins` with count0, count1, g.

#### 3.4.3 Leave

**Leave(tell)**, in this order:
1. If tell: send(reply target, [0x15]).
2. log `matchmaking: leaving%s` with "" when tell, else " (the party left)".
3. `online::mm_leave()`.
4. Clear vote block; s32 netInfo+0x34 = 0; u32 netInfo+0x20 = 0; NET_SM = 18.
5. If the top page is `0x719e9fec`, pop. Then (re-read) if the top page is `0xe35f0f1a`, pop.
6. Reset the machine.

**Register the yes callback** (idempotent):
1. count = u32 at menu+0x18ec. Look at entries 0 .. min(count,128)-1 (entry i at menu+0x18f0 + 0x18*i).
   If any entry's u32 id is `0x7d1ecad7`: done.
2. If count >= 128: log `matchmaking: menu callback table full, Leave Matchmaking answers are not seen`
   and stop.
3. Else at entry `count`: u32 id = `0x7d1ecad7`, u64 at +8 = the address of the yes callback (a guest
   ABI host function), u64 at +0x10 = 0. Bytes +4..+7 are not written. Then count += 1.

**The yes callback** (one pointer argument, ignored): sets "leave requested". Nothing else.

**Ask "Leave Matchmaking?"**:
1. Register the yes callback.
2. Read the u64 at menu+0x17d0. If NULL: set "leave requested" and stop (no box, no page).
3. Else write the descriptor fields of the table in 3.3 (box+0x08 first, then box+0x40..+0x5b), then
   show_page(`0x719e9fec`).

#### 3.4.4 Page handler `0x64e910` (event record pointer `ev`)

NULL `ev`: nothing. Else switch on the u32 at ev+0:

- **0 (closed)**: do nothing when "created" is set, or s32 netInfo+0x34 == 3, or the top page is
  `0x719e9fec`. Otherwise: reset the machine; clear vote block; set net game(-1); guest `0x64e810()`.
- **1 (shown)**: register the yes callback.
- **2 or 3 (back)**: read NET_SM first. If "leave requested" is not set: ask "Leave Matchmaking?",
  then u8 ev+0x10 = 1 (even if the ask set "leave requested" because the box pointer was NULL).
  If it is set: u8 ev+0x10 = 0 when NET_SM is 19 or 18, else 1.
- **6 (update)**: see 3.4.6.
- anything else: nothing.

#### 3.4.5 Vote row callback `0x64e8e0` (arg pointer)

1. NULL arg: nothing.
2. Nothing when vote+0x00 is 0, or vote+0x01 is nonzero, or vote+0x04 is 0 or 1.
3. choice = 1 if the u32 at arg+4 is nonzero, else 0.
4. vote+0x04 = choice; count[choice] += 1.
5. send(reply target, [0x13, choice]).
6. log `matchmaking: voted for net game %d` with the s32 game[choice].

#### 3.4.6 Update (page event 6)

In this order:

1. Read NET_SM (st) and s32 netInfo+0x34 (phase), once, at the start.
2. If st differs from the last logged NET_SM: log `matchmaking: NET_SM %d, phase %d` with st, phase;
   remember st.
3. Next round: if running, last seen phase is 3, and phase is 1: log
   `matchmaking: back from the match, next round`; reset the machine.
4. Last seen phase = phase.
5. If "created": stop.
6. If u8 netInfo+0x1c is nonzero: leave(false); stop.
7. If "leave requested": leave(true); stop.
8. host = `session::is_host()`. If not running: begin(host).
9. **Member** (not host):
   - If "ready sent" is false and st is 19 or 17: send(0, [0x11]); ready sent = true.
   - If s32 netInfo+0x34 (read now, after a possible begin) is 1: tip step: if (s32)(clock - next tip)
     >= 0: next tip = clock + 2400; guest DC lookup(`0xb25ab071`); if the table is not NULL and its
     count > 0: s32 at `0x113ddac` = (that value + 1) % count.
   - stop.
10. **Host**, by st:
    - **19**: if `online::mm_online()`:
      - call `online::mm_step()`:
        - Idle (0): if (s32)(clock - retry time) >= 0: playlist = u32 NetPlayerData+0x104; log
          `matchmaking: looking for players online (playlist 0x%x)` with it; `online::mm_begin(playlist)`.
          (Retry time is not changed here; this repeats every update while Idle.)
        - Hosting (2): online room = true; gather start = clock; research time =
          clock + 13500 + (rand() % 15) * 300; NET_SM = 16.
        - Joined (3): nothing.
        - Failed (4): `online::mm_reset()`; retry time = clock + 900.
        - other (Busy and unknown values): nothing.
      - Not online: NET_SM = 16.
    - **16** (gather): ready = number of set bits in the ready bits; waited = clock - gather start (u32).
      - Online room set: players = `session::member_count()`; minimum = environment variable
        `UNCHARTED_U2MP_MM_MIN` read with atoi, used when 1..10, else 2; missing variable = 2. The
        variable is read once per process, the first time this branch runs, and the value is kept.
        Start the vote when players >= 10, or when players >= minimum and (waited >= 6000 and
        ready >= players, or waited >= 12000). Otherwise, if players < minimum and
        (s32)(clock - research time) >= 0: log
        `matchmaking: %d player(s) after %u s: looking for another room` with players and waited/300
        (integer division); `online::mm_research()`; online room = false; NET_SM = 19.
      - No online room: start the vote when waited >= 3000, or waited >= 900 and
        ready >= `session::member_count()`. (member_count is only queried once waited >= 900 and
        < 3000.)
    - **20**: if s32 netInfo+0x34 is 2 and ticks left is 0: end the vote. Else if it is 3 and ticks left
      <= 600: created = true; log `matchmaking: starting net game %d` with the s16 at ngm+0x1ca0;
      `session::create_match()`; then s32 netInfo+0x34 = 3 (written after create_match, which may clear
      it).
    - any other st (17, 18, 21, ...): nothing.

#### 3.4.7 Leader back-out callbacks `0x5eba80`, `0x5eba30`

Both identical: if `session::is_host()`, send(-1, [0x16]). Then pop (always).

#### 3.4.8 `matchmaking::installed()`

Returns the installed flag (3.5).

#### 3.4.9 `matchmaking::leader_start()`

1. If not `session::is_host()`: nothing.
2. log `matchmaking: the leader starts matchmaking (%d players)` with `session::member_count()`.
3. send(-1, [0x10]).
4. Reset the machine.
5. Guest START_MATCHMAKING(NULL, NULL, NULL).

It does not check `installed()`.

#### 3.4.10 `matchmaking::on_peer_joined(slot)`

1. Nothing unless running, `session::is_host()`, and the top page is `0xe35f0f1a`.
2. send(slot, [0x10]).
3. phase = s32 netInfo+0x34. If phase is 2 and vote+0x00 is nonzero: send(slot, [0x12, game0 as s16,
   game1 as s16, ticks left u32]). Else if phase is 3: send(slot, [0x14, s16 ngm+0x1ca0, ticks left u32]).
4. log `matchmaking: slot %u joined the room (phase %d)` with slot, phase.

#### 3.4.11 `matchmaking::on_migrated(host)`

1. log `matchmaking: the room has a new host (%s)` with "this PC" or "another player".
2. Reset the machine; clear vote block; s32 netInfo+0x34 = 1.
3. `online::mm_reset()`.
4. NET_SM = 19 if host, else 17.

#### 3.4.12 `matchmaking::on_message(from, data, size)`

size 0: nothing. Else host = `session::is_host()` (queried for every non-empty message), then by
data[0]:

- **0x10**: member only: reset the machine; ensure page.
- **0x11**: host only, and from <= 31: set bit `from` in the ready bits.
- **0x12**: member only and size >= 9: ensure page; if not running, begin(false); open vote(s16 at +1,
  s16 at +3, u32 at +5); NET_SM = 21; log `matchmaking: vote between net games %d and %d` with the two
  s16 values.
- **0x13**: only if size >= 2 and data[1] <= 1: if vote+0x00 is nonzero and vote+0x01 is 0, count[data[1]]
  += 1. Then, on the host (whether counted or not), send(-1, [0x13, data[1]], except = from).
- **0x14**: member only and size >= 7: apply result(s16 at +1, u32 at +3); log
  `matchmaking: net game %d chosen` with the s16.
- **0x15**: ignore when not running and the top page is not `0xe35f0f1a`. Host with online room set:
  log `matchmaking: a player left matchmaking`, nothing else. Host otherwise: leave(true) (so the host
  sends 0x15 to all). Member: leave(false) only when from == 0.
- **0x16**: member only, from == 0, and (`frontend6::popup_up()` or the top page is `0x719e9fec`): pop.
- **0x17**: member only and from == 0: `frontend6::member_starting()`.
- other types: nothing.

### 3.5 Install

`install_frontend3(installed)`, in this order:

1. **Page handler**: if `0x64e910` is not a ret stub (`c3 66 66 66 66`): log
   `PAGE_MULTIPLAYER_MATCHMAKING at 0x%llx: not the stripped handler this port expects, left alone`
   with 0x64e910. Else `replace_function(0x64e910, 5, handler, "PAGE_MULTIPLAYER_MATCHMAKING")`; on
   success installed += 1 and the installed flag = true.
2. **Vote callback**: bytes at `0x64e8e0` must be `83 7f 04 00 0f 95 c0 0f b6 c0 48 8d 0d` (13 bytes:
   cmp dword [rdi+4],0; setne al; movzx eax,al; lea rcx,[rip+d]) and `0x64e8e0 + 17 + d` (d = s32 at
   0x64e8ed) must be `0x113db50`. If not: log
   `LIST_MATCHMAKING_VOTE at 0x%llx: not the function this port expects, left alone` with 0x64e8e0.
   Else `replace_function(0x64e8e0, 5, vote callback, "LIST_MATCHMAKING_VOTE")`; success: installed += 1.
3. **Leader back-out**, first `0x5eba80` named `leader back (bbb649ef)`, then `0x5eba30` named
   `leader back (bf6ce4d5)`. Check: bytes `48 8d 3d` at +0, byte `e9` at +7, (addr + 7 + s32 at +3) ==
   `0x1136640`, (addr + 12 + s32 at +8) == `0x5cc6f0` (lea rdi,[menu]; jmp PopPage). Fail: log
   `%s at 0x%llx: not the function this port expects, left alone` with name and address. Else
   `replace_function(addr, 5, back callback, name)`; success: installed += 1.
4. **START_MATCHMAKING check** (not patched): bytes at `0x5eba40` = `55 48 89 e5 53 50 48 8d 1d`, u32 at
   `0x5eba40 + 0x1b` == 0x13, u32 at `0x5eba40 + 0x20` == `0xe35f0f1a`. If any fails: log
   `START_MATCHMAKING at 0x%llx differs: matchmaking off (Start Match plays the party's game)` with
   0x5eba40, and the installed flag = false. The counter is not decreased and the page handler stays
   patched.

### 3.6 Tests

Fake image with NET_SM, netInfo, ngm, vote block, menu fields, tip table, net-games table; stubs for the
guest functions that record calls; a fake session (host or member, member count, clock) and online layer.

1. Install: all intact (counter +4, flag true); page not a ret stub; vote lea pointing elsewhere; each
   back-out callback with wrong lea target / wrong jmp target / missing e9; START_MATCHMAKING with wrong
   prologue, wrong 0x13, wrong page id (flag false, counter still counts the page).
2. leader_start as member (nothing) and as host (log, send 0x10, START_MATCHMAKING call).
3. Host LAN: event 6 with NET_SM 19 -> NET_SM 16, "started (host)" and NET_SM logs. Gather: clock at
   +899 (no vote), +900 with ready count below member count (no vote), +900 with all ready (vote),
   +3000 (vote). Ready messages from slots 1, 2, 31, 32 (32 ignored), from a member (ignored by member).
4. Vote pick: net-games table missing / records NULL / count 0 (fallback log, ngm game -1 -> 0 and 5);
   playlist false (all types); playlist with one type; pool of one (one rand call); pool of many;
   dlc false for some.
5. Vote: callback with arg NULL, inactive block, closed, mine already 0/1, row 0, row 7 (choice 1); as
   host (send -1) and member (send 0).
6. Vote end: count 2/1, 1/2, tie (rand). Deadline: ticks left 1 (nothing), 0 (end). Countdown: phase 3
   with ticks left 601 (nothing), 600 (create, phase 3 rewritten after create_match clears it), then
   every later update stops at "created".
7. Member: kMmStart with matchmaking page already on top (no START call) and not on top; update in
   NET_SM 19 and 17 (one ready message), 18 (none); tip cycling at +2399 / +2400; tip table NULL /
   count 0 / count 5 with index 4 -> 0.
8. Member messages: 0x12 with size 8 (ignored) and 9; 0x13 size 1, choice 2 (ignored), vote closed;
   0x14 size 6 and 7; 0x16 from slot 0 with popup up / message box on top / neither, and from slot 1;
   0x17 from 0 and from 1; unknown type 0x18; size 0.
9. Host messages: 0x13 relay with except = from, even when the vote is inactive; 0x15 with online room
   (log only), without (leave(true)), when not running and page not on top (ignored).
10. Leave: back event with "leave requested" unset (box shown, ev+0x10 = 1, callback registered once),
    box pointer NULL (flag set, no page, ev+0x10 = 1), callback table full (log), callback already
    registered; yes callback then update (leave(true): send, log, mm_leave, memory, NET_SM 18, pops of
    message box then page); back event after yes in NET_SM 18/19 (0) and 20 (1). Error flag set ->
    leave(false).
11. Page closed event: with created; with phase 3; with message box on top; otherwise (SetNetGame -1,
    `0x64e810` call, vote block cleared).
12. Next round: phase 3 seen, then phase 1 while running -> log and reset, NET_SM log again next frame.
13. Online host: mm_online true; Idle with retry time passed (log + mm_begin) and not passed; Hosting
    (rand, NET_SM 16); Joined; Failed (mm_reset, retry +900); Busy. Online gather with
    `UNCHARTED_U2MP_MM_MIN` unset, "3", "0", "11", "abc"; players 10; research path (log, mm_research,
    NET_SM 19).
14. on_peer_joined: not running / not host / page not on top (nothing); phase 1 (start only); phase 2
    active (options with ticks left); phase 2 inactive; phase 3 (result).
15. on_migrated true / false.
16. Leader back-out as host (send 0x16 + pop) and member (pop only).

---

## 4. frontend6.cpp: "Starting in" popup and vote level preload

### 4.1 Purpose

1. PS4 kept the timed popup page `0x453470dd` ("Starting in [A]...", 5 s) and its per-frame update,
   but lost the function that opens it and its callers. This file opens it on the party leader at
   Start Match and on the members when the leader tells them. When the popup runs out, the game calls
   the done callback id through its menu callback registry; the back control calls the cancel id.
2. PS4's registration of two done callbacks points at code unfit for the PC (`0x5eb830` for
   `e2707dbe`, plain START_MATCHMAKING `0x5eba40` for `4055f45f`). This file retargets those two
   registrations at its own callbacks.
3. It wraps the net game level preload `0x641690` for frontend3's vote.

### 4.2 Interface

Declared in `frontend6.h`, namespace `u2mp::frontend6`:

- `constexpr uint8_t kStartingMessage = 0x17;`
- `bool installed();` true when the popup and both done callbacks are installed.
- `bool leader_starting(bool matchmaking);` called by session.cpp `start_match`. False means "not shown,
  start right away".
- `void member_starting();` called by frontend3 on message 0x17 from slot 0.
- `bool popup_up();` called by frontend3.
- `void preload_net_game(bool request, bool keep_menu, bool flags_only);` called by frontend3.

Declared in `frontend6.h`: `void install_frontend6(int &installed);`.

Entry points the game reaches:

| site | original | becomes | game calls it as |
|---|---|---|---|
| `0x5ef31a` | `lea rcx,[rip+d]` loading `0x5eb830` (registration of `e2707dbe`) | loads a near stub that jumps to the create-game callback | registry callback, one pointer argument ({u32 id, u32, u64 arg}) |
| `0x5ef41e` | `lea rcx,[rip+d]` loading `0x5eba40` (registration of `4055f45f`) | loads a near stub that jumps to the start-matchmaking callback | same |

Retargeting a registration: the 7-byte instruction keeps its `48 8d 0d` and gets a new disp32 at site+3
such that site + 7 + disp32 is a fresh 16-byte near stub (`near_stub(16)`) holding `48 b8 <u64 host
function address> ff e0`.

### 4.3 Game facts

| fact | value | meaning |
|---|---|---|
| menu+0x18d4 | u32 | done callback id of the timed popup |
| menu+0x18d8 | u32 | cancel callback id (back control) |
| menu+0x1880 | u64 | timed popup descriptor pointer |
| desc+0x08 | u32 | text id |
| menu+0x370 | u64[...] | page objects |
| menu+0x9b0 | u32 | page object count (at most 200 looked at) |
| page+0x10 | u32 | page id |
| page+0xf1 | u8 | back disabled (1) |
| menu+0x9b8 | u64 | overlay page pointer |
| menu+0x24f0 | u32 | page list count (at most 32) |
| menu+0x24f8 | entries of 0x18 bytes | {u32 page id at +0, u64 fn at +8, u64 arg at +0x10} |
| `0x5ae8a0` | guest (desc*, s32 seconds) | sets the popup timer (desc+0x40 now, +0x44 deadline) |
| `0x5eba40` | guest (p, p, p) | START_MATCHMAKING |
| `0x641690` | guest (ngm*, s32 request, s32 keep_menu, s32 flags_only) | level preload |
| `0x5eb830` | | PS4 create-game path (original target of `e2707dbe`) |
| ngm | `0x113b760`; s16 at +0x1ca0 = current net game | |
| `0x453470dd` | page id | timed popup page |
| `0x73a73dd8` | text id | "Starting in [A]..." |
| 5 | seconds | popup time |
| `0x4055f45f` / `0xbbb649ef` | ids | matchmaking done / cancel |
| `0xe2707dbe` / `0xbf6ce4d5` | ids | custom game done / cancel |
| `0xee8f5f9b` | id | member done (does nothing in the game) |

Install byte checks, in this order (offset: bytes):

| # | offset | bytes | meaning |
|---|---|---|---|
| 1 | `0x5c7981` | `be dd 70 34 45` | popup page built |
| 2 | `0x5cfb54` | `49 8b 86 b8 09 00 00` | overlay page read |
| 3 | `0x5cfb64` | `81 78 10 dd 70 34 45` | overlay is the popup |
| 4 | `0x5cfb71` | `49 8b 86 80 18 00 00` | descriptor menu+0x1880 |
| 5 | `0x5cfb85` | `3b 48 44` | deadline desc+0x44 |
| 6 | `0x5cfbed` | `41 8b b6 d4 18 00 00` | done id menu+0x18d4 |
| 7 | `0x5caf6c` | `41 8b b5 d8 18 00 00` | cancel id menu+0x18d8 |
| 8 | `0x5a3416` | `80 bb f1 00 00 00 00` | page+0xf1 |
| 9 | `0x5c9068` | `41 8b 85 f0 24 00 00` | page list count |
| 10 | `0x5c90ed` | `49 8d 94 c5 f8 24 00 00` | page list entries |
| 11 | `0x5cb8d4` | `41 8b 85 ec 18 00 00` | callback registry dispatch |
| 12 | `0x5ae8a0` | `48 8d 05 d9 78 b6 00 8b 80 04 01 00 00 89 47 40` | timer setter |
| 13 | `0x5eba40` | `55 48 89 e5 53 50 48 8d 1d` | START_MATCHMAKING |
| 14 | `0x5ef30f` | `c7 84 c3 f0 18 00 00 be 7d 70 e2` | `e2707dbe` registered |
| 15 | `0x5ef413` | `c7 84 c3 f0 18 00 00 5f f4 55 40` | `4055f45f` registered |

Preload checks: `0x641690`: `55 48 89 e5 41 57 41 56 41 55 41 54 53`; `0x6416ba`: `bf 7d 06 9a 3b`;
`0x6418bc`: `45 88 a7 9d 20 00 00`.

### 4.4 Behaviour

**Open the timed popup (text, seconds, done, cancel, fn, arg)** (shared by leader and member):
1. desc = u64 at menu+0x1880. If NULL: log
   `starting popup: no popup descriptor yet (menu+0x1880), not shown`; result false; nothing written.
2. u32 menu+0x18d4 = done; u32 menu+0x18d8 = cancel.
3. Page objects: for i from 0 while i < u32 menu+0x9b0 and i < 200: pointer = u64 at menu+0x370 + 8*i;
   stop at the first NULL; for every page whose u32 at +0x10 is `0x453470dd`, u8 at +0xf1 = 1 if
   cancel is 0, else 0.
4. u32 desc+0x08 = text.
5. Guest `0x5ae8a0(desc, seconds)`.
6. Page list: count = u32 menu+0x24f0. Among entries 0 .. min(count,32)-1, the last one whose u32 page
   id is `0x453470dd` is chosen. If none and count < 32, entry `count` is chosen and count += 1. If an
   entry is chosen: u32 +0 = `0x453470dd`, u64 +8 = fn, u64 +0x10 = arg. If none: log
   `starting popup: page list full (%u entries)` with count (unchanged).
7. show_page(`0x453470dd`).
8. Result true.

All callers here pass fn = 0, arg = 0.

**`leader_starting(matchmaking)`**:
1. False when not installed, or `session::is_host()` is false (is_host is not queried when not installed).
2. Open the timed popup with text `0x73a73dd8`, 5 s, done `0x4055f45f` / cancel `0xbbb649ef` when
   matchmaking, else done `0xe2707dbe` / cancel `0xbf6ce4d5`. If it returns false: result false.
3. send(-1, [0x17]).
4. log `starting popup: the leader starts a %s (%d s)` with "matchmaking search" or "custom game", 5.
5. Result true.

**`member_starting()`**: nothing when not installed or `session::is_host()` is true. Else open the timed
popup with text `0x73a73dd8`, 5 s, done `0xee8f5f9b`, cancel 0. If it returned true, log
`starting popup: the leader starts the match`.

**`popup_up()`**: u64 at menu+0x9b8 is not NULL and the u32 at that pointer +0x10 is `0x453470dd`.

**`installed()`**: the popup flag.

**`preload_net_game(request, keep_menu, flags_only)`**: nothing when the preload flag is false. Else log
`level preload (PS3 0x4a12e8): request %d, keep menu %d, flags only %d, net game %d` with the three
values as 0/1 and the s16 at ngm+0x1ca0; then guest `0x641690(base+0x113b760, request, keep_menu,
flags_only)` with 0/1 ints.

**Create-game callback** (registry id `e2707dbe`; one pointer argument, ignored): log
`starting popup: done, creating the match`; `session::create_match()` (it checks host itself).

**Start-matchmaking callback** (registry id `4055f45f`; one pointer argument `call`): if
`session::is_host()` and `matchmaking::installed()` (only queried on the host): log
`starting popup: done, starting matchmaking`; `matchmaking::leader_start()`. Otherwise guest
START_MATCHMAKING(call, NULL, NULL), passing the same pointer through.

### 4.5 Install

`install_frontend6(installed)`:
1. Run the 15 byte checks in table order. At the first mismatch: log
   `starting popup: bytes at 0x%llx differ, popup off (Start Match starts right away)` with that offset,
   and skip the remaining checks and step 2.
2. Retarget `0x5ef31a` (must be `48 8d 0d` with site+7+disp == `0x5eb830`) to the create-game callback,
   name `done callback e2707dbe`. Only if that succeeded, retarget `0x5ef41e` (target must be
   `0x5eba40`) to the start-matchmaking callback, name `done callback 4055f45f`. Per retarget:
   - wrong bytes or target: log `%s at 0x%llx: not the registration this port expects, left alone` with
     name and site; fail.
   - no stub memory, or the stub out of rel32 reach: fail without a log (a stub may have been used up).
   - success: write the new disp32; log `%s: registration at 0x%llx -> C++` with name and site.
   If both succeed: popup flag = true, installed += 2. If only the first succeeded, it stays patched,
   the flag stays false, nothing is counted.
3. Independently of steps 1-2: preload flag = all three preload checks match. If not: log
   `level preload 0x641690: not the function this port expects, matchmaking does not preload`. The
   preload adds nothing to `installed`.

### 4.6 Tests

1. Install all intact (+2, both log lines, preload on). Break each of the 15 checks in turn (one log
   naming that offset, no retarget, +0). Break the first lea (target elsewhere): log, second not tried.
   Break the second only: first patched, +0, flag false. Break each preload check.
2. leader_starting: not installed (false, no session call); member (false); host with desc NULL (log,
   false, no send); host matchmaking and custom game (ids, page+0xf1 = 0, timer call with 5, list entry,
   ShowPage, send 0x17, log, true).
3. Page objects: count 0, count with a NULL early, two popup pages, count 300 (only 200 looked at).
4. Page list: existing popup entry (twice: last wins, count unchanged), new entry, count 32 (log
   "page list full (32 entries)"), count 40 (log with 40).
5. member_starting: not installed, host, member (page+0xf1 = 1, done `ee8f5f9b`, cancel 0, log), member
   with desc NULL (no second log).
6. popup_up: overlay NULL, other page, popup page.
7. preload: flag off (no log, no call); on with each argument mix, ngm game -1.
8. Create-game callback: log + create_match. Start-matchmaking callback: host and matchmaking installed
   (log + leader_start); host, not installed; member (START_MATCHMAKING with the same pointer, no log).

---

## 5. Points to keep exactly (odd but required)

- Every machine reset (begin, kMmStart on a member, next round, leave, page closed, migration,
  leader_start) forgets the last logged NET_SM, so the `NET_SM %d, phase %d` line repeats on the next
  update even when NET_SM did not change.
- The ready-bit set starts with bit 0 set, and a ready message from slot 0 sets the same bit.
- The host counts its own vote locally and also counts relayed votes from members; a vote message is
  relayed by the host even when the vote block is not active.
- A member's leave message to the host makes the host leave too and tell everyone (LAN). Online with a
  room, the host only logs.
- In the online Idle step the retry time is never advanced, so `mm_begin` and its log line repeat on
  every update while the online layer stays Idle.
- After a Hosting step sets NET_SM 16, the research time uses one rand() call.
- The back event always holds the back (ev+0x10 = 1) on the first press, even when the box could not be
  shown and the leave was requested directly.
- START_MATCHMAKING failing its check turns `matchmaking::installed()` off but leaves the page handler
  patched and counted.
- The "Leave Matchmaking" yes callback is a host function address written into game memory; its value
  differs between builds.
