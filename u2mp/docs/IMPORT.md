# Uncharted 2 multiplayer: the PS3 -> PS4 asset import

`u2mp/patcher/u2mp_import.py` builds, on the player's PC, an overlay with every multiplayer asset the PS4
Collection lacks. It takes them from the player's own PS3 copy and converts them to the PS4 format. Nothing
game-derived is in the repo or the release. The import list (`u2mp/docs/import-list.tsv`) holds only names, sizes
and SHA-1 hashes. The import never runs the game.

Rule: **if the PS4 game has an asset (skin, map, picture, sound ...), the PS4 one is kept**. Every import-list row is
checked against the PS4 game first (file present; for music: the PS4 music settings name it; for speech: the PS4
speech archive of that language has the line).

## Running it

```
python u2mp/patcher/u2mp_import.py --ps4 "<CUSA02320 folder>" --ps3 "<NPUA80698 folder>"
       [--ps3-dlc "<RPCS3 dev_hdd0/game/BCUS98123/USRDIR>"] --out "<overlay folder>"
       [--groups A,B,C,D,E] [--speech-langs english,french,sam-spanish,brazilian] [--no-dlc-ruins]
       [--drop-navmesh] [--validate] [--force] [--only REGEX] [--ps3-elf EBOOT.elf] [--objdump PATH]
```

* Output: `<out>/u2data/build/main/...`, which holds only the converted files plus `pak22_ps4.txt` and
  `sound1/config/ps4/banks.txt` when something was added to them. The run also writes `<out>/state.json` (per
  item: status, source, output, SHA-1 of the input, converter version), `<out>/import.log` and `<out>/work/`
  (caches: extracted PS3 paks, the retail material survey, level plans; it can be deleted).
* Progress: `@U2MP {json}` lines (`phase` item/done).
* Resumable: an item whose input SHA-1, converter version (`CONV_VERSION`) and output size are unchanged is
  skipped. Failed and unsupported items are tried again on every run.
* Inputs are verified by the SHA-1 in the import list. A mismatch is reported as `missing`, unless
  `--no-verify` is given.
* Needs Python 3 with numpy for skins and levels; sound works without it. Needs ffmpeg for sound banks with MP3
  samples (searched in `u3mp/bin`, msys64, PATH). The U3 converter `u3mp/patcher/tools` must sit next to `u2mp`
  (the launcher package has both), or `U2MP_PAKTOOLS` must point at its `paktools`.
* `u2conv/validate.py` re-runs the converters on files both platforms have and compares the results with retail
  PS4 (`--validate` on the import does the same).

Item statuses in `state.json`: `ok` (converted and installed), `ps4` (the PS4 game has it), `skipped`
(not needed: language without a PS4 counterpart, music the PS4 settings do not use, group F), `missing`
(not in the given PS3 folders, or SHA-1 differs), `unsupported` (no converter for its content yet, listed
below), `failed` (a converter or check raised; see import.log).

## Files

| file | what |
|---|---|
| `patcher/u2mp_import.py` | driver: sources, PS4-first check, conversion per type, registries, state |
| `patcher/u2conv/sources.py` | PS3 archive / DLC index (precedence dlc-patch7 > 5 > 3 > 2 > patch > dlc-map2 > dlc-map1 > base), PS4 index |
| `patcher/u2conv/xvag.py` | XVAG (PS-ADPCM) PS3 -> PS4 |
| `patcher/u2conv/u2bank.py` | sound banks via the U3 SBlk converter |
| `patcher/u2conv/u2paktools.py` | loads the U3 pak converters (`u3mp/patcher/tools/paktools`) with the U2 differences |
| `patcher/u2conv/u2light.py` | LIGHT_TABLE_1, U2 layout |
| `patcher/u2conv/u2vis.py` | VIS_INFO_1 version 7 |
| `patcher/u2conv/u2actor.py` | skins and props: U2 item names on the U3 actor converter, kinds, PS4 prop twins, template choice, checks |
| `patcher/u2conv/u2actorparts.py` | U2 ARTGROUP_DATA / PACKAGE_ITEM_LOOKUP / LEVEL_PATS_1 / SOUND_BANK_TABLE, prop skeleton ids, login order |
| `patcher/u2conv/u2anim.py` | ANIM (U3 clip converter + the U2 float channels), animation-only paks |
| `patcher/u2conv/u2skel.py` | JOINT_HIERARCHY (U3 skel.py + the U2 D-block packfile) |
| `patcher/u2conv/u2actorcoll.py` | COLLISION_DATA_FOREGROUND, COLLISION_HAVOK_JOINTED, D-block packfile wrapper |
| `patcher/u2conv/u2havok.py` | Havok packfiles 6.1.0 (PS3) -> 2011.3 (PS4) on the U3 hkconvert |
| `patcher/u2conv/u2hkgen.py` | builds the Havok class tables on the player's PC (work/u2hkclasses.json) |
| `patcher/u2conv/u2twin.py` | skins built from the PS4 campaign actor (twin) + the converted MP parts; `validate` against the retail pairs |
| `patcher/u2conv/u2level.py` | level paks: stage (geometry) and data (-ingame10 / -npc) |
| `patcher/u2conv/u2coll.py` | COLLISION_DATA_HAVOK_BG version 3 (background collision) |
| `patcher/u2conv/u2water.py` | WATER_DATA version 3 |
| `patcher/u2conv/u2nav.py` | NAV_MESH_1 (U2 layout) |
| `patcher/u2conv/u2text.py` | text banks: the PS4 bank plus the PS3 strings it lacks (DLC map titles) |
| `patcher/u2conv/u2ingame.py` | region scripts and FEATURE_DB_2 of U2 data paks |
| `patcher/u2conv/registry.py` | pak22_ps4.txt / banks.txt |
| `patcher/u2conv/validate.py` | reference-pair checks |

