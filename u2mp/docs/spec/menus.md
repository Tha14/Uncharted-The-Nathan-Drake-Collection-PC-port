# Spec: multiplayer menu queries and menu images (menus, menuimages)

Clean-room description for a new implementation of `gow3_pc/src/u2mp/menus.cpp` and
`gow3_pc/src/u2mp/menuimages.cpp`. It states what the code must do, as seen from outside. It does
not prescribe structure, helper names or loop shapes. All addresses are image offsets of the
Uncharted 2 PS4 1.02 build unless they are called "absolute". Background: `u2mp/docs/MENUS.md`
(sections 1, 2, 3, 6 to 9).

Notation:

- `B` is `image.base` (the loaded image, `u2mp::image`). An image offset `X` is the absolute address `B + X`.
- `q` is the query object the game passes (absolute pointer). `q+0xNN` is a byte offset into it.
- "u16", "s16", "u32", "s32", "u64", "f32" are little-endian host values of that size. Unaligned
  reads and writes are allowed and happen (for example `q+0x1c` as u64).
- "guest call F(a, b, ...)" means calling the game function at `B + F` with the x86-64 System V
  convention (`U2MP_GUEST`): integer and pointer arguments in rdi, rsi, rdx, rcx, r8, r9, float
  arguments in xmm0.., result in rax (or xmm0).
- "host" functions (session, online, voice, progression, dlc, log) are other files of the module.
  They are called through their declarations in the headers listed in section 0.
- All floating point is IEEE single precision (`float`). Every operation is rounded on its own
  (no fused multiply-add, no double intermediates except where a value is passed to a log format,
  no fast-math reassociation). The build has no `-march`, so the current code never fuses.
- `sid(s)` is the string id of `u2mp_internal.h` (CRC-32, MSB first, polynomial 0x04C11DB7, no
  init, no final xor). Hex values given next to a name were computed with it.
- A log line is written with `u2mp::log(format, args...)`. The text shown is the format string;
  `log` adds the `u2mp: ` prefix and the newline.

---

## 0. Headers and callers

Both files include `u2mp_internal.h` and `frontend3.h`. `menus.cpp` also uses `online.h`
(namespace `u2mp::online`), `progression.h`, `session_api.h` (namespace `u2mp::session`) and
`voice.h` (namespace `u2mp::voice`). `menuimages.cpp` also uses
`extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);` from the C runtime.

Who calls into these files:

| function | declared in | called by |
|---|---|---|
| `void install_menus(int &installed)` | `u2mp_internal.h` | `install.cpp`, after `install_remaining`, before `install_menuimages` |
| `void install_menuimages(int &installed)` | `u2mp_internal.h` | `install.cpp`, right after `install_menus` |
| `int32_t playlist_pick_type()` | `u2mp_internal.h` | `session.cpp` (Start Match in matchmaking mode, netInfo mode 1) |
| `bool playlist_game_types(std::vector<int32_t> &types)` | `frontend3.h` | `frontend3.cpp` (`pick_vote_games`: the vote's two games) |
| `const char *extra_net_package()` | `u2mp_internal.h` | `session.cpp` (`keep_net_package`: requests the package by that name before "net"), and menuimages itself |
| `uint8_t *menu_net_material(uint32_t id)` | `frontend3.h` | `loading.cpp` (loading screen cards, edges, title back) |
| `void menu_draw_quad(uint8_t *material, float x, float y, float w, float h, float u0, float v0, float u1, float v1, float alpha, float rgb = 1.0f)` | `frontend3.h` | `loading.cpp` |

All seven declarations stay exactly as they are. Everything else in the two files is private.

Host functions these files call (all exist already):

- `session::local_name()`, `session::member_count()`, `session::member_at(int, uint32_t&, std::string&)`, `session::is_host()`
- `online::active()`, `online::friend_count()`, `online::friend_online(int)`, `online::friend_row(int)`,
  `online::join_rows()`, `online::join_row(int)`, `online::join_title()`
- `voice::volume()`, `voice::set_volume(float)`
- `progression_recent_players(size_t)`
- `dlc_net_game_available(int32_t)` (dlc.cpp). Note: it makes guest calls of its own (DC lookups),
  so every call to it is observable. Section 1.4 states exactly when it is called.
- `package_by_id(uint32_t)` (session.cpp)
- `image.executable`, `image.matches`, `near_stub`, `replace_function`, `retarget_call`,
  `is_ret_stub`, `log` (install.cpp / u2mp_internal.h). `replace_function` and `retarget_call`
  print their own log lines (success: `"%s: 0x%llx -> C++"` and `"%s: call at 0x%llx -> C++"`);
  the `name` argument given to them is part of the contract and is stated below.

---

# Part 1: menus.cpp

## 1.1 Purpose and hook points

The PS4 1.02 build keeps the multiplayer menu pages (menu.bin is the same as PS3), but some
query functions behind their controls are bare `ret`, and others still have code but read data
that only the PS3 game filled (server live data, PSN). This file makes those pages show content.

Menu controls ask a query function for text, values or images. The function comes from the query
table at `0x105e370`: 132 entries of 0x18 bytes, `{u32 cond, u32 pad, u64 fn (absolute), u64 arg}`,
ended by an entry with `fn == 0`. The game reads the entry on every call, so changing `fn` in the
table redirects the query.

Two ways in:

- **Wrap**: the table entry's `fn` (absolute) is replaced by the address of a host function. The
  host function may call the original function (absolute address that was in the entry).
- **Replace**: the query function is a `ret` stub. Its first 5 bytes are replaced by a jump to a
  host function (`replace_function`). The table entry is not changed.

Every entry point is called by the game as `void fn(uint8_t *q)` (System V, `q` in rdi, no return value).

## 1.2 Query object fields

| field | type | meaning |
|---|---|---|
| `q+0x00` | u32 | op: 1 title text, 2 row text, 3 image, 4 get index, 5 last index, 7 enabled, 8 reset, 9 set index (others exist; they are "any other op") |
| `q+0x04` | s32 / u32 | value or row (input for op 2, 3, 9; output for op 4, 5, 7) |
| `q+0x08` | u32 | image type (op 3 output of the original; see image descriptor below) |
| `q+0x08..+0x33` | | image descriptor for op 3 (`q+0x08` is descriptor +0) |
| `q+0x40` | u64 | text out: absolute pointer to a NUL-terminated string |
| `q+0x54` | u32 | column (op 2) |

Text pointers written to `q+0x40` point to storage owned by the implementation. It must stay
valid at least until the same query (same cond) is called again. The pointer value is not part of
the contract; the string content is.

## 1.3 Game facts (menus.cpp)

| address / value | meaning |
|---|---|
| `0x105e370` | query table, first entry; 132 entries of 0x18 bytes; `+0` cond u32, `+8` fn u64 absolute |
| `0x655c40` | the function the first entry (cond 50, BUTTON_MULTIPLAYER_STARTMATCH) must point at |
| `0x104f1c8` | netInfo |
| netInfo `+0x10` | s32: the live-data count the status line query reads |
| netInfo `+0x298` | the local online id: 16 chars + NUL (17 bytes) |
| netInfo `+0x2c4` | s32: mode; 1 = matchmaking |
| `0x113b760` | NetGameManager (ngm) |
| ngm `+0x1ca0` | s16: index of the current net game in `*net-games*`; < 0 when not in Multiplayer |
| ngm `+0x20a0` | u64[32]: absolute pointers to playlist records |
| ngm `+0x21a0` | u32: number of playlist records |
| `0x11474f8` | NetPlayerData (npd) |
| npd `+0x104` | u32: the chosen playlist id (saved with the profile) |
| `0x1066660` | the text database object (its address is passed, not its content) |
| `0x14ce97c` | s32 NET_SM; multiplayer when > 1 |
| `0xcddc00` | guest `const char *TextFind(void *db, u32 id)`: text or NULL |
| `0xa15e70` | guest `void *DcLookup(u32 id)`: DC symbol by string id, or NULL |
| `0x63dc30` | guest `void SetNetGame(void *ngm, s16 index, const void *overrides, int n)` |
| `0x3b9a067d` | DC id `*net-games*`: `{s32 count; s32 pad; u8 *records}`; record stride 0x90; record `+0x0c` s32 game type index |
| `0xf985c7b6` | DC id game types: same table shape; record stride 0x44; `+0` u32 name text id, `+4` u32 description text id, `+8` u32 image id |
| text `0xba8db526` | "[A] players in party" |
| text `0xac182308` | "Party Leader" |
| text `0x5614c769` | name text of the cinema game type (Machinima) |

Query conditions and functions:

| cond | control | function | way in | host behaviour |
|---|---|---|---|---|
| 133 | PAGE_MULTIPLAYER profile panel | `0x65e890` | wrap | 1.5 |
| 49 | party status line | `0x659a90` | wrap | 1.6 |
| 55 | PAGE_LOBBY PLAYLIST | `0x6562d0` | wrap | 1.7 |
| 39 | LIST_PLAYERSMET | `0x65bcd0` | wrap | 1.10 |
| 104 | voice volume slider | `0x65f3f0` | wrap | 1.12 |
| 36 | LIST_FRIENDS | `0x65bbb0` (`ret` stub) | replace | 1.9 |
| 3 | join game list | `0x659590` (`ret` stub) | replace | 1.11 |
| 105, 77, 78, 134, 40, 41, 80, 82, 135, 108, 109, 110 | Profile page queries | whatever the table holds | wrap (diagnostic trace) | 1.13 |

## 1.4 Common behaviour

### Game text

"Game text of id T" means: guest call `TextFind(B + 0x1066660, T)`. If it returns NULL, the text
is the empty string.

### Plain list behaviour

Several queries behave as a plain list with `count` rows and a current index `cur` that the
implementation keeps per list (initially 0, process lifetime). For a given op:

- op 2: let `r` = s32 `q+4`. If u32 `q+0x54` == 0 and 0 <= r < count, `q+0x40` = the row text of
  row r (the row producer is called only in this case). Otherwise `q+0x40` = an empty string.
- op 4: `q+4` = -1 when count is 0; else `cur` when `cur < count` (signed compare; a negative
  `cur` is returned as it is); else `count - 1`.
- op 5: `q+4` = count - 1.
- op 7: `q+4` = 1.
- op 8: `cur` = 0.
- op 9: `cur` = s32 `q+4`.
- any other op (including 1 and 3): nothing is written.

Each list has its own `cur`: one for LIST_FRIENDS (shared between its online and LAN forms), one
for LIST_PLAYERSMET, one for the join list (shared between its online and LAN forms).

### Formatting a count into a text

"Format count V into text T": take the game text of T, replace the **first** occurrence of the
three characters `[A]` with V in decimal (`-` sign for negative values, no padding), leave the
rest unchanged (no `[A]`: unchanged). Store the result in a 96-byte buffer, truncated to 95 bytes
plus NUL.

## 1.5 cond 133: profile panel (wrap of 0x65e890)

On every call, whatever the op:

1. Write the local name to netInfo+0x298: set the 17 bytes netInfo+0x298..+0x2a8 to 0, then copy
   the first min(len, 16) bytes of `session::local_name()` there.
2. Call the original function with `q`.

## 1.6 cond 49: party status line (wrap of 0x659a90)

On every call: save s32 netInfo+0x10, write `session::member_count()` there, call the original
with `q`, then write the saved value back.

## 1.7 cond 55: PLAYLIST (wrap of 0x6562d0)

### Playlist record (host-owned memory)

The game reads playlist records through the pointer array at ngm+0x20a0. Records are host memory
with process lifetime, at most 32 of them. Layout (0x6c bytes used; all bytes not listed are 0):

| offset | type | value |
|---|---|---|
| `+0x00` | u32 | playlist id (also what op 3 type 0x25 / 0x24 descriptors carry as value) |
| `+0x04` | u32 | name text id |
| `+0x08` | u32 | description text id |
| `+0x10` | u32 | picture image id (read by the type 0x24 / 0x25 drawing in menuimages, section 2.9) |
| `+0x28` | u64 | game list pointer: 0 (any) |
| `+0x30` | u32 | ranked: 0 |
| `+0x34` | s32 | skill board: -1 |
| `+0x40` | s32 | the game type index the lobby uses (first of the type list) |
| `+0x44` | u32 | picture image id (same value as +0x10) |
| `+0x48` | s32[8] | game type indices of the playlist (unused slots 0) |
| `+0x68` | s32 | number of game type indices (1..8) |

The game's own readers use +0x00..+0x37. Keep +0x38..+0x6b as listed so any other reader of the
record sees the same bytes.

### Playlist definitions

Eleven definitions, in this order. "types" are game type **name text ids**; "pictures" is a
comma-separated list of picture names, each meaning image id `sid("general/net/playlists/<name>")`.
The playlist id is `sid("u2mp/playlist/<key>")`. A description of 0 means: the description text
(`+4`) of the playlist's first game type record.

| # | key (id) | name text | description text | pictures | types (name text ids, in order) |
|---|---|---|---|---|---|
| 1 | all-competitive (`013dc564`) | `sid("u2mp/playlist/all-competitive")` = `013dc564` | `sid("u2mp/playlist/all-competitive/desc")` = `a2d9233c` | grab-bag | a30341e7, 8dfdede9, 3d9470a5, 0544e6f6, 13f76fcc, 71ede2dc, f625a024, 3b8de400 |
| 2 | ranked (`d96118bb`) | 47b24464 | 57fc82aa | mlg | same 8 as #1 |
| 3 | deathmatch (`a14747a2`) | a30341e7 | fc8dd900 | deathmatch | a30341e7, 8dfdede9, 3b8de400 |
| 4 | elimination (`b81c4496`) | 3b8de400 | `sid("u2mp/playlist/elimination/desc")` = `afdefe07` | elimination | 3b8de400 |
| 5 | objectives (`72a5f8c5`) | 4bf30e73 | 675ef552 | objective | 3d9470a5, 0544e6f6, 13f76fcc, 71ede2dc, f625a024 |
| 6 | plunder (`e16fbf61`) | 3d9470a5 | 32cc1acb | plunder | 3d9470a5 |
| 7 | coop-objective (`e0c85229`) | 5136a578 | c35e16e8 | coop | 5136a578 |
| 8 | coop-arena (`03fdfa7b`) | 2ee192ab | 0 | coop, survival, default | 2ee192ab |
| 9 | gold-rush (`6590fddd`) | 00248bb5 | 0 | default, survival, coop | 00248bb5, e9995f4c |
| 10 | survival (`a6481abb`) | 2959eca4 | 0 | survival, default, coop | 2959eca4, b81a7c18 |
| 11 | the-lab (`811585a4`) | `sid("u2mp/playlist/the-lab")` = `811585a4` | `sid("u2mp/playlist/the-lab/desc")` = `09af6e6f` | the-lab, default | a30341e7, 8dfdede9 |

Note: in #1 and #11 the name text id equals the playlist id (both are sids of the same string).

Picture image ids: grab-bag `f268b146`, mlg `a756f41a`, deathmatch `f892a814`, elimination
`37ada99e`, objective `9dd25791`, plunder `4f7d9fcd`, coop `e80c4be1`, survival `79c940bf`,
default `b1d3d87b`, the-lab `2f07a508`.

Picture choice for one definition: walk its picture list in order; take the first picture whose
id is not the picture (+0x10) of a playlist already built in this build pass. If every one is
taken, take the first of the list. (With all eleven built: #8 gets survival, #9 default, #10
survival again.)

### Building the playlists

Done at the start of every cond 55 call:

1. If u32 ngm+0x21a0 != 0: do nothing more.
2. Else, if playlists were built before (host count > 0): write the record pointers into
   ngm+0x20a0[0..n-1] (u64 absolute, in record order) and set ngm+0x21a0 = n. No log. Done.
3. Else: guest call `DcLookup(0x3b9a067d)` then `DcLookup(0xf985c7b6)` (both always, in this
   order). If either is NULL, or either count <= 0, or either records pointer is NULL: stop (no
   log; the next call tries again).
4. Playable types: for each net game i = 0 .. games.count-1 in order, read its type index `t`
   (s32 at record+0x0c). If 0 <= t < types.count, call `dlc_net_game_available(i)`; if true, type
   t is playable. (Out-of-range types do not call it.)
5. For each definition in order, while fewer than 32 playlists are built:
   - Collect type indices: for each wanted name text of the definition, in order (stop at the
     first 0 or after 8), for each type index t = 0 .. types.count-1 ascending: if t is playable
     and u32 at type record t (+0) equals the wanted text, add t unless it is already in the list
     or the list already has 8 entries.
   - No type collected: skip the definition (no log).
   - Otherwise build a record: +0 id, +4 name, +8 description (definition's, or u32 at +4 of the
     type record of the first collected type when the definition's is 0), +0x10 and +0x44 the
     picture, +0x34 = -1, +0x40 = first collected type, +0x48.. the collected types, +0x68 their
     count, all else 0.
   - Log `"menus: playlist %s -> picture %08x"` with the key and the picture id.
6. If at least one playlist was built: write the pointers and the count as in step 2, then log
   `"menus: %d PS3 playlists"` with the count. Done.
7. Fallback (no definition matched): for each net game i = 0 .. games.count-1 in order, while
   fewer than 32 playlists are built:
   - t = s32 record+0x0c. If t < 0 or t >= types.count: skip (no dlc call). Else call
     `dlc_net_game_available(i)`; false: skip.
   - If a playlist of this pass already has type t (+0x40): skip.
   - If the type record's name text (+0) is `0x5614c769`: skip.
   - Build a record: name = type record +0, description = type record +4, id = type record +8.
     If that id is 0 or equals the id of a playlist built before it in this pass, id =
     `0x55320000 + t`. +0x34 = -1, +0x40 = t, +0x48 = {t}, +0x68 = 1, picture (+0x10 and +0x44)
     by the name text:
     - a30341e7, 8dfdede9, 3b8de400: deathmatch picture
     - 3d9470a5, 0544e6f6, 13f76fcc, 8f86cb5f, 4bf30e73: objective picture
     - 2ee192ab, 5136a578, 00248bb5, e9995f4c: coop picture
     - 2959eca4, b81a7c18: survival picture
     - anything else: default picture
   - Then reorder: playlists whose picture is the deathmatch picture first, the rest after; the
     relative order inside each group stays.
   - Write the pointers and the count as in step 2 (even when the count is 0), then log
     `"menus: %d playlists from %d net games"` with the playlist count and games.count.
   - Edge case to keep: when the fallback builds nothing, the count written is 0, so every later
     cond 55 call runs steps 3..7 again and logs this line again.

### Applying the chosen playlist

"Apply" (only from op 4, see below). The implementation keeps `applied` (u32, initially 0).

1. If `session::is_host()` is false, or s32 netInfo+0x2c4 != 1: stop.
2. `id` = u32 npd+0x104. If id == applied: stop.
3. Find the built playlist with this id (if several match, the last one). None: stop.
4. Guest call `DcLookup(0x3b9a067d)`. NULL or records NULL: stop. (The count is not checked.)
5. `cur` = s16 ngm+0x1ca0. If 0 <= cur < games.count and the type of game `cur` equals the
   playlist's +0x40 type and `dlc_net_game_available(cur)` is true (called only if the type
   matched): applied = id; stop. No log, no SetNetGame.
6. Otherwise, for i = 0 .. games.count-1 in order: if the type of game i equals the playlist's
   type, call `dlc_net_game_available(i)` (only when the type matches); on the first true: guest
   call `SetNetGame(B + 0x113b760, (s16)i, NULL, 10)` (second argument sign-extended to 32 bits
   in esi), applied = id, log `"menus: playlist %08x -> net game %d"` with id and i, stop.
7. No game found: applied stays unchanged.

### The cond 55 entry point

1. Build the playlists (above).
2. If s16 ngm+0x1ca0 < 0: applied = 0.
3. Call the original 0x6562d0 with `q`.
4. If u32 `q+0` == 3 and u32 `q+8` == 0x25: let row = u32 `q+4`. If row < number of built
   playlists (host count), write the picture as a net image descriptor (below) for record `row`
   (host record order, which is also the pointer array order). Return in both cases.
5. If u32 `q+0` != 4 or no playlist is built: return.
6. If s32 `q+4` < 0: npd+0x104 = id of the first record, `q+4` = 0.
7. Apply.

Net image descriptor written into `q` (only these bytes; `q+0x0c..+0x0f` are left as they are):

| field | value |
|---|---|
| `q+0x08` u32 | 3 |
| `q+0x10` u64 | 0 |
| `q+0x18` u32 | 0 |
| `q+0x1c` u32 | the image id |
| `q+0x20` u32 | 1 (selector: net image table) |
| `q+0x24` s32 | -1 (look up by id) |
| `q+0x28` u32 | 0 |
| `q+0x2c` s32 | -1 |
| `q+0x30` f32 | 1.0 |

### `int32_t playlist_pick_type()`

id = u32 npd+0x104. Take the **first** built record (host order) with this id and a type count
> 0. Return its type entry `types[rand() % count]`, where `rand()` is the C library `std::rand()`
(called exactly once, unseeded here; the result taken as unsigned before the modulo). No match:
return -1 without calling `rand()`.

### `bool playlist_game_types(std::vector<int32_t> &types)`

Clear `types`. id = u32 npd+0x104. For every built record with this id, in host order, append all
its type entries (in order). Return true when `types` is not empty.

## 1.8 Shared state note

Building happens only from the cond 55 entry point. Before the first successful build,
`playlist_pick_type` returns -1 and `playlist_game_types` returns false.

## 1.9 cond 36: LIST_FRIENDS (replaces the `ret` stub 0x65bbb0)

**Online** (`online::active()` true):

1. n = `online::friend_count()` (called first, for every op).
2. op 1: count k = number of i in 0..n-1 with `online::friend_online(i)` true (called for each i
   in order). Format `"Friends (%d of %d online)"` with k, n into a 96-byte title buffer
   (truncated to 95). `q+0x40` = that buffer. Return.
3. Other ops: plain list with count = 1 + n when n > 0, else 2. Row 0 is
   `"Browse Online Games"`. Row r >= 1: when n == 0, `"No RPCN friends yet. Add friends in the launcher."`;
   else the pointer returned by `online::friend_row(r - 1)` (written as is).

**LAN** (`online::active()` false):

1. count = `session::member_count()` (called first, for every op).
2. op 1: format count `count` into text `0xba8db526` (section 1.4) into the title buffer (the
   same buffer as online); `q+0x40` = it. Return.
3. Other ops: plain list with this count. Row r: if r >= 16, empty string (no member_at call).
   Else call `session::member_at(r, id, name)`; false: empty string. True: the text is `name`;
   when id == 1, append `" - "` and the game text of `0xac182308` (TextFind called only then,
   after member_at). Copy into a 96-byte buffer owned by row r (truncated to 95), return it.

## 1.10 cond 39: LIST_PLAYERSMET (wrap of 0x65bcd0)

The original function is **never called** (the entry is redirected, the saved original unused).

1. If op == 8, or the cached name list is empty: cached list = `progression_recent_players(50)`.
2. Plain list over the cached list (count = its size), row r = the r-th name.

(Op 8 thus reloads the list and also resets `cur` to 0. An empty list is reloaded on every call.)

## 1.11 cond 3: join game list (replaces the `ret` stub 0x659590)

1. rows = `online::join_rows()` (called first).
2. rows >= 0: op 1: `q+0x40` = `online::join_title()` (pointer as is). Other ops: plain list with
   count = rows, row r = `online::join_row(r)` (pointer as is).
3. rows < 0: op 1: `q+0x40` = empty string. Other ops: plain list with count 0 (so op 2 gives an
   empty string, op 4 gives -1, op 5 gives -1, op 7 gives 1, op 8 / 9 change `cur`).

## 1.12 cond 104: voice volume slider (wrap of 0x65f3f0)

- op 4 and s32 NET_SM (`0x14ce97c`) > 1: v = `voice::volume()` * 20.0f + 0.5f (float); `q+4` =
  the integer part of v (truncation toward zero), clamped to 0..20. The original is not called.
- op 9 and NET_SM > 1: `voice::set_volume((float)(s32 q+4) * 0.05f)`. The original is not called.
- Otherwise: call the original with `q`.

## 1.13 Diagnostic trace of the Profile page queries

For each cond of the list 105, 77, 78, 134, 40, 41, 80, 82, 135, 108, 109, 110 (in this order),
search the 132 table entries from the first (this search does **not** stop at the `fn == 0`
terminator; it skips entries with `fn == 0`) for the first entry with that cond and `fn != 0`.
None: skip. Else remember its `fn` and point the entry at a tracer of its own (one tracer per
cond in the list; two conds may share one original function, each gets its own tracer).

A tracer, when called with `q`: read u32 op = `q+0` and u32 row = `q+4` before anything else,
call the remembered original with `q`, then, for the first 12 calls of that tracer only, log:

`"profile query cond %u: op %u row %u -> +4 %d +8 %u +0x40 %p +0x54 %u"`

with the cond, op, row (values from before the call), s32 `q+4`, u32 `q+8`, the u64 at `q+0x40`
as a pointer, u32 `q+0x54` (all read after the call). Calls after the 12th are passed through
silently. The trace adds nothing to `installed` and logs nothing at install.

## 1.14 Install: `void install_menus(int &installed)`

In order:

1. Table check: if `0x105e370 + 132 * 0x18 > image.size`, or the first entry's cond != 50, or its
   fn != B + 0x655c40: log `"menus: query table 0x%llx differs, menu queries not installed"` with
   0x105e370 and return.
2. Helper check: `image.executable(0xcddc00, 16)`, `image.executable(0x63dc30, 16)`,
   `image.executable(0xa15e70, 16)`. Any false: log
   `"menus: guest helpers not where expected, menu queries not installed"` and return.
3. Wraps, in this order, each with (cond, expected function, name):
   (133, 0x65e890, "profile panel"), (49, 0x659a90, "party status line"), (55, 0x6562d0,
   "PLAYLIST"), (39, 0x65bcd0, "LIST_PLAYERSMET"), (104, 0x65f3f0, "voice volume slider").
   For each: find the entry for the cond = the first entry, scanning from the first and stopping
   at the first entry with fn == 0, whose cond matches. It qualifies only if that entry's fn ==
   B + expected (a later entry with the same cond is not considered). It must also hold that
   `image.executable(expected, 1)`. Not qualified: log
   `"%s (cond %u): table entry is not 0x%llx, left alone"` (name, cond, expected). Qualified:
   remember the entry's fn as the original, set fn = the host function's absolute address, log
   `"%s (cond %u): 0x%llx wrapped"` (name, cond, expected), `installed += 1`.
4. The diagnostic trace (1.13).
5. Replacements, in this order: (36, 0x65bbb0, "LIST_FRIENDS query"), (3, 0x659590,
   "join game list query"). For each: the entry for the cond (same search as step 3) must exist
   with fn == B + address, and `is_ret_stub(address)` must be true (bytes `c3 66 66 66 66`).
   Otherwise log `"%s at 0x%llx: not the stripped query this port expects, left alone"` (name,
   address) and go on. Else call `replace_function(address, 5, host, name)`; on true,
   `installed += 1`.

Maximum `installed` increase: 7.

---

# Part 2: menuimages.cpp

## 2.1 Purpose and hook points

Three problems on PS4 are solved here:

1. **Net image ids.** The menus ask the net image table for PS3 ids, `sid("general/net/<set>/<name>")`.
   The PS4 net package names its materials `general/net/<set>:<name>`, and the table's ids are the
   sid of that name (up to the first `,`). So no PS3 id is found. The image lookup function
   0x5558c0 is replaced with a version that also finds an entry whose name, read with `:` as `/`,
   has the asked id, and (for net ids) falls back to the imported `net-u2mp` package.
2. **Image size.** PS4 net textures are twice the PS3 resolution, and an image takes its size
   from its texture. Two calls of the size function are retargeted so images from the PS4 net
   table are measured at a scale (default 0.5).
3. **Missing renderer cases.** The menu image renderer has no drawing for image types 0x1e,
   0x22, 0x24, 0x25, 0x27 (and 0x23). Their jump table entries are pointed at a stub that calls a
   host drawing function and then jumps to the renderer's exit.

Plus a diagnostic: the image converter 0x5bc290 is wrapped to log the size of net images.

## 2.2 Game facts (menuimages.cpp)

| address / value | meaning |
|---|---|
| `0x1130008` | image table pointers: `+0x68` hud table, `+0x70` net table, `+0x78` menu table (each u64 absolute, may be NULL) |
| material table | `{u16 ?, u16 count at +2, u32 pad, u64 entries at +8}`; entry 0x28 bytes, `+8` u32 id |
| entry name | entry `+0` -> pointer p1; p1 `+0x10` -> p2; p2 `+0` -> p3; p3 `+0` -> `const char *` "set:name,guid,shader". NULL at any step: no name |
| `0x5558c0` | guest image lookup `u8 *Lookup(u64 id_sel, s32 index)`: id = low 32 bits, selector = high 32 bits (0 hud, 1 net, 2 menu); returns an entry or NULL |
| `0x5bbd90` | guest `float *ImageSize(float out[2], u8 *material)`: writes width, height |
| `0x5bcc1a`, `0x5bdb30` | `call 0x5bbd90` sites (type 3 sizing; flag 0x80 sizing) |
| `0x5bc290` | guest image converter `u64 Convert(u8 *out, const u8 *src, u64 a2, const float *box, const void *colour, u32 a5, float scale)`; out `+0x3c` f32 width, `+0x40` f32 height, `+0x48` u64 material; src `+0` u32 type, `+0x14` u32 id, `+0x18` u32 selector |
| `0xbdce90` | guest textured quad `void DrawQuad(void *ctx, void *info, void *vertices)` |
| `0x5bcd70` | menu image renderer; `0x5bd852` its type dispatch; `0x5c2ec4` jump table (s32 entries relative to `0x5c2ec4`, entry for type t at `0x5c2ec4 + 4*(t-4)`, types 4..0x29); `0x5c1ab1` its exit (stack check and epilogue) |
| `0xa15e70` | guest `void *DcLookup(u32 id)` |
| `0x916e03b0` | DC booster table `{s32 count; pad; u8 *records at +8}`, record 0x20 bytes: `+8` u32 image id, `+0xc` u32 key |
| `0x05454b23`, `0x01845694` | booster keys that mean "no booster" (placeholders) |
| `0x0dec97a4` | DC map table, same shape, record 0x38 bytes: `+0` u32 map id, `+0xc` u32 image id |
| `0x113b760` ngm `+0x20a0` / `+0x21a0` | playlist pointer array / u32 count (section 1.7) |
| package `+0x30` | s32 state; 7 = logged in |
| package `+0x940` | u64: the package's material table |
| `sid("net-u2mp")` = `5e516de8` | package id of the imported extra net package |
| `"/app0/u2data/build/main/pak22/ps4/net-u2mp.pak"` | guest path of that package |
| `sid("general/net/rank-badges/default")` = `bda0f568` | rank badge sheet, ranks 0..63 |
| `sid("general/net/rank-badges/default2")` = `f1b63553` | rank badge sheet, ranks above 63 |
| `UNCHARTED_U2MP_NET_IMAGE_SCALE` | environment variable: net image scale |

### Renderer register state at the dispatch (used by the stub)

At `0x5bd852`, for the redirected types: r11 = the image descriptor (absolute pointer), rbp = the
renderer frame, `[rbp-0x1fe0]` = u64 draw list (the renderer's rsi; first field of a draw
context), `[rbp-0x1ff0..-0x1fe4]` = f32 x, y, w, h of the image box, ebx = c2 << 16,
r14d = c1 << 8, r12d = c0, r13d = c3 << 8 (the four modulated colour bytes), rsp 16-byte aligned.
Descriptor: `+0` u32 type, `+0x10` u32 value, `+0x50` f32 alpha.

## 2.3 Interface

Public (declarations in section 0): `menu_net_material`, `menu_draw_quad`, `extra_net_package`,
`install_menuimages`.

Entry points the game reaches:

| address | how | host function convention |
|---|---|---|
| `0x5558c0` | `replace_function(0x5558c0, 16, ...)`, name `"menu image lookup (PS3 names)"` | `u8 *(u64 id_sel, s32 index)` |
| call at `0x5bcc1a` | `retarget_call(0x5bcc1a, 0x5bbd90, ...)`, name `"net image size (type 3)"` | `float *(float *out, u8 *material)` |
| call at `0x5bdb30` | `retarget_call(0x5bdb30, 0x5bbd90, ...)`, name `"net image size (flag 0x80)"` | same |
| `0x5bc290` | `replace_function(0x5bc290, 18, ...)`, name `"image converter (size log)"`; the original stays callable (2.8) | `u64 (u8*, const u8*, u64, const float*, const void*, u32, float)`; the float in xmm0 |
| jump table entries of types 0x1e, 0x22, 0x23, 0x24, 0x25, 0x27 | rel32 -> stub (2.9) | stub, then host `void (u8 *desc, u8 *frame, u32 c2, u32 c1, u32 c0, u32 c3)` |

## 2.4 Extra package presence: `extra_net_package()`

Decided once per process, at the first call: call `runtime_file_translate(path, buf, 1024)` with
the guest path above; present when it returns 0 and the translated host path names an existing
regular file (a file system error counts as absent). At that first call, log
`"menu images: net-u2mp.pak %s"` with `"imported (PS3 menu images PS4 lacks)"` or
`"not imported"`. Returns the string `"net-u2mp"` when present, else NULL. Later calls do not log.

## 2.5 Image lookup (replaces 0x5558c0)

Input: id = low 32 bits of the first argument, sel = high 32 bits, index = s32 second argument.

1. Table: sel 2 -> `[0x1130008+0x78]`, sel 1 -> `[0x1130008+0x70]`, any other sel -> `[0x1130008+0x68]`.
   NULL table: return NULL.
2. index >= 0: return entry `index` when `(unsigned)index < count`, else NULL (also for sel 0).
3. index < 0:
   - sel not 1 and not 2: return NULL.
   - Search the entries in order for u32 `+8` == id; return the first match.
   - Not found: **PS3-name search** (2.6) gives an entry index or -1.
   - If that gives -1 and sel == 1: **extra search** (2.7) gives an entry or NULL. (sel 2 never
     uses the extra search.)
   - Log, only while the shared budget lasts (48 lines in total for the process, net and menu
     together; thread safe): `"menu images: lookup %s id %08x -> %s (%d)"` with `"net"` when sel
     == 1 else `"menu"`, the id, the outcome (`"found by PS3 name"` when the PS3-name index >= 0,
     else `"found in net-u2mp"` when the extra search found an entry, else `"missing"`), and the
     PS3-name index (-1 when not found).
   - Return the extra entry when there is one; else NULL when the PS3-name index is -1; else the
     entry at that index (with the same range check as step 2).

Lookups may run on several threads at once; the caches below must be safe for that.

## 2.6 PS3-name search

The PS3 id of an entry: the string id of its name up to (not including) the first `,` or the end,
with every `:` read as `/` (same CRC as `sid`). An entry without a name has PS3 id 0. (So a
lookup of id 0 finds the first nameless entry. Keep this.)

Per selector (1 and 2 separately) a cache of the PS3 ids is kept, valid for one (table pointer,
count) pair. When the table pointer or its count differs from the cached pair, recompute the ids
for all entries and log the table:

- `"menu images: %s table %p, %u materials"` with `"net"` (sel 1) or `"menu"` (sel 2), the table
  pointer, the count;
- then, for each of the first min(count, 6) entries i:
  `"menu images:   [%u] id %08x  ps3 id %08x  %.60s"` (three spaces after the colon, two spaces
  between fields) with i, u32 `+8`, the PS3 id (0 when no name), the name or `"(no name)"`.

Result: the first index whose PS3 id equals the asked id, else -1.

## 2.7 Extra search (net-u2mp)

1. If `extra_net_package()` is NULL: nothing.
2. pkg = `package_by_id(0x5e516de8)`. NULL, or s32 pkg+0x30 != 7: nothing.
3. Table = u64 pkg+0x940. NULL, or its entries pointer NULL: nothing.
4. When this table pointer differs from the one last logged here (initially none), log
   `"menu images: net-u2mp table %p, %u materials"` with the pointer and count.
5. Return the first entry with u32 `+8` == id, else nothing.

## 2.8 Image size and converter log

### Net image scale

`scale` = 0.5. At install, if `UNCHARTED_U2MP_NET_IMAGE_SCALE` is set and `atof` of it is > 0,
`scale` = (float)atof(value). (A value <= 0 or unparsable keeps 0.5.)

### Retargeted size calls (0x5bcc1a, 0x5bdb30)

Called with (out, material): guest call `0x5bbd90(out, material)` first (its return value is
ignored). Then, if out is not NULL and the material lies in the PS4 net table (the table at
`[0x1130008+0x70]` is not NULL, its entries pointer is not NULL, and
`entries <= material < entries + 0x28 * count`): out[0] *= scale, out[1] *= scale. Return `out`.

### Converter wrap (0x5bc290)

Called with (out, src, a2, box, colour, a5, scale_arg). Note the return address of this call (the
address the game's `call` would return to).

1. Run the original converter with the same seven arguments; keep its u64 result r. Return r at
   the end in every case.
2. Stop (return r) when out or src is NULL, u32 src+0 != 3, or u32 src+0x18 != 1.
3. id = u32 src+0x14. caller = return address - B when it lies inside [B, B + image.size), else
   the absolute return address.
4. A set of logged (caller, id) pairs, at most 96 (thread safe): if the set already holds 96
   pairs, or holds this pair: stop. Else add the pair.
5. m = u64 out+0x48. tex = {0.0, 0.0}. If m is not NULL: guest call `0x5bbd90(tex, m)` (the
   original size function, not the scaled one).
6. Log `"menu images: size id %08x from 0x%llx: scale %.3f box %.0fx%.0f texture %.0fx%.0f (%s) -> %.1fx%.1f"`
   with id, caller, scale_arg, box[0] and box[1] (both -1 when box is NULL), tex[0], tex[1], the
   kind (`"no material"` when m is NULL, `"PS4 net.pak"` when m lies in the PS4 net table as
   defined above, else `"net-u2mp"`), f32 out+0x3c, f32 out+0x40.

## 2.9 Drawing the missing image types

### The stub

Executable memory within rel32 reach of the jump table (from `near_stub`), entered by the
renderer's `jmp` with the register state of 2.2. It must:

1. Call the host draw function (System V) with rdi = r11, rsi = rbp - 0x1fe0, edx = ebx,
   ecx = r14d, r8d = r12d, r9d = r13d. (rsp is 16-byte aligned at entry, so a plain `call`
   keeps the ABI alignment.)
2. Then jump to B + 0x5c1ab1.

The current stub is these 45 bytes (any equivalent code is fine):
`4c 89 df` `48 8d b5 20 e0 ff ff` `89 da` `44 89 f1` `45 89 e0` `45 89 e9`
`48 b8 <host fn, 8 bytes>` `ff d0` `48 b8 <B+0x5c1ab1, 8 bytes>` `ff e0`.
The renderer relies only on callee-saved registers and rbp after the exit, which a System V host
function preserves.

### Host draw function: inputs

From the arguments (desc, frame, c2, c1, c0, c3):

- list = u64 at frame+0; x, y, w, h = f32 at frame-0x10, -0x0c, -0x08, -0x04.
- alpha = f32 desc+0x50.
- colour r = (float)((c2 >> 16) & 0xff) / 255.0f, g = (float)((c1 >> 8) & 0xff) / 255.0f,
  b = (float)(c0 & 0xff) / 255.0f, a = ((float)((c3 >> 8) & 0xff) / 255.0f) * alpha.
- type = u32 desc+0, value = u32 desc+0x10.

### Net material

"Net material of id": NULL when id is 0; else the result of the image lookup (2.5) with
sel 1, that id and index -1 (host call, no guest call).

### Quad drawing

"Draw quad (material, x, y, w, h, u0, v0, u1, v1)" with a colour: nothing when material is NULL,
or w <= 0, or h <= 0 (a NaN passes these tests). Otherwise one guest call `DrawQuad(ctx, info, verts)`:

- ctx: 16-byte aligned, 48 bytes: u64 list, then five u64 0.
- info: 16-byte aligned, 32 bytes: u64 material, then three u64 0.
- verts: 16-byte aligned, 0x110 bytes, all 0 except four vertices of 0x40 bytes at 0, 0x40, 0x80,
  0xc0. Vertex: `+0` f32 x, `+4` f32 y, `+8` f32 z = 0, `+0xc` f32 u, `+0x10` f32 v,
  `+0x20..+0x2f` f32 r, g, b, a; the rest 0. The four vertices, in order:
  (x, y, u0, v0), (x + w, y, u1, v0), (x + w, y + h, u1, v1), (x, y + h, u0, v1).
  The 16 bytes after the fourth vertex are 0.

### Per type

- **0x27 rank badge**: index = (s32)value. id = `f1b63553` when index > 63, else `bda0f568`.
  m = net material of id. u = (float)(index & 7) * 0.125f, v = (float)((index & 63) / 8) * 0.125f
  (bit masks on the two's complement value, so negative indices give non-negative cells and use
  the first sheet). Draw quad (m, x, y, w, h, u, v, u + 0.125f, v + 0.125f).
- **0x1e booster pair**: bottom = y + h, half = w * 0.5f. Image a = booster image of
  (s32)(value & 0xffff); image b = booster image of (s32)(value >> 16) (in this order). The
  logged id is a. m = net material of a. Draw quad (m, x, y * 0.72f + bottom * 0.28f - half * 0.5f,
  w, half, 0, 0, 1, 1) (computed as ((y*0.72f) + (bottom*0.28f)) - (half*0.5f)). Then net material
  of b and draw quad (it, x, y * 0.28f + bottom * 0.72f - half * 0.5f, w, half, 0, 0, 1, 1) (same
  evaluation order).
  - Booster image of i: guest call `DcLookup(0x916e03b0)` (always, first). Result 0 when the
    table is NULL, i < 0, or s32 count <= i, or the records pointer is NULL. Else the record i
    (0x20 bytes): 0 when its u32 `+0xc` is `0x05454b23` or `0x01845694`, else u32 `+8`.
- **0x22 map picture**: id = map image of value: guest call `DcLookup(0x0dec97a4)`; NULL -> 0;
  else the first of count records (records pointer not NULL) with u32 `+0` == value gives u32
  `+0xc`; none -> 0. m = net material of id. Draw quad (m, x, y, w, h, 0.08984375f, 0,
  0.91015625f, 1).
- **0x24, 0x25 playlist picture**: id = the u32 `+0x10` of the first guest playlist record (from
  ngm+0x20a0[i], i < min(u32 ngm+0x21a0, 32), skipping NULL pointers) whose u32 `+0` == value;
  none -> 0. No guest call. m = net material of id. hh = h for 0x24; for 0x25 hh = max(0, h - 50.0f).
  Draw quad (m, x, y, w, hh, 0.0703125f, 0, 0.9296875f, 1).
- **any other type (0x23)**: nothing is drawn; id 0, no material.

Then, while the shared draw-log budget lasts (24 lines for the process, thread safe), log
`"menu images: draw type 0x%x value %08x image %08x %s at %.0f,%.0f size %.0fx%.0f"` with type,
value, id, `"found"` when the material (for 0x1e: the first booster's) is not NULL else
`"missing"`, x, y, w, h (the unadjusted frame values).

### `menu_net_material(id)`

The net material of id (as above).

### `menu_draw_quad(material, x, y, w, h, u0, v0, u1, v1, alpha, rgb)`

Draw quad with list = NULL and colour (rgb, rgb, rgb, alpha), same rules (no call when material is
NULL or w <= 0 or h <= 0).

## 2.10 Install: `void install_menuimages(int &installed)`

In order:

1. **Scale.** Read the environment variable (2.8). If scale != 1.0f: `retarget_call(0x5bcc1a,
   0x5bbd90, size host, "net image size (type 3)")`; only if that succeeded,
   `retarget_call(0x5bdb30, 0x5bbd90, size host, "net image size (flag 0x80)")`. If both
   succeeded: log `"menu images: PS4 net.pak images measured at %.2f of their texture size (PS3 resolution)"`
   with the scale (as double) and `installed += 1`. (If only the first succeeded, it stays
   retargeted; no log, no count.) scale == 1.0f: nothing is retargeted, nothing logged.
2. **Converter.** The 18 bytes at 0x5bc290 must be
   `55 48 89 e5 41 57 41 56 41 54 53 48 81 ec e0 00 00 00` and stub memory must be available.
   The original must stay callable: currently those 18 bytes (position independent) are copied
   to a 48-byte `near_stub` block followed by `ff 25 00 00 00 00` and the 8-byte absolute address
   B + 0x5bc290 + 18. Then `replace_function(0x5bc290, 18, wrap host, "image converter (size log)")`;
   on true `installed += 1`. Bytes differ or no stub memory: log
   `"menu images: converter 0x%llx differs, sizes not logged"` with 0x5bc290.
3. **Lookup.** The 16 bytes at 0x5558c0 must be
   `55 48 89 e5 41 56 53 89 f3 48 89 f8 48 c1 e8 20`. If they match and
   `replace_function(0x5558c0, 16, lookup host, "menu image lookup (PS3 names)")` returns true:
   `installed += 1`. Otherwise (mismatch or failure): log
   `"menu images: lookup 0x%llx differs, left alone"` with 0x5558c0.
4. **Renderer.** All of these must hold, else log
   `"menu images: renderer 0x5bcd70 differs, missing image types not added"` and return:
   - 36 bytes at 0x5bd852: `83 c0 fc 83 f8 25 0f 87 53 42 00 00 4c 8d bd 30 e0 ff ff 49 ba 00 00 00 00 01 00 00 00 48 8d 0d 4e 56 00 00`;
   - 7 bytes at 0x5c1ab1: `48 8b 05 48 91 a8 00`;
   - 16 bytes at 0xbdce90: `55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 40 49`;
   - `0x5c2ec4 + 0x26 * 4 <= image.size`.
   Then for types 0x1e, 0x22, 0x23, 0x24, 0x25, 0x27 in this order: the s32 entry must satisfy
   `0x5c2ec4 + entry == 0x5c1ab1`. The first that does not: log
   `"menu images: jump table entry of type 0x%x is not the exit, left alone"` with the type and
   return (no entry changed).
   Get stub memory (`near_stub(64)`); none: log `"menu images: no stub memory near the image"`,
   return. If `stub - (B + 0x5c2ec4)` does not fit in s32: log
   `"menu images: stub out of jump-table range"`, return. Write the stub (2.9), then set each of
   the six entries to that s32 difference. Log
   `"menu images: renderer types 0x1e, 0x22..0x25, 0x27 -> C++ (stub %p)"` with the stub address,
   `installed += 1`.

Maximum `installed` increase: 4.

---

## 3. Test scenarios

General: run old and new code on the same fake image; compare guest calls (function, arguments,
float bits, order), log lines, image memory afterwards, and the strings behind `q+0x40` (not the
pointer values). Host stubs (session, online, voice, progression, dlc, package_by_id,
runtime_file_translate) must return the same scripted values for both, and the tester should
record their calls too.

### menus install

1. Good table: all 5 wraps and 2 replacements; `installed` + 7; the wrap and `-> C++` log lines.
2. First entry cond != 50, or fn != B+0x655c40, or image too small: only the "table differs" line.
3. One helper not executable (each of the three): only the "guest helpers" line.
4. Wrap entry fn wrong (e.g. cond 55 points elsewhere), and a second later entry with cond 55
   that is right: still "left alone" (first entry only). Entry after the `fn == 0` terminator:
   not found.
5. Stub not `ret` (bytes differ) or table entry not pointing at it: the "not the stripped query" line.
6. Trace: a traced cond placed after the terminator is still traced; a traced cond with fn 0 is
   skipped; call one traced query 13 times: 12 log lines.

### Query behaviour

7. cond 133: local name of 0, 5, 16 and 20 chars; check the 17 bytes at netInfo+0x298, then the
   original's call.
8. cond 49: netInfo+0x10 seen by the original = member count; restored afterwards.
9. cond 36 online: n = 0 (title "Friends (0 of 0 online)", rows "Browse Online Games", "No RPCN
   friends yet..."), n = 3 with 2 online; op 4 / 5 / 7 / 8 / 9 sequences, negative op 9 value,
   column != 0, row out of range.
10. cond 36 LAN: count 0, 1, 3, 17; row with id 1 (leader suffix, TextFind call) and TextFind
    returning NULL (suffix `" - "` only); member_at false; title text with no `[A]`, with two
    `[A]`, and longer than 95 chars.
11. cond 39: empty store (reload every call), op 8 reload, rows out of range; check the original
    is never called.
12. cond 3: join_rows -1 (all ops) and >= 0 (title and rows).
13. cond 104: NET_SM 0 and 2; op 4 with volume 0, 0.5, 1.0, 1.04, -0.1, 2.0; op 9 with 0, 7, 20,
    -3; op 5 passes to the original.

### Playlists

14. All game types playable: 11 records, picture log lines (check coop-arena -> survival,
    gold-rush -> default, survival -> survival again), "11 PS3 playlists", pointer array and count.
15. Only some types playable (dlc false for some games, out-of-range type indices): skipped
    definitions, descriptions taken from the type record where the definition has 0, more than 8
    matching types for one definition, duplicate name ids across several type records.
16. No definition matches (type names unknown): fallback, Machinima skipped, duplicate and zero
    image ids replaced by 0x55320000 + t, deathmatch-picture records sorted first, the "from N
    net games" line. Fallback with nothing playable: count 0 and the line repeated on every call.
17. DcLookup returning NULL / count 0 / NULL records for either table: no build, retried.
18. Count cleared by the game after a build: pointers rewritten without a log; count set by
    someone else: no build.
19. op 3 with `q+8` == 0x25 for row 0, last row, row == count, row 0xffffffff; op 3 with another
    image type; check the descriptor bytes and that `q+0x0c..0x0f` keep their value.
20. op 4: saved id not ours (`q+4` < 0 from the original) -> npd+0x104 and `q+4` rewritten;
    apply as host / client, mode 1 / 0; current game already of the type (no SetNetGame);
    SetNetGame call with index and args; no available game of the type; ngm+0x1ca0 < 0 resets
    applied so the same id is applied again.
21. `playlist_pick_type`: before build (-1, no rand call), with a fixed `srand` seed and several
    calls; `playlist_game_types` with a chosen id and with an unknown id.

### menuimages install

22. Default env: all four parts installed, `installed` + 4, all lines. Env "1": no retarget, no
    scale line, `installed` + 3. Env "0", "-2", "abc": scale 0.5. Env "0.75": scale line "0.75".
23. Wrong bytes at 0x5bcc1a (not e8 / other target) for the first and for the second retarget.
24. Converter bytes differ; lookup bytes differ; each renderer check failing; one jump entry
    (e.g. 0x25) not the exit: no entry changed.

### Lookup

25. sel 0/1/2 with index in and out of range; NULL table; index -1 with sel 0 and 3.
26. Id present directly; present only as a `:` name (cache built, table dump with <= 6 and > 6
    entries, nameless entries, names with and without `,`); id 0 with a nameless entry; table
    pointer or count changing (dump again).
27. net id missing in PS4 with net-u2mp absent, present but state != 7, present with the id
    (the "net-u2mp table" line once per table pointer); menu id missing (no extra search).
28. More than 48 misses: the lookup log stops.

### Size and converter

29. Size call with a PS4 net material (scaled), a net-u2mp or hud material (unchanged), NULL out.
30. Converter calls: type 3 sel 1 (logged once per caller/id), other type or selector (not
    logged), NULL box, NULL material at out+0x48, caller outside the image, 97 distinct pairs.

### Drawing

31. Each type 0x1e, 0x22, 0x23, 0x24, 0x25, 0x27 through the jump table with a scripted frame,
    descriptor and colour registers; check DcLookup and DrawQuad calls and their buffers.
32. 0x27 with index 0, 9, 63, 64, 100, -1. 0x1e with placeholder keys, index beyond count,
    NULL table. 0x22 with an unknown map. 0x24 / 0x25 with no matching playlist, h = 30 and
    h = 80 for 0x25. w = 0 or h = 0 (no DrawQuad). 25 draws: the draw log stops after 24.
33. `menu_draw_quad` with and without a material; `menu_net_material(0)` returns NULL.
34. `extra_net_package` twice: one log line; translate failing; path a directory.

## Clarifications found by the differential test

- LIST_FRIENDS online (1.9 step 3): the count is 2 only when friend_count() is 0, else 1 + n, also for negative n.
- cond 104 op 4 (1.12): v converts to s32 like x86 truncation: a v that does not fit (>= 2^31, +inf, NaN) is INT_MIN,
  which the clamp turns into 0.
- type 0x1e (2.9): the offset half*0.5f is negated as its own step and added, so a NaN width gives a NaN y with the
  sign bit flipped (finite results are the same as the subtraction).
