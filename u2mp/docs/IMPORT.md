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
       [--groups A,B,C,D,E] [--speech-langs english,french,sam-spanish,brazilian] [--dlc-ruins]
       [--drop-navmesh] [--validate] [--force] [--only REGEX]
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
| `patcher/u2conv/u2actor.py` | skins: U2 item names on the U3 actor converter, template choice, checks |
| `patcher/u2conv/u2level.py` | level paks: stage (geometry) and data (-ingame10 / -npc) |
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
| NAV_MESH_1 | no 'NavM' header; another header layout (polys 0x50 -> 0x60 as U3) | NavM v3 | not converted |
| COLLISION_DATA_HAVOK_BG | version 3, ND layout, PS4 keeps it in the same family (no Havok packfile), item 0x3e0 -> 0x20ee8 with the blocks inline | version 6 -> Havok hkcdStaticMeshTree | not converted |
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
| skins (`mp-hero-*`) | `u2actor`: U3 `ps4actor` geometry and texture conversion; materials, effects and joint table from a retail PS4 mp-hero with the same skeleton (all MP heroes use skeleton 0x27e57f43) that shares the most material names | the 16 retail skins converted with another template: see Validation |
| props, anims, Havok actors | not converted (see gaps) | |
| level background paks | `u2level` stage: mkplan (materials by name from retail U2 paks, otherwise the closest retail background material) -> fxextract -> ps4stage (+ u2light, u2vis) -> stagecheck + U2 login order | sky/wide pairs: see Validation |
| level data paks (`-ingame10`, `-npc-ingame10`, `*-npc`) | `u2ingame` (U3 ps4ingame + U2 scripts / FEATURE_DB_2) | 6 retail pairs block by block |
| registry | `pak22_ps4.txt` = retail file + appended levels from the PS3 per-level `.txt` (only for levels whose package converted; package/actor/bank lines kept when the file exists on PS4 or in the overlay; the 8 MP `package-ts texturedict1/...` lines added) | |

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
* **DLC maps in menus / playlists**: the PS4 dc (`net.bin`, `part-*-multi`) already has the DLC map
  parts. The executable still checks DLC entitlements (`GetDlcRequired`, `Fake DLC Entitlement`); that is
  runtime (C++) work, outside this import.

## Validation (reference pairs, 2026-11-07, `validate.py`)

| check | result |
|---|---|
| XVAG: 4 PS-ADPCM music tracks | 4/4 byte-identical |
| LIGHT_TABLE_1: village-multi-wide, gompa-multi-wide, dig-multi, stage-multi | 4/4 byte-identical (pointers as offsets) |
| VIS_INFO_1: village-multi-wide, sham-multi-wide | 2/2 byte-identical |
| data paks: sky-village-multi, gompa/village-multi-npc, stage, treasure, dig -ingame10 | SPAWNER_GROUP, FEATURE_DB_2, SPLINE_TABLE, LEVEL_INFO_1, TAG_INT: every block matches. REGION_TREE: dig 74/74; stage 38/39 (one 0x3f/0x42 opcode); treasure 285/292 |
| 16 retail skins, each converted with another retail skin as template | ps4check clean 16/16; prototypes equal 16/16; 619 of 635 submeshes have the same index count and material. Vertex counts are 1-3% below retail, because Bluepoint split more seams. mp-hero-pirate has 36 submeshes vs 32; mp-hero-red matches 35 of 46 |
| stage paks: sky-village-multi, village-multi-wide, gompa-multi-wide | stagecheck clean, same item types and U2 login order, identical index totals; materials: gompa 8/9 by name, village 1/6 (others fallback) |
| 40 sound banks both have | 40/40 same structure (sound / grain / name counts). The samples differ because retail was rebuilt from source |

## Test run (the copies on this PC, `scratch/u2mp/import-test`)

