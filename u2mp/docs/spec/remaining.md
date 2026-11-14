# Spec: remaining.cpp and clantag.cpp (u2mp)

Clean-room description of two files in `gow3_pc/src/u2mp/`. It says what the code must do, as the
game and the tester can see it. It does not say how to structure the code.

All addresses are image offsets of Uncharted 2 PS4 1.02 (CUSA02320 / CUSA02344). "Guest" means the
game image. Guest functions use the System V x86-64 convention (`U2MP_GUEST` in `u2mp_internal.h`).
All guest integers are little-endian.

Shared helpers from other files (use them, do not reimplement them):

- `image`, `Image::matches`, `Image::executable`, `near_stub`, `replace_function`, `retarget_call`,
  `log`, `sid`, `is_ret_stub`, `addr::menu_manager` (0x1136640), `package_by_id`, `show_page`
  (`u2mp_internal.h`, `install.cpp`, `frontend.cpp`, `session.cpp`).
- `log` prefixes every line with `u2mp: ` and adds a newline. All log texts below are the part
  after that prefix. `%llx` and `%u` / `%d` are printf conversions.
- `replace_function(offset, length, host, name)` writes a jump to `host` at `offset`, fills bytes
  5..length-1 with 0xCC, and logs `"%s: 0x%llx -> C++"` on success. On failure it logs one of its
  own lines (see `install.cpp`) and returns false.
- `retarget_call(offset, expected, host, name)` points an `E8` call at `host` and logs
  `"%s: call at 0x%llx -> C++"` on success, or its own failure line.
- `show_page(page)` calls the guest ShowPage 0x5cb990 with (menu manager, page id, 0).
- Text functions (`texts.h`): `text_set_player`, `text_set_slot`, `own_text`, `text_find`,
  `text_find_original`, constants `kTextPlayer`, `kTextSlot[8]`, `kTextSlots` (8) and the
  `kText*` ids listed below.
- Session (`session_api.h`): `session::member_at`, `member_name`, `local_id`, `is_host`, `kick`,
  `local_name`.
- Online (`online.h`): `online::active`, `linked`, `promote`, `open_join_list`, `friend_count`,
  `friend_row`, `friend_name`, `friend_kind`, `friend_online`, `friend_joinable`, `join_friend`,
  `invite_friend`, `invite_count`, `friend_add`, `accept_request`, `remove_friend`,
  `profile_text`, `send_cinema`, `cinema_received`, and `kRowFriend` 0, `kRowRequestIn` 1,
  `kRowRequestOut` 2.
- Voice (`voice.h`): `voice::enabled`, `voice::toggle_mute`.
- Progression (`progression.h`): `progression_publish`, `progression_recent_players`,
  `progression_player_summary`.
- Runtime text input (C linkage, declared by the user of it, implemented in
  `gpu/shim/gow3gpu.cpp`): `int gow3gpu_text_input_begin(const char *initial_utf8, const char
  *prompt_utf8)` returns non-zero when a text box opened; `int gow3gpu_text_input_poll(char *out_utf8,
  uint64_t size)` returns 0 while open, 1 when accepted (text written, NUL-terminated, cut to
  size-1), and another non-zero value (2) when cancelled or no window exists.

---

## 1. remaining.cpp

### 1.1 Purpose

The PS4 1.02 build left a set of multiplayer functions as a bare `ret` (or cut them short), and a
few code sites read address 0 because a PS4 accessor was folded to NULL. This file:

- restores two net event Applies: NetEventNpcDealDamage (event 44) and NetEventKickGriefer (event 69);
- gives the frontend menu callbacks of the party, player, friends, players-met, invites, clan tag,
  store, Twitter and cinema-sharing buttons PC behaviour (popups, message boxes, the session and
  online layers);
- fixes four NULL-read sites (level record package, tracker values, empty tag table search,
  NetStatGame host name);
- hooks TextDb::Find so PC texts can be served (`texts.cpp` provides the hook body);
- provides the one-button message box `show_notice` and the cinema friend picker to other files.

It is installed once, from `u2mp_install` (`install.cpp`), after `install_cinema` and before
`install_menus`. `install_spawn` runs later and retargets the call at 0x662fd0, which calls
0x644130; the replacement of 0x644130 made here must therefore keep 0x644130 as a valid entry.

### 1.2 Interface

Declarations other files use (namespace `u2mp`):

| Declaration | Header | Users |
|---|---|---|
| `void install_remaining(int &installed);` | `u2mp_internal.h` | `install.cpp` |
| `void show_notice(const char *text);` | `online.h` | `online.cpp`, `session.cpp` |
| `void cinema_pick_friend();` | `cinema.h` | `cinema.cpp` (upload button) |

The current code also gives the 0x644130 replacement external linkage in namespace `u2mp`, but no
header declares it and nothing calls it by name. It is not part of the contract.

Entry points the game reaches. All are System V functions. "arg" is the first argument (rdi).
Menu callbacks have three pointer arguments (arg, unused, unused) and return nothing unless noted.

