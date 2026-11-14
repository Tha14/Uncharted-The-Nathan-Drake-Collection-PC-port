# Spec: progression (progression.cpp, netinit.cpp, netpatch.cpp)

Clean-room behaviour description of three files in `gow3_pc/src/u2mp/`. It states what the code must do, as seen
from the game image, the host calls it makes, the files it touches, the log and the install counter. It does not
prescribe code structure, helper names or loop shapes. All addresses are image offsets of Uncharted 2 PS4 1.02
(`image.base + offset`). "u32 at X" means a 4-byte little-endian read or write at image offset X; "pointer at X"
means an 8-byte little-endian value that holds an absolute host address. Reads may be unaligned. Signed types are
"s16", "s32", "s64".

`progression_store.cpp` is not in scope. It stays as it is; the new `progression.cpp` calls it through
`progression_store.h` (namespace `u2mp::progression`), described in 1.2.3.

Shared facilities from `u2mp_internal.h` (already provided by the project, unchanged):

- `image` (`u2mp::Image`): `image.base` (NULL before `u2mp_install`), `image.at<T>(offset)`,
  `image.matches(offset, bytes, length)` (true only when the range lies inside the image and the bytes are equal),
  `image.executable(offset, length)` (the range lies inside one executable segment).
- `replace_function(offset, length, host, name)`: writes a jump at `offset` to a U2MP_GUEST host function.
  Returns true on success. It logs on its own; the caller adds nothing on success.
- `retarget_call(offset, expected_target, host, name)`: the `call rel32` at `offset` (which must call
  `expected_target`) is pointed at `host`. Returns true on success.
- `log(format, ...)`: one line, printf style. The project prefixes `u2mp: `; the strings below are the format
  strings passed to `log`, without that prefix. Every log line listed here must be produced with exactly that
  format string and those arguments (so the formatted text is identical, including `%#llx` printing `0` for zero).
- `U2MP_GUEST`: System V x86-64 calling convention (arguments in rdi, rsi, rdx, ...; result in rax). Every guest
  function called and every function the guest calls uses it.
- `netheap_alloc(size, align)` (netheap.cpp): host allocation from the network heap; NULL on failure.

"Guest call F(a, ...)" means calling the game function at `image.base + F` with U2MP_GUEST. "Host call" means a
call to another module of the runtime (online, unlocks, store, netheap, C runtime). The order of all guest and host
calls listed below is part of the required behaviour.

---

## Part 1: progression.cpp

### 1.1 Purpose and hooks

The PS4 1.02 build keeps the multiplayer progression block (NetPlayerData Data, 0x228 bytes, version 10) in RAM
only. Nothing loads it and nothing saves it, and its state word never reaches "connected" (3). The leaderboard
list of PAGE_MENU_LEADERBOARD no longer fetches any rows. This file:

1. Connects NetPlayerData: loads the Data block from a local store file and sets state 3, from the function at
   `0x64e810` (a remnant whose body is only the "offline -> clear" guard; called by PAGE_MULTIPLAYER, PAGE_LOBBY,
   post-game and match start). The whole function is replaced.
2. Publishes: writes the Data block to the store file when it changed (from `0x64e810` and from
   `progression_publish()`), and always on Leave::Enter (the call at `0x5f0952`, which follows the stat updates,
   is retargeted). On Leave it also records the players of the match just left into the store and, when the
   online link is up, posts online scores.
3. Leaderboards: the leaderboard list update at `0x5b3000` is replaced whole. It fills the game's leaderboard
   object with rows from the online score service when linked, else from the local store.

### 1.2 Interface

#### 1.2.1 Declarations other files use (`progression.h`, unchanged)

```
namespace u2mp {
void install_progression(int &installed);
void progression_publish();
std::vector<std::string> progression_recent_players(size_t max);
std::string progression_player_summary(const std::string &name);
bool edit_clan_tag();      // defined in clantag.cpp, not here
void clan_tag_frame();     // defined in clantag.cpp, not here
}
```

Callers: `install.cpp` calls `install_progression(installed)` once (after `install_natives`, before
`install_cinema`). `session.cpp` calls `progression_publish()` when Multiplayer is left; `clantag.cpp` calls it after
the clan tag changed. `menus.cpp` (LIST_PLAYERSMET) and `remaining.cpp` call `progression_recent_players(50)`;
`remaining.cpp` (Players Met: View Profile) calls `progression_player_summary(name)`.

`gow3_pc/tests/test_u2mp_progression.cpp` tests only `progression_store.cpp`; it does not link progression.cpp.

#### 1.2.2 Host functions this file calls

- `extern "C" const char *runtime_file_user_dir(void);` (C runtime): the per-user data directory, may be NULL or "".
- `u2mp::unlocks_refresh()` (unlocks.h).
- `u2mp::online::member_data_changed()`, `linked()`, `record_score(board, score, comment, info, info_len)`,
  `board_request(board, mode, start, count)`, `board_state()`, `board_rows()`, `board_total()`, `board_row(i)`,
  `my_npid_text()` (online.h). `board_row` returns `const U2oScore *` (u2_online_abi.h): `npid` char[20] at +0,
  `name` char[52], `rank` u32 (1-based), `pc_id` s32, `score` s64, `record_date` u64, `comment` char[64],
  `info` u8[64], `info_len` u32.
- The store functions below.

#### 1.2.3 Store interface (`progression_store.h`, namespace `u2mp::progression`, unchanged)

Constants: `kBlobSize` 0x228, `kBlobVersion` 10, `kNameMax` 31, `kBoards` 4, `kRowsPerPage` 11.
Types: `Store {publish_count u32; has_blob bool; blob u8[0x228]; local_name string; recent vector<PlayerStats>}`,
`PlayerStats {name; kills, deaths, wins, losses, seconds, coop_points, money, matches, rank u32; last_seen u64}`,
`MatchPlayer {name; local bool; result int; kills, deaths, seconds, coop_points, money, rank u32}`,
`LocalStats {name; money, kills, deaths, wins, losses, seconds, coop_points, coop_seconds, rank u32}`,
`Entry {name; score s64; rank, badge, seconds, deaths, losses u32; local bool}`,
`Page {page int; total u32; rows vector<Entry>}` (at most 11 rows).
Functions used: `load(path, store)`, `save(path, store, now)`, `note_match(store, players, now)`,
`board(store, local, board_index)`, `page(entries, request)`, `crc32(data, size, crc = 0)` (standard reflected
CRC-32, chainable: passing the previous result as `crc` continues the same checksum).

#### 1.2.4 Entry points the game reaches