The U3 converter files are used as they are on disk. Where U2 differs, `u2paktools` / `u2actor` either patch a
module attribute or load a private copy of the U3 source with a few asserted text edits (`_STAGE_EDITS`,
`_MKPLAN_EDITS`, `u2actor._EDITS`). If an edit point no longer matches, the import stops with an error instead of
converting differently.

## Format differences found (U2 pak22 / actor27 vs U3 pak23 / actor33)

The container is the same: 0x0a79 paks, PS3 big-endian with 32-bit pointers, PS4 little-endian with 64-bit
pointers, and the same page and fixup tables (`ndpak.py` reads both). What differs:

| area | U2 (PS3 -> PS4) | U3 | where |
|---|---|---|---|
| texture cook path (PS4) | `d:/dev/CP9/Data/U2/CookData/ND/build/texture2/` | `.../U3/...` | u2paktools |
| skin geometry items (PS3) | `FOREGROUND_SECTION` -> 0x20 LOD record {ptr prototype, ptr, 0, 2}; prototypes listed by unnamed `PM_COLLECTION_2` {1, ptr} items | `FOREGROUND_SECTION_2` -> prototype; `PM_FG_PROTOTYPES` | u2actor |
| skin streams (PS4) | also slot 12 (2nd UV, half2) and slot 4 (vertex colour, half4) | slots 0-3 | u2actor |
| skin submesh flags (PS4) | 0 on every submesh | 0x10000 except the last LOD | u2actor |
| LIGHT_TABLE_1 | header 0x40 -> 0x68: +0 ptr flags; u32 +4..+0x20 -> +8..+0x24; 7 pointer slots +0x24..+0x3c -> 8-byte slots +0x30..+0x60; records 0x3c (all words swapped), local lights 0xf0 | 0x60 -> 0xa0, record word +0x30 raw | u2light |
| VIS_INFO_1 | version 7 = U3 version 9 without the H table (header 0xa0 -> 0xd0) | version 9 | u2vis |
| level data paks | `-ingame10` | `-ingame12` | u2paktools.INGAME |
| region scripts | two {sid, ptr lambda} entries; lambda {ptr body, sid}; body {code*, consts*, hash, tag}; PS4 replaces the lambda sid, the body tag and a trailing tag word in the code with padding | one entry; lambda {code*, consts*, ...} with the code at +20 | u2ingame |
| FEATURE_DB_2 | 2 arrays (elements 0x40 and 0x34) + 2 trees; PS3 0x1c -> PS4 0x40 | 3 arrays (0x40, 0x38, 0x28) + 3 trees | u2ingame |
| stage login order (PS4) | small items, EFFECT_TABLE, TEXTURE_TABLE, LEVEL_OFFSET_1, VIS_INFO_1, GEOMETRY_1, LIGHT_TABLE_1, collision | ... GEOMETRY_1, VIS_INFO_1, ..., ATTACHED_FG_MAPPING, ..., TAG_FLOAT | u2paktools._STAGE_EDITS |
| stage extra items | no ATTACHED_FG_MAPPING, no TAG_FLOAT | both | u2paktools._STAGE_EDITS |
| level materials (PS4) | mostly their own shaders (`blend-color-bump-specular.fx`, `color-bump-diffuse.fx` ...); prime-shader is used by characters | prime-shader | u2paktools._MKPLAN_EDITS |
| NAV_MESH_1 | no 'NavM' header: header 0x80 -> 0xa0, polys 0x50 -> 0x60, portals 0x10 -> 0x18, info 0x50 -> 0x54; PS4 order header, verts, name, polys, info, bytes, portals | NavM v3 | u2nav |
| COLLISION_DATA_HAVOK_BG | version 3: Havok-6 era extended mesh (subparts of float3 vertices + u16 indices, per-triangle u8 material index, ND materials, MOPP, welding) widened to 64-bit pointers, all blocks inline in the item (header 0x70, subpart 0x40 -> 0x90 with a Havok 2011 identity transform, stridingType +1, materials {a, b} -> {a, 1.0f, b}, vertices / indices / MOPP code / welding 128-aligned, item data 128-aligned in its page) | version 6 -> Havok hkcdStaticMeshTree | u2coll |
| WATER_DATA | version 3: objects 0x70 -> 0x88, mesh vertices 40 B with their own byte orders (3 words differ from U3) | version 5, objects 0xf0 -> 0x108 | u2water |
| big paks | fixup directory entries {page, offset, count}: 'page' = 0x80000-byte chunk of the header block (more than 65536 fixups); PS4 writes one table | same | u2paktools (ndpak edit) |
| text banks | PS4 networking banks lack 233 PS3 strings, among them the DLC map titles | | u2text |
| sound banks | SBlk v4 (as U3) -> v13 | same | U3 bnkconv |
| XVAG | PS4 keeps 4 PS-ADPCM music tracks: header little-endian, version 2, fmat +8 bytes {1, channels}, word 4 = interleave, data in per-channel blocks | U3 PS4 reads PS3 XVAGs as they are | xvag.py |