| Address | Install check | Kind | Name used in logs | Behaviour (1.4) |
|---|---|---|---|---|
| 0x644130 | 14 bytes `55 48 89 E5 41 56 53 48 83 EC 10 48 89 FB` | replace_function, length 14 | `level record package (644130)` | B1 |
| 0x640598 | 9 bytes `BF 00 00 00 00 48 0F 45 FE` | in-place byte patch | - | B2 |
| 0x4bf44c | 10 bytes `48 8B 02 8B 4A 08 31 D2 EB 0A` | jump to own stub | - | B3 |
| 0x66c75f | call site check (see 1.5) | retarget_call, expected target 0xde6490 (strlen) | `NetStatGame host name length` | B4 |
| 0x66c76d | call site check (see 1.5) | retarget_call, expected target 0xde6410 (memcpy) | `NetStatGame host name copy` | B4 |
| 0xcddc00 | 12 bytes `55 48 89 E5 41 56 53 89 F3 49 89 FE` | detour that keeps the original | `TextDb::Find` | B5 |
| 0x61fa70 | 11 bytes `55 48 89 E5 48 83 EC 10 4C 8B 05` plus kill-info check | replace_function, length 5 | `NetEventNpcDealDamage::Apply` | B6 |
| 0x5ebee0 | 20 bytes `55 48 89 E5 53 50 48 8D 05 CB 30 A7 00 80 B8 C8 00 00 00 00` | replace_function, length 5 | `Invite To Party (45a9c65b)` | B20 |
| 0x659b50 | 14 bytes `55 48 89 E5 53 50 48 89 FB 8B 03 83 F8 02` | replace_function, length 5 | `BUTTON_MULTIPLAYER_INVITES query (cond 48)` | B21 |
| 0x5ebdf0 | ShowPopup check and 12 bytes `55 48 89 E5 53 48 83 EC 58 48 8B 1D` | replace_function, length 5 | `LIST_PLAYERSMET row (369314b2)` | B11 |
| 0x61b900 | ret stub | replace_function, length 5 | `NetEventKickGriefer::Apply` | B7 |
| 0x5ec490 | ShowPopup check, ret stub | same | `BUTTON_MULTIPLAYER_PARTY_PLAYERn` | B8 |
| 0x5ec460 | ShowPopup check, ret stub | same | `BUTTON_MULTIPLAYER_PLAYERn` | B8 |
| 0x5ebde0 | ShowPopup check, ret stub | same | `LIST_FRIENDS` | B9 |
| 0x5ec4a0 | ret stub | same | `Kick From Party (7cc46b94)` | B13 |
| 0x5ec4b0 | ret stub | same | `Mute (6547d63c)` | B14 |
| 0x5ec480 | ret stub | same | `Mute (7273924c)` | B14 |
| 0x5ec4c0 | ret stub | same | `Promote To Party Leader (2d32c8b0)` | B15 |
| 0x5ecb70 | ret stub | same | `party IsHost (56126d00)` | B16 |
| 0x5ebdd0 | ret stub | same | `Save (37c9cb76)` | B17 |
| 0x5ebea0 | ret stub | same | `BUTTON_MULTIPLAYER_INVITES` | B18 |
| 0x5ec000 | ret stub | same | `Join Party (0ace8463)` | B19 |
| 0x5ed8a0 | ret stub | same | `BUTTON_CLANTAG` | B22 |
| 0x5ed8b0 | ret stub | same | `PAGE_MULTIPLAYER_PLAYER_OPTIONS` | B23 |
| 0x5ed8c0 | ret stub | same | `store browse` | B24 |
| 0x64e900 | ret stub | same | `matchmaking box (24df6961)` | B25 |
| 0x6769d0 | ret stub | same | `PAGE_TWITTER` | B26 |
| 0x6769e0 | ret stub | same | `BUTTON_TWITTER_*` | B27 |
| 0x5e4510 | ret stub | same | `cinema send to friend (fe0e7de7)` | B28 |
| 0x5e4520 | ret stub | same | `cinema receive from friend (6b2970e2)` | B29 |

"ret stub" means `is_ret_stub`: the bytes `C3 66 66 66 66` at the address.

Callbacks that the game reaches only through the menu callback table (registered at run time, see
F-register): friend Accept, friend Remove, friend View Profile, players-met Add Friend, players-met
View Profile, cinema pick 0..7 (B10, B12, B28). They have the menu callback signature.

Not touched (empty on PS3 too, or not stubs): 0x5eb7c0, 0x5eba90, 0x5ec470, 0x61d250, 0x5f8240,
0x63c740, 0x5fb7a0, 0x604e80, 0x673900.

### 1.3 Game facts

Data:

| Address / offset | Meaning |
|---|---|
| 0x14ce97c | NET_SM state, signed 32-bit |
| 0x105efb8 | party session object |
| 0x1065b08 | match session object (g_matchSession) |
| session +0x10 | 64-bit context pointer; 0 means no room |
| session +0xb00 | 32-bit own member id |
| session +0xb04 | 32-bit host member id (for the match session: the host's net id) |
| 0x104f1c8 | NetInfo object |
| NetInfo +0x2c4 | 32-bit signed mode; non-zero means a mode is chosen |
| NetInfo +0x18 | 32-bit "cinema messages waiting" flag (online.cpp sets it, the L1 "Messages" entry shows while set) |
| 0x113b760 | NetGameManager (NGM) |
| NGM +0x10 + i*0x2c0, i = 0..9 | player trackers (10) |
| tracker +0x10 | 64-bit address of the process slot (the slot holds the process pointer) |
| tracker +0x18 | 32-bit process unique id expected in that slot |
| tracker +0x120 | 32-bit net id |
| NGM +0x1ba0 | 64-bit address of the npc tracker bucket array (array of 64-bit node addresses) |
| NGM +0x1bdc | 32-bit bucket count |
| bucket array [count] | the end node (an entry one past the buckets) |
| node +0 | 64-bit next node |
| node +0x10 | 32-bit npc id |
| node +0x18 | npc tracker: +0 64-bit process slot address, +8 32-bit unique id |
| process +0x44 | 32-bit process unique id |
| npc vtable +0x250 | IsDead(npc) -> 8-bit bool |
| npc vtable +0x370 | TakeDamage(npc, signed 32-bit damage) |
| npc +0x262a | flags byte; bit value 2 = killed |
| 0xdef290 | 45 attack-type ids, 32-bit each; [0] is 0, [2] is 0xdf95da88 |
| 0x113b760 + 0x1ca0 | 16-bit signed index of the current net game |
| menu manager 0x1136640 | frontend menu manager object |
| menu manager +0x17d0 | 64-bit pointer to the one-button message box descriptor |
| menu manager +0x18ec | 32-bit menu callback count |
| menu manager +0x18f0 + i*0x18 | menu callback entries: +0 32-bit id, +8 64-bit function, +0x10 64-bit argument; at most 128 |
| 0x11474f8 | NetPlayerData (used by clantag.cpp) |

Message box descriptor (pointer at menu manager +0x17d0): +0x08 32-bit text id; +0x40 .. +0x5b
seven 32-bit words.

Popup descriptor (passed to ShowPopup): 0x13 32-bit words (76 bytes): title text id, page id,
option count, then 8 option pairs {text id, callback id}. Unused pairs are 0.

Event payloads (the event object is the first argument of an Apply):

| Event | Offset | Meaning |
|---|---|---|
| NpcDealDamage | +0x0c | 32-bit attacker net id |
| NpcDealDamage | +0x10 | 32-bit npc id |
| NpcDealDamage | +0x14 | signed 32-bit damage |
| NpcDealDamage | +0x18 | float time in seconds |
| NpcDealDamage | +0x1c | 32-bit attack type index |
| KickGriefer | +0x0c | 32-bit net id to kick |

Menu callback arguments: arg +0 32-bit page event; arg +4 32-bit list row; arg +8 32-bit button
number / answer. When arg is NULL, every such field reads as 0.

Query object of cond 48 (0x659b50): +0 32-bit op; +0x40 64-bit text pointer (output); +0x54
32-bit column.

Guest functions called:

| Address | Function | Arguments |
|---|---|---|
| 0x5cc6f0 | PopPage | (menu manager) |
| 0x5cc480 | ShowPopup | (menu manager, popup descriptor pointer, 32-bit 1) |
| 0x5cb990 | ShowPage (via `show_page`) | (menu manager, page id, 0) |
| 0x60eb90 | build and send NetEventKillInfo | (32-bit attack type, attacker process, npc process, pointer to signed 32-bit time, 0, 0, 0) |
| 0xa15e70 | data lookup by id | (32-bit id) -> table pointer {+0 signed 32-bit count, +8 64-bit records} |
| npc vtable slots | IsDead, TakeDamage | see above |

Code sites:

| Address | Meaning |
|---|---|
| 0x644130 | level record package lookup; one caller 0x662fd0 (GetSpawnLocator) reads result +0x9a0 |
| 0x640598 | `mov edi,0; cmovne rdi,rsi` (stripped room accessor folded to NULL) |
| 0x4bf44c | tag table binary search start: `mov rax,[rdx]; mov ecx,[rdx+8]; xor edx,edx; jmp 0x4bf460` |
| 0x4bf460 | the search loop |
| 0x4bf510 | "tag not found" |
| 0x66c75d | `xor edi,edi` then the strlen call at 0x66c75f |
| 0x66c764 | `lea rdx,[rax+1]; xor esi,esi; mov rdi,rbx` then the memcpy call at 0x66c76d |
| 0xde6490 / 0xde6410 | guest strlen / memcpy |
| 0xcddc00 | TextDb::Find(db, id) -> text or NULL |

Ids:

| Id | Meaning |
|---|---|
| 0x719e9fec | one-button message box page |
| 0x99ab57c2 | popup page |
| 0xb99a3235 | PAGE_LOBBY (sid of the name) |
| 0xc6467f2d | text "OK" (message box word 1) |
| 0x11d8e78a, 0x1519fa3d | message box words 2 and 4 (constants) |
| 0xffbbac12 / 0x6547d63c | Mute text / callback |
| 0x9fdf8887 / 0x2d32c8b0 | Promote To Party Leader text / callback |
| 0x1e2955af / 0x7cc46b94 | Kick From Party text / callback |
| 0xb1600ce3 / 0x0ace8463 | Join Party text / callback |
| 0x4a555973 / 0x45a9c65b | Invite To Party text / callback |
| 0x4040b706 | View Profile text |
| 0x3fe6f70e | View Profile callback id (registered by us for friends) |
| 0x910fb4f9 | Add Friend text |
| 0x3b9a067d | *net-games* table (record size 0x90, +4 32-bit level id) |
| 0x0dec97a4 | *levels* table (record size 0x38, +0 32-bit level id, +0x18 name pointer, +0x20 32-bit package id) |
| package +0x9a0 | 64-bit spawner table pointer |

Own ids (`sid` of the name; values for reference):

| Name | Value |
|---|---|
| U2MP_PC_CB_FRIEND_ACCEPT | 0x2026899d |
| U2MP_PC_CB_FRIEND_REMOVE | 0x9086a03a |
| U2MP_PC_CB_MET_ADD | 0xdca79283 |
| U2MP_PC_CB_MET_PROFILE | 0x9b1f0694 |
| U2MP_PC_CB_PICK0 .. PICK7 | 0xdf1d185b, 0xdbdc05ec, 0xd69f2335, 0xd25e3e82, 0xcc196e87, 0xc8d87330, 0xc59b55e9, 0xc15a485e |
| U2MP_PC_TEXT_SLOT0 .. SLOT7 (`kTextSlot`) | 0x2a0acddd, 0x2ecbd06a, 0x2388f6b3, 0x2749eb04, 0x390ebb01, 0x3dcfa6b6, 0x308c806f, 0x344d9dd8 |
| U2MP_PC_TEXT_PLAYER (`kTextPlayer`) | 0x36fa9dae |
| U2MP_PC_TEXT_STORE | 0x3653b15e |
| U2MP_PC_TEXT_VOICE | 0xf7b8e34a |
| U2MP_PC_TEXT_LEADER | 0x284adbe1 |
| U2MP_PC_TEXT_INVITE | 0xb6cb3439 |
| U2MP_PC_TEXT_CLANTAG | 0xbc1d9250 |
| U2MP_PC_TEXT_TWITTER | 0x1d8edde7 |
| U2MP_PC_TEXT_CINEMA_SEND | 0xac28c06d |
| U2MP_PC_TEXT_CINEMA_RECEIVE | 0x111b4b9d |
| U2MP_PC_TEXT_YOU | 0xa4692016 |
| U2MP_PC_TEXT_MUTED | 0xa892cf65 |
| U2MP_PC_TEXT_UNMUTED | 0x8cfe968f |
| U2MP_PC_TEXT_DECLINE_REQUEST | 0xa517c6c8 |
| U2MP_PC_TEXT_CANCEL_REQUEST | 0xc803cf6f |
| U2MP_PC_TEXT_REMOVE_FRIEND | 0x322aee50 |

Other constants: frame clock rate 300.0 (single-precision float); 45 attack types; 10 player
trackers; 128 callback entries; NetStatGame name buffer 0x40 bytes; recent players asked: 50;
cinema picker: at most 8 friends.

### 1.4 Behaviour

#### Shared operations

**F-host (IsHost of a session).** A session "is host" when its +0x10 pointer is 0, or when its
+0xb00 and +0xb04 words are equal.

**F-box (message box with text T).** Read the pointer at menu manager +0x17d0. If it is NULL, do
nothing else (no guest call). Otherwise write T at box +0x08, then write the seven words
0, 0xc6467f2d, 0x11d8e78a, 0, 0x1519fa3d, 0, 0 at box +0x40 .. +0x5b, then call ShowPage
(menu manager, 0x719e9fec, 0).

**F-notice = `show_notice(text)`.** If text is NULL or empty, do nothing. Otherwise set the player
text (`text_set_player`) to text, then F-box(`kTextPlayer`).

**F-popup.** Build a popup descriptor with title `kTextPlayer`, page 0x99ab57c2, count 0, all pairs
0. "Add (text, callback)" appends a pair when fewer than 8 are present (otherwise ignored). "Show"
calls ShowPopup (menu manager, descriptor, 1). The descriptor must hold exactly the 76 bytes above
while the guest runs.

**F-register (register our menu callback id with function f).** Read the count at menu manager
+0x18ec. Look at entries 0 .. min(count, 128)-1 in order; the first entry whose id equals the id
gets its +8 function field overwritten with f (the +0x10 argument is left as is); success. If none
matches and count >= 128: failure, nothing written. Otherwise write a new entry at index count:
id at +0, f at +8, 0 at +0x10 (the 4 bytes at +4 are not written), then add 1 to the count;
success. f is the host address of the entry point the guest calls.

**F-pop.** Call PopPage (menu manager).

Persistent state of the file (process lifetime, initial values): selected member net id 0;
selected friend row -1; selected players-met name empty; cinema picker name list empty; last
"level package" name logged: empty; matchmaking answer flag false; invites count text buffer.

#### B1. 0x644130 level record package (rdi ignored; returns a package pointer or NULL)

1. Call guest 0xa15e70 with 0x3b9a067d (net games), then with 0x0dec97a4 (levels). Both calls
   always happen, in this order.
2. Read the 16-bit signed game index at 0x113d400 (NGM +0x1ca0).
3. Return NULL if either table is NULL, either table's records pointer is NULL, the index is
   negative, or the index is >= the net-games count.
4. The level id is the 32-bit value at net-games records + 0x90*index + 4.
5. Walk the levels records (stride 0x38, `count` records, count read as signed) and take the FIRST
   record whose +0 equals the level id. If none: return NULL.
6. For that record: call `package_by_id` with its +0x20 id. If non-NULL, return it.
7. Otherwise read the name pointer at +0x18. If the name is NULL, or not longer than 4 bytes, or
   does not end with `-pak`: return NULL (no other record is tried).
8. Otherwise let MAP be the name without `-pak`. Logging is on for this call when the name text
   differs from the last name stored for this purpose; the stored name becomes this name either way.
9. Try the candidates MAP+`-ingame10`, MAP+`-ingame`, MAP (in this order). For each: package =
   `package_by_id(sid(candidate))`; table = 64-bit value at package +0x9a0 when the package exists,
   else 0. If logging is on, log
   `"level package: %s candidate %s: %s, spawner table %s"` with (name, candidate,
   `loaded` / `missing`, `present` / `none`). If package and table are both non-zero: if logging is
   on, log `"level package: %s is defined nowhere, using %s"` (name, candidate) and return the
   package. If only the package exists and no fallback is kept yet, keep it (and its candidate
   name) as the fallback.
10. After all three: if logging is on, log
    `"level package: %s is defined nowhere, no candidate has spawners; using %s"` with (name,
    fallback candidate name, or `nothing` when there is no fallback). Return the fallback (may be
    NULL).

#### B2. 0x640598 tracker values

At install the 9 bytes become `48 89 F7 90 90 90 90 90 90` (`mov rdi,rsi` and six NOPs). Effect:
the player tracker values always come from the match session's own block. No run-time code.

#### B3. 0x4bf44c empty tag table search

At install, 0x4bf44c becomes `E9 rel32` to a stub in near-image memory, and the next 5 bytes
(0x4bf451 .. 0x4bf455) become 0x90. The stub must behave exactly like this, with rdx pointing at
the table {64-bit data, 32-bit count}:

- rax := 64-bit [rdx]; ecx := 32-bit [rdx+8] (upper half of rcx cleared).
- If rax is 0, or ecx as signed is <= 0: continue at image+0x4bf510 (rdx unchanged).
- Otherwise: edx := 0 (rdx cleared) and continue at image+0x4bf460.
- No other register or stack change. Flags are free.

One encoding (45 bytes, using absolute indirect jumps with the 8-byte targets inline):
`48 8B 02 8B 4A 08 48 85 C0 74 14 85 C9 7E 10 31 D2 FF 25 00 00 00 00 <8-byte image+0x4bf460>
FF 25 00 00 00 00 <8-byte image+0x4bf510>`. The stub memory is taken with `near_stub(64)`.

#### B4. NetStatGame host name (calls at 0x66c75f and 0x66c76d)

The guest code at match start copies the match host's name into a 0x40-byte field with "length =
strlen(name); memcpy(dst, name, length + 1)", but the name pointer is 0.

Host name: `session::member_name` of the 32-bit value at 0x1065b08+0xb04; if that fails,
`session::local_name()`. If the name has 64 or more bytes, cut it to its first 63 bytes. It is
computed afresh on each call.

- Length call (rdi = string): if the string pointer is non-NULL, return its strlen. Otherwise
  return the host name's byte length.
- Copy call (rdi = dst, rsi = src, rdx = n): if src is non-NULL, copy n bytes from src to dst.
  Otherwise copy the host name and its terminating NUL to dst (n is ignored). Return dst in both
  cases.

#### B5. TextDb::Find detour at 0xcddc00

The first 12 bytes (whole, position-independent instructions) move to a trampoline that then jumps
back to image+0xcddc00+12. The entry jumps (`E9 rel32`, then bytes 5..11 = 0xCC) to a stub that
jumps to `text_find` (texts.cpp). The trampoline address is handed to `text_find_original`. Memory:
one `near_stub(0x100)` block; trampoline at its start, the jump stub at +0x80. Run-time behaviour
is entirely `texts.cpp`'s.

#### B6. NetEventNpcDealDamage::Apply 0x61fa70 (rdi = event, rsi unused)

1. Player tracker: the first of the 10 trackers whose +0x120 equals the event's +0x0c (full 32
   bits).