| Entry | Address | Byte check | Convention, arguments, result |
|---|---|---|---|
| player info update | function `0x64e810`, replaced whole (19 bytes) | `55 48 89 e5 53 50 48 8d 1d db 8c af 00 8b 83 c0 06 00 00` at `0x64e810` | no arguments used; returns nothing meaningful (rax not set on purpose) |
| leave publish | the call instruction at `0x5f0952` (originally calls `0xa15e70`) | `bf 9d 19 5e c8` at `0x5f094d` (mov edi, 0xc85e199d) | one argument: u32 symbol id in edi (the game passes 0xc85e199d); returns a pointer (rax) |
| leaderboard list update | function `0x5b3000`, replaced whole (18 bytes) | `4c 8d 0d 59 a9 b8 00 49 83 79 58 00 0f 85 03 01 00 00` at `0x5b3000`, and the board table check (1.5) | one argument: pointer to the list object (rdi); returns nothing |

### 1.3 Game facts

Global values:

| Address | Type | Meaning |
|---|---|---|
| `0x14ce97c` | s32 | NET_SM state. "Online" means value > 1; "offline" means value <= 1 |
| `0x11474f8` | object | NetPlayerData ("NPD") |
| `0x11474f8 + 0x6c0` = `0x1147bb8` | s32 | NPD state: 3 = connected, 2 = offline (written by this file) |
| `0x11474f8 + 0x44` = `0x114753c` | 0x228 bytes | NPD Data block ("Data"); u32 at its start is the version (10) |

Career values, read as u32 at NPD base + offset (all inside Data):

| Offset from `0x11474f8` | Absolute | Value |
|---|---|---|
| +0xd8 | `0x11475d0` | cash (money) |
| +0x184 | `0x114767c` | kills |
| +0x188 | `0x1147680` | deaths |
| +0x18c | `0x1147684` | wins |
| +0x190 | `0x1147688` | losses |
| +0x198 | `0x1147690` | seconds played (competitive) |
| +0x19c | `0x1147694` | co-op points |
| +0x1a0 | `0x1147698` | co-op seconds |
| +0x1a4 | `0x114769c` | publish counter (incremented by each publish) |

NetStatGame (`0x1147bd0`), the tallies of the match being left:

| Address | Type | Meaning |
|---|---|---|
| `0x1147bd0 + 0x8` = `0x1147bd8` | u32 | player count |
| `0x1147bd0 + 0x38ec` = `0x114b4bc` | records, stride 0x80 | player record i at `0x114b4bc + i*0x80` |
| record +0x7 | u8 | team (0 or 1; other values = no team) |
| record +0xa | u8 | non-zero = local player |
| record +0xb | u8 | rank |
| record +0xc | u32 | seconds played |
| record +0x14 | char[] | name (read up to 31 bytes, stops at NUL) |
| `0x1147bd0 + 0x3dec` = `0x114b9bc` | team tallies, stride 0x34 | team t tally at `0x114b9bc + t*0x34`; u16 at +0 is the "won" marker (1 = won), u16 at +2 is the team score |
| `0x1147bd0 + 0x3e54` = `0x114ba24` | player tallies, stride 0x34 | tally i at `0x114ba24 + i*0x34`: u16 +4 kills, u16 +6 deaths, u32 +0x14 co-op points, u32 +0x18 money |

Other game data:

| Address | Type | Meaning |
|---|---|---|
| `0xdef0a0` | table, 4 entries, stride 12 | NP Score board table. Entry i: u32 +0 board id (300 + i: 300 kills, 301 cash, 302 wins, 303 co-op points), u32 +4 column set |
| `0x113b760 + 0x1ca0` = `0x113d400` | s16 | current net game index (row of `*net-games*`) |
| `0x104f1c8 + 0x34` = `0x104f1fc` | s32 | non-zero = the match came from matchmaking |
| `0x113d960` | object | leaderboard object ("LB") |

Leaderboard object fields (offsets from `0x113d960`):

| Offset | Type | Meaning |
|---|---|---|
| +0x24 | u32 | total entries on the board |
| +0x28 | pointer | rows array (11 records of 0x80 bytes) |
| +0x30 | pointer | game-info array (11 records of 0x40 bytes) |
| +0x50 | u32 | status (0 = ok) |
| +0x58 | u64 | busy (non-zero = a fetch is running; the list waits) |
| +0x70 | u32 | rows on the current page |
| +0x74 | s32 | page shown (0-based) |
| +0x78 | u8 | reset request |
| +0x80 | u32 | selected board index (0..3) |
| +0x84 | u8 | friends filter |
| +0x85 | u8 | refresh request |

List object fields (the argument of the list update; offsets from the pointer):

| Offset | Type | Meaning |
|---|---|---|
| +0x40 | u32 | column set (the 16 bytes +0x40..+0x4f are cleared together on reset) |
| +0x44 | s32 | page delta requested by the player (scroll) |
| +0x4c | s32 | page |
| +0x50 | s32 | board index shown (-1 = none) |
| +0x54 | u8 | friends filter shown |

Rank record (one per row, 0x80 bytes, PS3 SceNpScoreRankData layout): +0 name/NpId text (zero padded),
u32 +0x58 serial rank, u32 +0x5c rank (the printed column), u32 +0x60 highest rank, s64 +0x70 score,
u64 +0x78 record date. Game-info record (0x40 bytes): u32 +0 badge, u32 +4 seconds, u32 +8 deaths,
u32 +0xc losses (host little-endian u32 values).

Guest functions:

| Address | Meaning |
|---|---|
| `0xa15e70` | LookupSymbolValue(u32 sid) -> pointer ("DcLookup"; asserts in the real game when absent) |

DC symbols looked up with `0xa15e70`:

- `0xc85e199d`: the rank table. s32 at +0x50 = number of thresholds; pointer at +0x58 = array of s32 cash
  thresholds.
- `0x3b9a067d`: `*net-games*`. s32 at +0 = record count; pointer at +8 = records, stride 0x90; s32 at record +0xc =
  game type. Co-op game types are 4, 8, 9 and 12.

### 1.4 Module state

Process-lifetime state of this file (all starts zero/empty):

- the store (a `Store` value), and a flag "store load attempted";
- "last published": a 0x228-byte copy of Data (starts all zero);
- "last match signature": u32, starts 0;
- two host row buffers, 16-byte aligned: rows 11 x 0x80 bytes and info 11 x 0x40 bytes. Their addresses are what
  the leaderboard object's +0x28 / +0x30 receive. Both the local and the online fetch fill these same buffers;
- the online fetch state: stage (0 none, 1 "find my page", 2 "range", 3 "friends"), board id (u32),
  requested page (s32).

