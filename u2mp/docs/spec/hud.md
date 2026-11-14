# Spec: multiplayer HUD draw (hud)

Clean-room description for a new implementation of `gow3_pc/src/u2mp/hud.cpp`. It states what the
code must do, as seen from outside: guest calls (function, arguments, order), log lines, and writes
to game memory. It does not prescribe structure, helper names or loop shapes. All addresses are
image offsets of the Uncharted 2 PS4 1.02 build unless they are called "absolute".

Notation:

- `B` is `image.base` (`u2mp::image`). An image offset `X` is the absolute address `B + X`.
- `[X]` is the value stored at image offset `X`. `p+0xNN` is a byte offset into an object at an
  absolute address `p`. Pointers stored in game memory are 8-byte absolute addresses.
- "u8", "u16", "s16", "u32", "s32", "u64", "f32" are little-endian host values of that size.
- "guest call F(a, b, ...)" means calling the game function at `B + F` with the x86-64 System V
  convention (`U2MP_GUEST`). Integer and pointer arguments go in rdi, rsi, rdx, rcx, r8, r9; float
  arguments in xmm0, xmm1, ... in order. A "V2" is a struct of two f32 (x, y) passed by value: it is
  one SSE eightbyte and takes one xmm register. A "V4" is four f32 (x, y, z, w = r, g, b, a), 16-byte
  aligned, passed by pointer.
- All floating point is IEEE single precision. Every operation is rounded on its own, in the order
  the formulas below are written (left to right unless brackets say otherwise). No fused
  multiply-add, no double intermediates. `cos`, `sin`, `fmod`, `fabs` are the single-precision C
  library functions (`cosf`, `sinf`, `fmodf`, `fabsf`). "1/300" as a factor means multiplying by the
  f32 constant `1.0f/300.0f`; "/ 300" means a real division by 300.0f. The two are not
  interchangeable: each place below says which one it is.
- `int(v)` of an f32 is conversion toward zero to s32 (x86 `cvttss2si`). Integer adds and subtracts
  on s32 wrap. `/` and `%` on integers are C truncating division and remainder.
- `clamp01(v)`: 0 when v < 0, 1 when v > 1, else v (a NaN passes through unchanged).
- `argb(c)` of a u32 colour 0xAARRGGBB is the V4 (R / 255, G / 255, B / 255, A / 255): each byte is
  converted to f32 and divided by 255.0f.
- `pack(v4)` is the u32 `A << 24 | R << 16 | G << 8 | B` where each byte is
  `int(clamp01(component) * 255.0f)` (A from w, R from x, G from y, B from z).

---

## 1. Purpose and hook points

The HUD manager (object at image offset 0x1130008) owns one multiplayer HUD element. Its class has
the vtable 0x103f550. Slot 0 (Update) is intact in the PS4 build: every frame it fills the
element's data (message list, names over heads, world markers, centre message, fade, HUD flag
bits). Slot 1 (Draw) is a bare `ret` stub at 0x55c5b0. The HUD element loop still calls slot 1
every frame while the element's bits (0x2000) are set, so in a match nothing draws the scores,
score limit, time, territory panel, carrier icon, co-op panel, boosters, message list, names,
markers, theater controls or centre message.

This module puts a host Draw function into vtable slot 1 (an 8-byte absolute pointer at
0x103f558). It reads the data the intact Update leaves in the element and in the game state, and
draws it with the surviving PS4 HUD primitives (text widget, image widget, textured quad, menu
quad, DrawText, projection).

It also adds a weapon-panel trigger: for 3 seconds after the local player's current weapon
changes, the multiplayer Draw calls the game's ShowWeaponHud, and two vtable slots get counting
probes (section 4.12).

Who calls what:

- `install.cpp` calls `install_hud(installed)` once, after `install_scripttime` and before
  `install_scoreboard`. `install_menuimages` runs before it and has already replaced the material
  lookup 0x5558c0; this module calls 0x5558c0 but does not check its bytes.
- No other file uses anything from this module. `u2mp_internal.h` declares it.

## 2. Interface

### 2.1 Declarations other files use

- `void u2mp::install_hud(int &installed);` (declared in `u2mp_internal.h`). Nothing else is
  exported. Everything else has internal linkage.

### 2.2 Things this module uses from `u2mp_internal.h` / `install.cpp`

- `u2mp::image` (`base`, `size`, `at<T>()`, `matches()`), `u2mp::log(fmt, ...)` (prints
  `u2mp: ` + the formatted text + newline), `u2mp::is_ret_stub(offset)` (true when the bytes at
  `offset` are `c3 66 66 66 66`), `U2MP_GUEST`.
- Environment: `std::getenv` for `UNCHARTED_U2MP_HUD`, `UNCHARTED_U2MP_WEAPON_HUD`,
  `UNCHARTED_U2MP_NO_THEATER`.

### 2.3 Entry points the game reaches

| Entry | Where it is installed | Original value expected | Guest signature |
|---|---|---|---|
| Multiplayer Draw | vtable 0x103f550 slot 1 = pointer at 0x103f558 | `B + 0x55c5b0`, and 0x55c5b0 is a ret stub | `void(element*, draw_ctx*)` |
| ShowWeaponHud probe | pointer at 0x1027030 (vtable 0x1026d40, slot +0x2f0) | `B + 0x954740` | `void(controller*, float arg)` (arg in xmm0) |
| Weapon panel Draw probe | pointer at 0x103f768 (vtable 0x103f760 slot 1) | `B + 0x563a20` | `void(element*, draw_ctx*)` |

All three are host functions with the System V convention. `draw_ctx` may be NULL (on PS4 it is
NULL for every HUD element). It is passed on unchanged to every guest call that takes a draw
context. Nothing tests it.

### 2.4 Guest functions called