2. Npc tracker: read bucket count (NGM +0x1bdc) and bucket array (NGM +0x1ba0). If either is 0, no
   tracker. Hash = 32-bit FNV-1a of the 4 bytes of the npc id (event +0x10) in little-endian order
   (offset basis 0x811c9dc5, prime 0x01000193). Start at bucket array [hash mod count]. Follow +0
   next links until a node whose +0x10 equals the id, or a NULL node. If the result is NULL or equals
   bucket array [count] (the end node), no tracker. Else the npc tracker is node +0x18.
3. If either tracker is missing: return.
4. Attacker process: slot = tracker +0x10, id = tracker +0x18. Npc process: slot = npc tracker +0,
   id = npc tracker +8. For each: none if slot is 0 or id is 0; else process = 64-bit value at
   slot; none if process is NULL or its +0x44 differs from id. If either is none: return.
5. Call IsDead(npc) through the npc's vtable (+0x250). Non-zero result: return.
6. Call TakeDamage(npc, damage at event +0x14) through vtable +0x370.
7. Call IsDead(npc) again (vtable read again). Zero result: return.
8. If the match session (0x1065b08) is not host (F-host): return.
9. Attack type: index = event +0x1c (unsigned); type = 32-bit at 0xdef290 + 4*index when index < 45,
   else 0.
