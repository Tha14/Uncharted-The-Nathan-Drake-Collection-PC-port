# Spec: unlocks.cpp (u2mp)

Clean-room description of `gow3_pc/src/u2mp/unlocks.cpp`. It says what the code must do, as the
game and the tester can see it. It does not say how to structure the code. Background notes:
`u2mp/docs/UNLOCKS.md`.

All addresses are image offsets of Uncharted 2 PS4 1.02 (CUSA02320). "Guest" means the game
image. `B` is `image.base`; an image offset `X` is the absolute address `B + X`. Guest functions
use the System V x86-64 convention (`U2MP_GUEST` in `u2mp_internal.h`): arguments in rdi, rsi,
rdx, rcx, r8, r9; result in rax (or al for an 8-bit bool). All integers are little-endian.
"u32", "s32", "u64" are values of that size and sign. "ptr" is a 64-bit absolute address.

Shared helpers from other files (use them, do not reimplement them):

- `image`, `Image::matches`, `Image::executable`, `replace_function`, `retarget_call`, `log`,
  `sid`, `netheap_alloc`, `package_by_id` (`u2mp_internal.h`).
- `log` prefixes every line with `u2mp: ` and adds a newline. All log texts below are the part
  after that prefix. Conversions (`%s`, `%d`, `%u`, `%08x`, `%p`) are printf conversions of the
  host C library.
- `replace_function(offset, length, host, name)` and `retarget_call(offset, expected, host,
  name)` log their own success and failure lines (see `install.cpp`). Their `name` texts are
  given below and are part of the observable log.
- `netheap_alloc(size, align)` returns memory of the multiplayer heap, or NULL. Its memory is not
  zeroed by it.
- `runtime_file_translate(const char *guest, char *out, size_t size)` (C linkage, declared by the
  user) maps a guest path to a host path. It returns 0 on success. The buffer used is 1024 bytes.
- `sid(text, crc = 0)`: CRC-32 string id (see `u2mp_internal.h`). The second argument chains a
  previous value.

---

## 1. Purpose and hook points

The module does four things:

1. **Every multiplayer item counts as unlocked.** The game's single item lock test (IsLocked,
   0xa4bb40) is replaced by a host function. For items that belong to the multiplayer store
   table it always answers "not locked". For other items (the single-player Rewards) it evaluates
   the item's condition list as the game does.
2. **Every co-op weapon upgrade tier counts as owned.** The co-op upgrade level function
   (0xa4d280) is replaced by a host function.
3. **The GOTY data tables are bound over the PS4 stubs.** The PS4 `net.bin` has stub versions of
   several data symbols. The module loads its own copy of the imported PS3 GOTY server module
   `net-patch9` (the player's own imported data), builds new tables from it, and binds them over
   the stubs with the game's symbol binder. Tables: skins, boosters, the multiplayer store (a copy
   with extra skin items), and five more (npc info, character attributes, speakers, material
   parameter set, default game settings). It also refills the game's booster list.
4. **Skin choice and diagnostics.** Before the game builds the local player's data message, the
   equipped skin ids are checked and fixed (default Drake, or a skin forced by an environment
   variable). Two spawn model lookups and the two random skin picks are wrapped to log what
   happens.

Hook methods:

- `replace_function` at 0xa4bb40 (length 16) and 0xa4d280 (length 16).
- `retarget_call` at 0x663339, 0x663782 (spawn lookups), 0x5f2f49, 0x609551, 0x609c2b (player
  data message), 0x6145f5, 0x614688 (random skin picks).
- Plain host functions called by other files (section 2).

Callers in other files:

- `install.cpp` calls `install_unlocks(installed)` once, after `install_menuimages` and before
  `install_loading`.
- `progression.cpp` calls `unlocks_refresh()` from its 0x64e810 hook, whenever NET_SM > 1.
- `texts.cpp` calls `unlocks_text(id)` on the text miss path, after its imported banks and its own
  texts, under its own text lock.
- `session.cpp` calls `unlocks_skin_fingerprint()` when it sends its join info and when the host
  compares a client's fingerprint.

---

## 2. Interface

### 2.1 Declarations (`unlocks.h`, namespace `u2mp`)

```
void install_unlocks(int &installed);
void unlocks_refresh();
const char *unlocks_text(uint32_t id);
uint32_t unlocks_skin_fingerprint();
```

`install_unlocks` is also declared in `u2mp_internal.h` with the same signature. `unlocks.h`
must keep these four declarations unchanged. Nothing else is exported. No variable is exported.

### 2.2 Entry points the game reaches

| site | what the game calls | check before hooking (section 7) | host function signature |
|---|---|---|---|
| 0xa4bb40 (replaced, 16 bytes) | IsLocked(ctx, item, s32 *reason) -> 8-bit bool | byte signatures | `bool (void *ctx, const uint8_t *item, int32_t *reason)` |
| 0xa4d280 (replaced, 16 bytes) | CoopUpgradeLevel(unused, weapon record) -> s32 | byte signatures | `int32_t (void *, uint8_t *weapon)` |
| call at 0x663339 (expects 0x56a980) | art lookup (table, name) -> ptr | `retarget_call` (section 7) | `void *(void *table, const char *name)` |
| call at 0x663782 (expects 0x56a980) | same | `retarget_call` | same |
| calls at 0x5f2f49, 0x609551, 0x609c2b (expect 0x6144b0) | build player data message, 6 integer args -> u64 | byte signatures | `uint64_t (u64 a0..a5)` |
| calls at 0x6145f5, 0x614688 (expect 0x64b430) | random skin pick (list, s32 side) -> u64 | byte signatures | `uint64_t (void *list, int32_t side)` |

The host functions for the two spawn lookups are separate functions (they log different site
texts). The three player data calls share one host function. The two random pick calls share one
host function.

---

## 3. Game facts

### 3.1 Guest functions called

| address | name | arguments -> result |
|---|---|---|
| 0xa4bcb0 | ConditionMet | (ctx, condition record ptr, s32 *out or NULL) -> 8-bit bool in al |
| 0xa19a00 | RelocateModule | (module buffer ptr) -> ptr to the root slot |
| 0xa16d70 | BindSymbol | (u32 symbol id, const u64 *value) -> nothing used |
| 0xa15300 | DcFind | (u32 symbol id) -> ptr to the value slot, or NULL |
| 0x56a980 | art lookup | (table, name) -> ptr or NULL |
| 0x6144b0 | build player data message | six integer register args -> u64 |
| 0x64b430 | random skin pick | (list ptr, s32 side) -> index in the low 32 bits |

### 3.2 Guest globals

