# PORT-RENDER: the first-frame crash of a match (test runs 35-38)

Code: `gow3_pc/src/u2mp/render.cpp` (`install_render`, chained from `install_nullsites`, so install.cpp
needs no new line), `u2mp/patcher/u2conv/u2actor.py` (`fix_cbuffer_capacity`, `cbuffer_capacity_errors`),
`u2mp/patcher/u2conv/u2twin.py` (calls it after a twin build). Addresses are Uncharted 2 PS4 1.02 image
offsets.

## Symptom

The match reached Playing on Gompa / Shambhala and crashed in the render jobs on the first frame:

| run | crash | what was seen |
|---|---|---|
| 35 | 0xc12173 in 0xc12150 | `res` = 0x33d5c036'f1b17bc2 is not a pointer |
| 36 | 0xc25ddb (walker reads `res->type`) | the whole entry: res 0x33d5c036'f1b17bc2, buffer 0x08d9a770'6f1c0396, def = the transform cbuffer {0x8704e118, 0x2b444d15, scope 3, 0x40 bytes} |
| 37 | 0xc04403 | the bad entry was skipped, the draw path read it anyway |
| 38 | 0xc04403 (`[[r14]]`, rdi = 0), from 0xc31340 in 0xc30ec0 | the entry was repaired with a zero block; the draw lookup then found no resource |

The value looked like an unresolved parameter reference. It is not: it is two cbuffer keys written over
an entry by an overflow.

## Mechanism (proven from the code and the paks)

Mesh instances keep one list of constant buffers per cbuffer scope. The scope comes from the scope sid of
each cbuffer record (const-buffer-mgr.cpp `CBufferScope`, switch at 0xc26309: 0x27e5a0d9 = 0,
0x460911b9 = 1, 0x01f21598 = 2, **0x6bbcf890 = 3 (instance)**, 0xce69e74c = 4, 0xf25e9b07 = 5,
0x8d170e75 = 6, 0x446945fe = 7).

* **InitCbList 0xc24780** `(list, scope, capacity)`: `count = 0`, `capacity`, `scope`, and **one**
  allocation of `capacity * 0x38` bytes: `keys = mem` (capacity u64) and `entries = mem + capacity*8`
  (capacity entries of 0x30 bytes).
* **MeshInstance::Init 0xc3b6f0** (callers 0xbe4bb3, 0xc29a74, 0xd256b7): `instance+0x170` = scope 3
  list with capacity **prototype+0x30** (0xc3f23b), `instance+0x1b0` = scope 4 list with capacity 4,
  per sub-element `+0x340` = scope 5 list (capacity from the submesh, at least 1). Then for each
  sub-element and each of its 0x20 material programs, 0xc31c10 adds the program's cbuffers of that
  scope (0xc3f2ec / 0xc3f33f / 0xc3ed52).
* **0xc31c10** walks program+8 and program+0x10 (the two stages), their cbuffer defs (+0x40, count
  +0x3c), and calls **AddCb 0xc24b30** for each def of the scope (0xc31cc0, 0xc31d60 - its only callers).
* **AddCb 0xc24b30**: key = `def.name << 32 | def.id`. If the key is in `keys[0..count)` the refcount
  goes up. Otherwise the first entry with `used == 0` is reused, else index `count`. The entry is written
  (`res` = 0xc16820(tag 0x31, {size aligned 16 | bit 34, flags}, 0, 0, 1), `buffer` = 0, `def`, data,
  `used` = id, refs = 1, dirty = 1, flags), `keys[index] = key`, and `count++`. **There is no capacity
  check.**

So with capacity 2 and 4 distinct instance cbuffers, key 3 goes to `keys[2]` = `entry[0].res`, key 4 to
`keys[3]` = `entry[0].buffer`, and entries 2 and 3 are written past the end of the allocation.

The converted mp-hero.pak declares exactly 4 scope-3 cbuffers (records `{ptr members, n, size, name,
0x6bbcf890, id, 1}`):

| name | id | size | |
|---|---|---|---|
| 0x8704e118 | 0x2b444d15 | 0x40 | the instance transform (UpdateTransforms 0xb90ba0 marks it dirty) |
| 0x0252db4e | 0x9263c223 | 0x08 | |
| 0x33d5c036 | 0xf1b17bc2 | 0x10 | -> run 36 entry[0].res |
| 0x08d9a770 | 0x6f1c0396 (`general`) | 0x60 | -> run 36 entry[0].buffer |

and every prototype states capacity 2: ps4actor.py (the U3 actor converter, `protob.u32(2).u32(1)`)
writes a fixed 2. Retail U2 actors state the number of distinct scope-3 cbuffers of their pak: of 1139
retail actor27 paks, 1138 have max(prototype+0x30) equal to that number (mp-hero-elena 4, costume-hero-nd
4, mp-hero-merc 3, 9mm-casing 3) and none has less (sullivan.pak states 4 for 5 records). Of the 103
converted actor27 paks of the import test, 94 state too few. Converted levels (ps4stage, already on the
U3 rule max(8, feature sets + 2)) are fine; their capacity-1 prototypes are the water dummies.