Texture formats (DXT1/5 -> BC1/3, PS4 8x8 tiling, streamed low mips) and the shader / material ids work the same as in
U3. Converted paks keep their textures in the pak (no texturedict1 stream). Retail U2 PS4 paks keep only low mips
in the pak, so converted paks are larger than retail.

## Conversion per asset type

| type | how | checked against |
|---|---|---|
| streams / music (`.xvag`) | `xvag.py`: container rewrite, ADPCM frames kept, no re-encode | 146/148/149/150_JungleExplore: byte-identical with the retail interleave |
| speech (`pnk-*`, `rka-*`) | `xvag.py` -> `sound1/speech/<ps4 language>/<line>.xvag` (loose). PS3 english/french -> same; spanish -> sam-spanish; portuguese -> brazilian (NPUA80698 is the North American release); the other 6 PS3 languages have no PS4 counterpart | the format as above (the PS4 speech lines are ATRAC9, no PS-ADPCM pair) |
| music | only tracks the PS4 `dc1/ps4/music-settings.bin` names and the PS4 folder lacks: 21 (MP_Win1-3, MP_Lose1-3, 15 `*_Coop1/2/3`). The PS3 873/892/901 `*_Coop3` are on PS4 renumbered as 620/610/609 and stay PS4 | as above |
| sound banks | U3 `bnkconv` (SBlk v4 -> v13, MP3 samples re-encoded to PS-ADPCM via ffmpeg + vagenc) -> `sound1/banks/ps4/`; new names appended to `sound1/config/ps4/banks.txt` | banks both have: same header counts, different samples (retail U2 PS4 banks were rebuilt from source) |
| skins (`mp-hero-*`) | with a PS4 campaign twin: `u2twin` (below: the PS4 mesh and materials, MP parts converted). Otherwise `u2actor`: U3 `ps4actor` geometry and texture conversion; materials, effects and joint table from a retail PS4 mp-hero with the same skeleton (all MP heroes use skeleton 0x27e57f43) that shares the most material names | twin builds: the 15 retail pairs (below); the 16 retail skins converted with another template: see Validation |
| props, anims, Havok actors | `u2actor.convert_prop` (section "Props and animated actors"): props through the skin path plus skeleton, anims, collision, sound banks; animation paks and loose ragdolls on their own | the actor27 reference pairs, below |
| level background paks | `u2level` stage: mkplan (materials by name from retail U2 paks, otherwise the closest retail background material) -> fxextract -> ps4stage (+ u2light, u2vis, u2coll, u2water) -> stagecheck + U2 login order + u2coll.check_ps4 | sky/wide pairs, collision and water pairs: see Validation |
| level data paks (`-ingame10`, `-npc-ingame10`, `*-npc`) | `u2ingame` (U3 ps4ingame + U2 scripts / FEATURE_DB_2 + u2nav) | 6 retail pairs block by block; nav meshes: 4 pairs |
| text banks | `u2text`: `text1/ps4/<lang>.networking` and `.common` = the PS4 bank (from `core.psarc`) plus the ids only PS3 (newest copy) has; PS4 strings are never changed. The game reads `core.psarc`; the runtime serves the added ids (remaining.cpp, MENUS.md section 4) | |
| registry | `pak22_ps4.txt` = retail file + appended levels from the PS3 per-level `.txt` (only for levels whose package converted; package/actor/bank lines kept when the file exists on PS4 or in the overlay; the 8 MP `package-ts texturedict1/...` lines added) | |

