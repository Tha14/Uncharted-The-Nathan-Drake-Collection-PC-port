# pak23-levels: the converted level paks of overlay-port / -r2 / -r3

`u3fix.exe levels <port|r2|r3> --apply ...` (driver `levels.cpp`; recipe steps `convert-levels-port`, `-r2`, `-r3`)
builds every row of `plan.tsv` from the player's own PS3 / PS4 files with **the latest converter** (f525859, in C++ in
`tools/fixes/dctools`; the sky rows still run `paktools-sky`, the only sky converter, in Python). Since 2026-11-13 no older converter
version and no recorded per-level choice is kept (owner's decision: all assets with the latest converter).

| file | what |
|---|---|
| `plan.tsv` | one row per level pak: method, options (`mb` = map-build fix passes), PS3 source archive / pak, sha256 of the latest converter's build |
| `levels.cpp` | the driver: skip a file already at its sha256, build the rest, install only a matching build, back up a replaced file to `work/backup-pak23-levels/` |
| `lib/` | the scripts the driver still starts for the sky rows (build_skies, anim_actors) |
| `data/conv_anim_actors.json` | actor names the converter treats as animated |
| `data/overrides/overlay-r3/coop-syria-area1.json`, `-area2.json` | `gobo`: the projector-light gobo textures taken from the retail PS4 syria paks |
| `paktools-sky/` | the sky converter (Python, until it is ported) |
| `lost-batches/`, `cloud-batches/` | the two replay steps `lost-batches-r3` / `cloud-batches-r3` (to be redone on the latest converter's output) |

Methods: stage (C++ `dctools/stageplan.cpp` + `ps4stage.cpp`: mkplan -> fxextract -> ps4stage, `mb`: + paramfix + cubefix), ingame (ps4ingame), vischeat, empty (the
248-byte empty level), hud (retail hud.pak + 2 PS3 HUD textures), windoff (a retail pak with some wind strengths zeroed),
sky. Work folder: `<project>/scratch/pak23-levels-work` (PS3 extracts, ps3idx.tsv, converter trees, the mkplan survey).