### 1.5 Behaviour

#### 1.5.1 Store path

Computed afresh at every use: the environment variable `UNCHARTED_U2MP_PROGRESSION` when set and non-empty;
otherwise `runtime_file_user_dir()` (or `.` when that is NULL or empty) followed by `/u2mp/progression.bin`.

#### 1.5.2 Store load (at most once per process)

Whenever a step below says "load the store": if it was attempted before, nothing happens. Otherwise mark it
attempted (even if it fails), then `load(path, store)`. On success, done. On failure the store is (or stays) the
empty default. Then, if a file exists at `path` (filesystem errors count as "does not exist"), it is renamed to
`path + ".unreadable"` with the standard filesystem rename (errors ignored), and this is logged whether or not the
rename worked:

`progression: %s unreadable, kept as .unreadable; starting a new profile` (path)

When `path` does not exist (for example only a bad `.bak` exists), there is no rename and no log.

#### 1.5.3 Connect

Nothing happens when NPD state is 3 or NET_SM <= 1. Otherwise: load the store; read the version as the first 4
bytes (u32 LE) of the store blob. If the store has a blob and that version is 10, copy the 0x228 blob bytes into
Data and log

`progression: profile loaded (cash %u, publish %u)` (u32 at `0x11475d0` after the copy, store publish count)

Otherwise clear Data to zero, write u32 10 at its start, and log `progression: new profile`.
Then set "last published" to the current Data, and set NPD state to 3.

#### 1.5.4 Publish

Nothing happens unless NPD state is 3. Otherwise: load the store; add 1 to the u32 at `0x114769c` (wrapping);
copy Data into the store blob; set the store's has-blob flag; set the store publish count to the new counter value;
set "last published" to Data; call `save(store path, store, now)` where now is the current Unix time in seconds
(`time(nullptr)`). When save returns false, log `progression: could not write %s` (store path).

"Publish if changed": only when NPD state is 3 and Data differs from "last published" in any of its 0x228 bytes,
do a publish.

#### 1.5.5 Offline guard

When NPD state is 3 and NET_SM <= 1: publish if changed, then clear Data to zero with u32 10 at its start, then set
NPD state to 2. Otherwise nothing.

#### 1.5.6 Local stats

The local name: the environment variable `UNCHARTED_U2MP_NAME` when set and non-empty, cut to its first 31 bytes;
otherwise the store's local name when non-empty; otherwise `Drake`. This does not load the store by itself.

The values money, kills, deaths, wins, losses, seconds, co-op points, co-op seconds are read from the career
table (1.3). Then the rank is computed from the money (one guest call):

- guest call `0xa15e70(0xc85e199d)`. NULL result -> rank 0.
- last = (s32 at result +0x50) - 1; thresholds = pointer at result +0x58.
- If last <= 0: rank 0. Else if thresholds is NULL: rank = last.
- Else the first index i in 0..last-1 with `thr[i] <= m` and `thr[i+1] > m`, where m is the money taken as s32
  and all comparisons are signed, is the rank; when no index qualifies the rank is last.

#### 1.5.7 Record match (Leave)

1. count = u32 at `0x1147bd8`. If count is 0 or greater than 10, stop (nothing else, signature unchanged).
2. Count the players per team: records with team 0 and with team 1.
3. "Decided" = both teams have at least one player and the u16 at `0x114b9be` (team 0 score) differs from the u16
   at `0x114b9f2` (team 1 score).
4. For each record i in order build a match player: name (up to 31 bytes from record +0x14, ending at the first
   NUL), local (record +0xa != 0), rank (record +0xb), seconds (record +0xc), kills (tally +4), deaths (tally +6),
   co-op points (tally +0x14), money (tally +0x18). Result: when decided and the record's team is 0 or 1, 1 if the
   u16 at `0x114b9bc + team*0x34` (offset +0, not the score) equals 1, else -1; otherwise 0.
5. Signature: CRC-32 (store `crc32`) of the 4 bytes of count (LE), then continued for each player in order with
   the bytes of the name (its length, no terminator), then with 16 bytes: kills, deaths, seconds, money as LE u32.
6. If the signature equals "last match signature", stop (the same match seen twice; this also skips a match whose
   signature is 0 while nothing was recorded before). Otherwise store the new signature, load the store, and call
   `note_match(store, players, now)` (now = current Unix seconds).

#### 1.5.8 Online scores (Leave)

1. If `online::linked()` is false, stop.
2. Compute the local stats (1.5.6; one guest call `0xa15e70(0xc85e199d)`).
3. Guest call `0xa15e70(0x3b9a067d)`. The game type is -1, unless the result is non-NULL, its records pointer
   (+8) is non-NULL, and the s16 at `0x113d400` is >= 0 and < the s32 count at +0; then it is the s32 at
   records + 0x90 * index + 0xc.
4. co-op = type is 4, 8, 9 or 12. matchmaking = s32 at `0x104f1fc` != 0.
5. A post to board i (0..3) with score S and time T: the board id is the u32 at `0xdef0a0 + 12*i`; the info is 64
   bytes, all zero except the first 16 bytes, which hold rank, T, deaths, losses as big-endian u32 each (rank,
   deaths, losses from the local stats). Host call `online::record_score(board id, S as s64 (zero-extended from u32),
   NULL, info, 64)`. Its result is ignored.
6. co-op: post board 3 (co-op points, co-op seconds), then board 1 (money, co-op seconds).
   Else matchmaking: T = seconds + co-op seconds (u32, wrapping); post board 0 (kills, T), board 1 (money, T),
   board 2 (wins, T).
   Else log `rankings: custom game: no scores posted (PS3 posted matchmaking and co-op only)`.

#### 1.5.9 Entry `0x64e810` (player info update)

1. If NET_SM > 1: host call `unlocks_refresh()`; connect (1.5.3); publish if changed (1.5.4); host call
   `online::member_data_changed()`.
2. Always afterwards: the offline guard (1.5.5), which reads NET_SM again.

#### 1.5.10 Entry at `0x5f0952` (Leave publish), argument id

In this order: record match (1.5.7); publish (1.5.4; does nothing unless state is 3, and does not connect);
online scores (1.5.8); then guest call `0xa15e70(id)` and return its result.

#### 1.5.11 Local fetch (board index b, requested page r)