Run 38 also explains itself: the repair stub replaced `entry[0].res` (= `keys[2]`) with the zero block,
so the key of cbuffer 3 was gone and the draw lookup in 0xc30420 (0xc306f0..0xc30790) found nothing,
stored NULL, and 0xc04340 read through it at 0xc04403. There was no missing PS3 parameter provider and
nothing PS3 registered that PS4 stripped: PS3 1.10 has no equivalent, the cause is the converter value.

## Readers of the lists (all reviewed)

| function | reads | note |
|---|---|---|
| walker 0xc25670 (about 30 render callers, e.g. 0xc419d0 for +0x170 / +0x1b0 / sub-element +0x340) | count, entries: used +0x20, def +0x10, dirty +0x28, res +0, buffer +8 | read lock on +0x20; calls 0xc12150(&res, def.size, &entry.buffer) at 0xc258b2 |
| 0xc12150 | `res` descriptor (type u16 +0x18) | allocates `size` bytes; types 4/7/8 also get a copy of the descriptor in `entry.buffer` |
| draw lookup 0xc30420 (from 0xc30ec0 / 0xc31800) | keys, entries +0x2c flags, +8 or +0 | key not found -> NULL in the array 0xc04340 reads (0xc04403) |
| RemoveCb 0xc24dc0 (0xc31d80), 0xc27cxx | keys, refs +0x24, releases with 0xc11ed0 | |
| ClearCbList 0xc24890 (0xc6efde, 0xc7023c) | entries, def +0xc | |
| relocation 0xc249a0 | keys / entries pointer, moved when inside the moved block | arrays outside the block are left alone |

## What was changed

### Converter (root cause)

`u2actor.convert` now ends with `fix_cbuffer_capacity(out)`: every GEOMETRY_1 prototype whose +0x30 is
below the number of distinct scope-3 cbuffer records of the pak gets that number (one u32 in place, no
relocation). `u2twin.build` calls it again on its output (the twin's LOD0 words are the retail ones for
the twin's materials, but a skin keeps converted materials and LOD1-3 keep the converter's value).
`u2actor.check`, run after every actor, skin, twin and prop conversion (and by `validate.py skins`),
fails a pak whose prototypes state fewer.

### Engine (any asset that still states too few)

1. **AddCb guard** (0xc31cc0, 0xc31d60 -> `add_cbuffer_checked`): under the list's write lock, the same
   search AddCb does; when the new cbuffer would go to index `count >= capacity`, the list gets new
   arrays (capacity x2, at least +4) with the old keys and entries copied, then the original AddCb runs.
   The new arrays are host memory and are never freed (the game releases the instance block whole; the old
   arrays stay in it unused). Logged as `render: cbuffer list ... grown to N`.
2. **Entry repair** (stub at 0xc2574f, moved here from nullsites.cpp): a live entry whose `res` is not a
   user-space pointer gets a real cbuffer resource made the way AddCb makes it (0xc16820, size from the
   def, the entry's flags, zero data), `buffer` = 0 and dirty, so 0xc12150, the type read at 0xc25ddb
   and the draw lookup all see a valid resource. No usable def: the entry is marked unused and skipped.
   The stub saves rax, rsi, rdi, r8-r11 and xmm0-15 around the C++ call.
3. **Lookup fallback** (stub at 0xc30790): a NULL result of the draw lookup becomes a zeroed 4 KiB cbuffer
   resource (made once with 0xc16820 at the first AddCb), counted and logged as
   `render: N cbuffer lookups found no entry`.
4. **Bind guard** (0xc258b2 -> `bind_copy_checked`): kept from run 35 as the last check, logs only.

With the converter fixed, items 1-4 should never log. If they do, the log names the list scope, the
cbuffer key and id.

## Re-run

All actor27 imports (skins, twins, props). The step hash covers u2actor.py / u2twin.py, but files are
skipped per item by `done_before` unless `ACTOR_VERSION` in u2mp_import.py changes, so that constant has
to be bumped (e.g. `'u2ag2'`), or the import run with `--force`. Levels (pak22) need nothing.

## Review notes (2026-11-08)
- A repair that cannot make a resource now also points `res` at the shared zeroed cbuffer, because the draw
  lookup 0xc30420 matches by key without testing `used`.
- Type-9 cbuffers (data pointer wrapped, not copied) get no shared data pointer.
- Open (leak only, no crash): `grow` uses host calloc. DestroyCbList (0xc249e0) frees `keys` through 0xb356d0,
  which skips a pointer no game allocator owns, and the original InitCbList block is not freed. Per undersized
  instance per map load. Fix later with the game allocator (0xb333f0, tag 0x39) and 0xb356d0 for the old block.
  After the actor re-import (u2ag2) no list should need to grow.