10. Time: single-precision product of the float at event +0x18 and 300.0, truncated toward zero to
    a signed 32-bit integer; when the product is not in [-2^31, 2^31) (or NaN), the value is
    0x80000000. Keep it in memory the guest can read.
11. Call 0x60eb90 (type, attacker process, npc process, pointer to that time value, 0, 0, 0).
12. Set bit value 2 in the byte at npc +0x262a (OR; other bits unchanged).

#### B7. NetEventKickGriefer::Apply 0x61b900 (rdi = event)

If the match session is not host: return. Otherwise call `session::kick` with the event's +0x0c
word. If it returns false, log `"NetEventKickGriefer: can't kick net id %u"` (the net id).

#### B8. Party / match player rows (0x5ec490, 0x5ec460) and F-player-options(row)

- 0x5ec490: row = (arg +8) mod 10 (unsigned).
- 0x5ec460: row = arg +8, as a signed int.

F-player-options(row):

1. `session::member_at(row, id, name)`. If false: log `"player options: no player in row %d"` (row)
   and stop.
2. Selected member net id := id.
3. If id equals `session::local_id()`: player text := name + one space + `own_text(kTextYou)`;
   F-box(`kTextPlayer`); stop.
4. Player text := name. F-popup with: (0xffbbac12 Mute, 0x6547d63c); then, if `session::is_host()`
   and `online::active()`, (0x9fdf8887, 0x2d32c8b0); then, if `session::is_host()`, (0x1e2955af,
   0x7cc46b94). Show.

#### B9. LIST_FRIENDS row 0x5ebde0

row = arg +4 as signed int. If `online::active()` is false: F-player-options(row). Else if row is 0:
`online::open_join_list()` (result ignored). Else F-friend-options(row - 1).

F-friend-options(r):

1. If r < 0 or r >= `online::friend_count()`: nothing.
2. Selected friend row := r. Player text := `online::friend_row(r)`.
3. F-register U2MP_PC_CB_FRIEND_ACCEPT (B10 accept), then U2MP_PC_CB_FRIEND_REMOVE (B10 remove),
   then 0x3fe6f70e (B10 profile). Stop registering at the first failure. "cbs" = all three
   succeeded.
4. kind = `online::friend_kind(r)`. Popup options:
   - kind 1 (request in): if cbs, (0x910fb4f9 Add Friend, ACCEPT) and (`kTextDeclineRequest`,
     REMOVE).
   - kind 2 (request out): if cbs, (`kTextCancelRequest`, REMOVE).
   - any other kind: if `online::friend_joinable(r)`, (0xb1600ce3, 0x0ace8463); if
     `online::friend_online(r)`, (0x4a555973, 0x45a9c65b); if cbs, (0x4040b706 View Profile,
     0x3fe6f70e) and (`kTextRemoveFriend`, REMOVE).
5. If no option was added: F-box(`kTextPlayer`). Otherwise show the popup.

#### B10. Friend popup callbacks (registered ids)

- Accept: `online::accept_request(selected friend row)`.
- Remove: `online::remove_friend(selected friend row)`.
- Profile (0x3fe6f70e): player text := `online::profile_text(selected friend row)`;
  F-box(`kTextPlayer`).

#### B11. LIST_PLAYERSMET row 0x5ebdf0

1. row = arg +4 as signed int. names = `progression_recent_players(50)`. If row < 0 or row >= the
   number of names: nothing.
2. Selected players-met name := names[row]. Player text := that name.
3. F-register U2MP_PC_CB_MET_ADD (B12 add), then U2MP_PC_CB_MET_PROFILE (B12 profile); stop at the
   first failure. On any failure: F-box(`kTextPlayer`) and stop.
4. Popup: if `online::active()`, (0x910fb4f9, MET_ADD); then always (0x4040b706, MET_PROFILE).
   Show.

#### B12. Players-met popup callbacks