| offset | type | meaning |
|---|---|---|
| 0x15340d0 | u64 | script symbol hash buckets; 0 before the symbol table exists |
| 0x1114178 | u8 | the game's debug "unlock all" switch (read only, never written) |
| 0x14ce97c | s32 | NET_SM (net state) |
| 0x11474f8 | struct | NetPlayerData (local player) |
| 0x113a788 | struct | booster list `{u32 count; u32 pad; ptr recs[64]}` |
| 0x14705f0 | array | art name table, entries of 0x38 bytes, +0x20 u32 name id |
| 0x14775f0 | s32 | number of entries in the art name table |

NetPlayerData fields used: +0x10c u32 equipped hero skin id, +0x110 u32 equipped villain skin id,
the owned bit array: bit `b` is bit `b % 32` of the u32 at NetPlayerData + 0x4c + 4 * (0x25 + b / 32)
(that is +0xe0 + 4 * (b / 32)).

### 3.3 Symbol ids (data symbols bound or read)

| id | meaning |
|---|---|
| 0x92c6db02 | multiplayer store table (items) |
| 0x5a4731d2 | skin table |
| 0x916e03b0 | booster table (= `sid("*net-boosters*")`) |
| 0xa977dad3 | net-npc-info |
| 0x42d1ec02 | character attributes (= `sid("*net-skins*")`) |
| 0xbdf0cfd6 | speakers |
| 0xef68c757 | material parameter set |
| 0x1ad223b4 | default game settings |
| 0x681022f5 | store section id: hero skins |
| 0x4252add5 | store section id: villain skins |
| 0x916aa605 | DC module root type id (net-patch9 file header) |

### 3.4 Data layouts

All "tables" below are `{s32 count; u32 pad; ptr records}` (16-byte header) unless stated.

**Store table** (0x92c6db02): `{s32 sections; pad; ptr section array}`. Section, 0x18 bytes: +0 u32
section id, +4 title text, +8 s32 item count, +0x10 ptr item array. Item, 0x50 bytes: +0 u32 bit
index (owned bit), +4 u32 type (10 skin, 11 booster, 12 co-op weapon upgrade, 13 taunt), +8 u32 id,
+0x18 ptr condition list, +0x2c u32 weapon id (type 12), +0x34 s32 tier (type 12), +0x40 u64 and
+0x48 u64 (written for added skin items, see 6.4).

**Condition list**: +0 s32 count, +8 ptr condition array (0x18 bytes each), +0x10 s32 "any" flag
(non-zero: unlocked when any condition is met; zero: unlocked only when all are met).

**Co-op weapon record** (argument of 0xa4d280): +0 u32 weapon id, +0x20 s32 cached tier (negative
until computed).

**Skin record**, PS4 layout, 0x30 bytes: +0 u32 index, +4 u32 side (0 hero, 1 villain), +8 ptr
package name (C string), +0x10 u64 id (high half 0), +0x18 u32 name text id, +0x1c u32 photo
image id, +0x20 u32 icon image id, +0x24 u32, +0x28 u32, +0x2c unused.

**Skin record, converted layout** (as the importer writes it into net-patch9), 0x28 bytes: +0 u32
index, +4 u32 side, +8 ptr package name, +0x10 u32 id, +0x14..+0x24 five u32 (name, photo, icon,
the +0x24 value, the +0x28 value of the PS4 layout, in that order).

**Booster record**, 0x20 bytes: +0 name, +4 description, +8 picture, +0xc u32 id, +0x10 u32 slot
(0 or 1), +0x14 amount, +0x18 u32 (non-zero: listed), +0x1c.

**net-npc-info record**, 12 bytes: +0 u32 archetype id, +4 class id, +8 co-op points.

**Character attributes record**, PS4 layout, 0x20 bytes: +0 ptr actor name, +8 u32 key, +0xc u32
(0), +0x10, +0x14, +0x18, +0x1c u32 four values (the last is a non-zero id). Converted layout,
0x20 bytes: +0 ptr name, +8 u32 key, +0xc..+0x18 the four values, +0x1c 0.

**Speakers record**, 0x10 bytes: +0 u32 key (table sorted ascending, unsigned), +4 u32 (0), +8 ptr
value. The value holds four ptrs at +0, +8, +0x10, +0x18.

**Material parameter record**, 0x30 bytes: +0 ptr material path string, +8 ptr parameter string,
+0x10 u32 type (1 or 3), +0x18 ptr from, +0x20 ptr to, +0x28 u64 cache (must be 0).

**Default game settings object**: +0 s32 player health, then values up to +0x38 on PS4. The
net-patch9 object provides +0..+0x34.

**net-patch9 file** (`/app0/u2data/build/main/dc1/ps4/net-patch9.bin`): +0 ASCII "00CD", +8 u64
relocation table offset, +0x10 u64 root slot (0x20 in the file), +0x1c u32 root type. At the
relocation table offset: u32 count. After relocation (in the buffer): +0x10 holds ptr to buffer
+0x20. The root object at buffer +0x20: +4 u32 symbol count; symbol entries start at root +8,
16 bytes each: +0 u32 symbol id, +8 ptr value.

### 3.5 Other constants

| value | meaning |
|---|---|
| `sid("mp-hero")` (0xb095d270) | default Drake skin id |
| `sid("mp-hero-red")` (0x76474d48) | Baseball Shirt Drake id (hero store item 5) |
| 0x05454b23, 0x01845694 | the two "None" booster ids (6.5, 9.3) |
| "CUSA02320" | substring of a host path that marks a file of the PS4 game |
| `/app0/u2data/build/main/actor27/ps4/<name>.pak` | guest path of an actor package |

Limits: store sections 1..32; store items per section 1..1024 (item test) or 1..200 (store copy);
skin records 1..512; PS4 stub tables 1..8 records; net-patch9 tables 1..1024 records; boosters
1..63; booster list capacity 63 entries used; owned bit index < 288; package name at most 64
bytes including the NUL; net-patch9 strings 1..255 characters plus NUL; health 1..1000.

---

## 4. Module state and environment

State that lives for the whole process (never reset):

- spoof on/off (default on).
- GOTY extra tables on/off (default on).
- "ready": the loader functions passed their byte check at install.
- "given up": net-patch9 is missing or useless; never retried.
- "bound": `unlocks_refresh` finished at least once (or gave up). Atomic.
- our net-patch9 copy (buffer and size), the merged skin table, our store copy, the booster table
  ptr (inside our net-patch9 copy) and its count, the five extra tables, the PS4 stub ptrs of the
  skin table and the five extra symbols (captured once).
- skin fingerprint (u32, atomic, 0 until computed).
- the name label list (id -> text), and a "ready" flag (atomic, release/acquire).
- log budgets: "IsLocked" note 1; "skins:" choice/random lines 32 (shared); spawn lines 32.
- bind budget 4 (shared across all symbols and all calls); store rebuild budget 4; "skin choice
  applied at bind" done flag.