## Props and animated actors (`u2actor` + `u2actorparts`, `u2anim`, `u2skel`, `u2actorcoll`, `u2havok`)

The 55 actors the skin path left out: Fort / DLC-map / Train Wreck props with their own skeleton, animations,
collision or sound banks, 5 animation-only paks (`anim-mp-*`, `taps-mp-*`) and `mp-hero-chloe-ponytail` (a loose
ragdoll). `u2actor.kind()` sorts a PS3 actor pak into skin / prop / anim / havok; `convert_prop()` builds it.

**PS4 first.** Every prop was searched in the PS4 game under another name (64-byte blocks of the PS3 MP pak against
the PS3 copies of all 1285 PS4 actors, then item and texture lists). Twins (`u2actor.PROP_TWINS`) are converted
with the PS4 twin as template, so the twin's material records (shader, parameters) and remastered textures are used;
mesh, skeleton, anims, collision and sound banks come from the PS3 MP pak because every MP version adds or lacks
items (LODs, the sound bank `phys-metal-kitchen`, anims):
`debris-hi-pot-a/-b/-hang-a/-hang-b/-n-lid` (debris-wz-pot-*), `multiplayer-generator(-explode)` (dig-generator*),
`mi-24-heli-mp-lod0` (mi-24-heli-lod0), `mi-24-heli-missle-mp` (mi-24-heli-missle), `mp-wrk-crash-boxcar`
(wrk-box-car-02), `mp-wrk-explode-tanker` (wrk-exploding-tanker-b). The other 44 have no PS4 twin (the facility,
highrise, museum, fort and ruins MP props and the MP animation sets are not in the PS4 game) and are converted from
PS3; their materials come from the retail actor that shares most material names / folders (usually none: the PS3
textures on the closest retail material, as for level materials).

What differs from U3 / the skin path (all found on the PS3/PS4 actor27 pairs):

| item | U2 PS3 -> PS4 | pairs, result |
|---|---|---|
| ANIM | U3 clip (ICE -> OrbisAnim, `anim.convert_clip`) and header; the float channels sit in an ANIM_RAWCHANNELS item on PS3 and inside the ANIM on PS4: {n, f32 rate, ptr entry[n]}, entry {sid, ptr D}, D = two keys {quat, scale, trans} (0x58), ptr samples, u16 {stride, ?, ?, count}, samples f32 (`u2anim`) | 11,114 ANIMs: 5,585 byte-identical, 5,529 identical apart from the 8 uninitialised bytes of the clip tail pad |
| animation paks | `anim.convert_pak` with the U2 ANIM reader and the U2 artgroup / lookup | 132 pairs: same items and sizes |
| JOINT_HIERARCHY | U3 `skel.py`; props with a ragdoll keep a Havok packfile behind the D block (PS3 D +0x4c, PS4 +0x90; wrapper {size, 1, 0...} PS3 0x10 / PS4 0x20 with the 8 magic bytes) (`u2skel`) | 949 pairs: 861 identical apart from uninitialised pad bytes, 68 ragdolls differ only in the packfile floats below, 20 not reachable (character bases with another skeleton blob variant; not needed) |
| FG skeleton | FOREGROUND_SECTION_2 +8 = PS3 FG +8, +0xc = skeleton blob +4, joint table = the JH joint sids | 937 / 944 pairs |
| COLLISION_DATA_FOREGROUND | {0x1001, n, parts}; part as U3; hk block {size, 1, trailer} + packfile (PS4 header 0x30, +0x2c = magic word); trailer 3 x {elements, count}, element records 0x40 / **0x34** (U3 0x38) (`u2actorcoll`) | 660 items: 659 byte-identical (1: an uninitialised word in the retail packfile) |
| COLLISION_HAVOK_JOINTED | {0x2001, 0, 0, 0} + the D-block wrapper + packfile | 3 pairs: same layout and sizes, packfile floats as below |
| Havok packfiles | Havok-6.1.0-r1 (PS3) -> hk_2011.3.0-r1 (PS4): U3 `hkconvert` with 6.1 / 2011 class tables (`u2hkgen`, below) and the 6.1 -> 2011 changes (`u2havok.transform_61`): motion type enum (6.1 has the stabilized types), hkMotionState dampings as hkHalf + timeFactor / gravityFactor 1.0, quality type 2 -> 1, hkaBone inline, hkaSkeletonMapperData -> hkaSkeletonMapper, hkVariant -> pointer, mesh subparts typeAndFlags / INT8 index type / transform, mesh materials, setupStabilization / ball-socket atoms; strings unaligned, local fixups in target order | 2,000 packfiles: 1,925 byte-identical; 68 ragdolls equal apart from sub-ULP floats (retail recomputed the mapper / convex-transform quaternions); 7 differ (5 a retail-rebuilt fuel-tank skeleton, 2 uninitialised words) |
| ARTGROUP_DATA | U2 layout: name block with attach lists (sid, f32, k-1 pairs), bucket table inline (U3: separate table, no name block); keys sid('geo.'+name) for FG sections, linear probing in entry order (`u2actorparts`) | 1,124 pairs: 1,109 byte-identical (pointer slots aside); 15 character bases |
| PACKAGE_ITEM_LOOKUP | {ptr name, 0, n, {sid, aa, ptr}[n] inline, name} (U3: a pointer to the entries) | as retail |
| LEVEL_PATS_1 | the PS3 u64 byte-swapped (U3 writes 0) | 400 pairs |
| SOUND_BANK_TABLE | {n, aa, ptr name[n]} + names | as retail |
| names | `Prototype0_<art>`, `<art>_Prototype0_Submesh0` ('-' -> '_') | as retail |
| login order | ARTGROUP_DATA / LEVEL_PATS_1, SOUND_BANK_TABLE, EFFECT_TABLE, TEXTURE_TABLE, GEOMETRY_1, JOINT_HIERARCHY, ANIM*, FG*, collision, PACKAGE_ITEM_LOOKUP, tags | retail props |
| not on PS4 | SKELETON_FLIPDATA, ANIM_RAWCHANNELS (in ANIM), COLLISION_DATA_BACKGROUND, the ragdoll `mesh.collision.*` PM_COLLECTION_2 (its packfile is the D-block one) | all pairs |

