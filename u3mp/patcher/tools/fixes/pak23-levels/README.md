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
| `lost-batches/`, `cloud-batches/` | the two replay steps `lost-batches-r3` / `cloud-batches-r3` (to be redone on the latest converter's output) |

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