- skin hooks installed flag; forced skin package name (string, may be empty).

The table work in `unlocks_refresh` after the fast check runs under a mutex.

Environment variables (read once, in `install_unlocks`):

| variable | effect |
|---|---|
| `UNCHARTED_U2MP_UNLOCKS` | exactly `off`: spoof off |
| `UNCHARTED_U2MP_GOTY_TABLES` | exactly `off`: the five extra tables are not built |
| `UNCHARTED_U2MP_SKIN` | non-empty: package name of a skin to force |

---

## 5. Behaviour

### 5.1 Common operations

**DC lookup of a symbol `s`**: if the u64 at 0x15340d0 is 0, the result is NULL and no guest call
is made. Otherwise guest call DcFind(s). A NULL slot gives NULL. Otherwise the result is the ptr
stored in the slot. Every "DcFind(s)" below means one such lookup, and each one is one guest call
(when the bucket u64 is non-zero). The tester compares these calls, so their number and order
matter.

**Inside our copy** (`in(p, n)`): true when our net-patch9 buffer exists, `p >= buffer` and
`p + n <= buffer + size`. Default `n` is 1.

**net-patch9 symbol `s`**: walk the root's entries (count = u32 at root +4) in order while the
16-byte entry is inside our copy. At the first entry whose id equals `s`: return its value ptr if
`in(value, 16)`, else NULL. No match: NULL.

**Package name check** (`name`, `ours`): false if NULL; if `ours`, `in(name, 4)` must hold. The
first three characters must be `mp-`. Then scan up to 64 characters: a NUL at position i gives
true; if `ours`, before moving on the byte at position i+1 must be inside our copy, else false.
No NUL in 64 characters: false.

**Skin layout check** (`recs`, `n`, `stride`, `ps4`, `ours`): false if `recs` is NULL, `n <= 0`
or `n > 512`, or (`ours` and not `in(recs, stride * n)`). Each record: if `ps4`, the u32 at +0x14
must be 0; the ptr at +8 must pass the package name check with `ours`.

**net-patch9 string**: for i = 0..255: the byte must be inside our copy, else false; a NUL at i
gives true when i > 0 and false when i = 0. No NUL in 256: false.

**net-patch9 table** (`s`, `stride`, `what`): t = net-patch9 symbol `s`; n = s32 at t+0 (0 when t
is NULL); recs = ptr at t+8. If t is NULL, `n <= 0`, `n > 1024`, or not `in(recs, stride * n)`:
log `unlocks: net-patch9 %s 0x%08x missing or out of range (%d records); PS4 stub kept` (what, s,
n) and fail. Else the result is (recs, n).

**New table** (count, stride): `netheap_alloc(0x10 + stride * count, 16)`, all of it zeroed, s32
count at +0, ptr (table + 0x10) at +8. NULL on allocation failure.

**Label of a package name**: drop a leading `mp-hero-` (8 characters) if present. Then build the
text character by character: a `-` or `_` adds one space if the text is not empty and does not
already end in a space, and makes the next character a "word start". Any other character is
added, upper-cased (C locale `toupper`) when it is at a word start; the first character is a word
start. Examples: `mp-hero-killzone-sev` -> `Killzone Sev`; `mp-hero-u1-merc-brsr-2a` ->
`U1 Merc Brsr 2a`; `mp-hero` -> `Mp Hero`; `a--b` -> `A B`; `x-` -> `X ` (trailing space).

**Actor source of a package name**: guest path `/app0/u2data/build/main/actor27/ps4/` + name +
`.pak`. If translation fails or the host file cannot be opened for reading: `no file`. Otherwise
(file closed again) `PS4 game` when the host path contains `CUSA02320`, else `import overlay`.

**Skin index of an id**: t = DcFind(0x5a4731d2). NULL: -1. Else scan records (stride 0x30) i = 0
.. while records ptr non-NULL, i < count (s32 at t+0), i < 512; the first record whose u32 at +0x10
equals the id gives i, and its package ptr (+8, may be NULL) is reported. None: -1.

**Skin id of a package name**: t = DcFind(0x5a4731d2) (always made first). If t is NULL, the name
is NULL or empty: 0. Else scan as above; the first record whose package ptr is non-NULL and equals
the name (strcmp) gives its u32 at +0x10. None: 0.

**"skins:" log slot**: if the shared budget (start 32) is > 0, consume one and allow the log;
else the log is skipped. A slot is only consumed where the text below says "if a skins slot".

### 5.2 IsLocked (0xa4bb40)

Arguments: ctx, item, reason (may be NULL). Returns an 8-bit bool (true = locked).

1. item NULL: return false. Nothing else happens (reason untouched).
2. If reason is non-NULL, write 0 to it.
3. If "bound" is not set, call `unlocks_refresh()` (5.4) first.
4. If spoof is on and the item is a multiplayer item: the first time ever, log
   `unlocks: multiplayer items reported unlocked (IsLocked 0xa4bb40)`. Return false.
   Multiplayer item test: s = DcFind(0x92c6db02). NULL: not. sections = s32 at s+0, array = ptr at
   s+8; not when sections <= 0, > 32, or array NULL. For each section: n = s32 at +8, items = ptr
   at +0x10; skip when items NULL, n <= 0 or n > 1024. The item is a multiplayer item when it lies
   in [items, items + 0x50 * n) and its distance from items is a multiple of 0x50.
   When spoof is off, this test is not made (no DcFind call).
5. list = ptr at item+0x18. NULL: return false.
6. Lock evaluation. If the u8 at 0x1114178 is non-zero, the result is false and no condition is
   evaluated here. Otherwise:
   - start value: locked = (any flag at list+0x10 != 0) and (count at list+0 > 0).
   - for i = 0, 1, ... while i < the count of the current list: guest call ConditionMet(ctx,
     condition i of the current list (ptr at list+8 + 0x18 * i), NULL). After each call, re-read
     the list ptr from item+0x18 and read its any flag. If the condition was met (al non-zero) and
     any is set: locked = false, stop. If it was not met and any is clear: locked = true, stop.
     Otherwise go on.
7. Reason. Only when reason is non-NULL (this runs even when the debug switch is set): for i = 0,
   1, ... while i < the count of the current list: guest call ConditionMet(ctx, condition i, &out)
   with out = 0 before the call. If it returns non-zero, write out to *reason and stop. Otherwise
   re-read the list ptr from item+0x18 and go on.
8. Return locked.

### 5.3 CoopUpgradeLevel (0xa4d280)

Arguments: first unused, weapon record. Returns s32.