The artgroup / lookup / LEVEL_PATS / name layouts apply to the skins too (the earlier skins had the U3 artgroup and
lookup, which the U2 PS4 runtime reads differently; `u2twin.rekey_artgroup` finds nothing to change on them now).
actor27 items carry `ACTOR_VERSION` in state.json, so a resumed import rebuilds them once.

**Havok class tables** (U3 precedent: derived from the executables, so never shipped). `u2hkgen` builds them on the
player's PC the first time an actor needs them and writes `<out>/work/u2hkclasses.json`: Havok 6.1 layouts from the
hkClass constructor calls and hkClassMember arrays of the decrypted PS3 `EBOOT.elf` (`--ps3-elf`, default
`<PS3 USRDIR>/EBOOT.elf`; RPCS3 Utilities -> Decrypt PS3 Binaries); Havok 2011.3 layouts from the PS4
`big2-ps4_Shipping.elf` in the `--ps4` folder (fake-signed SELF unwrapped, SCE RELA relocations applied, text
disassembled with GNU objdump: `--objdump`, u3mp/bin, msys64 or PATH); the five 2011 classes whose member tables the
executable builds at run time (hkpExtendedMeshShape + triangle subpart, hkpMoppCode, hkaSkeletonMapperData,
hkpBallSocketConstraintAtom) from member declarations laid out with the C rules and checked on the player's retail PS4
packfiles; the PS4 class signatures from those packfiles. The repo holds only the generator (no offsets or sizes).
On this PC the generated tables equal the ones the validation above used, class for class (3.5 minutes, objdump
most of it). Without the EBOOT.elf, the PS4 executable or objdump the actors with collision / ragdolls are reported
`unsupported` ("Havok tables missing (...)"); the rest still convert.

Checks on the 55: every prop passes ps4check; the animation paks and the ponytail parse. Not verified in game.

## Skins from the PS4 campaign (`u2conv/u2twin.py`)

Rule: PS4 assets first. Most MP skins are, on PS3, a campaign character (`costume-hero-*`, `hero-heist`,
`hero-parka`, `lieutenant` ...) that the PS4 Collection also ships, remastered. The import builds such a skin from
the PS4 campaign actor (its "twin") and converts from PS3 only what the PS4 game lacks.

What an MP skin has that its campaign twin does not (PS3 pairs, all 49 skins; confirmed on the 15 retail PS4
pairs):

