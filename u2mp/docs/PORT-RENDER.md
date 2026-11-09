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

## Runs 41-42: the player character is not seen (open)

The spawn resolves the model (`skins: spawn 0x663280: model 'mp-hero-year-1' -> ...`, package state 7), and
there are no render repairs, no crash and no lasting "Draws without a pipeline" after the spawn. Nothing
draws the character.

### Structure compared with retail (retail mp-hero-navarro, costume-hero-nd; import builds mp-hero, mp-hero-year-1)

| structure | import build vs retail |
|---|---|
| FOREGROUND_SECTION_2 x4 | same layout: prototype, skeleton 0x27e57f43 / hierarchy 0xef94465d, the same 0x40-byte list at +0x10, LOD distances 0 / 6 / 15 / 20 |
| ARTGROUP_DATA | same layout, 8 keys: sid('geo.' + FG name) for the 4 sections + 4 others, same as retail |
| tags | 4 vis_sphere (0,0,0,1.25), 3 lod_distance, 4 vis_joint_index (0) like retail; login order differs (harmless) |
| prototypes | same fields; flags byte 3 is 1 (retail mostly 0, mp-hero-udf 1); +0x30 capacity fixed (4) |
| submeshes | same 0x88 record, 230 joints, stream formats 00000c06 / 01000422 / 0200080a / 0300080a (+0c000422 / 0400080a on swapped twin submeshes) like retail |
| skinning | same {0, n, counts, offsets, joint/weight pairs, weights} layout; weights sorted the other way (harmless) |
| skeleton | the hero skeleton (JOINT_HIERARCHY 'skel' in proto.pak) loads in the frontend |

Nothing found in the paks explains a whole character that does not draw. Note that the twin build's LOD0
(mp-hero) is the PS4 campaign mesh unchanged. Not proven either way: whether retail skins draw in a
match. **Test: pick a skin the PS4 game ships (Navarro, Elena, Sullivan, Merc ...) in the character menu.**
If it doesn't draw either, the cause is in the runtime (spawn / show of the net player), not in the converter.

### Runtime evidence added (render.cpp section 5, `render diag:` lines)

* `init <instance> prototype '<name>'`: MeshInstance::Init (calls 0xbe4bb3, 0xc29a74, 0xd256b7) for every
  prototype whose name contains `mp_hero`: submesh count, cbuffer capacity, sub-elements, transform, bounds,
  flags.
* `first submit of <instance>`: the binding walker 0xc25670 (hooked at its entry with a trampoline) ran
  for one of that instance's cbuffer lists, i.e. the renderer submitted it. Logged with the override table
  (per-draw data, e.g. the skinning palette) and the transform at that time. After that, counts of submits
  every 0x40000 walker calls.
* `UNCHARTED_U2MP_RENDER_DIAG=all` records every prototype; `=0` turns it off.

Reading it: no `init` = the character never gets a mesh instance (spawn / look); `init` without
`first submit` = it is culled or never queued (visibility, LOD, bounds); `first submit` with a zero or far
transform = drawn off-screen; submits with a sane transform = drawn but invisible (skinning palette,
shaders).

### Run 43 and what run 44 logs

Run 43: the spawn resolved `mp-hero-year-1`; MeshInstance::Init ran 4 times from 0xbe4bb8 (Prototype0..3,
14/10/10/6 submeshes, capacity 4, identity transform, zero bounds, flags +c0 0046 +d8 d03b), and in 60 s
none of the 4 was ever submitted to the binding walker. The user saw no character.

The hero's instances come from the actor path: DrawControl::AddMesh 0x4b6e20 (game/drawcontrol.cpp,
calls 0x4b6faf / 0x4b7007) -> 0xbe48d0, which makes the MeshInstance at actor +0x1c0 (Init at 0xbe4bb3),
a scope-6 cbuffer list at actor +0x3c0 (= MeshInstance +0x200), and is torn down by 0xbe45e0
(0xc3f4b0 at 0xbe46c5). Weapons and props use the same path.

Run 44 diagnostics (render.cpp section 5):
* references next to the hero: the last 8 other actors (path 0xbe4bb3) and the last 8 level instances
  (0xc29a74). If they show submits, the counter counts drawn objects and the hero is really not queued.
* the actor's +0x200 list is watched too; the first-submit line names the list that matched.
* `destroy` lines from the teardown: an early destroy is a different diagnosis from "alive, not queued".
* every summary repeats the hero's transform and bounds: still identity / zero after 10 s means nothing
  positions or animates the character (upstream of the renderer: the character process or spawn).

### Skin choice (unlocks.cpp)

Runs 42-43 used `mp-hero-year-1` because the player's skin id (NetPlayerData 0x11474f8 +0x10c hero /
+0x110 villain, -1 when nothing is equipped) is not in the skin table, so 0x6144b0 takes a random
unlocked record (0x64b430); with all 107 unlocked and the same PRNG state each start, that was year-1.
Now, before 0x6144b0 (calls 0x5f2f49, 0x609551, 0x609c2b): an id that is not in the table becomes Drake
(b095d270, the PS4 record at index 57), and `UNCHARTED_U2MP_SKIN=<package>` forces a skin for both
sides (e.g. `mp-hero-navarro`, a PS4 retail file). Logged as `skins: hero id ... -> ...` and
`skins: random pick ...`.

Reading run 44: the actor references may all be the hero's own weapon and attachments; if the character
is not shown, those may read 0 as well. The 8 level references then decide: level submits > 0 = the
counter works. If every reference reads 0 too, re-run with `UNCHARTED_U2MP_RENDER_DIAG=all` (the summary
line reports the list table fill; lists past 8192 are not watched).