1. weapon NULL or NET_SM < 2: return 0 (nothing written).
2. cached = s32 at weapon+0x20. If cached >= 0: return it.
3. s = DcFind(0x92c6db02). best = 0.
4. If s is non-NULL and s32 at s+0 > 0: all = (spoof on) or (u8 at 0x1114178 non-zero). id = u32
   at weapon+0. For each section (while the section array ptr is non-NULL and index < s32 at s+0)
   and each item of it (while the item array ptr is non-NULL and index < s32 at section+8; no
   other limits): consider items with type u32 +4 == 12 and u32 +0x2c == id. Such an item is owned
   when all is set, or when its bit index (u32 +0) is < 288 and that bit is set in NetPlayerData
   (3.2). best = maximum of best and the s32 at item+0x34 of every owned item.
5. Write best to weapon+0x20 (also when s was NULL: then 0). Return best.

### 5.4 unlocks_refresh()

Steps in order:

1. Return at once (no guest call) if: not "ready", `image.base` is NULL, NET_SM <= 1, or the u64
   at 0x15340d0 is 0.
2. If the skin choice at bind was not done yet and the merged skin table exists: DcFind(0x5a4731d2).
   If it returns the merged table: mark it done and apply the skin choice with the text `skin table
   bound` (5.6).
3. Fast check, only when "bound" is set. Lookups in this order: DcFind(0x5a4731d2),
   DcFind(0x916e03b0) (both always). Then for each extra table in the fixed order (5.7) that
   was built: DcFind(its id); stop making these lookups at the first one that does not return our
   table. Then the condition, evaluated left to right with short-circuit:
   (no merged skin table, or the skin lookup returned it) and (no store copy, or DcFind(0x92c6db02)
   returns it) and (no booster table, or the booster lookup returned it) and (no booster table, or
   the booster list is ours) and (all built extras were ours). If it holds: return.
   "Booster list is ours": u32 at 0x113a788 is 1..63 and the ptr at 0x113a790 satisfies
   `in(ptr, 0x20)`.
4. Take the mutex. If "given up": return.
5. First time only (our net-patch9 copy does not exist yet):
   1. DcFind(0x5a4731d2) -> remember as the PS4 skin stub. Then DcFind of each extra id in the
      fixed order -> remember as that table's PS4 stub.
   2. Load net-patch9 (6.2). On failure: log `unlocks: %s not imported: the PS4 skin / booster
      tables stay (2 skins, no boosters)` with the guest path, set "given up" and "bound", return.
   3. Build the merged skin table (6.3), then the store copy (6.4), then the booster table (6.5).
   4. If the extra tables are on: build them (6.6). Else log `unlocks:
      UNCHARTED_U2MP_GOTY_TABLES=off: the other net.bin stubs stay`.
   5. If none of skin table, booster table, store copy and extra tables was built: set "given up"
      and "bound", return.
6. Binding (bind budget starts at 4, shared and never refilled). For a bind, the second argument
   to BindSymbol is a ptr to a u64 that holds the table ptr. "Not effective" means a DcFind of the
   symbol right after the bind does not return our table; only then the budget drops by 1.
   1. Skin: if the merged table exists and DcFind(0x5a4731d2) does not return it and the budget is
      > 0: BindSymbol(0x5a4731d2, &table); DcFind (effect check); log `unlocks: skin table 0x%08x
      bound (%d records)%s` with the id, the merged count, and `` or ` - NOT effective` from one
      more DcFind.
   2. Store: if the store copy exists and DcFind(0x92c6db02) does not return it and the budget is
      > 0: if the rebuild budget (4) is left, use one and build the store copy again (6.4; it may
      return without a new copy). BindSymbol(0x92c6db02, &store copy); DcFind check; log
      `unlocks: MP store 0x%08x bound%s` (one more DcFind for the suffix).
   3. Boosters: same pattern with 0x916e03b0 and the booster table ptr; log `unlocks: booster
      table 0x%08x bound (%d records)%s` with the booster count.
   4. Each extra table in the fixed order: skip if not built; else DcFind(id); skip if it returns
      ours or the budget is <= 0. Else BindSymbol(id, &table); DcFind check; log `unlocks: %s
      0x%08x bound%s` (its label, id, suffix from one more DcFind).
   5. If the booster table exists and the booster list is not ours: fill the booster list (6.7).
7. Set "bound". Release the mutex.

Note on the order of lookups in steps 6.1 to 6.3: the DcFind that tests "does not return it" is
made before the budget is tested.

### 5.5 unlocks_text(id) and unlocks_skin_fingerprint()

`unlocks_text`: id 0, or the label list not ready: NULL. Else the label of the first entry with
that id, or NULL. The returned pointer stays valid for the rest of the process. Thread-safe
against the build (the ready flag is published after the list is complete).

`unlocks_skin_fingerprint`: the stored fingerprint (0 until the merged skin table was built).

### 5.6 Skin choice, random pick, spawn lookup

**Apply the skin choice** (`when` text): only when the skin hooks are installed (section 7, step 7) and
DcFind(0x5a4731d2) is non-NULL (that lookup is made only when the hooks are installed).
1. forced = 0. If a forced package name is set: forced = skin id of that name (5.1). If forced is
   0 and a skins slot is free: log `skins: UNCHARTED_U2MP_SKIN=%s is not a package of the skin
   table; not forced`.
2. If a skins slot is free: log `skins: choice applied (%s)` with `when`.
3. Hero side (`hero`, NetPlayerData +0x10c), then villain side (`villain`, +0x110):
   before = the u32 there; (at, pk) = skin index of before. If forced is non-zero: write forced,
   reason `UNCHARTED_U2MP_SKIN`. Else if at < 0: write `sid("mp-hero")`, reason `not in the table:
   Drake`. Else leave it, reason `kept`. Then if a skins slot is free: (now_at, now) = skin index
   of the current value; log `skins: %s id %08x (index %d '%s') -> %08x (index %d '%s'): %s` with
   side, before, at, pk or `-`, the current value, now_at, now or `-`, reason.

**Player data message hook** (calls at 0x5f2f49, 0x609551, 0x609c2b): apply the skin choice with
`player data message 0x6144b0`, then guest call 0x6144b0 with the same six integer arguments and
return its u64 result unchanged.

**Random skin pick hook** (calls at 0x6145f5, 0x614688): r = guest call 0x64b430(list, side). If a
skins slot is free: i = low 32 bits of r as s32; t = DcFind(0x5a4731d2); pk = package ptr of
record i when t is non-NULL and 0 <= i < count; log `skins: random pick (0x64b430), side %d ->
index %d '%s'` with side, i, pk or `-`. Return r unchanged (all 64 bits).