Groups A-E, 3,768 rows, about 13 minutes on the first run (bank MP3 re-encoding is most of it), 16 s on a resume run:
* **ok 1,554**:
  * 48 of 49 skins (all but chloe-ponytail; Pinkerton and Rika from dlc-patch2)
  * 7 banks: fort, facility, highrise, museum, museum-phys, ruins, war-wide-coop
  * 12 streams
  * 21 music tracks
  * 1,424 DLC voice lines (356 x english, french, sam-spanish, brazilian)
  * 16 level paks: sky-fort/facility/ruins/museum-multi, the-fort-multi-wide, -wide-2, facility-multi-wide, 9 data paks
  * registry inputs
* `pak22_ps4.txt`: 12 levels appended; the retail part is byte-identical. `banks.txt`: 6 banks appended.
* **ps4 10**: the PS4 copy is kept (ruins-multi and its -ingame10, the 4 base `*-multi-npc` paks and their -ingame10).
* **skipped 2,139**: speech in the 6 PS3 languages the PS4 game lacks; 3 Coop3 tracks PS4 has renumbered.
* **unsupported 65**:
  * 55 actors (gap 4): 52 Fort / DLC map props and anims, the 2 Train Wreck props, chloe-ponytail
  * 10 level paks (gaps 1-3): facility/highrise/museum main paks, museum-eva, 3 nav-mesh -ingame10, 3 wide paks with collision
* missing 0, failed 0. The overlay is 388 MB.

What this means for play: all 12 base/patch maps already work from PS4 data. The skins, music, voices and banks are
in place. The Fort's sky and wide pieces now exist, but the Fort props (gap 4) are still missing. The DLC maps
(Facility, Highrise, Museum, DLC Ruins) are **not playable yet** (gaps 1-3).

## Known gaps (as of this version)

1. **Background collision (COLLISION_DATA_HAVOK_BG v3)**: no converter. This blocks every playable map
   level: facility-multi, highrise-multi-2(-wide), museum-multi(-eva, -wide), ruins-multi(-wide) DLC, siege/wt.
   Retail pairs to work from: dig-multi, stage-multi, sham-multi-wide and every other base MP level.
2. **NAV_MESH_1 (U2 layout)**: no converter; `facility-multi-ingame10`, `highrise-multi-2-ingame10`,
   `museum-multi*-ingame10` fail (or convert without nav meshes with `--drop-navmesh`).
3. **Main map paks** (facility-multi, highrise-multi-2, museum-multi, museum-multi-eva, ruins-multi DLC,
   siege/wt worlds) also have their fixup directory outside the header page, which `ndpak` does not read.
4. **Props and animated actors** (fac-*, hi-*, mus-*, treasure-set-*, debris-*, generator, heli, Fort props,
   mp-wrk-*, anim-mp-*, taps-*, `mp-hero-chloe-ponytail`): they have their own skeleton
   (JOINT_HIERARCHY / SKELETON_FLIPDATA), ANIM, foreground/background collision or Havok jointed collision.
   None of these has a U2 converter yet. The U3 ANIM converter (`anim.py`) has not been tried on U2 clips.
5. **Level materials without a retail twin**: these get the closest retail background material (path,
   texture roles), not their own shader. They may look different in game.
6. **Region script opcodes**: retail re-encodes some `LoadStatic` (0x3f) instructions as 0x42 and keeps
   others. The rule is unknown, so the import keeps 0x3f (1 of 2 in stage-multi; 7 of 292 blocks differ in
   treasure-multi). Spawners, splines and feature db match retail exactly.
7. **Speech as loose files**: not verified in game (see above).
8. **Sound banks** are converted from the PS3 samples. Retail U2 PS4 banks were rebuilt from source,
   so imported banks sound like the PS3 ones.
9. Not imported on purpose: group F (siege-v1/wt-v1 test levels, `lost-city-storm.bnk`, `*.networking.crypt`).
   The PS4 game does not reference them. By default the PS4 base `ruins-multi` stays (PS4-first);
   `--dlc-ruins` replaces it with the map pack version, which still needs gaps 1-3.