The skin choice is applied twice: once when our skin table is bound (unlocks_refresh, in the lobby,
before LoadLevels requests the package) and again before every 0x6144b0 message. The equip code stores
the store item id; the type-10 store items parsed from net-patch9 (7 of them; the store layout of the
converted file was not fully parsed) all have ids that are skin record ids (+0x10), so an equipped skin
is found and kept. The Drake default is written into NetPlayerData and may be saved with the profile.

### Run 44 and the run-45 gate probe

Run 44 proved the probe: reference actors (pigeon-fma, temple-cloth-banner, title-vine) and level
instances were submitted (28 first submits, actors 12,000-18,000 submits before teardown). The pigeons
have the hero's init state exactly (identity transform, zero bounds, +c0 0046, +d8 d03b), so the init
flags, bounds and transform are not what keeps the hero out. Run 43: 0 hero submits in 60 s.

How an actor's mesh gets drawn (static):
* DrawControl::AddMesh 0x4b6e20 -> 0xbd3bc0(manager 0x1248600, render object) allocates a 0x4b0-byte
  draw data (manager +0x400 array) and adds its index to the render object (+0xf8 capacity, +0xfc
  count, +0x100 indices). 0xbe48d0 builds it: flags +0x1b8 |= 0x2000 (and |= 1 for an empty prototype),
  MeshInstance at +0x1c0.
* DrawControl (game object +0x180 for a NetCharacter): +0x328 mesh count, meshes 0x50 apart, each with
  its LOD draw data list (+0x8/+0x10 stride 0x10, count +0x48) and a shown byte +0x50. Show/Hide
  (0x4b7720 / 0x4b7850, inlined in the NetCharacter update 0x605070 at 0x605aaa / 0x605c0a) clear / set
  bit 2 of every draw data's +0x1b8. The NetCharacter shows itself when +0x2f03 is set (it is set back
  to 1 every frame; 0x602a00 clears it), remembering the last state in +0x2f04.
* Per frame 0xbd4f80 walks render objects and skips those whose per-frame byte (+5 at the frame offset)
  lacks bit 4; 0xbd6d00 collects a render object's draw data, skipping flags & 7. Collected draw data
  reach the per-instance draw job (0xc42250 -> 0xc419d0) and the binding walker.
* The PS3 local spawn 0x4c9b58 and PS4 0x663280 do the same steps after the spawn (0x6b84f4 / 0x976300:
  position, 0x497e60 = the field reset PS4 inlines at 0x6635c3, 0x61fa18 -> 0x70f068 = PS4 0x9be760 on
  the handle 0x11579c8). No stripped call there.

Run 45 logs, per tracked instance (`render diag: ... draw data flags +1b8 ...; render object ...:
N collects`): the render object from AddMesh (0xbd3bc0 calls 0x4b6faf / 0x4b7163 hooked), the draw data
flags, and how often 0xbd6d00 ran for that render object (entry hooked with a trampoline). Hero with
collects > 0 but no submits = draw data flags (bit 2 = hidden: the NetCharacter's +0x2f03 / Show path);
collects == 0 = the render object is never visible (the object-level visibility of the player process).

### Run 45: the hero's draw data flags, and the fix (render.cpp section 6)

Run 45: the hero's render object is collected every frame, but its draw data flags are 0x6a00
(= 0x2000 + 0x4000 + 0x0800 + 0x0200) where every drawn actor has 0x2000.

| bit | set by | cleared by (campaign) | effect |
|---|---|---|---|
| 0x0200 | Character::Init 0x473f80 (0x474003, object +0x155 \|= 2) for every character | the active-state enters: NdGameObject 0x987840 / 0x988c00 (states 0x9ad5f0 / 0x9adbb0), the player 0x260920 (0x260c33) | the draw preparation 0xc47790 skips the draw data (0xc47db1: `test byte +0x1b9, 2`) |
| 0x0800 | the player Init 0x724b70 (0x724cf3, also 0x200 at 0x724ca3); player events 0x7edd80 (with 0x20, "hide") | the player event handler / weapon code (0xadada3: `& ~0x820`) | hide |
| 0x4000 | NetCharacter::Init 0x5fd443 / 0x45ba83 / 0xb21bf3 (object +0x154 \|= 2) | NetCharacter state enter 0x5f9750, player 0x260920 / 0x7a1d80 | spawning / dead hide |

The local hero is spawned by 0x663280 through 0x976300 (its only caller, 0x6634de; the process type sid
0x42a5be7f is the player, not NetCharacter f2c86c22). It never runs a clearing state enter, so its draw
data keep 0x200 and are dropped at 0xc47db1 before the binding walker. PS3 1.10's local spawn 0x4c9b58
makes the same calls after the spawn (0x6b84f4); the clearing belongs to the object's state machine,
whose missing transition was not located.

Fix: the call at 0x6634de is retargeted; right after 0x976300 returns the hero, 0x4000 / 0x800 / 0x200 are
cleared on every draw data of its DrawControl (object +0x180), and object +0x154 / +0x155 bit 2 with them
(the effect of the active-state enter). Logged as `render: hero spawned ...: N draw data, flags A -> B`.
`UNCHARTED_U2MP_HERO_SHOW=0` turns it off. If the log shows 0 draw data at that point (meshes added
later), or the run-45 style summary shows the bits back, the clearing has to move later (next step).