1. Connect (1.5.3), then load the store.
2. Compute the local stats (one guest call), entries = `board(store, stats, b as int)`, pg = `page(entries, r)`.
3. Zero both row buffers entirely (all 11 records each).
4. For each row k of pg: rank record k: the name bytes (at most 31; the rest stays zero), u32 rank at +0x58,
   +0x5c and +0x60, s64 score at +0x70 (+0x78 stays zero). Info record k: u32 badge +0, seconds +4, deaths +8,
   losses +0xc (plain LE values, no byte swap).
5. LB +0x28 = address of the rows buffer, +0x30 = address of the info buffer, u32 +0x24 = pg.total,
   u32 +0x70 = number of rows, s32 +0x74 = pg.page, u32 +0x50 = 0. LB busy (+0x58) is not touched.

#### 1.5.12 Online result fill (total, page, first, n)

Zero both row buffers entirely. Then for i = first, first+1, ... while i < first + n and fewer than 11 rows are
filled: r = `online::board_row(i)`; when NULL stop. Row k (k counts filled rows from 0): copy the npid bytes,
length = number of bytes of `npid` before its first NUL within its 20 bytes (and at most 31); u32 r.rank at +0x58,
+0x5c, +0x60; s64 r.score at +0x70; u64 r.record_date at +0x78. Info k: take the first min(r.info_len, 16) bytes
of r.info into a zeroed 16-byte value, read it as four u32 words in big-endian order and store them as LE u32 at
+0, +4, +8, +0xc (that is, each 4-byte group is byte-reversed).
Then LB +0x28 = rows buffer, +0x30 = info buffer, u32 +0x24 = total, u32 +0x70 = rows filled, s32 +0x74 = page,
u32 +0x50 = 0, u64 +0x58 = 0, and the online fetch stage becomes 0.

#### 1.5.13 Fetch start (board index b < 4, requested page r)

1. If `online::linked()` is true (it is called first, every time): the board id is the u32 at `0xdef0a0 + 12*b`;
   friends = LB u8 +0x84 != 0. Reset the online fetch state, then record the board id and page r.
   - friends: stage 3, host call `online::board_request(id, 1, 0, 100)`;
   - else r < 0: stage 1, `online::board_request(id, 2, 0, 1)`;
   - else: stage 2, `online::board_request(id, 0, r*11 + 1 (u32 arithmetic), 11)`.
   When the request returns true: LB u64 +0x58 = 1, LB u32 +0x70 = 0, and stop. When false: stage 0 and continue.
2. Local fetch (1.5.11) with b and r.

#### 1.5.14 Online fetch progress (start of every list update)

Returns "wait" (the list update stops at once) or "go on".