| part | MP skin | campaign twin |
|---|---|---|
| LOD0 mesh | the twin's mesh (often re-exported: other vertex order, other quantisation, sometimes other materials on some submeshes) | the mesh |
| lower LODs | `<name>-lod1/2/3`: FOREGROUND_SECTION(_2) + prototype each (PS3 also `mesh.collision.<name>-lodN` PM_COLLECTION_2, dropped on PS4 for both) | none |
| tags | 3 TAG_FLOAT `lod_distance` (6/15/20, some 5/16/20 or 6/15/30), one `vis_sphere` + `vis_joint_index` per LOD | one `vis_sphere` + `vis_joint_index` |
| ARTGROUP_DATA | two keys per LOD: `geo.<fg name>` and `mesh.collision.<fg name>` | 2 keys |
| everything else | same item types, skeleton 0x27e57f43, same effects; no other hit / collision data | |

Retail PS4 `mp-hero-X` against PS4 `costume-hero-X`: LOD0 = the campaign mesh (same submeshes, index buffers,
positions, UVs; normals/tangents within 0.0015 = half-float rounding of a separate cook; a few submeshes with 2-8
more seam vertices), material records identical (name, shader, parameters, bindings), texture descriptors identical
(the in-pak low mips differ only in tile padding).

Build (`u2twin.build`): the PS3 skin is converted by `u2actor` with the PS4 twin as template (LOD1-3, FG sections,
tags, artgroup). Then, in that pak:
* every LOD0 submesh whose PS3 blocks equal a twin submesh (same triangles and skinning, positions within 1 mm,
  UVs within 0.002, or the same triangle surface in another vertex order) gets the twin's PS4 buffers (vertex
  streams, index buffer, skinning): the remastered mesh. Other submeshes stay converted.
* every material the twin has by name gets the twin's record (and its textures; twin textures the converter
  did not carry over, e.g. `default-mask.tga/<hash>.ndb.error`, are copied with their GEOMETRY_1 texture entries).
  MP-only materials (re-textures such as chloe-heist, lazarevic, skeleton glow) stay converted from PS3.
* the artgroup keys of the FG sections are `sid('geo.' + name)` as retail and PS3 have them. The converter wrote
  `sid(name)` until the u2actor artgroup rework; `fix_artgroup` / `rekey_artgroup` (also run on skins that go the PS3
  way) re-key only when such keys are present, so they are a no-op with the current converter.
The twin's buffers go to new pages; the converter's replaced LOD0 buffers stay in the pak unreferenced (larger file).
A skin goes the PS3 way when it has no PS4 twin (`u2twin.TWINS`, names only), the twin shares no LOD0 submesh, or
the twin build fails (logged, `PS3 (<why>)`).

Validation (`python u2mp/patcher/u2conv/u2twin.py validate ...`: the 15 retail PS4 skins that have a PS4 twin,
built from their PS3 files and compared with retail):

| result | |
|---|---|
| built as twin | 15/15 (143 of 178 LOD0 submeshes from PS4) |
| swapped submesh = retail (index buffer, positions within 1 mm) | 120/143; the rest are retail's own seam splits (eddie, elena, roman, udf, chloe-variation 1-2 each, red, sullivan-winter 1) and sullivan-blue / -red (9 + 3), whose retail cook orders the vertices differently |
| materials | every twin material record identical to retail (14/14 divesuit, 18/18 eddie ...); MP-only materials converted |
| FG sections, LOD distances, tags, artgroup keys | equal to retail 15/15 |
| LOD1-3 submesh / index counts | equal to retail 13/15 (chloe-variation and pirate differ by one strip) |
| ps4check | clean 15/15 |

Import (`u2mp_import.py`, group A): a skin with a PS4 twin is built by `u2twin`, else converted from PS3 as before;
the log says which (`twin costume-hero-x: 14/15 LOD0 submeshes from PS4 (88% of triangles), 13/15 materials from
PS4` or `PS3 (<why>); template ...`). `SKIN_VERSION` in the driver redoes skins built by an older rule on resume.
Test run (2026-11-07, `--only mp-hero`, 49 skins):
* **twin 34**: mp-hero, chloe, chloe-heist, chloe-winter, dead-explorer, doughnut, elena-reporter, elena-winter, flynn,
  flynn-frosty, flynn-heist, flynn-winter, frosty, heist, jeff, lazarevic, lieutenant, nd, nd-frosty, parka,
  pirate-nd, rescuer, schafer, skeleton-1, skeleton-ghengis, skeleton-marco, sol-med-b-1a, -1a-v2, -1b, -2a, -2b,
  -3a, sullivan, year-1. 27 of them take 85-100% of the LOD0 triangles from PS4, elena-reporter 56%; mp-hero,
  year-1, frosty, elena-winter, dead-explorer, pirate-nd take 25-40% (the rest of their mesh is MP-only).