- Add: `online::friend_add(selected name)`. False: F-notice
  `The friend request could not be sent.` True: F-notice `Friend request sent to ` + name + `.`
- Profile: F-notice of `progression_player_summary(selected name)` (empty text: nothing happens).

#### B13. Kick From Party 0x5ec4a0

If the selected member net id is 0, or `session::kick(id)` returns false: log
`"Kick From Party: net id %u not kicked"` (the selected id). (`kick` is not called when the id is 0.)

#### B14. Mute 0x5ec4b0 and 0x5ec480 (same behaviour)

If `voice::enabled()` is false: F-box(`kTextVoice`). Else if `session::member_name(selected id)`
fails: nothing. Else muted = `voice::toggle_mute(selected id)`; player text := name + `: ` +
`own_text(muted ? kTextMuted : kTextUnmuted)`; F-box(`kTextPlayer`).

#### B15. Promote To Party Leader 0x5ec4c0

If the selected id is non-zero and `online::promote(id)` returns true: nothing more. Otherwise
F-box(`kTextLeader`). (`promote` is not called when the id is 0.)

#### B16. Party IsHost 0x5ecb70

Returns (in al) 1 when the party session 0x105efb8 is host (F-host), else 0.

#### B17. Save 0x5ebdd0

Logs `Save`.

#### B18. BUTTON_MULTIPLAYER_INVITES 0x5ebea0

If `online::open_join_list()` returns true: nothing more. Otherwise, if the signed 32-bit value at
NetInfo +0x2c4 is non-zero, `show_page(0xb99a3235)`. Then, in both cases, F-box(`kTextInvite`).

#### B19. Join Party 0x5ec000

If `online::active()` and the selected friend row >= 0: `online::join_friend(row)`. Otherwise
F-box(`kTextInvite`).

#### B20. Invite To Party 0x5ebee0

If `online::active()` and the selected friend row >= 0: `online::invite_friend(row)`. Otherwise
F-box(`kTextInvite`).

#### B21. Cond 48 query 0x659b50 (rdi = query object; no meaningful return value)

- op (q +0) == 1 and `online::invite_count()` == 0: write at q +0x40 a pointer to an empty
  NUL-terminated string that stays valid for the process lifetime.
- op == 2 and column (q +0x54) == 1: n = `online::invite_count()`; if n > 0, format n in decimal
  into a buffer that stays valid (one shared buffer, 16 bytes) and write its pointer at q +0x40.
- Anything else: no write. (`invite_count` is called only in the two cases above; for op 1 it is
  called once; for op 2 only when the column is 1.)

#### B22. BUTTON_CLANTAG 0x5ed8a0

Call `edit_clan_tag()` (section 2). If it returns false: F-box(`kTextClanTag`).

#### B23. PAGE_MULTIPLAYER_PLAYER_OPTIONS 0x5ed8b0

If the page event (arg +0) is 6: call `clan_tag_frame()` (section 2); then, if NET_SM (0x14ce97c,
signed) is < 2, F-pop. Other events: nothing.

#### B24. Store browse 0x5ed8c0

F-box(`kTextStore`).

#### B25. Matchmaking box answer 0x64e900

v = arg +8. If v is non-zero, set the matchmaking answer flag (nothing reads it). Log
`"matchmaking message box answered (%u)"` with v, always.

#### B26. PAGE_TWITTER 0x6769d0

If the page event (arg +0) is 6: F-pop, then F-box(`kTextTwitter`).

#### B27. BUTTON_TWITTER_* 0x6769e0

F-box(`kTextTwitter`).

#### B28. Cinema send 0x5e4510 and `cinema_pick_friend()` (same behaviour)

1. If `online::linked()` is false: F-box(`kTextCinemaSend`); stop (the stored list is kept).
2. Clear the stored name list. For i = 0, 1, ... while i < `online::friend_count()` (asked again
   before each step) and the list has fewer than 8 names: if `online::friend_kind(i)` is 0 and
   `online::friend_online(i)` is true (asked only when the kind is 0), append
   `online::friend_name(i)`.
3. If the list is empty: F-notice `None of your RPCN friends is online to receive the cinema file.`
   and stop.
4. Player text := `Send the cinema file to`. For each list position i in order: `text_set_slot(i,
   name)`; then F-register U2MP_PC_CB_PICKi with pick callback i; if that succeeded, add
   (`kTextSlot[i]`, U2MP_PC_CB_PICKi) to the popup.
5. Show the popup (even when every registration failed and it has no options).

Pick callback i (registered id U2MP_PC_CB_PICKi): if i < the size of the stored list,
`online::send_cinema(list[i])`; else nothing.

#### B29. Cinema receive 0x5e4520

If `online::linked()` is false: F-box(`kTextCinemaReceive`); stop. Otherwise n =
`online::cinema_received()`; write signed 32-bit 0 at NetInfo +0x18 (0x104f1e0); then F-notice:
when n is non-zero, the decimal of n + ` cinema file(s) from friends are in your cinema list.`;
when n is 0, `No cinema files from friends yet. Friends send them from a cinema's page.`

### 1.5 Install-time behaviour (`install_remaining`)

Steps in this exact order (logs and `near_stub` use follow it). "+1" means `installed` grows by one.

1. **0x644130.** If its 14 bytes match: `replace_function(0x644130, 14, B1, "level record package
   (644130)")`, +1 on success. Mismatch: no log line at all.
2. **0x640598.** Match: write the 9 new bytes, log
   `tracker values (640598): read from the session block, not the stripped room accessor`, +1.
   Mismatch: log `tracker values (640598): bytes differ, left alone`.
3. **0x4bf44c.** Only when the 10 bytes match is `near_stub(64)` asked. If the stub exists and
   (stub - (image+0x4bf451)) fits in a signed 32-bit value: write the stub, patch the site, log
   `empty tag table search (4bf44c): checked -> C++ stub`, +1. Otherwise (mismatch, no stub, out of
   range) log `empty tag table search (4bf44c): bytes differ, left alone`.