**Spawn lookup hooks** (calls at 0x663339 with site text `0x663280`, 0x663782 with `0x6636b0`):
r = guest call 0x56a980(table, name). If the spawn budget (32) is used up: return r. Else consume
one. id = `sid(name)` (0 when name is NULL). pkg = `package_by_id(id)` when id is non-zero, else
NULL. Scan the art name table: i from 0 while i < s32 at 0x14775f0 and i < 512; count entries;
alias = i + 1 of the last entry whose u32 at +0x20 equals id (also matches id 0). Log `skins: spawn
%s: model '%s' -> %p; package %08x %s (state %d), actor file from %s; name table %d entries, %s`
with site, name or `(null)`, r, id, `loaded` or `not loaded`, the s32 at pkg+0x30 or -1, the actor
source of name or `-` when name is NULL, the entry count, `name listed` (alias non-zero) or `name
NOT listed`. Return r.

### 5.7 Fixed order of the extra tables

1. 0xa977dad3, label `net-npc-info`
2. 0x42d1ec02, label `character attributes`
3. 0xbdf0cfd6, label `speakers`
4. 0xef68c757, label `material parameter set`
5. 0x1ad223b4, label `default game settings`

---

## 6. Table building

### 6.1 Overview

All new tables are allocated with `netheap_alloc(..., 16)` unless noted. Pointers stored into
them that point into our net-patch9 copy stay valid because that copy is never freed.

### 6.2 Loading net-patch9

1. Translate `/app0/u2data/build/main/dc1/ps4/net-patch9.bin`. On failure, or if the host file
   cannot be opened: fail silently.
2. Read the whole file.
3. Fail with log `unlocks: %s is not a PS4 DC module` (guest path) unless: size >= 0x40, the first
   four bytes are `00CD`, u64 at +0x10 is 0x20, and u32 at +0x1c is 0x916aa605.
4. r = u64 at +8. Fail with log `unlocks: %s has a bad relocation table` if r < 0x28, r not a
   multiple of 8, r + 4 > size, or r + 4 + (u32 at r) > size (the u32 count itself is added, not
   multiplied).
5. Allocate a host buffer of the file size, 16-byte aligned (host heap, not the net heap). On
   failure: fail silently. Copy the file into it.
6. Guest call RelocateModule(buffer). If the result is not buffer+0x10, or the ptr stored there is
   not buffer+0x20: log `unlocks: relocating %s failed`, free the buffer, fail.
7. Keep the buffer and its size for the rest of the process.

### 6.3 Merged skin table

1. t = net-patch9 symbol 0x5a4731d2. Missing: log `unlocks: net-patch9 has no skin table 0x%08x`,
   no table.
2. n = s32 at t+0, recs = ptr at t+8. Layout: PS4 if the skin layout check (recs, n, 0x30, ps4,
   ours) passes; else converted if the check (recs, n, 0x28, not ps4, ours) passes. Else log
   `unlocks: net-patch9 skin table (%d records) has an unknown layout; PS4 skins kept` with n, no
   table.
3. PS4 stub (captured in 5.4 step 5.1): usable when non-NULL, its count is 1..8 and the skin layout
   check (its recs, count, 0x30, ps4, not ours) passes.
4. Allocate 0x10 + 0x30 * (n + usable stub count + 1 + 7) bytes; zero all of it. NULL: no table,
   no log.
5. Records 0..n-1 = net-patch9's, in order. PS4 layout: copied whole. Converted: +0, +4, +8 (8
   bytes), +0x10 (4 bytes; high half stays 0), and the five u32 at +0x14.. go to +0x18, +0x1c,
   +0x20, +0x24, +0x28.