- Stage 0: go on (no host calls).
- Else st = `online::board_state()`.
  - st == 1: wait.
  - st < 0: log `rankings: online board %u failed: local board shown` (the fetch's board id); stage 0;
    LB u64 +0x58 = 0; local fetch (1.5.11) with board index = LB u32 +0x80 and the fetch's requested page; go on.
  - stage 1: r = `online::board_row(0)`; page = (r.rank - 1) / 11 (unsigned division) when r is non-NULL and its rank
    is non-zero, else 0. Store page as the fetch page, stage 2, then `online::board_request(board id, 0,
    page*11 + 1, 11)`; if that returns false, online result fill (1.5.12) with (0, 0, 0, 0). Wait (in both cases).
  - stage 2: host call `online::board_total()` first, then `online::board_rows()`; online result fill with
    (total, fetch page, 0, rows). Go on.
  - any other stage (3): n = `online::board_rows()`; page = the fetch page. If page < 0: page = 0, me =
    `online::my_npid_text()`, then for every i from 0 to n-1 in order call `online::board_row(i)`; when non-NULL and
    its npid (as NUL-terminated text) equals me, page = i / 11 (the last match wins; the scan never stops early).
    Online result fill with (n, page, page*11, 11). Go on.

#### 1.5.15 Entry `0x5b3000` (leaderboard list update), argument list

1. Online fetch progress (1.5.14). On "wait", return.
2. If LB u64 +0x58 is non-zero, return.
3. first = LB s32 +0x74; write it to list +0x4c.
4. If LB u8 +0x78 is non-zero (reset): list +0x50 = -1; clear list bytes +0x40..+0x4f (this also clears the page just
   written and the delta); list u8 +0x54 = 0; LB u8 +0x78 = 0. Then shown = -1, delta = 0, first = 0.
   Otherwise delta = list s32 +0x44 and shown = list s32 +0x50.
5. b = LB u32 +0x80, refresh = LB u8 +0x85. Compare shown with b as s32.
   - shown == b and delta == 0: if list u8 +0x54 differs from LB u8 +0x84, or refresh is non-zero, go to step 7;
     otherwise return.
   - shown != b and delta == 0: go to step 7.
   - delta != 0: step 6.
6. Scroll: p = first + delta. list +0x4c = p, list +0x44 = 0. last = (LB u32 +0x24 - 1) / 11 in unsigned 32-bit
   arithmetic, taken as s32 (a total of 0 gives 390451572). Clamp p to 0..last (below 0 -> 0, above last -> last)
   and write it to list +0x4c. If p == first, return. Otherwise refresh = LB u8 +0x85 and go on.
7. If refresh is non-zero: list +0x4c = 0 and LB u8 +0x85 = 0.
8. b = LB u32 +0x80, friends = LB u8 +0x84. If shown != b (as s32) or list u8 +0x54 != friends: list +0x4c = -1.
   list u8 +0x54 = friends; list s32 +0x50 = b.
9. If b < 4 (unsigned): fetch start (1.5.13) with b and the s32 at list +0x4c.
10. list u32 +0x40 = the u32 at image offset `0xdef0a4 + 12 * (b taken as s32, sign-extended to 64 bits)`
    (64-bit wrapping address arithmetic). This happens also for b >= 4, reading past or before the table.

#### 1.5.16 `progression_publish()`

When `image.base` is NULL, nothing. Otherwise publish if changed (1.5.4).

#### 1.5.17 `progression_recent_players(max)`

Load the store. Return the names of the store's recent players ordered by last-seen time, newest first, players
with equal times keeping their store order (stable), at most `max` names. Does not touch the image.

#### 1.5.18 `progression_player_summary(name)`

Load the store. For the first recent player whose name equals `name` exactly, return the text
`%s: %u matches with you, %u kills, %u deaths, %u wins, %u losses, rank %u.` with name, matches, kills, deaths,
wins, losses, rank, cut to at most 255 characters. When no player has that name, return `name` unchanged.

### 1.6 Install (`install_progression(installed)`)

1. Board table check: the u32 at `0xdef0a0 + 12*i` equals 300 + i for i = 0, 1, 2, 3 (all four must match).
2. If the 19 bytes at `0x64e810` match (1.2.4) and `replace_function(0x64e810, 19, <entry 1.5.9>,
   "NetPlayerData connect/publish (64e810)")` returns true: `installed` + 1. Otherwise (mismatch or false) log
   `progression: 64e810 bytes differ, NetPlayerData connect not installed`.
3. If the 5 bytes at `0x5f094d` match and `retarget_call(0x5f0952, 0xa15e70, <entry 1.5.10>, "Leave publish")`
   returns true: + 1. Otherwise log `progression: Leave::Enter call site differs, publish on leave not installed`.
4. If the board table check passed, the 18 bytes at `0x5b3000` match, and `replace_function(0x5b3000, 18,
   <entry 1.5.15>, "leaderboard list update (5b3000)")` returns true: + 1. Otherwise log
   `progression: leaderboard list bytes differ, leaderboards not installed`.

Steps run in this order; a failing step does not stop the next. Maximum added: 3. Nothing else happens at install
(no store load, no file access).

### 1.7 Test scenarios (progression)

Install:
- P-I1 all bytes match: two replace_function and one retarget_call records with the exact names and lengths,
  installed + 3, no log.
- P-I2 `0x64e810` differs: its log, + 2. P-I3 `0x5f094d` differs: its log, + 2.
- P-I4 one board id wrong (e.g. entry 2 = 303) with `0x5b3000` bytes right: leaderboard log, + 2. P-I5 `0x5b3000`
  bytes differ.

Player info (`0x64e810`), store path set by `UNCHARTED_U2MP_PROGRESSION` to a temp file:
- P-1 NET_SM = 1, state 0: no host calls, nothing written.
- P-2 NET_SM = 2, no store file: unlocks_refresh, "new profile", Data = version 10 + zeros, state 3,
  member_data_changed, no file written (Data equals last published).
- P-3 as P-2 but the store file holds a version-10 blob: "profile loaded (cash N, publish M)", Data = blob.
- P-4 store blob with version 9: "new profile".
- P-5 unreadable store file (garbage, no .bak): renamed to `.unreadable`, the rename log, "new profile".
- P-6 state 3, NET_SM = 2, Data changed since last publish: publish counter +1, file written, no connect log.
- P-7 state 3, NET_SM = 2, Data unchanged: no file write.
- P-8 state 3, NET_SM = 1, Data changed: publish, then Data cleared to version 10, state 2; no unlocks_refresh.
- P-9 save fails (path in a non-creatable location): "could not write" log.
- P-10 second call after a load failure: no second load attempt (store load once).

Leave (`0x5f0952` entry, argument 0xc85e199d):
- P-L1 count 0 or 11: no record, publish only if state 3, returns DcLookup(id) result.
- P-L2 2 players, teams 0/1, scores 5/3, team-0 marker 1: results +1/-1 passed to note_match; file saved.
- P-L3 same tallies again: signature equal, note_match not called.
- P-L4 one team empty or equal scores: results 0.
- P-L5 player team 2: result 0 for that player.
- P-L6 name of 40 bytes without NUL: name cut to 31.
- P-L7 linked, co-op type (4, 8, 9, 12 each): record_score boards 303 then 301 with co-op seconds; info bytes
  big-endian.
- P-L8 linked, competitive, matchmaking: 300, 301, 302 with seconds + co-op seconds.
- P-L9 linked, competitive, not matchmaking: the custom game log.
- P-L10 `*net-games*` NULL, records NULL, index -1, index = count: type -1 (competitive branch).
- P-L11 rank table: NULL; count 0 or 1; thresholds NULL with count 5 (rank 4); money below thr[0]; money between
  thr[2] and thr[3] (rank 2); money above all (rank last); money >= 0x80000000 (signed compare).
- P-L12 not linked: only DcLookup(id) after record/publish.
- P-L13 state 2 at Leave: no publish, no connect.

Leaderboard list (`0x5b3000`):
- P-B1 LB busy non-zero with no online fetch: returns, list untouched.
- P-B2 reset set: list cleared as described, fetch for board b, list +0x50 = b, page -1 requested, columns
  written.
- P-B3 same board, delta 0, same friends, no refresh: only list +0x4c = LB page.
- P-B4 same board, friends differ: fetch with page -1.
- P-B5 same board, refresh: page 0, LB refresh cleared, fetch page 0.
- P-B6 delta +1 within range; delta -5 clamps to 0 equal to first (returns after clamp); delta past last; total 0.
- P-B7 delta != 0 and shown != b: fetch with page -1.
- P-B8 board index 4 and 0xffffffff: no fetch, columns read at the computed address.
- P-B9 not linked: local rows (names, ranks, scores, info), LB fields, busy untouched.
- P-B10 linked, request ok for each stage start (friends / r < 0 / r >= 0): busy 1, count 0, correct
  board_request arguments; request false: local fetch.
- P-B11 progress: state 1 (wait); state -1 (log, local fetch, then the rest of the update runs); stage 1 with rank
  0 / rank 23 (page 2), and request false; stage 2 fill with info_len 0, 8 and 64 and npid of 20 bytes without
  NUL; stage 3 with page -1 and my npid at rows 3 and 14 (page 1), and with page 2.

Others:
- P-O1 `progression_publish` with `image.base` NULL: nothing.
- P-O2 recent players: three entries with times 5, 9, 5 and max 2 -> the time-9 name, then the first time-5 name.
- P-O3 summary for a known and an unknown name.
- P-O4 `UNCHARTED_U2MP_NAME` of 40 characters: local name cut to 31 in the local board rows.

---

## Part 2: netinit.cpp

### 2.1 Purpose and hooks

The PS3 multiplayer initialisation (NetInit, the connecting page update, NetGameManager::Init) is gone from PS4
1.02. This file sets up the state those created that PS4 code still reads: two hash tables in the
NetGameManager (without them the game reset at match start reads a NULL bucket array), the recent-game history,
the leave status of two sessions, and the booster list. It patches no code; it is called from session.cpp.

### 2.2 Interface

Declared in `u2mp_internal.h` (unchanged):

```
void netinit_prepare();               // when Multiplayer is entered
void netinit_match();                 // at match start
void netinit_dump(const char *when);  // diagnostics
void *netheap_alloc(size_t size, size_t align);   // netheap.cpp, used here
```

Callers: session.cpp calls `netinit_prepare()` on entering Multiplayer (then `netpatch_load()`),
`netinit_match()` at match start and before creating a game, and `netinit_dump()` with the texts
`"before create"`, `"after create"`, `"state 16"`, `"state 3"`. No game code reaches this file directly; there is
nothing to install and no `installed` counter.

### 2.3 Game facts

| Address | Meaning |
|---|---|
| `0x113b760` | NetGameManager ("NGM") |
| `0x113d300` (NGM +0x1ba0) | hash table A: 16 entries, node size 0x60 |
| `0x113d370` (NGM +0x1c10) | hash table B: 256 entries, node size 0x38 |
| `0x113d402` (NGM +0x1ca2) | recent-game history: 4 x s16 (8 bytes) |
| `0x1065b08` | match session; u8 at +0xc8 (`0x1065bd0`) = leave status |
| `0x105efb8` | party session; u8 at +0xc8 (`0x105f080`) = leave status |
| `0x113a788` | booster list: u32 count at +0, then pointers to records at +8 + 8*k (room for 64; at most 63 used) |
| `0xa15300` | guest DcFind(u32 sid) -> pointer to the symbol's value slot, or NULL (does not assert) |
| `0xb332a0` | guest Alloc(size, const int *context, align): only byte-checked, never called |

Hash table layout (offsets from the table address): pointer +0 bucket array; pointer +8 free-list head;
u32 +0x10 node size; u32 +0x14 node count; pointer +0x18 first pool node; pointer +0x20 sentinel node;
pointer +0x28 the allocation; pointer +0x30 end of buckets (address of bucket slot [bucket count]);
s32 +0x3c bucket count; u8 +0x40 ready. Node: pointer +0 next, pointer +8 prev (+0x10 key, +0x18 value are not
written here).

`*boosters*` DC symbol `0x916e03b0`: DcFind returns a slot; the slot holds the table address. Table: s32 count at
+0, pointer to records at +8. Records have stride 0x20; u32 at record +0x18 is the DLC mask.

Byte checks (all must match; each is checked even after an earlier one failed):

| Address | Bytes | Label |
|---|---|---|
| `0x640fe4` | `49 8b bf a0 1b 00 00` | reset table A |
| `0x641012` | `4d 63 87 dc 1b 00 00` | reset table A buckets |
| `0x6410e0` | `4c 8d 60 60` | table A node size |
| `0x64119e` | `49 8b bf 10 1c 00 00` | reset table B |
| `0x6411d2` | `49 63 8f 4c 1c 00 00` | reset table B buckets |
| `0x641290` | `4c 8d 68 38` | table B node size |
| `0x5d90bb` | `41 c7 47 10 58 00 00` | hash table layout |
| `0x5ec590` | `48 8d 0d 71 95 a7 00 31 c0 80 b9 c8 00 00 00 00` | session leave status |
| `0x5f6750` | `48 8b 44 d7 08 39 70 0c 74` | booster list |
| `0x5fd8ef` | `48 8d 05 92 ce b3 00` | booster object |
| `0xb332a0` | `55 48 89 e5 41 57 41 56 41 55 41 54 53 50 41 89` | Alloc |
| `0xa15300` | `55 48 89 e5 53 48 83 ec 18 4c 8b 05 f0 58 63 00` | DcFind |

### 2.4 Behaviour

Module state (process lifetime): "checks done" and "checks passed" flags; "history done"; "boosters done".

#### 2.4.1 `netinit_prepare()` and `netinit_match()`

Both do exactly the same:

1. Code check, only on the first call ever: every row of the table above is compared, in table order; each
   mismatch logs `netinit: bytes at %#llx (%s) differ; nothing done` (address, label). The result (all matched or
   not) is remembered; later calls reuse it without checking or logging again. If it failed, stop now (and on every
   later call).
2. On the first call that gets here: write -1 to each of the 4 s16 at `0x113d402` (8 bytes 0xff). Once only.
3. Every call: u8 at `0x1065bd0` = 1 and u8 at `0x105f080` = 1.
4. Table A (`0x113d300`, 16 entries, node 0x60, name `ngm table 0x1ba0`), then table B (`0x113d370`, 256 entries,
   node 0x38, name `ngm table 0x1c10`), as in 2.4.2.
5. Boosters, as in 2.4.3.

#### 2.4.2 Hash table setup (table T, entries N, node size S, name)

- If the pointer at T+0 is non-zero and the u8 at T+0x40 is non-zero: already set up, nothing (no log).
- Otherwise: bucket count B = the smallest power of two >= N (computed by smearing the bits of N-1 and adding 1;
  16 for A, 256 for B). Pool offset P = (B*8 + 8) rounded up to a multiple of 16. Size = P + (N+1)*S
  (A: 0x6f0 = 1776 bytes; B: 0x4048 = 16456 bytes).
- Host call `netheap_alloc(size, 16)`. If NULL: log `netinit: %s: allocation of %llu bytes failed` (name, size);
  the table is not touched; continue with the next step of 2.4.1.
- Otherwise, with M the allocation: bucket slots 0..B-1 (8 bytes each at M) = 0; slot B (at M + B*8) = address of
  the sentinel. Pool = M + P; sentinel = pool + N*S; sentinel +0 and +8 = the sentinel's own address. Pool node i
  (at pool + i*S): +0 = address of node i+1, or 0 for the last; +8 = address of node i-1, or 0 for the first. No
  other allocation bytes are written (padding, keys, values and the rest of the sentinel keep what the allocator
  gave).
- Table fields: +8 = pool; u32 +0x10 = S; u32 +0x14 = N; +0x18 = pool; +0x20 = sentinel; +0x28 = M; +0x30 =
  M + B*8; s32 +0x3c = B; +0 = M; u8 +0x40 = 1. No other table byte is written.
- Log `netinit: %s: %d entries of %#x, %d buckets` (name, N, S, B), e.g.
  `netinit: ngm table 0x1ba0: 16 entries of 0x60, 16 buckets`.

#### 2.4.3 Boosters

- If "boosters done": nothing.
- If the u32 at `0x113a788` is non-zero: set "boosters done", nothing else (no guest call, no log).
- Guest call `0xa15300(0x916e03b0)`. If the result is NULL, or the pointer it points to is NULL: log
  `netinit: *boosters* not loaded yet` and stop (not done: the next call tries again).
- n = s32 at table +0; recs = pointer at table +8. If n <= 0 or recs is NULL: stop silently (not done: the next
  call tries again, with another guest call).
- "Any mask" = at least one of the n records has a non-zero u32 at +0x18.
- Walk records 0..n-1 in order while fewer than 63 are listed: when "any mask" is set, skip records whose mask is
  0; otherwise list every record. Listing record k-th: pointer at `0x113a788 + 8 + 8*k` = record address.
- u32 at `0x113a788` = number listed; set "boosters done"; log `netinit: %u of %d boosters listed` (listed, n).

#### 2.4.4 `netinit_dump(when)`

No code check, no writes. For table A (label `0x1ba0`) and then table B (label `0x1c10`): b = pointer at +0,
end = pointer at +0x30, n = s32 at +0x3c. When b is non-zero and 0 < n <= 4096, read the n+1 u64 slots at b
(slots 0..n) and count the non-zero ones ("used") and those equal to 0xffffffffffffffff ("minus"); otherwise both
counts are 0. Log

`netinit[%s] table %s: buckets %#llx end %#llx (%s) n %d free %#llx pool %#llx sentinel %#llx ready %u; non-null slots %d, -1 slots %d`

with when, label, b, end, `ok` when b is non-zero and end == b + n*8 (n sign-extended to 64 bits, 64-bit wrapping
arithmetic) else `MISMATCH`, n, pointer +8, pointer +0x18, pointer +0x20, u8 +0x40, used, minus.

### 2.5 Install

None. netinit has no install function and does not change `installed`.

### 2.6 Test scenarios (netinit)

- N-1 all checks match, empty state: history -1, both leave bytes 1, two allocations (1776, 16456 bytes, align 16),
  table fields and free lists exact, two table logs; boosters: table with 3 records, masks 0/5/0 -> 1 listed.
- N-2 one byte check differs (e.g. `0x5f6750`): its log only, nothing written; second call: no log, nothing.
- N-3 two checks differ: two logs in table order.
- N-4 second call after N-1: only the two leave bytes are written again (set them to 0 first to see it); no table
  log, no booster guest call.
- N-5 table A has buckets non-zero but ready 0: rebuilt (new allocation, log).
- N-6 netheap_alloc returns NULL for table A: failure log, table A untouched, table B still built.
- N-7 boosters: count already non-zero -> no guest call, later calls none either.
- N-8 DcFind NULL: "not loaded yet"; next call tries again and succeeds.
- N-9 DcFind slot holding NULL: same log.
- N-10 n = 0 / recs NULL: silent, retried next call.
- N-11 all masks 0: all listed; 70 records with masks: at most 63 listed.
- N-12 history written only once: set it to 0 after the first call, call again: stays 0.
- N-13 dump: built tables ("ok", counts: table A one non-null slot, the sentinel slot), empty tables (b 0 prints
  `0`, `MISMATCH`), n = 5000 (no slot reads), end wrong (`MISMATCH`).

---

## Part 3: netpatch.cpp

### 3.1 Purpose and hooks

PS3 1.10 downloaded the server DC module `net-patch9.bin` and bound its symbols over the disc data. PS4 1.02
has neither the download nor the loader, so symbols only that module defines (for example the weapon set
`0x8cf8e1cc`) are missing and a match asserts. This file loads the PS4 conversion of that module (written by the
importer) when Multiplayer is entered, relocates it with the game's own module relocator and binds its root
entries with the game's own BindSymbol. It patches no code.

### 3.2 Interface

Declared in `u2mp_internal.h` (unchanged; netpatch.cpp may repeat the two declarations):

```
void install_netpatch(int &installed);
int netpatch_load();
```

Callers: install.cpp calls `install_netpatch(installed)` once, as the last install step. session.cpp calls
`netpatch_load()` on entering Multiplayer, right after `netinit_prepare()`.

Host function used: `extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);` (0 on
success; `out` receives the host path).

Guest functions:

| Address | Meaning | Call |
|---|---|---|
| `0xa19a00` | RelocateModule(buf) -> pointer to the root slot (buf + 0x10) | `(void *buf)` -> pointer |
| `0xa16d70` | BindSymbol(u32 sid, const u64 *value) | `(sid, pointer to an 8-byte value)` |
| `0xa15300` | DcFind(u32 sid) -> pointer to the value slot or NULL | `(sid)` -> pointer |

Prologue checks (each range must be executable and the bytes must match):

| Address | Bytes |
|---|---|
| `0xa19a00` | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 48 89 7d d0 40 f6 c7` (24) |
| `0xa16d70` | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 4c 8b 0d 78 3e 63 00` (24) |
| `0xa15300` | `55 48 89 e5 53 48 83 ec 18 4c 8b 05 f0 58 63 00` (16) |

### 3.3 Game facts

| Address / value | Meaning |
|---|---|
| `0x15340d0` | pointer: symbol hash-map bucket array (NULL before the script manager is set up) |
| `0x15340d8` | pointer: free-node list head of that hash map; each free node's first 8 bytes point to the next (NULL ends) |
| `0x153410c` | s32: bucket count |
| `0x916aa605` | type header of a module root table |
| `0x87b03989` | a root entry with this id is a script to run (PS3), not a symbol |
| `0x8cf8e1cc` | the weapon set whose absence asserted |
| `0x837a26b6` | online player weapon settings map |
| `0x974cf651` | offline player weapon settings map (defined by the PS4 game) |
| `/app0/u2data/build/main/dc1/ps4/net-patch9.bin` | the guest path of the module |

Module file (`00CD`, little-endian): bytes 0..3 `00CD`; u64 +8 relocation offset R; u64 +0x10 root pointer
(0x20 before relocation); u32 +0x1c root type `0x916aa605`; root at +0x20: u32 +0x20 (1), u32 +0x24 entry count C,
entries at +0x28 + 16*i: u32 +0 sid, u32 +4 (0), u64 +8 value pointer. At R: u32 bitmap length L in bytes, then the
bitmap: bit k (value 1 << k) of bitmap byte i marks the 8-byte slot at file offset (8*i + k) * 8 as a pointer.

### 3.4 Behaviour

Module state (process lifetime): "ready" (set by install), "load done", "bound count" (int, starts 0).

#### 3.4.1 `netpatch_load()`

1. If "load done": return the bound count (no other effect).
2. Set "load done".
3. If not "ready": log `netpatch: not installed (guest functions differ), net-patch9 not loaded`, return 0.
4. Read the environment variable `UNCHARTED_U2MP_NETPATCH`. "all" mode = it equals `all` exactly. If it equals
   `off` exactly: log `netpatch: disabled (UNCHARTED_U2MP_NETPATCH=off)`, return 0.
5. If the pointer at `0x15340d0` is NULL or the s32 at `0x153410c` is <= 0: log
   `netpatch: script symbol table not set up yet, net-patch9 not loaded`, clear "load done" (the next call runs
   again from step 2, reading the environment again), return 0.
6. Read the file: `runtime_file_translate(path, buffer, 1024)`; non-zero result, or the host file cannot be opened
   for binary reading, is "not found". Otherwise read it whole. Not found: log
   `netpatch: %s not found (run the u2mp import with the PS3 GOTY data folder NPUA80698DATA2): weapon set 0x%08x and the other server symbols stay undefined`
   (path, 0x8cf8e1cc), return 0.
7. Validate (3.4.2). On a reason: log `netpatch: %s rejected: %s` (path, reason), return 0.
8. Copy the file into a new 16-byte-aligned host block of exactly the file size, never freed. If the allocation
   fails: log `netpatch: out of memory (%zu bytes)` (size), return 0.
9. Guest call `0xa19a00(block)`. If the result is not block + 0x10, or the pointer stored at the result is not
   block + 0x20: log `netpatch: relocation failed (root slot %p)` (the result), return 0.
10. C = u32 at block + 0x24. Room = the number of nodes in the free list at `0x15340d8`, counting at most C + 64
    (stop at NULL or at that limit).
11. For each root entry i = 0..C-1 in order (sid = u32 at block + 0x28 + 16*i, value = u64 at block + 0x30 + 16*i,
    already relocated):
    - sid `0x87b03989`: log `netpatch: entry 0x%08x is a script to run (PS3 0x151e18), not supported; skipped`
      (sid); next entry (no guest call).
    - have = guest call `0xa15300(sid)`.
    - sid `0x837a26b6` and have is NULL: guest call `0xa15300(0x974cf651)`. When that is non-NULL: guest call
      `0xa16d70(0x837a26b6, pointer to an 8-byte copy of the u64 stored in the offline slot)`, bound count + 1, log
      `netpatch: online weapon settings 0x%08x -> the PS4 offline map 0x%08x` (0x837a26b6, 0x974cf651), next
      entry. When it is NULL, go on with the rules below.
    - have is non-NULL and not "all" mode: "kept" + 1, next entry.
    - have is NULL and bound count >= room - 48: "not bound" + 1, next entry.
    - Otherwise guest call `0xa16d70(sid, pointer to an 8-byte location holding value)`, bound count + 1.
    ("all" mode with have non-NULL binds without the room rule.)
12. Final guest call `0xa15300(0x8cf8e1cc)`, then log
    `netpatch: net-patch9 loaded at %p (%zu bytes, %u symbols): %d bound, %d kept from the PS4 game%s, %d not bound (symbol pool full, %d free nodes); weapon set 0x%08x %s`
    with block, file size, C, bound count, kept, `""` in "all" mode else ` (PS4 data wins)`, not bound, room,
    0x8cf8e1cc, `defined` when that final DcFind was non-NULL else `STILL MISSING`.
13. Return the bound count.

The bound count is never reset; after a successful load later calls return it.

#### 3.4.2 Validation (size Z, file bytes d; all arithmetic unsigned 64-bit, wrapping)

The first failing rule gives the reason:

1. Z < 0x40, or the first 4 bytes are not `00CD`: `not a PS4 DC module ('00CD')`.
2. R = u64 at +8, root = u64 at +0x10. R < 0x28, or R + 4 > Z, or R not a multiple of 8:
   `bad relocation offset`.
3. L = u32 at R. R + 4 + L > Z, or L * 64 < R: `bad relocation bitmap`.
4. root != 0x20, or u32 at +0x1c != 0x916aa605: `unexpected root table`.
5. C = u32 at +0x24. 0x28 + C*16 > R: `root table past the data`.
6. For every slot s = 0 .. L*8 - 1 in increasing order whose bit is set (bit s%8 of bitmap byte s/8, the bitmap
   starting at R + 4): if s*8 + 8 > R: `pointer slot past the data`; else if the u64 at s*8 is >= R:
   `pointer outside the module`.

### 3.5 Install (`install_netpatch(installed)`)

Check, in order, RelocateModule, BindSymbol, DcFind: each range executable and the bytes equal (3.2). If any fails:
log `netpatch: RelocateModule 0x%llx / BindSymbol 0x%llx / DcFind 0x%llx differ, net-patch9 loader not installed`
(0xa19a00, 0xa16d70, 0xa15300) and return. Otherwise set "ready", `installed` + 1, and log
`netpatch: net-patch9 loader ready (RelocateModule 0x%llx, BindSymbol 0x%llx); loads when Multiplayer is entered`
(0xa19a00, 0xa16d70). No code is patched; no file is read at install.

### 3.6 Test scenarios (netpatch)

Build a module image in a temp folder and point `runtime_file_translate` at it.

- X-I1 install, all match: ready log, + 1. X-I2 BindSymbol byte differs: the differ log, + 0; then
  `netpatch_load()` logs "not installed" and returns 0; a second call returns 0 silently.
- X-1 `UNCHARTED_U2MP_NETPATCH=off`: disabled log; second call returns 0, no log.
- X-2 hash buckets NULL (or count 0): "not set up yet"; set them, call again: loads.
- X-3 file missing; translate returns non-zero: the not-found log; later calls return 0 silently.
- X-4 validation, one case per rule: Z = 0x3f; wrong magic; R = 0x20; R = 0x2c; R + 4 > Z; L too large;
  L*64 < R; root 0x28; wrong root type; C too large; a marked slot at or past R; a marked slot holding a value >= R.
- X-5 fake relocate returning another slot, or not writing block + 0x20: relocation failed log.
- X-6 entries: one 0x87b03989 (script log), one defined by the fake DcFind (kept), two undefined (bound, Bind
  arguments sid and pointer to the relocated value), weapon set 0x8cf8e1cc (final "defined").
- X-7 0x837a26b6 undefined with 0x974cf651 defined: bound to the offline value, its log; with 0x974cf651 also
  undefined: normal rules.
- X-8 "all" mode: defined entries bound too; summary without " (PS4 data wins)".
- X-9 free list of 50 nodes and 10 undefined entries: room 50, two bound, eight "not bound".
- X-10 after success, a further call returns the same bound count with no calls and no log.

---

## Notes for the tester

- Host addresses appear in guest memory (leaderboard +0x28/+0x30, the hash-table allocations, the booster list
  points into game data) and in log lines (`%p` in netpatch). Those values differ between two implementations;
  compare what they point to, not the addresses.
- `time(nullptr)` reaches `save` and `note_match`; fix or ignore the time fields when comparing store files.
- The pointer passed as BindSymbol's second argument points at a host copy of the value; compare the value read
  through it during the call.

## Clarifications found by the differential test

- Rank lookup (1.5.6): last = count - 1 is computed without 32-bit wrap, so a count of INT_MIN gives rank 0.
- Install (1.6): each board id must equal 300 + i and its 4 bytes must lie inside the image (as image.matches
  checks); a too small image fails the board check.
