# Spec: net-event serializers and connection-event Applies (events, gameplay2)

Clean-room description for a new implementation of `gow3_pc/src/u2mp/events.cpp` and
`gow3_pc/src/u2mp/gameplay2.cpp`. It states what the code must do, as seen from outside. It does
not prescribe structure, helper names or loop shapes. All addresses are image offsets of the
Uncharted 2 PS4 1.02 build unless they are called "absolute".

Notation:

- `B` is `image.base` (the loaded image, `u2mp::image`). An image offset `X` is the absolute address `B + X`.
- `ev` is the event object the game passes (absolute pointer). `ev+0xNN` is a byte offset into it.
- "u32", "s32", "u16", "s16", "u8", "u64", "f32" are little-endian host values of that size.
- "guest call F(a, b, ...)" means calling the game function at `B + F` with the x86-64 System V
  convention (`U2MP_GUEST`), arguments in order in rdi, rsi, rdx, rcx.
- All floating point is IEEE single precision (`float`). Every operation is rounded on its own
  (no fused multiply-add, no double intermediates, no fast-math reassociation). The build has
  no `-march`, so the current code never fuses.

---

## 1. Purpose and hook points

### 1.1 events (net-event serializers)

Every net event type has a vtable. Slot 2 is `Read(event, BitStream*)`, slot 3 is
`Write(event, BitStream*)`, slot 4 is `Apply(event, connection)`. (The slot labels in
`FUNCTIONS-MATCH-GAMEPLAY.md` section 7 are swapped: slot 2 is Read, slot 3 is Write.)

In the PS4 1.02 build, 14 event types have Read and/or Write bodies (and type 37 also its Apply)
reduced to a bare `ret`. Those events cross the network empty, and a received NPC spawn does
nothing. This module supplies working Read/Write bodies for the PS4 payload layout and an Apply
for NetEventNpcSpawn. It hooks in by overwriting the vtable slot (an 8-byte absolute pointer)
with the address of the new host function. The `ret` stub code itself is left as it is.

### 1.2 gameplay2 (connection handshake Applies)

Two Apply slots of the connection handshake events are bare `ret` on PS4:

- type 0 NetEventStartConnection::Apply (vtable 0x1041520 slot 4, stub 0x63c0e0)
- type 1 NetEventConnectionDone::Apply (vtable 0x10414d0 slot 4, stub 0x63bda0)

PS4 lost the code and the globals the PS3 handshake used (the pending net id and the
"asked to start connection" flag). The new Applies make the events do something useful if
anything sends them:

- StartConnection on a client answers at once with ConnectionDone(host net id, canTalkToAll = 1,
  0, 0) through the surviving PS4 sender function.
- ConnectionDone on the host finds the sending player from the connection record the event came
  in on, marks that player's tracker as connected and stores canTalkToAll.

Today nothing on PS4 sends either event. Hook method: same vtable-slot overwrite.

### 1.3 Who calls what

- `install.cpp` (`u2mp_install`) calls `install_events(installed)` and then
  `install_gameplay2(installed)`, in that order, once, after the image identity checks.
- `gow3_pc/tests/test_u2mp_events.cpp` (target `u2mp-events-test`, built from that test file plus
  `events.cpp` only) uses `kPorts`, `kPortCount`, `Port::type/size/read/write`, `game`, and the
  `BitStream` helpers. It supplies its own `u2mp::image`, a `Image::matches` that always returns
  false, and a `log` that prints nothing. The new `events.cpp` must link against exactly that
  (it must not need any other symbol from `install.cpp`), and the test must keep passing.
- No other source file references anything in these two files.

---

## 2. Interface

### 2.1 Declarations that stay unchanged

`events.h` (unchanged, copied verbatim):

```
namespace u2mp::events {

using SerializeFn = void(U2MP_GUEST *)(void *event, BitStream *bs);
using ApplyFn = void(U2MP_GUEST *)(void *event, void *connection);

struct Port {
    int type;
    const char *name;
    uint64_t vtable;        // PS4 image offset of the event's vtable
    uint32_t size;          // allocation size in CreateNetEventByType 0x6180f0
    uint64_t read_stub;     // PS4 slot 2 stub replaced by `read` (0: slot left alone)
    uint64_t write_stub;    // PS4 slot 3 stub replaced by `write`
    uint64_t apply_stub;    // PS4 slot 4 stub replaced by `apply`
    SerializeFn read;
    SerializeFn write;
    ApplyFn apply;
};
extern const Port kPorts[];
extern const size_t kPortCount;

struct Game {
    const int32_t *frame_clock = nullptr;   // 300 Hz frame counter, image 0x1116180 + 0x204
    int (*game_mode)() = nullptr;           // the current game mode (12 = TerritoryOwn sends a float)
    void *(*lookup_process)(uint32_t id) = nullptr;   // LookupProcessByUniqueId
};
extern Game game;

void install_events(int &installed);

}  // namespace u2mp::events

namespace u2mp {
using events::install_events;
}
```