* **PS3 14** (no PS4 twin with the same mesh): infamous-cole, -cole-evil, -zeke, killzone-infantry, -sev, lazarevic-fat,
  pinkerton, resistance-hale, -hybrid, rika, u1-merc-brsr-1a, -brsr-2a, -grdr-1a, u1-shotgun-1a.
* chloe-ponytail (an attachment with Havok jointed collision, no mesh section) is converted as a prop-path actor ("Props and animated actors"). Failed 0.
* `--only` runs no longer rewrite `pak22_ps4.txt` when they include no level rows (they used to delete it).

## What the game needs to see the new content

* **Levels**: an entry in `build/main/pak22_ps4.txt` (the registry is read whole). The PS4 executable also has
  `%s/build/main/pak%d/ps4/%s.txt`, but no per-level `.txt` exists on PS4, so the import extends the registry.
  Existing PS4 entries are never changed. Retail already defines `ruins-multi`, with the DLC actor list,
  and `the-fort-multi`.
* **Actors (skins, props)**: the pak file in `actor27/ps4/` is enough when a level or the net code asks for
  it by name. The skins are listed in the PS4 `dc1/ps4/net.bin` hero table (58 names, 43 of them PS3-only). The
  dc data needs no change.
* **Sound banks**: the `.bnk` in `sound1/banks/ps4/` and its name in `sound1/config/ps4/banks.txt`.
  The executable never reads `.sbf`.
* **Music / streams**: loose files; music by the names `music-settings.bin` uses.
* **Speech**: the PS4 game mounts `sound1/speech/<lang>_ps4.psarc` and opens
  `build/main/sound1/speech/<lang>/<line>.xvag`. The import writes the lines loose at that path. Whether the
  file layer finds loose files next to the mounted archive is **not verified** (the game was not run). If it
  does not, the archive has to be rebuilt with the lines added.
* **DLC maps in menus / playlists**: the PS4 dc (`net.bin` map table and `*net-games*`, `part-*-multi`) already
  has the DLC maps and the PS4 executable lists them without any entitlement check (u2mp/docs/DLC.md). The runtime
  (`gow3_pc/src/u2mp/dlc.cpp`) hides a map pack from the custom game list and matchmaking unless the import produced
  its level paks and registry entries. Map pack 1 = facility-multi + ruins-multi-wide, map pack 2 =
  highrise-multi-2, museum-multi, museum-multi-eva.
* **DLC map titles**: missing from the PS4 networking text banks; the import writes merged banks
  (`text1/ps4/<lang>.networking`, loose: like the speech lines, **not verified** that loose files override
  `core.psarc`).
* **DLC map card images** (`art/general/net/maps/*` in `net.pak`): the PS4 `net.pak` has only the base map cards;
  the PS3 dlc-patch7 `net.pak` has the DLC ones. Not imported yet (it means rebuilding the retail `net.pak` with
  extra textures): the menus show the DLC maps without a picture.
* **DLC Ruins**: map pack 1 ships another `ruins-multi` (78 MB, with -wide / sky / npc) than the PS4 (base game)
  one under the same name. With `--ps3-dlc` the import replaces it (as a PS3 with map pack 1 did);
  `--no-dlc-ruins` keeps the PS4 one. Players of one match must use the same version.

## Validation (reference pairs, 2026-11-07, `validate.py`)

| check | result |
|---|---|
| XVAG: 4 PS-ADPCM music tracks | 4/4 byte-identical |
| LIGHT_TABLE_1: village-multi-wide, gompa-multi-wide, dig-multi, stage-multi | 4/4 byte-identical (pointers as offsets) |
| VIS_INFO_1: village-multi-wide, sham-multi-wide | 2/2 byte-identical |
| data paks: sky-village-multi, gompa/village-multi-npc, stage, treasure, dig -ingame10 | SPAWNER_GROUP, FEATURE_DB_2, SPLINE_TABLE, LEVEL_INFO_1, TAG_INT: every block matches. REGION_TREE: dig 74/74; stage 38/39 (one 0x3f/0x42 opcode); treasure 285/292 |
| 16 retail skins, each converted with another retail skin as template | ps4check clean 16/16; prototypes equal 16/16; 619 of 635 submeshes have the same index count and material. Vertex counts are 1-3% below retail, because Bluepoint split more seams. mp-hero-pirate has 36 submeshes vs 32; mp-hero-red matches 35 of 46 |
| stage paks: sky-village-multi, village-multi-wide, gompa-multi-wide | stagecheck clean, same item types and U2 login order, identical index totals; materials: gompa 8/9 by name, village 1/6 (others fallback) |
| COLLISION_DATA_HAVOK_BG: dig, stage, sham-wide, village, treasure, gompa, warzone, shambhala, ice-cave, train-wreck, sniper-towers | 20/20 items byte-identical (pointers as offsets); u2coll.check_ps4 clean on retail and converted. Also identical inside full converted paks (dig-multi, village-multi through u2level) |
| WATER_DATA: village, treasure, gompa, warzone, shambhala | 5/5 byte-identical except the material sid (set from the PS4 material the stage picks) and 4 exporter garbage words per object |
| NAV_MESH_1: gompa, village, shambhala, warzone -ingame10 | 47/52 byte-identical; 5 differ only in the last word of 0x18-byte portal records that retail leaves uninitialised |
| big paks (fixups beyond 512 KiB): village, treasure, facility, museum, highrise ... PS3 | read, ndpak.validate clean |
| 40 sound banks both have | 40/40 same structure (sound / grain / name counts). The samples differ because retail was rebuilt from source |