6. Each usable PS4 stub record in order: id = u32 at +0x10. Find the first of records 0..n-1 (only
   the net-patch9 ones) with that u32 at +0x10. Found: overwrite it (count as replaced). Not found:
   append at the end (count as appended). The record is copied whole (0x30 bytes). Then its package
   string is copied into a new `netheap_alloc(length + 1, 16)` block and +8 points to the copy (if
   the allocation fails, +8 keeps the stub's pointer).
7. Baseball Shirt Drake: if no record so far has u32 +0x10 == `sid("mp-hero-red")`, append one
   (counts as appended): +0 = 1 (not its index), +4 = 0 (hero), +8 = ptr to a static host string
   `mp-hero-red`, +0x10 = the id, +0x18 / +0x1c / +0x20 = the values of 9.2, rest 0.
8. Campaign skins: each of the 7 entries of 9.1 in order: if no record so far has its id at
   +0x10, append one (counts as appended): +0 = its own index in the table, +4 = 1 for villain,
   0 for hero, +8 = ptr to a static host string with its package name, +0x10 = id, +0x18 name,
   +0x1c photo, +0x20 icon, rest 0.
9. Header: s32 count at +0, ptr (block + 0x10) at +8.
10. Fingerprint: fp = 0; for each record in order: fp = `sid(text, fp)` where text is the u32 at
    +0x10 printed as `%08x` (8 lowercase hex digits). Log `unlocks: skin table order fingerprint
    %08x (%d records; every PC of a match must log the same)` with fp and count. Store fp.
11. Labels: for each record in order whose u32 at +0x18 is non-zero and not yet in the list: add
    (that id, label of the record's package name). The first record wins.
12. Actor sources: for each record: the actor source of its package name. Count `PS4 game` and
    `import overlay`. For any other result: if the missing text is shorter than 400 characters and
    does not already contain the package name as a substring, append a space and the name. Log
    `unlocks: skin actors: %d records from the PS4 game, %d from the import overlay, missing:%s`
    with the two counts and ` none` when the missing text is empty, else the missing text.
13. Publish the labels (ready flag).
14. Log `unlocks: skin table: %d GOTY records (%s layout) + PS4 %d (%d replaced, %d appended) = %d`
    with n, `PS4` or `0x28-byte converted`, the usable stub count (0 when not usable), replaced,
    appended (including steps 7 and 8), count.

### 6.4 Store copy

1. s = DcFind(0x92c6db02). Return (no change, no log) when s is NULL or already is our store copy.
2. sections = s32 at s+0, array = ptr at s+8. Return when sections <= 0, > 32, or array NULL.
3. Allocate 0x10 + 0x18 * sections. NULL: return. Zero the first 0x10 bytes. Copy the section array
   to block + 0x10.
4. For each copied section whose id is 0x681022f5 (hero) or 0x4252add5 (villain): n = s32 at +8,
   items = ptr at +0x10; skip when n <= 0, n > 200 or items NULL. The skins to add are the 9.1
   entries of that side (in table order) whose id is not the u32 at +8 of any of the n items. None:
   skip. Allocate 0x50 * (n + k). NULL: skip (the section keeps the game's array). Copy the n
   items. Each added item: a copy of the game's last item (index n-1), then +4 = 10, +8 = id (u32),
   +0x18 = 0 (u64), +0x40 = ptr to the entry's static package string (u64), +0x48 = id (u64). Set
   the section's count to n + k and its array ptr to the new array.
5. Header: s32 sections at +0, ptr (block + 0x10) at +8. This block becomes the store copy (the
   previous copy, if any, is dropped without freeing).
6. Log `unlocks: MP store copied with %d added skin item(s) (hero / villain lists)` with the number
   of added items over all sections.

### 6.5 Booster table

1. t = net-patch9 symbol 0x916e03b0. Missing: log `unlocks: net-patch9 has no booster table
   0x%08x`, no table.
2. n = s32 at t+0, recs = ptr at t+8. If n <= 0, n > 63 or not `in(recs, 0x20 * n)`: log
   `unlocks: net-patch9 booster table (%d records) out of range; PS4 boosters kept`, no table.
3. Each record i: id = u32 +0xc, slot = u32 +0x10. If id is 0 or slot > 1: log `unlocks:
   net-patch9 booster %d (id %08x, slot %u) is not a booster record; PS4 boosters kept`, no table.
4. Both "None" booster ids (9.3) must appear among the ids. Else log `unlocks: net-patch9 booster
   table lacks the two None boosters; PS4 boosters kept`, no table.
5. The booster table is t itself (inside our copy, not copied), count n. Log `unlocks: booster
   table: %d GOTY records`.

### 6.6 Extra tables (each failure leaves that table unbuilt; the others go on)

Built in the fixed order of 5.7. "Stub" means the PS4 stub table captured in 5.4 step 5.1; stub
count = s32 at stub+0, stub records = ptr at stub+8 (0 / NULL when the stub is NULL). "Stub usable"
= count 1..8 and records non-NULL (no other check).

**net-npc-info**: net-patch9 table (0xa977dad3, 12, `net-npc-info`). Any record with a zero
archetype: log `unlocks: net-npc-info record %d has no archetype; PS4 stub kept`, fail. New table
(n + usable stub count, 12). Copy the n records. Each usable stub record in order: append it when
its archetype is not among the first n records (only the net-patch9 ones). Set the count. Log
`unlocks: net-npc-info: %d GOTY records + %d PS4` (n, appended).

**character attributes**: net-patch9 table (0x42d1ec02, 0x20, `character attributes`). Layout
test over all records: both layouts fail for a record whose name ptr is not a net-patch9 string or
whose u32 +8 is 0. Converted fails when u32 +0x1c != 0 or u32 +0x18 == 0. PS4 fails when u32 +0xc
!= 0 or u32 +0x1c == 0. If both fail: log `unlocks: net-patch9 character attributes (%d records):
unknown layout; PS4 stub kept`, fail. New table (n + usable stub count, 0x20). PS4 layout: copy
records whole. Converted: +0 (8 bytes), +8 (4 bytes), and the four u32 at +0xc.. go to +0x10,
+0x14, +0x18, +0x1c (+0xc stays 0). Each usable stub record in order: key = u32 +8; find the first
of the n net-patch9 records with that key: replace it (replaced), else append. Copy 0x20 bytes;
if its name ptr is non-NULL, copy the string into `netheap_alloc(length + 1, 16)` and point +0 at
it (on allocation failure keep the stub's ptr). Set the count. Log `unlocks: character
attributes: %d GOTY records (%s layout), PS4 %d replaced, %d appended` (n, `PS4` or `converted`,
replaced, count - n).

**speakers**: net-patch9 table (0xbdf0cfd6, 0x10, `speakers`). Each record i: u32 +4 must be 0,
the value ptr must satisfy `in(value, 0x20)`, and for i > 0 its key must be greater (unsigned)
than the key of record i-1; then each of the four ptrs in the value must satisfy `in(p)`. Else log
`unlocks: net-patch9 speakers record %d is not a sorted {key, value} record; PS4 stub kept`, fail.
New table (n, 0x10), copy the records. Count stub records (when the stub records ptr is non-NULL
and the stub count is 1..8) whose key is not among the net-patch9 keys. Log `unlocks: speakers:
%d GOTY records%s` with ` (a PS4 stub key is not among them)` when that count is non-zero.

**material parameter set**: net-patch9 table (0xef68c757, 0x30, `material parameter set`). Each
record: type = u32 +0x10, need = 12 for type 3 else 4. Fail when +0 or +8 is not a net-patch9
string, type is not 1 or 3, the ptr at +0x18 or +0x20 does not satisfy `in(p, need)`, or the u64
at +0x28 is non-zero: log `unlocks: net-patch9 material parameter record %d has an unknown layout;
PS4 stub kept`. Else allocate 0x10 bytes (not zeroed beyond what is written): s32 n at +0, s32 0
at +4, ptr to the net-patch9 records at +8 (records are not copied). Log `unlocks: material
parameter set: %d GOTY records`. The stub is not used.

**default game settings**: g = net-patch9 symbol 0x1ad223b4. If g is NULL, the stub is NULL, or
not `in(g, 0x34)`: log `unlocks: default game settings: %s; PS4 values kept` with `no PS4 object`
when g is non-NULL, else `not in net-patch9` (so a g that fails only the 0x34 test also reports
`no PS4 object`). health = s32 at g+0; if <= 0 or > 1000: log `unlocks: net-patch9 default game
settings: health %d out of range; PS4 values kept`, fail. Allocate 0x40, zero it, copy 0x38
bytes of the stub, then 0x34 bytes of g over them. Log `unlocks: default game settings: player
health %d (PS4 %d)` (health, s32 at stub+0).

### 6.7 Booster list fill

Walk the booster records (ptr at table+8, stride 0x20) i = 0.. while i < count and kept < 63: skip
records whose u32 at +0x18 is 0; else write the record ptr at 0x113a790 + 8 * kept and add 1 to
kept. Then write kept as u32 at 0x113a788. Entries after kept are left as they were. Log
`unlocks: booster list 0x113a788: %u boosters`.

---

## 7. install_unlocks(int &installed)

In this order:

1. Environment: `UNCHARTED_U2MP_UNLOCKS` equal to `off` turns spoof off.
   `UNCHARTED_U2MP_GOTY_TABLES` equal to `off` turns the extra tables off.
2. Loader check ("ready"): `executable(off, len)` and `matches(off, bytes)` for each of:
   - 0xa19a00, 24 bytes: `55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC 18 48 89 7D D0 40 F6 C7`
   - 0xa16d70, 24 bytes: `55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC 18 4C 8B 0D 78 3E 63 00`
   - 0xa15300, 16 bytes: `55 48 89 E5 53 48 83 EC 18 4C 8B 05 F0 58 63 00`
   If any fails: log `unlocks: DC loader functions differ: GOTY skin / booster tables not bound`
   (and "ready" stays false, so `unlocks_refresh` never does anything).
3. IsLocked: all of
   - `executable(0xa4bb40, 0x100)`
   - 0xa4bb40, 23 bytes: `55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC 28 49 89 F6 49 89 FF`
   - 0xa4bb82, 7 bytes: `48 8D 05 EF 85 6C 00` (lea of 0x1114178)
   - 0xa4bbdc, 5 bytes: `E8 CF 00 00 00` (call 0xa4bcb0)
   - 0xa4bcb0, 16 bytes: `55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC`
   - `replace_function(0xa4bb40, 16, IsLocked host, "item IsLocked (0xa4bb40)")`
   Success: `installed += 1`. Else log `unlocks: IsLocked 0xa4bb40 differs, multiplayer items keep
   their rank conditions`. (Short-circuit: later checks are not made after a failed one.)
4. Upgrade level: all of
   - `executable(0xa4d280, 0x80)`
   - 0xa4d280, 41 bytes: `55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC 48 48 89 75 B8 31 C0
     48 85 F6 0F 84 78 01 00 00 8B 0D D6 16 A8 00 83 F9 02`
   - 0xa4d2f4, 7 bytes: `48 8D 05 7D 6E 6C 00`
   - `replace_function(0xa4d280, 16, upgrade host, "co-op upgrade level (0xa4d280)")`
   Success: `installed += 1`. Else log `unlocks: co-op upgrade level 0xa4d280 differs, upgrades stay
   as bought`.
5. Spawn lookups: `retarget_call(0x663339, 0x56a980, host A, "spawn model lookup A")` and then (only
   if A succeeded) `retarget_call(0x663782, 0x56a980, host B, "spawn model lookup B")`. Both
   succeed: `installed += 1`. Else log `unlocks: spawn model lookups (0x663339 / 0x663782) differ,
   no spawn log` (A stays retargeted if only B failed).
6. `UNCHARTED_U2MP_SKIN`: when set and non-empty, store it as the forced package name.
7. Skin hooks: all of (no executable test)
   - 0x6144b0, 17 bytes: `55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC 18`
   - 0x6145a7, 12 bytes: `41 8B 9F 0C 01 00 00 BF D2 31 47 5A`
   - 0x614632, 12 bytes: `41 8B 9F 10 01 00 00 BF D2 31 47 5A`
   If they match: try all five retargets in this order, each regardless of the others:
   0x5f2f49, 0x609551, 0x609c2b (expected 0x6144b0, name `skins: player data skin choice`), then
   0x6145f5 (expected 0x64b430, name `skins: random hero pick`), 0x614688 (expected 0x64b430, name
   `skins: random villain pick`). If at least one succeeded: `installed += 1`. Set "skin hooks
   installed" (even if none succeeded). Log `skins: default skin Drake when none is equipped%s%s`
   with `; forced: ` and the forced name when one is set, else two empty strings.
   If they do not match: log `skins: player data 0x6144b0 differs, skin choice left alone`.
8. Log `unlocks: every multiplayer skin, booster, co-op weapon upgrade and taunt unlocked` when spoof
   is on, else `unlocks: spoof off (UNCHARTED_U2MP_UNLOCKS=off); GOTY tables only`.

`installed` grows by 0 to 4. Install makes no guest calls.

---

## 8. Threading

`unlocks_refresh` can run on several threads (the 0x64e810 hook and IsLocked). The fast check
(steps 1..3 of 5.4) runs without the lock. The table work runs under one mutex. The "bound" flag
and the label "ready" flag are atomics with release on store and acquire on load. The log budgets
are atomics (decrement after a test; a race may let one extra line through, which is acceptable).

---

## 9. Data tables held in the current file (values included: decision of 2026-11-14 (made during the rewrite, to be confirmed by the owner))

The current file holds two small hand-written tables and a few single constants. The owner decided
that these values are string ids the game looks things up by (interoperability facts, like the
addresses and ids elsewhere in this spec), not game content tables. **They are included below.**
Where a value equals `sid()` of a known name, the name is given too (all checked with `sid()`).
Other values are given as plain ids.

### 9.1 Campaign / missing skins (7 entries)

Purpose: skins that the hero / villain lists cannot show because they have no skin record and/or
no store item. Fields per entry: package name, id, name text id, photo image id, icon image id,
side. Order matters (it fixes the appended record positions and thus the fingerprint).

| # | package name | side | id | name text id | photo | icon |
|---|---|---|---|---|---|---|
| 1 | `mp-hero-lazarevic` | villain | 0x345329d1 = `sid("mp-hero-lazarevic-fat")` | 0xe2ce50df | 0xeabed4cf = `sid("general/net/player-photos/lazarevic")` | 0xffa8b107 = `sid("general/net/player-icons/lazarevic")` |
| 2 | `mp-hero-pirate` | villain | 0x5a2e4839 = `sid("mp-hero-pirate-nd")` | 0xc0f12cfd | 0x9f8d3fe9 = `sid("general/net/player-photos/pirate")` | 0xb980d32e = `sid("general/net/player-icons/pirate")` |
| 3 | `costume-hero-lazarevic-healed` | villain | 0x1c14ad7b = `sid(package)` | 0x899e21dd | 0xeabed4cf (as entry 1) | 0xffa8b107 (as entry 1) |
| 4 | `hero-dirty-beatup` | hero | 0x1b3af509 = `sid(package)` | 0x1d8dbe6a | 0x05a8ba5f | 0x14c66775 |
| 5 | `costume-hero-skeleton-sir-francis` | hero | 0x7a66b9bb = `sid(package)` | 0xba9eb65d | 0x200eed64 | 0x3160304e |
| 6 | `mp-hero-nd` | hero | 0x258034ff = `sid(package)` | 0x9b7568c5 | 0xaa7dcc3a = `sid("general/net/player-photos/hero-nd")` | 0x2534f068 = `sid("general/net/player-icons/hero-nd")` |
| 7 | `mp-hero-frosty` | hero | 0xf0d8079a = `sid(package)` | 0xaacd3194 | 0x01bf230e | 0xb58cceaa |

The package names are static host strings. The store copy (6.4) points item +0x40 at them, and the
merged skin table (6.3 step 8) points record +8 at them.

Note (background only): entries 1, 2, 6 and 7 are GOTY multiplayer skins that normally already have
a record in net-patch9's skin table, so 6.3 step 8 appends nothing for them with a normal import;
only the store copy (6.4) uses their ids. Entries 3, 4 and 5 are campaign costumes; their pictures
are a close twin's.

### 9.2 Baseball Shirt Drake record (1 entry)

Fields: +0 = 1, +4 = 0 (hero), +8 = static host string `mp-hero-red`, +0x10 id 0x76474d48 =
`sid("mp-hero-red")`, +0x18 name text id 0x9a88ecdc, +0x1c photo 0xdafcd51b =
`sid("general/net/player-photos/hero-red")`, +0x20 icon 0xc283da48 =
`sid("general/net/player-icons/hero-red")`, everything else 0.

### 9.3 Single constants

- Default Drake id: 0xb095d270 = `sid("mp-hero")`.
- The two "None" booster ids: 0x05454b23 and 0x01845694. Used only to validate net-patch9's
  booster table (6.5 step 4): both must appear among the record ids at +0xc.
- Store section ids 0x681022f5 / 0x4252add5, the data symbol ids of 3.3 and the root type id are
  identifiers the game's own loader and readers use.
- The install byte signatures (section 7) are short instruction sequences of the game, used only to
  check that the image is the expected build, as in every other u2mp spec.

### 9.4 Tables that are *not* copied

The skin table (107 records), the booster table (32), net-npc-info (56), character attributes (98),
speakers (52), material parameter set (98) and default game settings all come at run time from the
player's own imported `net-patch9.bin`; the PS4 stubs come from the player's PS4 `net.bin`. No
record contents of these tables are in the source.

---

## 10. Test scenarios

Each scenario uses a fake image. Compare guest calls (with arguments; BindSymbol's second
argument by the u64 it points to), log lines in order, and image / net heap memory afterwards.

Install:

1. All signatures match: four `installed` increments; logs of `replace_function` / `retarget_call`
   and the two final lines (`skins: default skin Drake ...` and `unlocks: every ...`).
2. Each signature group mismatched in turn (loader, IsLocked at each of its five checks, upgrade
   at each check, spawn A, spawn B only, skin prologue, each skin read): the matching failure log;
   no hook for that group; spawn A stays retargeted when only B fails.
3. Skin signatures match but every retarget fails: no increment, skin hooks still on, log line
   still printed.
4. `UNCHARTED_U2MP_UNLOCKS=off` / `Off` / unset; `UNCHARTED_U2MP_GOTY_TABLES=off`;
   `UNCHARTED_U2MP_SKIN` set / empty.

IsLocked:

5. item NULL (reason untouched); reason NULL vs non-NULL.
6. "bound" clear: refresh called first (check its early return when NET_SM <= 1: IsLocked calls it
   again next time).
7. Spoof on, item inside a store item array: false, one-time log only on the first call; item
   inside the array but not on a 0x50 boundary; item past the end; sections 0 / 33; section count
   0 / 1025; NULL item array; bucket u64 0 (no DcFind).
8. Spoof off: no DcFind for the store; conditions evaluated.
9. Condition list NULL.
10. Debug switch set: locked false, but the reason loop still calls ConditionMet.
11. any = 1: first met -> unlocked; none met -> locked; count 0 -> unlocked.
12. any = 0: first unmet -> locked; all met -> unlocked.
13. A ConditionMet that changes item+0x18 (list re-read after each call).
14. Reason: first met condition's out value; no met condition -> 0.

Upgrade level:

15. weapon NULL; NET_SM 1 (no write); cached >= 0 returned; store missing (0 written).
16. Spoof on: highest tier of all matching type-12 items; other types / other weapon ids ignored.
17. Spoof off: owned bits (bit < 288 set / clear, bit >= 288), debug switch on.

Refresh:

18. Not ready / NET_SM <= 1 / buckets 0: no guest call.
19. File missing: the "not imported" log; given up; later calls: only the fast check (two DcFinds).
20. Bad magic, bad root slot, bad root type, size < 0x40, each bad relocation condition, relocation
    result wrong: each log; buffer freed.
21. Skin table: missing; PS4 layout; converted layout; unknown layout; PS4 stub usable / NULL /
    9 records / bad package name; stub record that replaces one, one appended, two with the same
    new id; Baseball Shirt record present / absent; each campaign entry present / absent; package
    copy allocation failure; fingerprint value; label list (first id wins, labels per 5.1 examples);
    actor sources (PS4 path containing CUSA02320, other host path, missing file, substring dedup,
    400-character cap); main allocation failure (no table, no log).
22. Store copy: no store; sections 0 / 33; hero / villain sections with all ids present (no new
    array); with missing ids (k items appended from the last item); n 0 / 201; array allocation
    failure; header allocation failure; store already ours.
23. Boosters: missing; n 0 / 64; records outside the copy; id 0; slot 2; one None id missing;
    valid -> list filled (records with +0x18 zero skipped; at most 63).
24. Extras on: each builder's success and each failure path, PS4 stubs NULL / 0 / 9 records;
    default settings with g outside the 0x34 range and stub present (`no PS4 object` text).
    Extras off: the off log, no extras built or bound.
25. Nothing built: given up after the build.
26. Bind: effective binds (one log each, no budget use); ineffective binds (` - NOT effective`,
    budget drops; after four ineffective binds no more BindSymbol calls); exact DcFind counts per
    bind (check, effect check, log suffix).
27. Later call, all ours: fast return. Later call, store replaced by the game: rebuild (up to 4
    times) then bind. Booster list overwritten by the game: refilled. An extra replaced: rebound.
28. Skin choice at bind: runs once, on the first refresh where the skin symbol returns our table.

Skin choice and hooks:

29. Equipped id in the table: kept. Not in the table: Drake id written. Forced name found: written
    to both sides. Forced name not found: the "not forced" log, then as unforced.
30. The 32-line budget shared by choice, "not forced", per-side and random-pick lines; after it,
    choices still applied without logs.
31. Player data hook passes all six arguments and returns the guest result.
32. Random pick: index in range / negative / too large / skin table missing.
33. Spawn lookups: name NULL (`(null)`, id 0, `-`), package loaded / not, name listed / not, art
    count > 512, budget of 32.

Texts and fingerprint:

34. `unlocks_text(0)`, before the build, known id, unknown id.
35. `unlocks_skin_fingerprint()` before (0) and after the build.

---

## 11. Ambiguous or risky points

- The guest call trace is long and order-sensitive: the fast check in refresh always makes two
  DcFind calls, and each bind makes up to three. A new implementation that caches lookups or
  reorders them will differ in the trace though behaviour is the same.
- Log budgets use test-then-decrement atomics; under contention one extra line can pass. Single
  threaded tests are exact.
- `%p` output depends on the host C library (MinGW prints 16 uppercase hex digits). Use the same
  `log` with `%p`.
- The values of section 9 must be used exactly and in table order: they fix the appended record
  positions, the added store items and the skin fingerprint.
- The Baseball Shirt record's +0 is 1, while appended campaign records get their own index. Keep.
- `build store copy` during a rebind may return early (the game's new store is invalid) and the
  old copy is bound again.
- IsLocked re-reads the list ptr after each ConditionMet call but reads the count from the list
  ptr held at the loop test; a NULL list after a call would crash in the old code too.
- Speakers and npc-info stub records are not validated beyond count and ptr; character attribute
  stub names are read with strlen from guest memory without a bound.
