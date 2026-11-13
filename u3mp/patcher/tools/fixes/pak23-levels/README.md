# pak23-levels: the converted level paks of overlay-port / -r2 / -r3

`u3fix.exe levels <port|r2|r3> --apply ...` (driver `levels.cpp`; recipe steps `convert-levels-port`, `-r2`, `-r3`)
builds every row of `plan.tsv` from the player's own PS3 / PS4 files with **the latest converter**, all in C++
(`tools/fixes/dctools`): f525859 for every row, the sky converter's version for the sky rows. No Python runs.
Since 2026-11-13 no older converter version and no recorded per-level choice is kept (owner's decision: all assets with
the latest converter).

| file | what |
|---|---|
| `plan.tsv` | one row per level pak: method, options (`mb` = map-build fix passes), PS3 source archive / pak, sha256 of the latest converter's build |
| `levels.cpp` | the driver: skip a file already at its sha256, build the rest, install only a matching build, back up a replaced file to `work/backup-pak23-levels/` |
| `data/conv_anim_actors.json` | actor names the converter treats as animated |
| `data/water_templates.json` | the retail water.fx templates the sky planner scores PS3 water materials against |
| `data/overrides/overlay-r3/coop-syria-area1.json`, `-area2.json` | `gobo`: the projector-light gobo textures taken from the retail PS4 syria paks |

Methods (C++ files in `dctools/`):

| method | rows | what |
|---|---|---|
| stage | 321 | `stageplan.cpp` (the survey of the retail PS4 paks, the material plan, the retail material / effect / texture extraction) and `ps4stage.cpp` (the PS3 background pak as one PS4 GEOMETRY_1 with its materials, effects, textures, light table, water, collision `collision.cpp`, VIS `visinfo.cpp`); `mb`: + paramfix + cubefix |
| ingame | 355 | `ps4ingame.cpp` |
| empty | 124 | the 248-byte empty level (`ps4writer.cpp`) |
| vischeat | 23 | `vischeat.cpp` |
| sky | 8 | `ps4stage.cpp` `sky_convert` (the sky converter's version: sky materials on retail sky-clouds.fx templates, its own BC3 encoder and collision fix, the PS3 fragment-program readers in `ps3fp.cpp`) + paramfix |
| hud | 1 | the retail hud.pak with 2 full-size PS3 HUD textures (`texcodec.cpp`) |
| windoff | 1 | a retail pak with some wind strengths zeroed (`windoff.cpp`) |

Work folder: `<project>/scratch/pak23-levels-work` (PS3 extracts, ps3idx.tsv, the survey `res/`, plan files).

## Level fixes (recipe 1.17.0)

The in-place batches of 2026-10-29 / 30 (cloud-batches b102 / b103 / h017c / h017co / h028, lost-batches 97 / 98 / 99 /
100 / 101 / 101m / n012 / n014 / n015 / n015b) patched the old converter's output at recorded md5s, so none of them
could apply to the latest converter. Their rules are part of the stage conversion now (`stage::Options::fixes`, set by
levels.cpp for every stage row; the sky rows always had them):

| batch | rule in `dctools/ps4stage.cpp` / `collision.cpp` |
|---|---|
| n015 / n015b | LEVEL_INFO_1 = the PS3 level flags |
| 99, 100 C | a placeholder lightmap-uv stream (all integers) does not flag the instance as lightmapped |
| h017c / h017co | degenerate collision triangles written so Havok never reads them as CUSTOM (the Borneo co-op freeze) |
| 101 / 101m | a detail-multiply colour layer gets the PS3 '-detail' map, else neutral white (not the black flat mask) |
| b102 | '-height' / '-blend2' / '-inc' maps in BlendMap / ColorTintMask3 / IncandescenceMask (BC1 UNORM) |
| b103 (+ b104) | blend-map polarity from the PS3 fragment program (inverted copy), blend factor from another texture's channel |
| h028 | spec terms of templates without a spec-map slot scaled by the PS3 mask mean |
| 98 | water materials on the best-scoring retail water.fx template (`data/water_templates.json`); the water collision layer was already right |
| n014 | ldn-train-multi-tunnel: light probes 3 and 4 take probe 5's data (a bad PS3 bake that lit the player white) |

batch100 A / B (shadow blockers) and n012 (Museum blockers and fades) are covered by the later steps map-lighting and
london-fade and by the converter's own fade records. Checked on the player's files: 0 of the 116 paks those batches
touched still has a LEVEL_INFO or lightmap-flag error (before: 60 and 52), no detail layer is black (before: 10), flat
blend / tint / glow bindings 182 -> 80 (the rest are the cases the batches left open too).