`u2mp_internal.h` (unchanged): `void install_gameplay2(int &installed);` in namespace `u2mp`.

Shared helpers the new code uses from unchanged headers: `u2mp::image` (`base`, `size`, `at`,
`matches`), `u2mp::log(format, ...)` (prints `u2mp: ` + text + newline), `u2mp::is_ret_stub`,
and `u2mp::read_bits` / `u2mp::write_bits` / `u2mp::BitStream` from `bitstream.h`.

### 2.2 The port table `kPorts`

`kPorts` must hold exactly these 14 entries, in this order (the order fixes the install log
order). `kPortCount` is 14. Every `read` and `write` field is non-null (the type 44 Write is
present even though it is not installed: the test round-trips through it). Only type 37 has an
`apply`; all others have `apply = nullptr`. A stub value of 0 means "never install that slot".

| type | name | vtable | size | read_stub (slot 2) | write_stub (slot 3) | apply_stub (slot 4) |
|---|---|---|---|---|---|---|
| 37 | `NetEventNpcSpawn` | 0x1040f30 | 0x20 | 0x62f240 | 0x62f230 | 0x62f220 |
| 38 | `NetEventNpcKill` | 0x1040760 | 0x10 | 0x622080 | 0x622070 | 0 |
| 44 | `NetEventNpcDealDamage` | 0x10405d0 | 0x20 | 0x61fe00 | 0 | 0 |
| 47 | `NetEventEvent` | 0x1040580 | 0x100 | 0x61fa60 | 0x61fa50 | 0 |
| 49 | `NetEventTerritoryOwn` | 0x1040440 | 0x50 | 0x61f030 | 0x61f020 | 0 |
| 51 | `NetEventTerritorySpawn` | 0x10403a0 | 0x18 | 0x61e890 | 0x61e880 | 0 |
| 52 | `NetEventTerritorySetTimer` | 0x1040350 | 0x20 | 0x61e700 | 0x61e6f0 | 0 |
| 53 | `NetEventPlayVox` | 0x1040300 | 0x28 | 0x61e600 | 0x61e5f0 | 0 |
| 54 | `NetEventSimpleSnapshot` | 0x10402b0 | 0x40 | 0x61e2a0 | 0x61e290 | 0 |
| 55 | `NetEventStartNpcFight` | 0x1040260 | 0x80 | 0x61e1b0 | 0x61e1a0 | 0 |
| 57 | `NetEventSpawnHiddenTreasure` | 0x10401c0 | 0x30 | 0x61d3a0 | 0x61d390 | 0 |
| 61 | `NetEventEarnMedal` | 0x1040120 | 0x18 | 0x61d240 | 0x61d230 | 0 |
| 67 | `NetEventTurretSnapshot` | 0x1040030 | 0x20 | 0x61bee0 | 0x61bed0 | 0 |
| 76 | `NetEventKillEntity` | 0x103fdb0 | 0x10 | 0x61afd0 | 0x61afc0 | 0 |

### 2.3 Entry points the game reaches

All are reached through vtable slots, called by the game with System V convention
(`U2MP_GUEST`), and return nothing.

| Entry | Called as | Slot it occupies |
|---|---|---|
| Read of type T (14 functions) | `(void *ev, BitStream *bs)` | `B + vtable + 0x10` (slot 2) |
| Write of type T (13 installed + type 44 not installed) | `(void *ev, BitStream *bs)` | `B + vtable + 0x18` (slot 3) |
| NpcSpawn Apply | `(void *ev, void *connection)`; `connection` ignored | `B + 0x1040f30 + 0x20` (slot 4) |
| StartConnection Apply | `(void *ev, void *connection)`; `ev` ignored | `B + 0x1041520 + 0x20` (slot 4), original value `B + 0x63c0e0` |
| ConnectionDone Apply | `(void *ev, void *connection)` | `B + 0x10414d0 + 0x20` (slot 4), original value `B + 0x63bda0` |

The test also calls the Read and Write functions directly through `Port::read` / `Port::write`
on host buffers.

---

## 3. Game facts

### 3.1 BitStream (from `bitstream.h`, unchanged)

`{ u32 *data @0; u32 numBits (cursor) @8; u32 maxBits @0xc; u32 pad }`. Bit n lives in
`data[n >> 5]` under mask `0x80000000 >> (n & 31)`: MSB first inside native u32 words.
`read_bits(bs, value, n)` / `write_bits(bs, value, n)` (n = 0..32) implement exactly the game's
semantics, including the bounds rules (see 4.1).

### 3.2 events: addresses and constants