| Offset | Meaning | Arguments (in order) | Returns |
|---|---|---|---|
| 0xa15e70 | LookupPointer | u32 id (edi) | pointer (NULL when absent) |
| 0x5555a0 | HUD text widget draw, variadic | widget*, pos* (2 f32), const char* fmt, f32 scale (xmm0), then one variadic argument (an int in ecx, or a char* in rcx); al = 1 | none |
| 0x5559f0 | HUD image widget draw | draw_ctx, widget*, pos* (2 f32), f32 alpha (xmm0) | none |
| 0xcddc00 | TextDb::Find | `B + 0x1066660`, u32 text id | const char* or NULL |
| 0x5558c0 | material for a handle | u64 handle = `id | table << 32` (rdi), s32 index (esi) | material pointer or NULL |
| 0x555c60 | textured quad | draw_ctx (rdi), u64 handle `id | table << 32` (rsi), s32 index (edx), V4* colour (rcx), bool centred (r8b), bool global_alpha (r9b, always 1); V2 pos (xmm0), V2 size (xmm1), V2 dir (xmm2), V2 uv0 (xmm3), V2 uv1 (xmm4) | none |
| 0xbdce90 | menu quad | context block* (48 bytes), info block* (32 bytes), vertex block* (0x110 bytes) | none |
| 0x5566b0 | world-to-screen projection | out* (0x30 bytes), V4* world position, f32 edge margin (xmm0) | none (fills out) |
| 0xcc9a70 | DrawText | state* (0xc0 bytes), const char* text | none (may write the state) |
| 0x555140 | handle by image name | const char* name (rdi), int 0 (esi) | 16-byte struct in rax:rdx: u64 `id | table << 32`, then u32 index |
| 0x64aeb0 | team colours | out* (0x40 bytes), `B + 0x113b760`, s32 team, s32 -1 | none (u32 colour at out+0x10) |
| 0x64cbd0 | co-op money multiplier | `B + 0x113b760` | s32 |
| 0x5c8840 | DoCallback | `B + 0x1136640`, u32 callback id, u32 0 | none |
| 0x3dd780 | one-shot sound init | buffer* (0x200 bytes), u32 sound id, u32 0 | none |
| 0x9b5c20 | one-shot sound play | buffer*, u32 0 | none |
| 0x71dec0 | player: current weapon (reached through the player's vtable, slot +0x260) | player* | pointer |
| 0x954740 | weapon controller ShowWeaponHud | controller*, f32 arg (xmm0) | none |
| 0x563a20 | weapon panel Draw | element*, draw_ctx* | none |
| (vtable) | held object of a character (vtable slot +0xe8 of the object at character+0x2928) | object*, int 0 | pointer |

Every pointer argument that is a host temporary (positions, colours, states, buffers) must hold
exactly the bytes described below at the moment of the call. Positions that point into game memory
are named explicitly; everywhere else the position is a fresh temporary.

## 3. Game facts

### 3.1 Globals (image offsets)

| Offset | Type | Meaning |
|---|---|---|
| 0x113b760 | object | net game manager ("ngm"); fields in 3.2 |
| 0x14ce97c | s32 | net state; multiplayer when >= 2 |
| 0x1116180 | object | clock block; all clocks in 1/300 s |
| 0x11161c4 (0x1116180+0x44) | s32 | message clock |
| 0x1116204 (0x1116180+0x84) | s32 | clock 1 frame |
| 0x1116210 (0x1116180+0x90) | s32 | clock 1 delta |
| 0x1116284 (0x1116180+0x104) | s32 | clock 2 frame |
| 0x1116384 (0x1116180+0x204) | s32 | game frame counter ("frames") |
| 0x1130008 | object | HUD manager |
| 0x1130068 (hud+0x60) | u64 | pointer to the HUD flags (u32 at +0; byte at +0x40c = "revive button" flag) |
| 0x1130070 / 0x1130078 / 0x1130080 (hud+0x68 + 8 * t) | u64 | material table pointer for table t = 0 (hud), 1 (net), 2 (menu) |
| 0x1130348 (hud+0x340) | f32 | HUD x offset (only the x is read) |
| 0x104ae70 | u64 | team score table pointer; score of team t is u16 at table+0x3dee+0x34*t |
| 0x104ed0c | f32 | HUD alpha |
| 0x104f4c4 (0x104f4b0+0x14) | u32 | world marker type mask (bit = type) |
| 0x104f564 (0x104f544+0x20) | f32 | wave screen alpha |
| 0x104f1bc | f32 | replay speed |
| 0x1066660 | object | TextDb |
| 0x113a788 | u32 | booster list count; record pointers (u64) at 0x113a790 + 8*i |
| 0x113a990 | object | cinema manager |
| 0x113b008 (cinema+0x678) | s32 | cinema mode |
| 0x113b030 (cinema+0x6a0) | s32 | film start clock |
| 0x113ab68 (cinema+0x1c8+0x10) | s32 | film length in seconds |
| 0x1147544 / 0x1147548 (0x11474f8+0x4c / +0x50) | u32 | local loadout booster ids, slot 0 / 1 |
| 0x1136640 | object | menu manager (DoCallback's first argument) |
| 0x14b7fb0 | u32 | raw pad buttons |
| 0x115bd20 | u64 | local player handle: cell pointer; u32 id at 0x115bd28 |

### 3.2 Net game manager fields (ngm = 0x113b760)

| Field | Type | Meaning |
|---|---|---|
| +0x0 | u64 | local tracker pointer |
| +0x10 + 0x2c0*i, i = 0..9 | object | tracker i (in place) |
| +0x1ca0 | s16 | current net game index |
| +0x1cc0 / +0x1cc8 | u64 / u32 | carry object handle: slot pointer / id |
| +0x1d5c | s32 | round start frame |
| +0x1d60 | s32 | match start frame |
| +0x1d64 / +0x1d68 | s32 | wave start / wave end frame |
| +0x1d6c | s32 | wave number (0-based) |
| +0x1d70 | s32 | round number (0-based) |
| +0x1d80 + 0x10*i, i = 0..4 | u64 slot pointer, u32 id at +8 | territory handles |
| +0x1dd0 | u32 | territory handle count |
| +0x1e80 | s32 | boosters off (0 = allowed) |
| +0x209c | u8 | a film is playing |
| +0x21b4 | s32 | score limit |
| +0x21b8 | f32 | time limit in seconds |

### 3.3 Tracker fields

| Field | Type | Meaning |
|---|---|---|
| +0x10 / +0x18 | u64 / u32 | character handle: slot pointer / id |
| +0x120 | s32 | tracker id |
| +0x134 | s32 | team |
| +0x2a0 / +0x2a4 | s32 | booster index of slot 0 / 1 (into the *boosters* table) |

### 3.4 Records looked up by id (LookupPointer 0xa15e70)

| Id | Meaning | Layout |
|---|---|---|
| 0x9353ed16 | HUD settings | +0x90: u64 pointer to the multiplayer widget set ("set") |
| 0x3b9a067d | *net-games* | s32 count at +0; u64 records pointer at +8; records of 0x90 bytes |
| 0x916e03b0 | *boosters* | s32 count at +0; u64 records pointer at +8; records of 0x20 bytes |
| (net game record +0x7c) | wave table | s32 count at +0; u64 entries pointer at +8; entries of 12 bytes {s32 wave, s32 time in seconds, s32 value} |

Net game record: +0xc s32 game type, +0x6c s32 boosters off, +0x7c u32 wave table id. Game types
4, 8, 9 and 12 are co-op. 7 is Turf War, 10 elimination, 11 has no score panel, 1 and 2 have no
carrier icon, 9 is the money / wave co-op game.

Booster record (both the listed ones and *boosters*): +0x8 u32 image sid (net table), +0xc u32
booster id. Placeholder ids: 0x05454b23 (never drawn in slot 0), 0x01845694 (never drawn in slot 1).

### 3.5 Widget set (pointer at HUD settings record +0x90)

Each field is a u64 widget pointer unless noted.

| Field | Meaning |
|---|---|
| +0x08 | text: own team score |
| +0x10 | text: other team score |
| +0x18 / +0x20 / +0x28 | image: carrier icon without carrier / carrier on the local team / carrier on the other team |
| +0x30 | text: value (score limit or round) |
| +0x38 | text: label |
| +0x40 | text: minutes |
| +0x48 | text: seconds |
| +0x58 | image: score panel frame |
| +0x60 | image: Turf War frame |
| +0x68 | 4 x f32 in place: Turf War rect x, y, w, h |
| +0x78 | text: Turf War number template |
| +0xb8 | image: message line background |
| +0xc0 | 2 x f32 in place: co-op panel position |
| +0xc8 | text: co-op money |
| +0xd0 | image: co-op panel background |

Text widget (0x2c bytes used): +0x00 x, +0x04 y, +0x08 w, +0x0c h (f32), +0x10 u32 alignment,
+0x14 f32 scale, +0x18 / +0x1c / +0x20 / +0x24 f32 r, g, b, a, +0x28 u32 font.

Image widget: +0x00 u64 pointer to its material name (C string), +0x10 u32 tag (0x4240ef2e =
sid "net" means table 1, anything else table 0), +0x14 u32 image id, +0x18 s32 index (negative =
not found), +0x24 / +0x28 f32 position x / y.

### 3.6 Material tables and handles

A material table (pointer at hud+0x68 + 8*t): u16 at +2 = count, u64 at +8 = entries pointer,
entries of 0x28 bytes, the entry's id at entry+8. Table names for logs: 0 "hud", 1 "net",
2 "menu", anything else "?". A table number above 2 has no table (NULL).

A handle is {u32 id, u32 table, s32 index}, 12 bytes in game memory in that order. Index -1 means
"look it up". A handle "of sid S" built by this module is {S, 1, -1}.

### 3.7 Multiplayer element (the `element` argument)

| Offset | Meaning |
|---|---|
| +0x40 | message list: s32 count; entries of 0x80 at +0x48 (list+8), at most 16 |
| +0x860 | names sub-object ("names") |
| +0x1560 | world markers sub-object ("markers") |
| +0x1920 | f32 element fade |
| +0x1928 | theater sub-object |
| +0x1a90 | centre message (0x90 bytes) |

Message entry: +0x00 text 1 (48 bytes), +0x30 u32 colour 1 (0x00RRGGBB), +0x34 text 2 (48 bytes),
+0x64 u32 colour 2, +0x68 u64 weapon icon record pointer, +0x70 f32 start time (s), +0x74 f32 end
time (s), +0x78 u32 flags (2 = red badge, 4 = badge, 8 = headshot), +0x7c s32 count.

Weapon icon record: +0x10 u64 image name pointer, +0x18 / +0x1c f32 size x / y, +0x28 f32 divisor,
+0x30 f32 multiplier.

Names sub-object: entries of 0xa0 at +0, at most 20; +0xc80 s32 count. Handles (12 bytes each):
+0xca8 player arrow, +0xcb4 player "ex" arrow, +0xcc0 triangle button, +0xccc knocked down,
+0xcd8 grappled, +0xce4 status glow, +0xcf0 status ring.

Name entry: +0x00 / +0x04 f32 screen x / y, +0x08 f32 scale s, +0x0c s32 side (0 above, 1 left,
2 right, 3 on screen; anything else is treated like 3), +0x10 s32 arrow kind (0 "ex" arrow, 1 or 2
player arrow, other none), +0x20 V4 arrow / status colour, +0x30 s32 status (0 none, 1 down,
anything else grappled), +0x34 f32 status fill, +0x38 name (64 bytes), +0x80 V4 name colour,
+0x90 u64 emblem pointer (non-zero = has emblem), +0x98 u8 talking, +0x9c s32 rank.

Markers sub-object: +0 s32 count (at most 8 are drawn); entries of 0x40 at +0x10; type records
of 0x24 at +0x210 (11 types, 0..10); +0x39c edge arrow handle; +0x3a8 ring meter handle.
Marker entry: +0x04 u32 type, +0x10 V4 world position, +0x20 u32 colour 0xAARRGGBB, +0x24 u32
ring colour 0xAARRGGBB, +0x2c f32 fade, +0x30 f32 fade target, +0x34 f32 ring fill, +0x3c s32
frame when last set (clock 1). Type record: +0x00 icon handle, +0x0c pulsing icon handle (id 0 =
none), +0x18 f32 size, +0x1c u8 dark flag, +0x20 s32 number label (-1 none).

Centre message: +0x00 text (128 bytes), +0x80 s32 start (clock 2 frame), +0x84 u32 colour
0xAARRGGBB, +0x88 s32 glyph counter.

Theater sub-object: +0x0 s32 selected index, +0x4 f32 smoothed index, +0x8 u32 pending release
id, +0xc cameras of 0x18 bytes (+0 u32 callback id, +4 u32 release id, +8 image handle, +0x14 u32
name text id), +0xfc s32 camera count, +0x100 shown list (u64 absolute camera pointers, 10),
+0x150 s32 shown count, +0x154 f32 fade, +0x158 s32 time stamp.

### 3.8 Territory, carry flag and character objects

Territory object: +0x44 u32 object id, +0x234 s32 owner team (-1 none), +0x238 s32 capturing
team (-1 none), +0x250 s32 index 1..5, +0x258 / +0x25c s32 capture / loss end frame (0 = none),
+0x268 / +0x26c s32 capture / loss length in seconds.

Carry flag object: +0x30 u32 type (0x310f465e = capture flag), +0x44 u32 object id, +0x440 s32
owner tracker id, +0x68 u64 tag table pointer. Tag table: +0 u64 entries pointer, +8 u32 count;
entries of 0x10: +0 u32 tag, +8 u64 value pointer. Value: +0 u16 type (1 = boolean), +8 u64
pointer to a u32.

Character object: +0x44 u32 object id, +0x2928 u64 pointer to an object whose vtable slot +0xe8
returns the held object.

Local player object: +0x0 vtable pointer (slot +0x260 = current weapon), +0x44 u32 object id,
+0x2768 u64 weapon controller pointer (controller vtable slot +0x2f0 = ShowWeaponHud).

A "live handle" {slot pointer, id}: both non-zero, the object pointer `*slot` non-zero, and that
object's +0x44 equals the id.

### 3.9 Constants

| Constant | Value |
|---|---|
| HUD flag: boosters (loadout) | 0x10 |
| HUD flag: co-op panel | 0x8000 |
| HUD flag: names and markers | 0x20000 |
| HUD flag: score panel, and film boosters | 0x40000 |
| HUD flag: message list | 0x80000 |
| HUD flag: theater | 0x100000 |
| Text ids | 0xd38a7d18 "score limit" label, 0xe1167dfa "round" label, 0x9b5b3c66 count prefix |
| Net image sids (table 1) | 0x12ffe6ae plain square, 0x68951e57 ring, 0x2fb98f98 headshot, 0xbda0f568 rank icons, 0xf1b63553 rank icons (rank > 63), 0xffa98ec5 talking |
| Carry blink tag | 0xe9a3dd67 |
| Theater camera ids | 0x3cf5deed always shown, 0xe8184ce8 never shown, 0xff502160 only in cinema mode 1, 0x806c9824 only in mode 2, 0x887c7d6e only in mode 0 |
| Pad bits | L1 0x400, R1 0x800, cross 0x4000 |
| Camera change sound | 0xf57d53ce |
| Screen | 1280 x 720 |
| pi, pi/2, 2 pi | f32 literals 3.14159265f, 1.57079633f, 6.28318531f |
| Down colour | (0.9411765f, 0.0941177f, 0.0627451f) |
| Grappled colour | (0.1254902f, 0.5019608f, 0.9411765f) |
| No-owner territory colour | 0xb0f0e030 |
| Badge colours | 0xff5495bb (blue), 0xffd73e60 (red) |

## 4. Behaviour

### 4.1 Shared primitives (observable rules)

**Lookup of the current net game record.** Guest call LookupPointer(0x3b9a067d). The record is
none when the result is NULL, or the s16 ngm+0x1ca0 is negative, or it is >= the s32 count at
+0, or the records pointer at +8 is NULL. Otherwise it is records + 0x90 * index. Each use below
that says "net game record" makes this guest call again.

**Team score of team t.** Read the u64 at 0x104ae70. When it is 0, or t < 0, or t > 7, the score
is 0. Otherwise the u16 at table + 0x3dee + 0x34*t. No guest call.

**Text widget draw ("widget text").** Read the widget pointer from the set field. When NULL,
nothing. Otherwise guest call 0x5555a0(widget, pos, fmt, 1.0f, value) with pos a temporary
(0, 0) unless stated.

**Label.** Read the widget pointer; when NULL, nothing (no TextDb call). Else guest call
TextDb::Find(B + 0x1066660, id). When it returns NULL, nothing. Else 0x5555a0(widget, temporary
(0, 0), "%s", 1.0f, the text).

**Image widget draw (with a name `what` for logs).** Given a widget pointer, a position pointer
and an alpha:

1. Widget NULL: nothing.
2. Guest call 0x5559f0(draw_ctx, widget, pos, alpha).
3. Then read the widget's table (1 when tag +0x10 is 0x4240ef2e, else 0), id +0x14 and index
   +0x18. When the index is >= 0, done.
4. When the id is non-zero, guest call 0x5558c0(id | table << 32, -1). When the id is 0 there is
   no call and the result counts as NULL. When the result is non-NULL, done.
5. Otherwise log once per key (see "log-once set"): the key is (id, table | 0x100), where id is
   replaced by the low 32 bits of the widget's absolute address when the id is 0. When the key is
   new, log
   `hud: image %s (widget %s) id %08x table %u (%s) not resolved: %s table %s, %u materials`
   with: what; the C string at the u64 pointer at widget+0 (or `(no name)` when that pointer is
   0); id; table; table name; table name again; `set` or `NULL` (material table of that number);
   that table's count (0 when NULL).

"Frame image of field F" means: image widget draw of the set's field F, position temporary
(0, 0), alpha 1.0f, what `frame`.

**Log-once set.** One process-wide set of up to 96 (u32 id, u32 table) pairs, shared by the image
widget log above and the resolve logs below. Asking "is it new" adds the pair and answers yes when
it is not present and there is room. When the set is full, every not-yet-present pair answers no
(and is not added). A present pair answers no.

**Resolve (handle, what) -> (ok, resolved handle).** The cache and its guest calls are
observable and must be reproduced exactly:

1. Id 0: not ok. No call, no cache entry, no log. The resolved handle is not produced (callers
   stop).
2. Find the material table of the handle's table number (NULL when table > 2) and its count
   (0 when NULL).
3. Cache: up to 64 process-wide entries, each keyed by (id, table, index) of the input handle
   plus the table pointer and the count seen at the time. A hit returns the stored result and
   makes no call.
4. On a miss: when the table pointer is NULL there is no guest call and the result is NULL.
   Else guest call 0x5558c0(id | table << 32, index). ok = result non-NULL. The resolved handle
   is the input handle, except: when the result is non-NULL, the table and its entries pointer
   are non-NULL, and the result lies in [entries, entries + 0x28*count), the index becomes
   (result - entries) / 0x28 and the id becomes the u32 at result+8.
5. Logs: when not ok and (id, table) is new in the log-once set:
   `hud: image %s id %08x table %u (%s) index %d not resolved: %s table %s, %u materials%s`
   with what, id, table, table name, input index, table name, `set` / `NULL`, count, and the
   suffix ` (PS4 net.pak by id and by PS3 name, then net-u2mp)` when table is 1, else empty.
   When ok and the resolved id differs from the input id and (id, table) is new:
   `hud: image %s id %08x table %u -> PS4 id %08x index %d` with what, input id, table, resolved
   id, resolved index. The set is only asked in these two cases (in that order of conditions).
6. Store the entry: append while fewer than 64; when full, overwrite entries 0, 1, 2, ... in
   turn (a round-robin counter that starts at 0 the first time the cache is full and never
   resets).

**Quad (handle, colour V4, pos, size, centred = no, dir = (0, -1), uv0 = (0, 0), uv1 = (1, 1),
what = `quad`).** When colour.w <= 0, nothing (no resolve). Else resolve; when not ok, nothing.
Else guest call 0x555c60(draw_ctx, resolved id | resolved table << 32, resolved index, &colour,
centred, 1, pos, size, dir, uv0, uv1).

**Rect (handle, colour, corner p0, corner p1, uv0 = (0, 0), uv1 = (1, 1), what = `rect`).** A
quad at pos p0 with size (p1.x - p0.x, p1.y - p0.y), computed by subtracting the corner values
given (not from a width), not centred, dir (0, -1).

**Free quad (handle, colour, 4 corners, 4 uvs, what).** When colour.w <= 0, nothing. Resolve the
handle given (callers pass an already resolved handle, so this is a second cache lookup with
that key). When not ok, nothing. Then always guest call 0x5558c0(id | table << 32, index) of
that handle's resolved value (no cache). When NULL, nothing. Else guest call 0xbdce90(context,
info, vertices) with:

- context: 48 bytes, 16-aligned: u64 draw_ctx, then zeros.
- info: 32 bytes, 16-aligned: u64 material pointer, then zeros.
- vertices: 0x110 bytes, zeroed, 4 vertices of 0x40 bytes: +0 x, +4 y, +8 z = 0, +0xc u, +0x10 v,
  +0x14..+0x1f zero, +0x20 colour V4 (as given, no HUD alpha applied here), +0x30 (1, 1, 1, 1);
  then +0x100..+0x10f zero.

**Ring meter (centre, handle, colour, scale, fill, r_inner, r_outer).**

1. Resolve the handle with what `ring meter`. Not ok: nothing more.
2. n = int(m * ((1 - c) * 10 + c * 30)) where c = clamp01(fill) and m = scale when scale > 1,
   else 1. When n <= 0, nothing more.
3. colour.w = colour.w * [0x104ed0c].
4. ri = scale * r_inner, ro = scale * r_outer; uv_out = 0.45; uv_in = r_inner * 0.45 / r_outer.
   step = fill * 2pi / float(n) (fill unclamped).
5. For i = 0 .. n-1: a0 = float(i) * step + pi/2, a1 = float(i+1) * step + pi/2. A free quad
   (what `ring meter`) of the resolved handle with corners, in order: inner at a0, outer at a0,
   outer at a1, inner at a1, where "radius r at angle a" is (centre.x + r * cos a, centre.y +
   r * sin a), and uvs (0.5 + k * cos a, 0.5 + k * sin a) with k = uv_in for inner and uv_out
   for outer.

All callers in this module use r_inner 30 and r_outer 50.

**Edge arrow (handle, pos, u32 colour, scale, angle, turned).** size = scale * 40;
back = size * (-0.25 when turned, else -0.5). dir = (cos(angle - pi/2), -sin(angle - pi/2)) when
turned, else (0, -1). The quad (what `arrow`) is centred at (pos.x + back * (-cos angle),
pos.y + back * sin angle), size (size, size), colour argb(colour), dir as above, default uvs.
The push-back uses the angle in both cases.

**DrawText (x, y, scale, colour V4, flags, box_w, box_h, text) -> (w, h).** When the text is NULL
or empty: no call, result (0, 0). Else a zeroed 16-aligned 0xc0-byte state:

- +0x00 x, +0x04 y, +0x14 -1.0, +0x1c -1.0, +0x20 scale, +0x24 scale;
- +0x28..+0x2b bytes r, g, b, a, each `int(clamp01(c) * 255.0f)` truncated to 8 bits;
- +0x2c 1.0, +0x30 u32 0xffffffff, +0x34 u32 flags;
- +0x50, +0x64, +0x78, +0x8c, +0x90 1.0;
- +0x94 box_w, +0x98 box_h; everything else 0.

Guest call 0xcc9a70(state, text). When flags has bit 4, the result is w = f32 state+0x9c and
h = f32 state+0x98 read after the call; otherwise (0, 0). Flags used: 1 centred, 8 normal,
12 measure.

**Time left (start field).** limit = int([ngm+0x21b8] * 300.0f). When limit <= 0, nothing.
elapsed = [frames] - [ngm + start field] (s32). secs = int(float(limit - elapsed) * 1/300).
mins = secs / 60. Widget text set+0x40 with "%i:" and (mins % 60); then widget text set+0x48
with "%02i" and (secs % 60). Negative values are printed as they come.

### 4.2 Multiplayer Draw (vtable 0x103f550 slot 1), overall order

Arguments: element (absolute), draw_ctx.

1. When s32 [0x14ce97c] < 2: return.
2. Read the u64 at 0x1130068. When 0: return. Else flags = the u32 it points to.
3. Guest call LookupPointer(0x9353ed16). set = the u64 at result+0x90 (NULL when the result is
   NULL).
4. local = u64 [ngm+0]. Net game record (guest LookupPointer(0x3b9a067d)). Steps 3 and 4 always
   both happen once step 2 passed.
5. When set, local or the record is NULL: return.
6. type = record+0xc. co-op = type is 4, 8, 9 or 12. team = local+0x134; other team = 1 when
   team is 0, else 0.
7. A process-wide draw counter (u64, starts at 0) is incremented. When it is 1 or a multiple of
   600, log
   `hud: multiplayer draw #%llu: type %d, flags 0x%x, team %d, scores %u/%u, limit %d, time limit %.0f s, names %d, markers %d, messages %d; draw list %p, image tables hud %u / net %u`
   with: counter; type; flags; team; team score of team; team score of other team; s32
   ngm+0x21b4; f32 ngm+0x21b8 as double; s32 element+0x860+0xc80; s32 element+0x1560; s32
   element+0x40; draw_ctx (`%p`); count of material table 0 (0 when NULL); count of table 1.
8. When the weapon trigger is on (4.12) and the counter is 1 or a multiple of 600, log
   `hud: weapon panel: %llu switches, ShowWeaponHud %llu by the game (from %llx %llx %llx %llx) / %llu here, panel draws %llu, flags 0x%x`
   with: switch count; game ShowWeaponHud count; caller slots 0, 1, 2, 3; own ShowWeaponHud
   count; panel draw count; flags.
9. Weapon trigger tick (4.12).
10. Score panel (4.3) when not co-op, type != 11 and flags has 0x40000.
11. Co-op panel (4.6), always reached (it checks its own conditions).
12. Theater (4.11) when u8 ngm+0x209c != 0 and flags has 0x100000 and the environment variable
    `UNCHARTED_U2MP_NO_THEATER` is not set (any value counts as set). The variable is only read
    when the first two conditions hold.
13. Boosters (4.7).
14. Message list (4.8) when not co-op, type != 11 and flags has 0x80000.
15. When flags has 0x20000: names (4.9), then markers (4.10).
16. Centre message (4.5), always.

### 4.3 Score panel

Type 7: the Turf War panel (4.4) instead of everything below.

Other types, in this order:

1. Frame image of set+0x58.
2. Widget text set+0x08, "%i", team score of team.
3. Widget text set+0x10, "%i", team score of other team.
4. Type 10: label set+0x38 with text 0xe1167dfa; widget text set+0x30, "%i", s32 ngm+0x1d70 + 1;
   time left from ngm+0x1d60.
5. Other types: label set+0x38 with text 0xd38a7d18; widget text set+0x30, "%i", s32 ngm+0x21b4;
   time left from ngm+0x1d5c; then, when type is neither 1 nor 2, the carrier icon (4.4b).

### 4.4 Turf War panel (type 7)

1. Frame image of set+0x60.
2. Time left from ngm+0x1d5c.
3. img = set+0x60 widget, tw = set+0x78 widget. When img is NULL: stop.
4. Rect values rx, ry, rw, rh = f32 set+0x68, +0x6c, +0x70, +0x74. step = rw * 0.2;
   half = rh * 0.5; x0 = (img+0x24 + rx) + step * 0.5; top = img+0x28 + ry.
5. A template text widget: a 16-aligned 0x40-byte temporary holding the first 0x2c bytes of tw
   (zeros when tw is NULL), bytes 0x2c..0x3f zero.
6. For slot i = 0..4, in order:
   - territory number n = 5 - i when local is non-NULL and local+0x134 == 1, else i + 1;
   - find the territory: for handles j = 0 .. min(u32 ngm+0x1dd0, 5) - 1, skip a handle whose slot
     pointer or id is 0; the object is `*slot`; take the first whose object is non-NULL, whose
     +0x44 equals the handle id and whose +0x250 equals n. None: next slot;
   - owner = obj+0x234. colour c = (0xb0f0e030 when owner < 0, else the team colour of owner) OR
     0xff000000. Team colour of t: a zeroed 16-aligned 0x40-byte temporary, guest call
     0x64aeb0(temp, B + 0x113b760, t, -1), result = u32 temp+0x10. col = argb(c);
   - p0 = ((x0 + step * float(i)) - half, top);
   - when obj+0x238 (capturer) >= 0: progress (below); when progress < 0.99: ring meter centred
     at (p0.x + half, p0.y + half), handle of sid 0x68951e57, colour argb(team colour of
     capturer OR 0xe0000000), scale 0.4, fill = progress;
   - rect of handle sid 0x12ffe6ae, colour (col.r * 0.5, col.g * 0.5, col.b * 0.5, col.w), from
     p0 to (p0.x + rh, p0.y + rh);
   - when tw is NULL: next slot;
   - the number text is "%i" of i + 1 (the slot number, also when the order is reversed);
   - a copy of the template with +0x08 = rh, +0x0c = rh, +0x14 = 0.7, r, g, b = 0, a = 0.5647059f;
     guest call 0x5555a0(copy, pos temporary (p0.x, p0.y), "%s", 1.0f, number);
   - then the same copy with +0x04 (y) decreased by 2.0 and r, g, b, a = col; same call again.

Progress: now = [frames]. For an end field E and a length field L:
len = int(float(obj+L) * 300.0f); part = (float(now - obj+E + len) * 1/300) / (float(len) * 1/300)
(the integer sum is s32, evaluated left to right). When obj+0x258 != 0: clamp01(part(0x258,
0x268)). Else when obj+0x25c != 0: clamp01(1 - part(0x25c, 0x26c)). Else 0. A zero length gives
NaN or infinity; NaN is not < 0.99, so no meter is drawn then.

### 4.4b Carrier icon

1. The flag: when the carry handle ngm+0x1cc0 / +0x1cc8 is live (3.8), its object. Otherwise the
   held capture flag (below). None: nothing.
2. owner = flag+0x440. Carrier = the first tracker i (0..9, at ngm+0x10+0x2c0*i) whose +0x120
   equals owner, or none.
3. Widget field: +0x18 without carrier; +0x20 when the carrier's +0x134 equals team; +0x28
   otherwise.
4. Blink: when there is a carrier, the flag's tag 0xe9a3dd67 is true, and
   fmod(float([frames]) / 300.0f, 1.0f) >= 0.5 (a real division here), nothing is drawn.
   "Tag true": tag table pointer at flag+0x68 non-NULL, its entries pointer non-NULL, its count
   from 1 to 4096; the first entry with that tag decides: its value pointer non-NULL with u16
   type 1, and the pointer at value+8 non-NULL and the u32 there non-zero. Any other case: false.
5. Else frame image of that field (what `frame`).

Held capture flag: for trackers i = 0..9 in order: skip unless the tracker's character handle
(+0x10 slot, +0x18 id) is live; obj = character+0x2928, skip when NULL; vtable = u64 obj+0, skip
when 0; fn = u64 at vtable+0xe8, skip when 0; guest call fn(obj, 0); when the result is non-NULL
and its +0x30 is 0x310f465e, that is the flag (stop). None found: none.

### 4.5 Centre message (element+0x1a90)

1. First byte 0: nothing.
2. Text = the 128 bytes, cut at the first NUL. glyphs = the number of bytes before the NUL whose
   top two bits are not `10` (UTF-8 code points).
3. u = float([0x1116284] - s32 msg+0x80) * (1/75 as f32 constant `1.0f/75.0f`).
   u_last = u - 0.1 * float(glyphs - 1).
4. When u_last > 10: write u8 0 at msg+0; nothing drawn.
5. Else write s32 0 at msg+0x88. alpha = ramp(u, 0, 1) - ramp(u_last, 9, 10), where ramp(x, a, b)
   is 0 when x < a, 1 when x >= b, else (cos(clamp01((x - a) / (b - a)) * pi) - 1) * -0.5.
6. DrawText at (240, 288) (that is (1280 - 800) * 0.5 and 720 * 0.4f), scale 1.2, colour
   (argb(msg+0x84).rgb, clamp01(alpha) * [0x104ed0c]), flags 1, box 800 x 20, the text.

### 4.6 Co-op money / wave panel

1. When flags lacks 0x8000: nothing.
2. Net game record (guest lookup). None, or type != 9: nothing.
3. When f32 [0x104f564] != 0 (NaN counts as != 0): nothing.
4. pos = the address set+0xc0 itself (in game memory, not a copy).
5. Image widget draw of set+0xd0 with that pos pointer, alpha 1.0, what `co-op meter back`.
6. txt = set+0xc8 widget. Guest call 0x64cbd0(B + 0x113b760) -> money (always, also when txt is
   NULL).
7. When txt: guest call 0x5555a0(txt, set+0xc0, "x%i", 1.0f, money).
8. left = wave left (below).
9. Ring meter centred at (f32 set+0xc0, f32 set+0xc4), handle of sid 0x68951e57, colour
   (txt+0x18, +0x1c, +0x20, +0x24) when txt, else (1, 1, 1, 1), scale 0.475, fill left.

Wave left: net game record (another guest lookup); id = record+0x7c (0 when no record); when the
id is non-zero, guest LookupPointer(id). No table: 0. start = ngm+0x1d64, end = ngm+0x1d68;
secs = float((end when end > start, else [frames]) - start) * 1/300. wave = ngm+0x1d6c + 1. When
the entries pointer is NULL: 0. For k = 0 .. count-1: cur = entry k, prev = entry k-1 (entry 0
for k = 0). When cur.wave != wave: when wave < cur.wave, result 0; else next entry. When it
matches: from = float(prev.time), to = float(cur.time); when secs < from, or not (to > secs),
next entry; else result (from - secs) / float(cur.time - prev.time) + 1 (s32 difference). No
result in the loop: 0.

### 4.7 Booster icons

fade = f32 element+0x1920.

1. Film site: when u8 ngm+0x209c != 0, flags has 0x40000, s32 [0x113b008] != 0 and local is
   non-NULL: a booster pair of (booster by index local+0x2a0, booster by index local+0x2a4).
2. Loadout site: when not co-op and flags has 0x10 and boosters are allowed: a booster pair of
   (booster by id [0x1147544], booster by id [0x1147548]). "Allowed" is checked only when the
   first two conditions hold.

Boosters allowed: false when [0x14ce97c] < 2. Else net game record (guest lookup); type = its +0xc
(-1 without record); off = s32 ngm+0x1e80. Co-op type: allowed when off == 0. Otherwise allowed
when off == 0, or there is no record, or record+0x6c == 0.

Booster by id: id 0 gives none. Else the first of the min(u32 [0x113a788], 64) listed record
pointers that is non-NULL and whose +0xc equals the id. No guest call.

Booster by index: index < 0 gives none, without a call. Else guest LookupPointer(0x916e03b0);
none when NULL, index >= its count, or its records pointer is NULL; else records + 0x20 * index.
The slot 0 lookup happens before the slot 1 lookup.

Booster pair (a, b): white = (1, 1, 1, fade * 0.75). x = f32 [0x1130348] + 90. size (76.8, 38.4).
When a is present and a+0xc != 0x05454b23: quad of handle {u32 a+8, 1, -1}, white, at (x, 150),
what `booster`; the second y becomes 180.72 (else it stays 150). Then when b is present and
b+0xc != 0x01845694: the same at (x, second y).

### 4.8 Message list (element+0x40)

1. count = s32 list+0. When count <= 0 or > 16: nothing.
2. now = float([0x11161c4]) * 1/300. hud_alpha = [0x104ed0c].
3. For entries i = 0 .. count-1 (oldest first), entry at list+8+0x80*i:
   appear = clamp01((now - f32 +0x70) * 2.2222223); vanish = clamp01((f32 +0x74 - now) * 2.0);
   used accumulates vanish * 25 in this order (starting at 0);
   x_i = 120 + ((-200 * (1 - appear)) * (1 - appear)); alpha_i = (appear * vanish) * hud_alpha.
4. back = set+0xb8 widget. y = 650 - used.
5. For i = count-1 down to 0 (newest on top), y increasing by 25 after each line:
   - w = the line measured (4.8b, measure, scale 1);
   - s = 350 / w when w > 350, else 1;
   - when back is non-NULL: image widget draw of back at temporary (x_i - 8, y - 2), alpha
     alpha_i, what `message back`;
   - the line drawn (4.8b, draw, scale s).

### 4.8b One message line (entry, x, y, alpha, s, measure or draw)

Entry colour k = (R/255, G/255, B/255, alpha) from the u32 0x00RRGGBB at +0x30 (text 1) or +0x64
(text 2). Texts are the 48 bytes at +0x00 / +0x34, cut at the first NUL. flags = u32 +0x78,
count = s32 +0x7c.

1. Badge, draw only, when count != 0 and flags has 2 or 4: red = flags has 2. Prefix = "" when red,
   else guest TextDb::Find(B + 0x1066660, 0x9b5b3c66) ("" when NULL). badge text = "%s%i" of
   (prefix, count), cut to 23 characters. bc = argb(0xffd73e60 when red, else 0xff5495bb).
   Resolve the handle of sid 0x12ffe6ae with what `message badge`. When ok: quad of the resolved
   handle, colour (bc.rgb, alpha), at (x - 58, y - 1), size (50, 25), what `message badge`.
   Then DrawText at (x - 58, y + 4), scale 0.4, colour (0, 0, 0, alpha) when the square resolved,
   else (bc.rgb, alpha), flags 1, box 50 x 25, the badge text.
2. ts = 0.5 * s; cx = x.
3. Text 1 non-empty: DrawText measure (cx, y, ts, colour 1, flags 12, box 999999 x 40); then, when
   drawing, DrawText (same, flags 8); cx += the measured w.
4. cx += 10 * s.
5. Weapon icon: when the pointer at +0x68 is non-NULL, its name pointer non-NULL with a non-empty
   string, and 0 < size x < 1024 and 0 < size y < 1024: k = 0.45 * s; w = sx * k; h = sy * k;
   advance = (w * mul) / div when div > 0, mul > 0 and mul / div < 4, else w. When drawing: guest
   call 0x555140(name, 0); quad of handle {low u32, high u32 of the first returned value,
   returned index}, colour (1, 1, 1, alpha), at (cx, (y + 10) - h * 0.5), size (w, h), what
   `message weapon icon`. Then cx += advance + 10 * s. Icons that fail the checks change nothing.
6. Headshot, when flags has 8: hs = 28 * s. When drawing: quad of handle sid 0x2fb98f98, colour
   (1, 1, 1, alpha), at (cx - 4 * s, (y + 10) - hs * 0.5), size (hs, hs), what `message headshot`.
   cx += 2 * s + hs.
7. Text 2 non-empty: DrawText measure (cx, y, ts, colour 2, 12, 999999 x 40), giving w2; when
   drawing, DrawText (same, flags 8). Empty: w2 = 0.
8. Result: (cx + w2) - x.

### 4.9 Names over heads (element+0x860)

For entries i = 0 .. min(count +0xc80, 20) - 1, each at names + 0xa0*i:

1. t = float([0x1116204]) * 1/300. s = +0x08, (x, y) = +0x00 / +0x04. Text = 64 bytes at +0x38,
   cut at NUL. name colour = V4 +0x80; name_alpha = its w; then its w *= [0x104ed0c].
2. (W, H) = DrawText measure (0, 0, s, name colour, flags 12, box 99999 x 40, text) (no call when
   the text is empty; then W = H = 0).
3. emblem = u64 +0x90 != 0; rank = s32 +0x9c. icon = s * 50 when emblem or rank > 58, else 0.
   talk = s * 40 when u8 +0x98 != 0, else 0. half = (W + icon) * 0.5.
4. left = x - half; raised to 128 when below 128; then lowered to (1280 - (W + icon)) - 128 when
   above that (this second limit wins). cx = left + half. base = y - s * 25.
   status_at = (cx, ((y - s * 150) - s * 25) + s * 30).
5. Arrow place by side (+0x0c):
   - 0: at (cx, base - H), arrow scale 1.5, angle pi/2;
   - 1: at ((cx - half) - 10, base - H * 0.5), 1.2, angle pi;
   - 2: pad = talk * 0.5 + 6 when the text is non-empty and name_alpha != 0, else 0;
     at (((cx + half) + pad) + 10, base - H * 0.5), 1.2, angle 0;
   - any other value: at (cx, base), 1.5, angle -pi/2.
6. status = +0x30; col = V4 +0x20. There is an arrow unless status != 0 and side == 3.
   When status != 0 and there is an arrow: k = clamp01(1 - fabs(cos(t * 8))) * 20;
   arrow x += cos(angle) * k; arrow y -= sin(angle) * k.
7. Arrow, by +0x10: 0: edge arrow of handle names+0xcb4, not turned; 1 or 2: edge arrow of handle
   names+0xca8, turned; other: none. Colour pack(col), scale arrow scale * s, the angle.
8. When status != 0:
   - c = down colour when status == 1, else grappled colour; c.w = col.w. ring = c, except
     (1, 1, 1, col.w) when fill (+0x34) < 0.25 and fmod(t * 3, 1) < 0.5;
   - p = clamp01(fabs(cos(t * 4))); big = s * 50; pulse = big * ((1 - p) * 3.5 + p * 5);
     still = big * 4.5; dir = (cos t, sin t);
   - quad of handle names+0xce4, colour (c.rgb, col.w * 0.75), at status_at, size (pulse, pulse),
     centred, dir, what `name status glow`;
   - quad of the same handle, colour (0, 0, 0, col.w), at status_at, size (still, still),
     centred, dir, what `name status glow`;
   - ring meter centred at status_at, handle names+0xcf0, colour ring, scale s, fill +0x34;
   - r = s * 30. button = the u64 at 0x1130068 is non-zero and the byte at that pointer + 0x40c is
     non-zero. Handle: status 1: names+0xcc0 when button, else names+0xccc; other status:
     names+0xcd8. Rect of it, colour (1, 1, 1, col.w), from (status_at.x - r, status_at.y - r)
     to (status_at.x + r, status_at.y + r), what `rect`.
9. Text empty: done with this entry.
10. ty = base - H.
11. Rank icon when there is no emblem and rank > 58: u0 = float(rank & 7) * 0.125;
    v0 = float((rank & 63) >> 3) * 0.125; handle of sid 0xf1b63553 when rank > 63, else
    0xbda0f568; hx = hy = icon * 0.5; ix = (left + hx) + s * -8; iy = (ty + s * -4) + hy. Rect,
    colour (1, 1, 1, name_alpha), from (ix - hx, iy - hy) to (ix + hx, iy + hy), uv0 (u0, v0),
    uv1 (u0 + 0.125, v0 + 0.125). An emblem is not drawn; its space is kept.
12. DrawText at (left + icon, ty), scale s, name colour (with HUD alpha), flags 8, box 99999 x 40.
13. When talk > 0: hx = hy = talk * 0.5; ix = ((cx + half) + hx) + s * 6; iy = ty + hy. Rect of
    handle sid 0xffa98ec5, colour (1, 1, 1, name_alpha), from (ix - hx, iy - hy) to
    (ix + hx, iy + hy).

### 4.10 World markers (element+0x1560)

1. count = s32 markers+0. When <= 0: nothing.
2. frame = [0x1116204]; t = float(frame) * 1/300; dt = float([0x1116210]) * 1/300;
   fade_in = dt * 4; fade_out = dt * -4. mask = [0x104f4c4].
3. For i = 0 while i < count (re-read each time) and i < 8, entry at markers+0x10+0x40*i:
   - type = +0x04. Skip when type >= 32 or its mask bit is clear.
   - When frame - s32 +0x3c > 300: write f32 0 at +0x30, then f32 0 at +0x2c; skip.
   - a = +0x2c, target = +0x30. When a < target: a = target when a + fade_in > target, else
     a + fade_in. Then (with the new a) when a > target: a = target when a + fade_out < target,
     else a + fade_out. Write a at +0x2c.
   - Skip when a == 0 or type >= 11.
   - rec = markers+0x210+0x24*type. c = argb(+0x20). Write f32 0 at +0x30.
   - alpha = a * 0.8.
   - Projection: copy the 16 bytes at +0x10 into a 16-aligned temporary; a zeroed 0x30-byte
     output; guest call 0x5566b0(out, temp, 100.0). Output: +0x00 pos (x, y used), +0x10 dir (x, y
     used), +0x20 angle, +0x28 scale, +0x2c s32 clamped.
   - sc = out.scale * f32 rec+0x18; size = sc * 40. pulse = clamp01(fabs(cos(t * 4))).
   - col = (c.rgb, alpha); for types 5..10: k = clamp01((a - 0.5) * 2); col.w = ((1 - k) * 0.1 +
     k) * 0.8.
   - When the handle at rec+0x0c has a non-zero id: ps = size * ((1 - pulse) * 1.5 + pulse * 2.5);
     quad of it, colour col, at (pos.x - ps * 0.5, pos.y - ps * 0.5), size (ps, ps), what
     `marker pulse`.
   - Icon: quad of the handle at rec+0x00, colour (0, 0, 0, alpha) when u8 rec+0x1c != 0, else
     (c.rgb, alpha), at (pos.x - size * 0.5, pos.y - size * 0.5), size (size, size), what
     `marker icon`.
   - When clamped != 0: q = clamp01(1 - fabs(cos(t * 8))); push = (sc * 30) * ((1 - q) * 0.8 +
     q * 1.1); edge arrow (turned) of the handle at markers+0x39c, at (pos.x + dir.x * push,
     pos.y + dir.y * push), colour = pack((c.rgb, alpha)), scale out.scale * 1.75, angle
     out.angle.
   - When fill +0x34 != 0: ring meter centred at (pos.x, pos.y), handle markers+0x3a8, colour
     argb(+0x24), scale out.scale * 1.75, that fill.
   - When s32 rec+0x20 >= 0: DrawText of "%i" of it at (pos.x - 200, pos.y - 100 * out.scale),
     scale 2 * out.scale, colour (c.rgb, clamp01(a) * [0x104ed0c]), flags 1, box 400 x 400.

### 4.11 Theater controls (element+0x1928)

1. cams = +0xfc. When <= 0 or > 10: nothing (no log, no writes).
2. mode = [0x113b008]. Shown cameras, in camera order: id 0x3cf5deed always; 0xe8184ce8 never;
   0xff502160 when mode == 1; 0x806c9824 when mode == 2; 0x887c7d6e when mode == 0; any other
   id always. Write their absolute addresses as u64 at +0x100 + 8*k (k < n; later slots
   untouched) and n at +0x150.
3. The first time ever this point is reached, log
   `hud: theater HUD: %d cameras, %d shown in cinema mode %d` (cams, n, mode).
4. n == 0: stop.
5. buttons = [0x14b7fb0]; pressed = buttons AND NOT the buttons remembered from the previous time
   this step ran (process-wide, starts 0); remember buttons.
6. sel = +0x0. When cross (0x4000) is not held: when the release id +0x8 is non-zero, guest call
   DoCallback(B + 0x1136640, release id, 0) and write u32 0 at +0x8; then sel -= 1 when L1 was
   pressed, sel += 1 when R1 was pressed.
7. sel is limited to n - 1, then raised to 0.
8. When sel differs from the stored +0x0: a zeroed 16-aligned 0x200-byte temporary; guest
   0x3dd780(temp, 0xf57d53ce, 0); guest 0x9b5c20(temp, 0).
9. Writes: +0x0 = sel; +0x4 = f32 +0x4 * 0.5 + float(sel) * 0.5 (smooth); +0x154 = f32 +0x154 *
   0.7 + 0.3 (fade); +0x158 = [frames].
10. When fade >= 0.9 and cross was pressed: DoCallback(B + 0x1136640, camera[sel]+0, 0); write
    camera[sel]+4 at +0x8; log `hud: theater HUD: camera %08x` (camera[sel]+0).
11. Drawing, with A = clamp01(fade). Resolve the handle of sid 0x12ffe6ae, what `theater panel`
    ("box" = ok; the quads below use the resolved handle and what `quad` unless named):
    - panel_w = float(n) * 72 + 40; when box: quad colour (0, 0, 0, 0.6 * A), at
      (640 - panel_w * 0.5, 600), size (panel_w, 100);
    - for i = 0 .. n-1: d = float(i) - smooth; ia = clamp01(1 - 0.3 * |d|) * A;
      x = (640 + d * 72) - 32; when i == sel and box: quad colour (1, 1, 1, 0.35 * A) at
      (x - 4, 618), size (72, 40); then quad of the camera's handle (+8), colour (1, 1, 1, ia),
      at (x, 622), size (64, 32), what `theater camera`;
    - guest TextDb::Find(B + 0x1066660, camera[sel]+0x14); when non-NULL: DrawText at (640, 578),
      scale 0.6, (1, 1, 1, A), flags 1, box 600 x 20;
    - length = s32 [0x113ab68]; pos = int(float([frames] - [0x113b030]) / 300.0f) (a real
      division). When length > 0: fill = clamp01(float(pos) / float(length)); when box: quad
      (1, 1, 1, 0.25 * A) at (440, 672) size (400, 6), then quad (1, 0.82, 0.25, 0.9 * A) at
      (440, 672) size (400 * fill, 6). Then DrawText of m:ss of min(pos, length) at (380, 666),
      and of m:ss of length at (850, 666); both scale 0.5, (1, 1, 1, A), flags 1, box 50 x 20.
      m:ss: negative seconds become 0; "%d:%02d" of (secs / 60, secs % 60).
    - speed = f32 [0x104f1bc]. When speed != 1, > 0 and < 100: DrawText of "x%2.1f" (speed as
      double) at (905, 666), scale 0.5, (1, 1, 1, A), flags 1, box 50 x 20.

### 4.12 Weapon panel trigger

State (process-wide): on/off (off until installed), switch count, game ShowWeaponHud count, own
ShowWeaponHud count, panel draw count, 6 caller slots (u64, 0 = empty), last weapon (u64, 0),
show-until frame (s32, 0), warned flag.

ShowWeaponHud probe (controller, arg): game count += 1. from = the caller's return address minus
B (u64). Put from into the first slot that is empty, unless an earlier slot already holds it; when
all 6 hold other values, nothing is stored. Then guest call 0x954740(controller, arg).

Panel draw probe (element, ctx): panel draw count += 1; guest call 0x563a20(element, ctx).

Tick (step 9 of 4.2):

1. Off: nothing.
2. Local player: cell = u64 [0x115bd20], id = u32 [0x115bd28]; both non-zero, object = u64 at
   cell, non-NULL, object+0x44 == id. Otherwise: last weapon = 0; stop.
3. get = u64 at (player vtable + 0x260) (0 when the vtable pointer is 0). ctrl = player+0x2768;
   show = u64 at (ctrl's vtable + 0x2f0) (0 when ctrl or its vtable is 0).
4. When get != B + 0x71dec0, or show is neither B + 0x954740 nor the address of the
   ShowWeaponHud probe: the first time only, log
   `hud: weapon panel: player slot +0x260 0x%llx / controller slot +0x2f0 0x%llx not the expected functions, weapon switch trigger off`
   with (get - B, or 0 when get is 0) and (show - B, or 0 when show is 0); stop (the next frame
   checks again).
5. weapon = guest call get(player). now = [frames].
6. When weapon != last weapon: when both are non-zero, switch count += 1; last weapon = weapon;
   when weapon != 0, show-until = now + 900.
7. When weapon != 0 and now - show-until < 0 (s32): own count += 1; guest call
   0x954740(ctrl, -100.0) directly (not through the probe).

## 5. Install

`install_hud(installed)`:

1. When `image.base` is NULL: return silently.
2. When `UNCHARTED_U2MP_HUD` is set and its first character is `0`: log
   `hud: UNCHARTED_U2MP_HUD=0, multiplayer HUD draw left as the PS4 stub`; return (the weapon
   trigger is not installed either).
3. Byte checks, in this order; the first mismatch logs
   `hud: %s bytes at 0x%llx differ, multiplayer HUD not installed` (name, offset in lowercase hex)
   and returns:

| Offset | Bytes | Name |
|---|---|---|
| 0x5555a0 | 55 48 89 e5 41 56 53 48 81 ec 10 02 00 00 84 c0 | `text 0x5555a0` |
| 0x5559f0 | 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec | `image 0x5559f0` |
| 0x55aab2 | 48 8d 05 97 4a ae 00 | `element vtable` |
| 0x55ab94 | c7 43 08 00 20 00 00 | `element bits` |
| 0x555c60 | 55 48 89 e5 41 57 41 56 53 48 81 ec b8 02 00 00 | `quad 0x555c60` |
| 0x5566b0 | 55 48 89 e5 41 56 53 48 83 ec 30 48 89 fb c5 78 | `projection 0x5566b0` |
| 0xbdce90 | 55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 40 49 | `menu quad 0xbdce90` |
| 0xcc9a70 | 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 58 12 00 00 | `DrawText 0xcc9a70` |
| 0x5557e7 | 48 8d bd 30 ff ff ff 48 89 de e8 7a 42 77 00 | `text -> DrawText` |
| 0x55d557 | 48 89 83 a8 0c 00 00 89 93 b0 0c 00 00 | `names handles` |
| 0x55b8d2 | 49 63 8f e0 14 00 00 | `names count` |
| 0x55b8e3 | 48 8d 1c 89 48 c1 e3 05 | `names stride` |
| 0x55b918 | 49 89 8c 1f 60 08 00 00 | `names position` |
| 0x55b953 | 41 c7 84 1f 6c 08 00 00 03 00 00 00 | `names side` |
| 0x55c0e1 | 88 84 1e f8 08 00 00 | `names talking` |
| 0x55c0e8 | 49 8b 85 b0 02 00 00 48 89 84 1e f0 08 00 00 | `names emblem` |
| 0x55c08f | 41 8b 85 28 01 00 00 89 84 1e fc 08 00 00 | `names rank` |
| 0x8921bb | c6 80 0c 04 00 00 01 | `HUD flags +0x40c` |
| 0x555340 | 48 8b bf 20 03 00 00 c5 f8 28 09 4c 8d 97 60 15 | `marker add` |
| 0x55ab69 | 48 8d bb 60 15 00 00 | `markers init` |
| 0x55ab81 | 48 8d bb 90 1a 00 00 | `centre message init` |
| 0x649bc1 | 41 89 87 10 1b 00 00 | `centre message start` |

4. When 0x55c5b0 is not a ret stub: log `hud: 0x55c5b0 is not the ret stub, multiplayer HUD not
   installed` (format `hud: 0x%llx is not the ret stub, multiplayer HUD not installed`); return.
5. The booster / co-op and territory bytes below must all match; otherwise log
   `hud: booster / co-op / territory bytes differ, multiplayer HUD not installed`; return.

| Offset | Bytes |
|---|---|
| 0xbdce90 | 55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 40 49 |
| 0x64cbd0 | 55 48 89 e5 41 56 53 48 89 fb bf 7d 06 9a 3b e8 |
| 0x662de6 | 41 89 9c 86 a0 02 00 00 |
| 0x5f61a4 | 47 39 74 ac 4c |
| 0x5e7ddd | 83 b9 78 06 00 00 00 |
| 0x55c515 | 8a 96 9c 20 00 00 |
| 0x560e38 | c5 fa 7e 80 40 03 00 00 |
| 0x66885f | 48 8d 05 de 6c 9e 00 c6 40 1c 00 |
| 0x669844 | 48 8d 05 15 1f ad 00 44 8b 80 d0 1d 00 00 |
| 0x66985e | 48 05 88 1d 00 00 |
| 0x66988c | 83 bf 50 02 00 00 ff |
| 0x67235e | 8b 93 34 02 00 00 |
| 0x6722a8 | 8b 93 38 02 00 00 |
| 0x6724cc | 8b 83 58 02 00 00 85 c0 74 47 c5 fa 2a 83 68 02 00 00 |
| 0x67251d | 8b 83 5c 02 00 00 |
| 0x67237f | 44 8b b5 48 fe ff ff |
| 0x64aeb0 | 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 |
| 0x6237ab | 48 89 8a c0 1c 00 00 89 82 c8 1c 00 00 |
| 0x669a77 | 8b 83 40 04 00 00 41 3b 86 20 01 00 00 |

6. When 0x103f550 + 0x10 > image.size or 0x104ae70 + 8 > image.size: return silently.
7. When the u64 at 0x103f558 is not B + 0x55c5b0: log
   `hud: vtable 0x103f550 slot 1 is not the stub 0x55c5b0, left alone` (format
   `hud: vtable 0x%llx slot 1 is not the stub 0x%llx, left alone`); return.
8. Write the Draw's address at 0x103f558. Log
   `hud: multiplayer HUD draw (PS3 0x8aef68) -> C++ (vtable 0x103f550 slot 1)` (format with
   `0x%llx` for the vtable). `installed` += 1 (only here).
9. Weapon trigger install:
   - when `UNCHARTED_U2MP_WEAPON_HUD` is set and starts with `0`: log
     `hud: UNCHARTED_U2MP_WEAPON_HUD=0, weapon panel trigger off`; stop;
   - bytes 0x954740 = 55 48 89 e5 48 83 ec 10 c5 fa 11 45 fc be fa 20 bd 2b, 0x95477a = 83 08 08,
     0x563a20 = 55 48 89 e5 41 57 41 56 41 55 41 54, 0x71dec0 = 48 63 8f b8 04 00 00; any
     mismatch: log `hud: weapon panel bytes differ, weapon switch trigger off`; stop;
   - when the u64 at 0x1027030 is B + 0x954740, replace it with the ShowWeaponHud probe; when the
     u64 at 0x103f768 is B + 0x563a20, replace it with the panel draw probe (each independent; a
     different value is left alone without a log);
   - trigger on; log `hud: weapon panel: shown for 3 s after a weapon switch (ShowWeaponHud
     0x954740 from the multiplayer draw), probes on` (the 3 comes from `%.0f` of 3.0).

## 6. Test scenarios

Each scenario: set up the fake image, call the Draw (or install), compare guest calls, logs and
memory. Run multi-frame scenarios with several Draw calls in a row; the process-wide state
(counters, caches, log-once set, remembered buttons) is part of what is compared.

Install:

1. `image.base` NULL: nothing. 2. `UNCHARTED_U2MP_HUD=0`: one log, no writes. 3. Each byte check
   in the first table broken in turn: its log. 4. Stub not ret: its log. 5. A booster byte and a
   territory byte broken: the shared log. 6. Image too small: silent. 7. Slot value not the stub:
   its log. 8. All good: slot written, log, installed + 1, weapon trigger installed (log).
9. `UNCHARTED_U2MP_WEAPON_HUD=0`; weapon bytes broken; one or both weapon slots holding other
   values.

Draw gates:

10. Net state 1; flags pointer 0; settings lookup NULL; local 0; net games NULL, index -1, index
    >= count, records pointer NULL: early return after the documented guest calls.
11. Draw counter: draws 1, 2, 600, 1200 (log lines), with and without the weapon trigger on.
12. Types 0, 1, 2, 4, 7, 8, 9, 10, 11, 12 with every flag bit set and with each bit clear.

Score panel:

13. Type 0 / 10: scores (team 0, team 1, team 5, team 9, table pointer 0), label found / NULL /
    widget NULL, time limit 0, positive, elapsed beyond the limit (negative values), each widget
    NULL.
14. Carrier: live carry handle; dead handle with id mismatch; fallback held flag found at tracker
    3, wrong type, vtable 0, slot 0; carrier on own / other team; no carrier; blink tag true at
    frame%300 < 150 and >= 150, tag value type != 1, count 0 / 4097, tag absent.
15. Type 7: local team 0 and 1, local tracker team 1 with reversed order; territories missing,
    id mismatch, owner -1 / team; capturer -1, capture progress 0.5, 0.995, gain and loss cases,
    zero length; tw NULL; img NULL; check both number texts per slot.

Co-op, boosters, theater:

16. Type 9 with flag 0x8000 off / on, wave alpha 0 / 0.5 / NaN, txt NULL, wave table missing,
    wave found at entry 2, wave greater than all entries, wave smaller, end > start and end <=
    start.
17. Boosters: film site with indices -1, 0, out of range; loadout site with ids 0, unknown,
    listed, placeholders 0x05454b23 / 0x01845694 in either slot; boosters off with co-op and
    non-co-op types, record +0x6c 0 and non-zero; fade 0 (no quads).
18. Theater: cams 0 / 11; all modes 0, 1, 2 with each special id; n = 0 (only the first log);
    L1 / R1 presses (edge detection across frames, clamping at both ends, sound on change);
    cross held with a pending release id; cross pressed with fade below and above 0.9 (several
    frames to let the fade rise); length 0 / positive with pos beyond length and negative pos;
    speed 1, 2, 0, 150; square not resolved (no box quads, camera icons still drawn); env
    `UNCHARTED_U2MP_NO_THEATER` set.

Messages, names, markers, centre message:

19. Message list: count 0, 17, 1, 16; entries appearing, fully visible, vanishing; long lines
    (w > 350); badge red / blue, count 0, flags 4 only; square resolving or not; weapon icon
    valid, name empty, size out of range, div / mul limits; headshot flag; text 1 or 2 empty;
    back widget NULL; DrawText stub that writes width and height into state+0x9c / +0x98.
20. Names: count 0, 21; sides 0, 1, 2 (with and without talk and alpha), 3, 7; arrow kinds 0, 1,
    2, 5; status 0, 1 (button byte on / off, flags pointer 0), 2; fill < 0.25 at both blink
    phases; emblem with rank 70; no emblem with ranks 58, 59, 63, 64, 100; empty name with a
    status; left clamping at both ends.
21. Markers: count 0, 9; type 32, mask bit clear, type 11 (fade updated, nothing drawn); stale
    entry (> 300 frames); fade in, fade out, reaching the target; types 4 and 5..10 (alpha
    formula); pulsing handle id 0 / set; dark flag; clamped on / off; fill 0 / 0.5; label -1 / 7.
22. Centre message: empty; ASCII and multi-byte UTF-8 text; u before 0, inside the fade-in, held,
    fading out, past the end (text cleared, nothing drawn).

Resolve cache and logs:

23. Material table NULL (no 0x5558c0 call; NULL log); 0x5558c0 returning NULL (log once); a result
    inside the table with a different id (the "-> PS4 id" log once); a result outside the table;
    more than 64 distinct handles over several frames (round-robin eviction changes which calls
    repeat); more than 96 distinct failing ids (logs stop); table pointer or count changing
    between frames (re-resolve); image widget with index >= 0, id 0, lookup success, lookup
    failure (log with name and `(no name)`).
24. Ring meter: fill 0, 0.5, 1, 1.5, negative; scale 0.4, 1, 2.5; HUD alpha 0 (resolve happens,
    no menu quads).

Weapon trigger:

25. No local player; id mismatch; player slot +0x260 wrong (warning once, then silent on later
    frames); controller slot holding the probe; first weapon seen (900-frame window, no switch
    counted); a switch; weapon NULL; window expiry; frame counter wrap; probe calls from 7
    different return addresses (only 6 kept) and the counts in the next periodic log line.