## Test run (the copies on this PC, `scratch/u2mp/import-test`)

Groups A-E, 3,768 rows, with `--ps3-dlc` (so the map pack 1 Ruins replaces the PS4 one). First run about 13 minutes
(bank MP3 re-encoding); the DLC main maps add 3-6 minutes each (facility-multi 327 s, ruins-multi 360 s,
highrise-multi-2 233 s, museum-multi 241 s, museum-multi-eva 180 s). Result of 2026-11-07 (run + resume):
* **ok 1,621**, ps4 8, skipped 2,139, **unsupported 0, failed 0, missing 0**; status `imported`. The overlay is 825 MB.
  * all 28 level paks of groups B-D: facility-multi, highrise-multi-2, museum-multi, museum-multi-eva, ruins-multi
    (map pack 1), their -wide / sky / -ingame10 / -npc-ingame10 paks, the Fort pieces
  * skins, props, banks, streams, music, DLC voices as before (see the actor section)
* `pak22_ps4.txt`: 19 levels appended (facility-multi, -npc, -wide, highrise-multi-2, -wide, highrise-multi-npc,
  museum-multi, -eva, -npc, -wide, ruins-multi-npc, -wide, sky-facility/fort/museum/ruins-multi, the-fort-multi-npc,
  -wide, -wide-2); the retail part is byte-identical. `banks.txt`: 6 banks appended.
* `text1/ps4/{2,3,6,7}.networking`: 233 PS3 strings added each (the 4 PS4 languages whose PS3 bank comes from
  dlc-patch7; the others have no newer PS3 bank than the base one, which adds nothing).
* ps4 8: the 4 base `*-multi-npc` paks and their -ingame10 stay PS4.

What this means for play: the five DLC maps now have every level pak, registry entry and title, and the runtime
offers them (dlc.cpp). Not tried in game. Level materials without a retail twin use the closest retail material
(facility-multi: 150 of 170), and the map cards are missing (see above).

## Known gaps (as of this version)

1. **Background collision, nav meshes, water, big fixup tables**: converted (u2coll, u2nav, u2water, ndpak edit),
   checked against retail pairs only. The PS4 game's own collision/nav loaders were not traced; u2coll.check_ps4
   checks pointers, ranges, alignment and index bounds. Retail main maps log in VIS_INFO_1 after LIGHT_TABLE_1 (wide
   paks before GEOMETRY_1); converted paks use the wide-pak order.
2. **DLC map cards** in `net.pak` are not imported (needs a rebuilt retail net.pak).
3. **Text banks and speech as loose files** over `core.psarc` / the speech archives: not verified in game.
4. **Props and animated actors**: converted (section above). Open: the 44 props without a PS4 twin get the closest
   retail material record (often none shares a name), so their shaders may differ from PS3; the ARTGROUP /
   LEVEL_PATS login order varies in retail and follows PS3. Not verified in game.
5. **Level materials without a retail twin**: these get the closest retail background material (path,
   texture roles), not their own shader. They may look different in game.
6. **Region script opcodes**: retail re-encodes some `LoadStatic` (0x3f) instructions as 0x42 and keeps
   others. The rule is unknown, so the import keeps 0x3f (1 of 2 in stage-multi; 7 of 292 blocks differ in
   treasure-multi). Spawners, splines and feature db match retail exactly.
7. **Speech as loose files**: not verified in game (see above).
8. **Sound banks** are converted from the PS3 samples. Retail U2 PS4 banks were rebuilt from source,
   so imported banks sound like the PS3 ones.
9. Not imported on purpose: group F (siege-v1/wt-v1 test levels, `lost-city-storm.bnk`, `*.networking.crypt`).
   The PS4 game does not reference them. With `--ps3-dlc` the map pack 1 `ruins-multi` replaces the PS4 (base game)
   one of the same name; `--no-dlc-ruins` keeps the PS4 one.