| Fact | Value | Meaning |
|---|---|---|
| frame clock | image offset 0x1116384 (= 0x1116180 + 0x204), s32 | frame counter, 300 per second |
| NetGameManager | image offset 0x113b760 | the global object (its address is passed, not dereferenced) |
| game mode index | s16 at NetGameManager + 0x1ca0 | index into the game-mode table |
| LookupPointer | guest function 0xa15e70, `(u32 id) -> pointer` | script-manager lookup by id |
| game-mode table id | 0x3b9a067d | id passed to LookupPointer |
| game-mode table | s32 entry count at table+0; entries pointer (u64) at table+8; entry stride 0x90; mode s32 at entry+0xc | |
| LookupProcessByUniqueId | guest function 0x9bbe40, `(void *unused, u32 id) -> process pointer` | first argument always NULL |
| process fields | u64 at process+0x38 (process handle), u32 at process+0x44 | copied into NetEventEvent |
| spawner manager | image offset 0x1130390 | global object (address passed) |
| LookupSpawner | guest 0x56c7b0, `(manager, u32 name id) -> spawner pointer` | |
| SpawnParams init | guest 0x233600, `(params, info)` | |
| SpawnParams set spawner | guest 0x235360, `(params, spawner)` | |
| spawner locator | guest 0x4bdfe0, `(locator out, spawner)` | |
| SpawnNpc | guest 0x4be330, `(spawner, params, locator or NULL, u64 0) -> npc pointer` | |
| AddNetNpcTracker | guest 0x644fa0, `(NetGameManager, npc, u32 value) -> u8` | only the low 8 bits of the return value count |
| spawner fields | u32 at spawner+0x40 | copied into SpawnInfo+0 |
| npc fields | u32 at npc+0x68 = unique id (written); u32 at npc+0x3708 (read, third AddNetNpcTracker argument) | |
| SpawnInfo buffer | 0x60 bytes, 16-byte aligned, zero-filled; +0x00 u32, +0x10 u64 spawner, +0x40 u64 spawner | |
| SpawnParams buffer | 0xb0 bytes, 16-byte aligned, zero-filled; +0x00 u32 second spawner id, +0x40 u32 tag value | |
| locator buffer | 0x40 bytes, 16-byte aligned, zero-filled | |
| payload start | ev+0x0c | every payload starts here; bytes before it are the event header, never touched |
| TerritoryOwn factory state | u32 0xffffffff at ev+0x10 | all other payload bytes of every type are 0 in factory state |
| TerritoryOwn slots | 10 | maximum id count |
| TerritoryOwn float mode | game mode 12 | the f32 field exists only in this mode |
| NetEventEvent max kept args | 7 | memento holds 8 argument slots |
| ret stub bytes | `C3 66 66 66 66` at the stub offset | checked by `is_ret_stub` |

Float constants (exact single-precision values; write them as these decimal literals with `f`):

| Name | Value | Role |
|---|---|---|
| time read scale | 0.006666666828095913f | float(1/150) |
| clock scale | 0.0033333334140479565f | float(1/300) |
| time half wrap | 6.826666831970215f | float(1024/150) |
| time wrap | 13.65333366394043f | float(2048/150) |
| time write scale | 150.0f | |
| position read scale | 0.001953125f | 2^-9 |
| position write scale | 512.0f | |
| quaternion read scale | 0.00024420025874860585f | float(1/4095) |
| quaternion write scale | 4095.0f | |
| quaternion presence threshold | 9.999999747378752e-05f | float(1e-4) |

### 3.3 gameplay2: addresses and constants

| Fact | Value | Meaning |
|---|---|---|
| net state | s32 at image offset 0x14ce97c | multiplayer when > 1 |
| match session | image offset 0x1065b08 | u64 at +0x10 = context; u32 at +0xb00 = my net id; u32 at +0xb04 = host net id |
| NetGameManager | image offset 0x113b760 | trackers at +0x10, 10 trackers, stride 0x2c0 |
| tracker +0x20 | char[] | player name (NUL-terminated); first byte 0 = empty tracker |
| tracker +0x120 | u32 | net id |
| tracker +0x23c | s32 | connection key |
| tracker +0x275 | u8 | connected flag |
| tracker +0x27c | u8 | canTalkToAll |
| connection records | image offset 0x1152a18, 32 records, stride 0x220 | the Apply's `connection` argument is record + 0x40 |
| record +0x00 | u32 | connection key |
| record +0x04 | u8 | record in use (non-zero) |
| ConnectionDone payload | u8 at ev+0x0c canTalkToAll; s32 at ev+0x10 a; s32 at ev+0x14 b | |
| SendConnectionDone | guest 0x60c0f0, `(u32 target net id, u8 canTalkToAll, s32 a, s32 b)` | surviving PS4 sender |
| StartConnection vtable / Apply stub | 0x1041520 / 0x63c0e0 | slot 4 |
| ConnectionDone vtable / Apply stub | 0x10414d0 / 0x63bda0 | slot 4 |

Byte anchors checked at install, in this order (offset: bytes):

1. 0x63bfe9: `41 0F 95 46 0C`
2. 0x63c059: `41 89 5E 10`
3. 0x63c0c9: `41 89 5E 14`
4. 0x60c0f0: `55 48 89 E5 41 57 41 56`
5. 0x60c174: `45 88 67 0C 41 89 5F 10`
6. 0x60c17f: `41 89 47 14`
7. 0x5f2f13: `41 80 BC 15 85 02 00 00 00`
8. 0x5f2ef8: `41 80 BC 15 7C 02 00 00 00`