4. **NetStatGame host name.** Requires bytes `31 FF E8` at 0x66c75d and
   `48 8D 50 01 31 F6 48 89 DF E8` at 0x66c764. Then `retarget_call(0x66c75f, 0xde6490, length,
   "NetStatGame host name length")` and, only if that succeeded, `retarget_call(0x66c76d,
   0xde6410, copy, "NetStatGame host name copy")`. Both succeed: +1 (one count for both).
   Otherwise log `NetStatGame host name: call sites differ, left alone`. (If the first retarget
   succeeded and the second failed, the first stays in place, and the "differ" line is still
   logged.)
5. **TextDb::Find.** Requires `executable(0xcddc00, 12)` and the 12 bytes. Mismatch: log
   `"%s at 0x%llx differs, not hooked"` with (`TextDb::Find`, 0xcddc00). Then `near_stub(0x100)`;
   NULL means failure without a line of its own. If the jump stub (block +0x80) is out of rel32
   range of image+0xcddc05: failure without a line of its own (the block stays used, nothing in the
   image changes). Success: patch, log `"%s: 0x%llx hooked"` (`TextDb::Find`, 0xcddc00), call
   `text_find_original(trampoline)`, +1. Any failure: log
   `PC texts unavailable: the PC message boxes will show the game's fallback text`.
6. **Checks of called guest functions** (no log of their own):
   kill-info OK = the 17 bytes `55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC 38` at 0x60eb90 match,
   and 0xdef290 + 180 <= image size, and the word at 0xdef290 is 0, and the word at 0xdef298 is
   0xdf95da88 (each later condition is evaluated only when the earlier ones hold; the table is not
   read when it is out of the image).
   popup OK = the 10 bytes `55 48 89 E5 41 56 53 48 89 F0` at 0x5cc480 match.
7. **0x61fa70.** If kill-info is not OK: log
   `NetEventNpcDealDamage::Apply: the kill-info sender or attack types differ, left alone`. Else if
   its 11 bytes differ: log `"NetEventNpcDealDamage::Apply at 0x%llx: not the cut-short function this
   port expects, left alone"` (0x61fa70). Else `replace_function(0x61fa70, 5, B6,
   "NetEventNpcDealDamage::Apply")`, +1 on success.
8. **0x5ebee0.** 20 bytes match: `replace_function(.., 5, B20, "Invite To Party (45a9c65b)")`, +1 on
   success. Else log `"Invite To Party at 0x%llx differs, left alone"` (0x5ebee0). Not gated on popup OK.
9. **0x659b50.** 14 bytes match: `replace_function(.., 5, B21, "BUTTON_MULTIPLAYER_INVITES query
   (cond 48)")`, +1 on success. Else log `"BUTTON_MULTIPLAYER_INVITES query at 0x%llx differs, left
   alone"` (0x659b50).
10. **0x5ebdf0.** popup OK and 12 bytes match: `replace_function(.., 5, B11, "LIST_PLAYERSMET row
    (369314b2)")`, +1 on success. Else log `LIST_PLAYERSMET row at 0x5ebdf0 differs, left alone`
    (fixed text).
11. **Ret stubs**, in the order of the table in 1.2 starting at 0x61b900 and ending at 0x5e4520
    (KickGriefer, PARTY_PLAYERn, PLAYERn, LIST_FRIENDS, Kick, Mute 0x5ec4b0, Mute 0x5ec480,
    Promote, party IsHost, Save, INVITES, Join Party, CLANTAG, PLAYER_OPTIONS page, store, matchmaking
    box, PAGE_TWITTER, BUTTON_TWITTER_*, cinema send, cinema receive). For each:
    - needs popup (only PARTY_PLAYERn, PLAYERn, LIST_FRIENDS) and popup not OK: log
      `"%s: ShowPopup 0x%llx differs, left alone"` (name, 0x5cc480); skip.
    - not a ret stub: log `"%s at 0x%llx: not the stripped function this port expects, left alone"`
      (name, address); skip.
    - else `replace_function(address, 5, fn, name)`; each success counts.
    The successes are added to `installed` after the loop.

The function never fails as a whole and returns nothing.

### 1.6 Test scenarios (remaining.cpp)

Install:
- Clean image: every step installs; check the log lines in order and `installed` (+4 for 1-4, +1
  TextDb, +1 NpcDealDamage, +3 for 8-10, +20 stubs = 29).
- Each byte check broken one at a time (0x644130, 0x640598, 0x4bf44c, 0x66c75d, 0x66c764, a wrong
  E8 target at 0x66c75f, then at 0x66c76d only, 0xcddc00, 0x60eb90, attack type [0] non-zero,
  [2] wrong, image size < 0xdef290+180, 0x61fa70, 0x5ebee0, 0x659b50, 0x5ebdf0, 0x5cc480, each
  ret stub). Compare logs, patched bytes and the count.
- near_stub exhausted (fake allocator returns NULL) for 0x4bf44c and TextDb::Find.

B1: games/levels table NULL; records NULL; index -1 and = count; no level record; record with a
loaded package id; name NULL, `abc` (<= 4 bytes), `x-pak` vs `map-pak`, name without `-pak`;
candidates: `-ingame10` with table, only `-ingame` with table, only map loaded without table
(fallback logged), none loaded (`nothing`); two levels records with the same id (only the first
counts); call twice with the same name (second call logs nothing), then a different name, then the
first again (logs again).

B3: run the stub with rdx -> {NULL, 5}, {p, 0}, {p, -1}, {p, 3}: check target and rax/rcx/rdx.

B4: length/copy with non-NULL pointers (pass-through); with NULL: host net id found / not found
(local name), names of 63, 64 and 100 bytes.

B6: no player tracker; tracker matched at index 9; bucket count 0; bucket array 0; id found in the
second node of a chain; chain ending at the end node; process slot 0, id 0, slot holding NULL,
unique id mismatch (attacker and npc separately); npc already dead; damage not lethal; lethal but
not host (match +0x10 non-zero and +0xb00 != +0xb04); lethal and host via +0x10 == 0 and via equal
ids; attack index 0, 44, 45, 0xffffffff; time 1.5 (450), -0.0, 1e10, NaN; check the 7 call
arguments, the time value, and the +0x262a byte (other bits kept).