RIP-relative reference checks, in this order (instruction offset, instruction length,
displacement position inside the instruction, expected target):

1. 0x60c165, 7, 3, 0x10414d0
2. 0x60bf8c, 7, 3, 0x1041520
3. 0x5f2ec2, 7, 3, 0x113b760

---

## 4. Behaviour

### 4.1 Common rules for all Read functions

- Fields are read one at a time, in the listed order. Each field is exactly one `read_bits` call
  of its width (a field is never split into several calls). A sub-field of a composite value
  (position component, quaternion presence bit, quaternion value) counts as its own field.
- Before each field the reader checks it fits (this is what `read_bits` does). The first field
  that does not fit stops the reading: no later field is read, and the BitStream cursor stays at
  the start of that field (`read_bits` leaves it unchanged on failure). The reader never reads
  past maxBits.
- If the reading stopped early (or a type-specific check rejects the input, see TerritoryOwn),
  the payload is put back into its factory state when the Read returns: bytes `ev+0x0c` up to
  `ev+size` (size from the type's `kPorts` entry) all become 0, and for type 49 the u32 at
  `ev+0x10` then becomes 0xffffffff. No log line is printed for a short stream.
- On success, only the listed destination bytes are written. Every other byte keeps its old
  value (for example TerritoryOwn id slots beyond the count, TerritorySetTimer +0x18 when the flag
  is 0, the gaps in NetEventEvent).
- Guest calls made by a Read (game mode lookup, process lookup) happen only at the point stated
  for that type, and only if every field before that point was read successfully.
- Reads never print anything.

Field encodings on read (the destination size is stated per field):

- `N bits` (N = 1..32): the value is the N bits read, zero-extended, stored as u32 unless a
  different size is stated.
- `U32`: 32 bits stored as u32. `F32`: 32 bits stored unchanged as the raw bit pattern of the f32.
- `Flag`: 1 bit; stored as u8 1 if the bit is 1, else u8 0.
- `Time`: 11 bits v. `t = float(v) * 0.006666666828095913f` (single-precision multiply). Then, if
  that field was read successfully and `game.frame_clock` is not NULL:
  `now = float(*frame_clock) * 0.0033333334140479565f` (the s32 read at that moment, converted
  to float, one multiply). While `now - t > 6.826666831970215f` (float subtraction, then compare),
  `t = t + 13.65333366394043f` (one float addition per step). The result must equal repeated
  single-precision additions; computing a multiple of the wrap at once is not allowed because
  rounding would differ. Stored as f32.
- `Position` (16 bytes at the destination): three 18-bit fields x, y, z, each sign-extended from
  18 bits to s32 (two's complement; bit 17 is the sign), converted to float and multiplied by
  0.001953125f. Stored as four f32: x, y, z, y (y is stored twice, at +4 and +0xc).
- `Locator` (32 bytes at the destination): a Position at +0x00..+0x0f, then four quaternion
  components x, y, z, w. For each component: read 1 presence bit; if it is 1, read 13 bits v;
  if it is 0, v = 4095 and no 13-bit field is read. Component `c = float(v) * 0.00024420025874860585f`,
  then `c = c + (-1.0f)` (two separate rounded operations). After all four:
  `s = ((x*x + y*y) + z*z) + w*w` in float, left to right, each product and sum rounded. If
  `s > 0`: `r = 1.0f / sqrtf(s)` (correctly rounded float square root, then a float division,
  no reciprocal-square-root approximation), and each component is multiplied by r. Otherwise the
  quaternion becomes (0, 0, 0, 1). Stored as four f32 at destination +0x10.

### 4.2 Common rules for all Write functions

- Fields are written in the listed order, each as exactly one `write_bits` call of its width.
  `write_bits` writes the low N bits of the value. When a field does not fit before maxBits it is
  not written, the cursor still advances by its width, and every later field also does not fit.
  Writers never touch memory past maxBits (that is `write_bits` behaviour).
- When a `write_bits` call reports that the field did not fit, the module prints, once per
  process lifetime (one flag shared by all Write functions of all types; later overflows print
  nothing):
  `net event: a Write ran past the BitStream's maxBits (fields dropped)`
- Writers do not modify the event.

Field encodings on write:

- `N bits`: the low N bits of the u32 at the source offset.
- `U32`: the u32. `F32`: the raw 32-bit pattern of the f32.
- `Flag`: the low bit of the u8 at the source (`byte & 1`), 1 bit.
- `Time`: `p = t * 150.0f` (float multiply). Convert p to s64 with truncation toward zero; if p
  is NaN or outside [-2^63, 2^63) the result is INT64_MIN (x86 `cvttss2si` 64-bit behaviour).
  Write the low 11 bits of that s64. (So -1 becomes 0x7ff, and NaN becomes 0.)
- `Position`: for x, y, z at source +0, +4, +8 (the f32 at +0xc is ignored): `p = f * 512.0f`
  (float), converted to s32 with truncation toward zero; NaN or outside [-2^31, 2^31) gives
  INT32_MIN. Write the low 18 bits.
- `Locator`: Position at source +0, then for each of the four f32 components at source +0x10,
  +0x14, +0x18, +0x1c: present when `fabsf(c) > 9.999999747378752e-05f` (NaN is not present).
  Write 1 bit (1 present, 0 absent). If present, write the low 13 bits of
  s32-truncation of `(c + 1.0f) * 4095.0f` (two rounded float operations, out of range or NaN
  gives INT32_MIN as above). No normalisation and no clamping on write.

### 4.3 Per-type field lists

Each line: wire field, width/encoding, payload offset (destination on read, source on write).
Read and Write use the same list unless stated.

**37 NetEventNpcSpawn** (size 0x20): 32 bits @0x0c (npc unique id); U32 @0x10 (spawner name id);
U32 @0x14 (second spawner id); U32 @0x18 (spawn tag value). 128 bits.

**38 NetEventNpcKill** (size 0x10): 32 bits @0x0c. 32 bits.

**44 NetEventNpcDealDamage** (size 0x20): 12 bits @0x0c; 32 bits @0x10; U32 @0x14; Time @0x18;
6 bits @0x1c. 93 bits. (Write exists for the test, it is not installed.)

**47 NetEventEvent** (size 0x100): see 4.4.

**49 NetEventTerritoryOwn** (size 0x50): see 4.5.

**51 NetEventTerritorySpawn** (size 0x18): U32 @0x0c; U32 @0x10. 64 bits.

**52 NetEventTerritorySetTimer** (size 0x20): U32 @0x0c; F32 @0x10; Flag @0x14 (u8); then, only
when the flag is set, 1 bit @0x18 (stored as u32 on read).
- Read: the extra bit is read only when the flag bit read was 1. When it was 0, `ev+0x18` is not
  written (on success).
- Write: the flag bit written is `u8@0x14 & 1`, but the extra bit is written whenever
  `u8@0x14 != 0` (whole byte test). Example: byte 2 writes flag 0 and still writes the low bit
  of u32@0x18. This mismatch is required behaviour.

**53 NetEventPlayVox** (size 0x28): Flag @0x0c (u8); then the field @0x10 (u32) is 12 bits when
the flag is set, else 32 bits; U32 @0x14; U32 @0x18; F32 @0x1c; F32 @0x20.
- Read: the width follows the flag bit read (if the flag read itself failed nothing further is
  read).
- Write: the flag bit written is `u8@0x0c & 1`, but the width is 12 when `u8@0x0c != 0` (whole
  byte), else 32. Same required mismatch as type 52.
- Totals: 141 bits with flag 1, 161 with flag 0.

**54 NetEventSimpleSnapshot** (size 0x40): Locator @0x10 (32 bytes); U32 @0x30; Time @0x34;
12 bits @0x38.

**55 NetEventStartNpcFight** (size 0x80): Locator @0x10; Locator @0x30; 12 bits @0x50;
32 bits @0x54; U32 @0x58, @0x5c, @0x60, @0x64, @0x68 (in that order); F32 @0x6c; F32 @0x70;
6 bits @0x74; Flag @0x78 (u8); Flag @0x79 (u8).

**57 NetEventSpawnHiddenTreasure** (size 0x30): Position @0x10 (16 bytes: x, y, z, y); U32 @0x20.
86 bits.

**61 NetEventEarnMedal** (size 0x18): U32 @0x0c; U32 @0x10; U32 @0x14. 96 bits.

**67 NetEventTurretSnapshot** (size 0x20): F32 @0x0c; F32 @0x10; U32 @0x14; Time @0x18;
12 bits @0x1c. 119 bits.

**76 NetEventKillEntity** (size 0x10): U32 @0x0c. 32 bits.

### 4.4 NetEventEvent (type 47) in detail

Payload layout: an event memento at ev+0x10:

| Offset in ev | Size | Content |
|---|---|---|
| 0x10 | u64 | source message (always 0 after Read) |
| 0x18 | u32 | message id |
| 0x20 | u64 | sender process handle |
| 0x28 | u32 | sender process field |
| 0x30 | s8 | argument count |
| 0x38 + i*0x18 (i = 0..7) | u64 value, u64 at +8, u32 type at +0x10 | argument slot i |
| 0xf8 | u32 | sender process unique id |
| 0xfc | u32 | target process unique id |

Wire format: U32 target, U32 message id, U32 sender, 4 bits count, then `count` times
{4 bits type, U32 value}.

Read:

1. Read target, message, sender, count, then every one of the `count` arguments (type, then
   value), even those beyond 7. Argument decoding: type 1 gives value `(raw != 0)` (0 or 1) with
   type 1; types 2, 3 and 6 give the raw u32 zero-extended to u64, with that type; any other type
   gives value 0, type 0.
2. If any field did not fit: payload reset to factory state (all zero from 0x0c to 0x100), no
   guest call, done.
3. On success, write: u32@0xfc = target; u32@0xf8 = sender; u64@0x10 = 0; u32@0x18 = message.
4. Then look up the sender process: if `game.lookup_process` is not NULL, call it with the sender
   id (in the installed game that is one guest call 0x9bbe40(NULL, sender)). If it returns a
   process: u64@0x20 = u64 at process+0x38, u32@0x28 = u32 at process+0x44. If there is no
   lookup function or it returns NULL: both are 0.
5. kept = min(count, 7). s8@0x30 = kept.
6. For all 8 slots i = 0..7: u64 value = the decoded value for i < kept, else 0; u64 at +8 = 0;
   u32 type = the decoded type for i < kept, else 0. Bytes +0x14..+0x17 of each slot are not
   written.
7. Bytes 0x0c..0x0f, 0x1c..0x1f, 0x2c..0x2f, 0x31..0x37, 0xf4..0xf7 are not written on success.

Write:

1. U32 = u32@0xfc; U32 = u32@0x18; U32 = u32@0xf8.
2. n = s8@0x30. If n > 8, n = 8. Write 4 bits: n, or 0 when n is negative.
3. For i = 0 .. n-1 (nothing when n <= 0): t = u32 at slot+0x10. Value: if t == 1, 1 when the
   first byte of the slot (u8 at slot+0) is non-zero, else 0; otherwise the low 32 bits of the
   slot value (u32 at slot+0). Write 4 bits of t (its low 4 bits), then U32 value.

### 4.5 NetEventTerritoryOwn (type 49) in detail

Read, in order:

1. U32 → u32@0x0c.
2. 16 bits → u16@0x10. 3. 16 bits → u16@0x12.
4. 4 bits count. If the count field was read and count > 10, the input is rejected (treated as
   failed: nothing more is read, no game mode lookup, payload reset). The cursor stays after the
   count field (68 bits consumed). Store u32@0x14 = count (0 when rejected or the read failed;
   it is reset anyway in that case).
5. count × 12 bits → u32 at 0x18 + 4*i for i = 0 .. count-1. Slots at and beyond count are not
   written.
6. 12 bits → u32@0x40. 7. 12 bits → u32@0x44.
8. Only if everything so far succeeded: determine the game mode (see 4.6). If it is 12, F32 →
   f32@0x48. Otherwise 0x48 is not read and not written.
9. On failure: factory state (zeros, then u32@0x10 = 0xffffffff).

Write, in order: U32 u32@0x0c; 16 bits u16@0x10; 16 bits u16@0x12; count = u32@0x14 clamped to
at most 10 (unsigned compare), 4 bits; count × 12 bits from u32 at 0x18 + 4*i; 12 bits u32@0x40;
12 bits u32@0x44; then the game mode is determined (always, after the 0x44 field), and when it is
12, F32 f32@0x48.

Bit totals with count 10: 212 bits, plus 32 in mode 12.

### 4.6 Game mode

The "current game mode" used by TerritoryOwn is `game.game_mode()` when that pointer is set, else
0. The installed function computes:

1. Guest call 0xa15e70(0x3b9a067d) (one u32 argument). If it returns NULL: mode 0.
2. index = s16 at image offset 0x113b760 + 0x1ca0 (= 0x113d400). If index < 0 or
   index >= s32 at table+0: mode 0.
3. entries = u64 pointer at table+8. If NULL: mode 0. Else mode = s32 at
   entries + index*0x90 + 0xc.

### 4.7 Process lookup

The installed `game.lookup_process(id)` is one guest call 0x9bbe40(NULL, id) returning a
pointer.

### 4.8 NetEventNpcSpawn Apply (type 37)

Inputs: npc id = u32@ev+0x0c, spawner id = u32@ev+0x10, second id = u32@ev+0x14,
tag = u32@ev+0x18. `manager` = absolute address of image offset 0x1130390. The second argument
of the Apply is ignored. Observable steps, in this order:

1. If `game.lookup_process` is set: call it with the npc id. If it returns non-NULL, log
   `NetEventNpcSpawn: npc %u already exists` (npc id) and stop.
2. spawner = guest 0x56c7b0(manager, spawner id).
3. If second id != 0: second = guest 0x56c7b0(manager, second id); else second = NULL and no call.
4. If spawner is NULL: log `NetEventNpcSpawn: can't find spawner %08x` (spawner id) and stop.
   (Steps 2 and 3 both happen before this check.)
5. Build SpawnInfo (0x60 bytes, 16-aligned, zeroed): u32 at +0 = u32 at spawner+0x40; u64 at
   +0x10 = spawner; u64 at +0x40 = spawner.
6. Build SpawnParams (0xb0 bytes, 16-aligned, zeroed). Guest call 0x233600(params, info).
7. Then u32 at params+0 = second id.
8. Guest call 0x56c7b0(manager, second id) again, unconditionally (also when second id is 0).
   If it returns non-NULL s: guest call 0x235360(params, s).
9. u32 at params+0x40 = tag.
10. Locator buffer (0x40 bytes, 16-aligned, zeroed). If second (from step 3) is non-NULL: guest
    call 0x4bdfe0(locator, second), and the locator argument for the spawn is this buffer;
    otherwise the locator argument is NULL.
11. npc = guest 0x4be330(spawner, params, locator argument, 0).
12. If npc is NULL: log `NetEventNpcSpawn: unable to spawn npc from spawner %08x` (spawner id)
    and stop.
13. u32 at npc+0x68 = npc id.
14. ok = guest 0x644fa0(absolute address of image offset 0x113b760, npc, u32 at npc+0x3708);
    only the low byte of the return counts. If it is 0: log
    `NetEventNpcSpawn: failed to add npc %u to the NetGameManager` (npc id).

The buffers are local scratch memory; the guest functions fill them. Nothing else is written.

### 4.9 NetEventStartConnection Apply (type 0)

1. Set a module-private "asked to start connection" flag (nothing reads it; not observable).
2. Log `************** Asked to start Connection` (always, first).
3. Stop (silently) unless all of: s32 at 0x14ce97c > 1; not host; `connection` non-NULL.
   Host means: u64 at match+0x10 == 0, or u32 at match+0xb00 == u32 at match+0xb04
   (match = image offset 0x1065b08).
4. host = u32 at match+0xb04. Guest call 0x60c0f0(host, 1, 0, 0) (u32, u8, s32, s32).
5. Log `************** Connections done sending to host %u` (host).
6. Clear the private flag.

The `ev` argument is not read.

### 4.10 NetEventConnectionDone Apply (type 1)

1. Read can = u8@ev+0x0c, a = s32@ev+0x10, b = s32@ev+0x14.
2. Connection key: 0 when `connection` is NULL, or when its absolute address is below
   `B + 0x1152a18 + 0x40`, or when the distance from that address is not a multiple of 0x220, or
   when distance / 0x220 >= 32. Otherwise record = connection - 0x40; key = u32 at record+0 if
   the u8 at record+4 is non-zero, else 0.
3. Key 0: stop silently.
4. Walk the 10 trackers in index order (tracker i at image offset 0x113b760 + 0x10 + i*0x2c0).
   Skip a tracker whose name byte at +0x20 is 0 or whose s32 at +0x23c differs from the key
   (compared as signed 32-bit, same bits). For the first match: u8 at +0x27c = can; u8 at +0x275
   = 1; log
   `************** Connection established for %s (net id %u)  canTalkToAll = %u (%d, %d)`
   with (name string at tracker+0x20, u32 at tracker+0x120, can as unsigned, a, b). Note the two
   spaces before `canTalkToAll`. Stop.
5. No match: log `NetEventConnectionDone: no player tracker for connection key %u` (key).

No guest calls.

---

## 5. Install-time behaviour

### 5.1 `install_events(int &installed)`

1. If `image.base` is NULL or 0x1116384 + 4 > `image.size`: return at once. No log, `installed`
   unchanged, `game` not touched.
2. Set `game.frame_clock` to the absolute address of image offset 0x1116384, `game.game_mode` to
   the function of 4.6, `game.lookup_process` to the function of 4.7.
3. For each `kPorts` entry in table order: if vtable + 0x28 > `image.size`, skip the entry
   silently. Otherwise for slot 2 (read_stub, read), slot 3 (write_stub, write), slot 4
   (apply_stub, apply), in that order: skip silently when the stub is 0 or the function is NULL.
   Else the slot is the u64 at `B + vtable + 8*slot`. If it is not equal to `B + stub`, or the
   bytes at the stub are not `C3 66 66 66 66` (`is_ret_stub`), log
   `%s slot %u: expected the stub 0x%llx, left alone` (port name, slot number, stub offset) and
   leave it. Otherwise store the absolute address of the new function in the slot and count it.
4. Log `net events: %d stubbed Read/Write/Apply slots ported` (count), then add the count to
   `installed`. On a good image the count is 28 (type 37: 3, type 38: 2, type 44: 1, the other
   11 types: 2 each).

### 5.2 `install_gameplay2(int &installed)`

1. If `image.base` is NULL: return, no log.
2. Check the 8 byte anchors (3.3) in order with `image.matches`. On the first mismatch log
   `gameplay2: code at 0x%llx differs, connection events left stubbed` (anchor offset) and
   return.
3. Check the 3 RIP references in order. If offset + length > `image.size`: return silently.
   Read the s32 displacement at offset + displacement position. If offset + length +
   displacement (signed 64-bit arithmetic) is not the expected target, log
   `gameplay2: 0x%llx does not reference 0x%llx, connection events left stubbed`
   (instruction offset, expected target) and return.
4. If 0x1152a18 + 32*0x220 > `image.size` or 0x113b760 + 0x1d00 > `image.size`: return silently.
5. For StartConnection (vtable 0x1041520, stub 0x63c0e0, name
   `NetEventStartConnection::Apply`), then ConnectionDone (vtable 0x10414d0, stub 0x63bda0, name
   `NetEventConnectionDone::Apply`):
   - if vtable + 0x28 > `image.size`: skip silently;
   - if the u64 at `B + vtable + 0x20` is not `B + stub`, or the stub is not a ret stub: log
     `%s: vtable 0x%llx slot 4 is not the stub 0x%llx, left alone` (name, vtable, stub);
   - otherwise store the new Apply's absolute address there, log
     `%s: vtable 0x%llx slot 4 -> C++` (name, vtable), and add 1 to `installed`.

---

## 6. Test scenarios

The existing `u2mp-events-test` must pass unchanged. In addition, run old and new code on the
same fake image and compare guest calls, log lines and memory.

Serializers (each type, Read and Write):

1. Round trip of typical values for every type; compare bit count, bit pattern and payload.
2. For every cut length 0 .. full length - 1: Read leaves the cursor at the start of the failing
   field and the payload in factory state (type 49: u32 0xffffffff at +0x10); pre-fill the
   event with 0x5a to see untouched bytes.
3. Successful Read over a pre-filled event: check the bytes that must stay untouched (type 47
   gaps, type 49 slots beyond count, type 52 +0x18 with flag 0, everything outside the payload).
4. Write into a BitStream whose maxBits ends inside the 1st, a middle and the last field: memory
   past maxBits unchanged, cursor advanced by the full length, overflow message printed only on
   the first overflow of the process.
5. Dirty output buffer (all 0xa5) to check the trailing-bit clearing of `write_bits`.

Encodings:

6. Time read with frame clock NULL, clock behind t, clock exactly 1024/150 s ahead, clock far
   ahead (e.g. 2^31-1 frames, many wrap steps), and negative clock. Compare floats bit for bit.
7. Time write with t negative, NaN, +inf, 1e30, values that truncate (0.0066), 2048/150 and above.
8. Position write with NaN, +-inf, values beyond +-256, negative fractions; read of every sign
   boundary (0x1ffff, 0x20000, 0x3ffff).
9. Locator write: components exactly +-1e-4, slightly above, NaN, > 1 (13-bit wrap), -1.
   Locator read: all absent (s = 0 path or tiny s), all present with value 0, mixed; compare
   floats bit for bit.

Type-specific:

10. Type 49 with count 0, 10, 11 and 15; game mode 12 and not 12; `game.game_mode` NULL; short
    stream before the count; check whether the mode lookup guest call happens (it must not on a
    failed read; it must happen on every write).
11. Game-mode function: LookupPointer returns NULL; index -1; index = count; entries NULL;
    valid entry.
12. Type 47: count 0, 4, 7, 8, 9, 15; unknown arg types (0, 4, 15); bool raw values 0 and 0x100
    on read; sender known and unknown; `game.lookup_process` NULL; write with s8 count -1, 8, 9,
    0x7f; bool slot whose first byte is 0 but higher bytes not; non-bool slot with high value
    bits set; type field > 15.
13. Types 52 and 53 with the flag byte 0, 1, 2 and 3 on write.
14. Type 44 Write is never installed (slot 3 of 0x10405d0 untouched).

NpcSpawn Apply:

15. Process already exists (stop after lookup + log).
16. `game.lookup_process` NULL (no lookup call).
17. Spawner not found, with second id 0 and with second id non-zero (the second lookup happens
    before the log).
18. Second id 0 (two spawner lookups plus the unconditional third lookup with id 0, no locator
    call, NULL locator).
19. Second id non-zero, found and not found (locator call only when found; the repeated lookup
    and 0x235360 call order).
20. SpawnNpc returns NULL; AddNetNpcTracker returns 0, 1, and 0x100 (low byte 0 → log).
21. Check buffer contents passed to 0x233600, 0x235360, 0x4bdfe0, 0x4be330 (sizes, alignment,
    zero fill, fields set before/after the init call).

Connection Applies:

22. StartConnection: net state 0/1/2; host by context 0; host by equal ids; client with
    connection NULL; client with a connection (sender call, two log lines).
23. ConnectionDone: connection NULL; below the records; misaligned; record 31 and record 32
    (out of range); record not in use; key with no tracker; empty-name tracker with the same key;
    two trackers with the same key (only the first is changed); negative key values.

Install:

24. Good image: 28 event slots, log counts, `installed` totals; gameplay2 adds 2 with two log lines.
25. A slot already replaced (value differs) and a stub whose bytes differ: the "left alone" log
    lines with correct slot numbers.
26. Image too small for the frame clock (no log, nothing done) and for a vtable (silent skip).
27. gameplay2: each anchor changed in turn (only the first mismatch logged); each RIP reference
    wrong; image too small for the records or the NetGameManager (silent); a slot not the stub.