B7: not host; host and kick true; host and kick false (log).

B8/B9/B10: row not found (log); own row (name + " " + You text); other member as client, as host
offline, as host online (3 options); `% 10` on 13 for 0x5ec490; LIST_FRIENDS offline (row from +4),
online row 0 (join list), online rows for kinds 0 (joinable/online combinations), 1, 2, -1; row out
of range; callback table full (128 entries, none ours) so cbs false: kind 1 and 2 show the message
box, kind 0 with joinable shows only Join; table with 127 entries (only ACCEPT fits); our ids
already present (overwritten in place, count unchanged); arg NULL.

B11/B12: empty list; row 49 of 50; online and offline; registration failure; add true/false;
profile summary empty and non-empty.

B13-B17: selected id 0; kick false; voice off; member gone; toggle to muted and unmuted; promote
true/false/id 0; party IsHost both ways; Save log.

B18-B21: join list true; false with mode 0 and non-zero (page then box); join/invite with online
off, selected row -1, valid row; cond 48 op 1 with 0 and 3 invites, op 2 column 1 with 0 and 12
invites, op 2 column 0, op 3.

B22-B27: clan tag begin ok / fails; page event 6 with NET_SM 1 and 2, event 5; store; matchmaking
answer 0 and 7; twitter event 6 and 2; twitter button.

B28/B29: not linked; no friend online; 3 of 5 online; 10 online friends (only 8); a request row
online (skipped); registration failing at slot 5; pick callback with index beyond the list; receive
with 0 and 2 files (check NetInfo +0x18 is 0 afterwards); `show_notice` with NULL and "".

Message box with a NULL descriptor pointer at +0x17d0: no write, no ShowPage.

---

## 2. clantag.cpp

### 2.1 Purpose

On PS3 the profile page's BUTTON_CLANTAG opened the on-screen keyboard and the page polled it every
frame. On PC the runtime's in-window text input is used instead (keyboard Enter or Cross accepts,
Esc or Circle cancels). The button (remaining.cpp B22) starts it; the page event 6 (B23) polls it.
The tag lives in NetPlayerData +0x1a8 (8 bytes), which the game's label query shows and which
progression.cpp saves and publishes.

### 2.2 Interface

Declared in `progression.h` (namespace `u2mp`), used only by remaining.cpp:

- `bool edit_clan_tag();`
- `void clan_tag_frame();`

It calls (C linkage) `gow3gpu_text_input_begin` and `gow3gpu_text_input_poll`, and
`progression_publish()`. It has no game entry points and no install step.

### 2.3 Game facts

| Address | Meaning |
|---|---|
| 0x11474f8 | NetPlayerData |
| 0x11474f8 + 0x1a8 = 0x11476a0 | clan tag, 8 bytes, NUL-padded ASCII, at most 4 characters used |

Constants: at most 4 tag characters; poll buffer 64 bytes; prompt text
`Clan Tag (up to 4 letters or digits; empty = none)`.

### 2.4 Behaviour

State: an "editing" flag, initially false.

`edit_clan_tag()`:
1. If editing: return true (nothing else).
2. The current tag = the bytes at 0x11476a0 up to the first NUL, at most 8 bytes.
3. Call `gow3gpu_text_input_begin(current tag, prompt)`. If it returns 0: log
   `clan tag: no text input available`, return false.
4. Otherwise editing := true, log `clan tag: text input open`, return true.

`clan_tag_frame()`:
1. If not editing: nothing.
2. Call `gow3gpu_text_input_poll` with a 64-byte buffer, all zero before the call, size 64.
3. Result 0: nothing (still editing).
4. Any other result: editing := false. Result 1: store the text (below). Other non-zero results:
   log `clan tag: unchanged`.

Storing a text:
1. Go through the text bytes until the NUL or until 4 characters are taken. `A`-`Z` and `0`-`9`
   are taken as they are; `a`-`z` are taken upper-cased; every other byte (including bytes >= 0x80)
   is skipped.
2. Write 8 bytes at 0x11476a0: the taken characters, then zeros.
3. Log `"clan tag: [%.4s]"` with the taken characters (empty brackets when none).
4. Call `progression_publish()`.

### 2.5 Install-time behaviour

None. Nothing is patched and `installed` is not changed.

### 2.6 Test scenarios

- begin returns 0: false, log, B22 shows the CLANTAG message box.
- begin ok: log; second `edit_clan_tag` while editing returns true with no begin call and no log.
- Current tag `AB\0...`, and a full 8-byte tag with no NUL (initial text = 8 bytes).
- poll 0 (nothing), poll 2 (log unchanged, editing ends), poll 1 with `ab-1c9z` (`AB1C`),
  `a` (`A`), `!!` (empty: 8 zero bytes, `clan tag: []`), UTF-8 `é1` (`1`), 63-byte text.
- `clan_tag_frame` when not editing: no poll call.
- After accept, a new `edit_clan_tag` opens again.

---

## 3. Ambiguous or risky points

- `own_text(kTextYou / kTextMuted / kTextUnmuted)` is appended to a name; if `own_text` returned
  NULL the current code has undefined behaviour. texts.cpp has these texts, so it does not happen in
  practice; a new implementation should not crash there.
- `text_set_player(online::friend_row(r))` with a NULL row text: the current code passes NULL to
  texts.cpp, which formats it with `%s` (MinGW prints `(null)`).
- Host function pointers written into the guest callback table and the popup/query pointers differ
  between builds; the tester must compare them by meaning, not by value. The same holds for the
  descriptor and time-value pointers passed to ShowPopup and 0x60eb90.
- B3 and B5 emit machine code. The register contract in B3 and the trampoline layout in B5 must be
  kept; stub memory is outside the image.
- B21 returns no value; the guest's cond-query caller may read rax. The current code leaves rax
  undefined.
- In B6 the npc lookup does not stop at the end node while walking; it only rejects the end node
  as the final result. Keep that.
