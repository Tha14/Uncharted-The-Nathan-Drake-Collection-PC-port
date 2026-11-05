# pak23-levels: the converted level paks of overlay-port / -r2 / -r3 and the Lab levels

`levels.py <family> --apply` rebuilds the level paks listed in `plan.tsv` from the user's own PS3 / PS4 files.
Recipe steps: `convert-levels-port` (port), `convert-levels-r2` (r2), `convert-levels-r3` (r3), `levels-lab` (lab),
`levels-maps-batch` (maps-batch). The registries are the separate job `pak23-registry` (step `registry-final`).

## How the parameters were recovered (2026-11-05)

Nothing of the cloud runs (plan / req / bundle json, res files, texture caps) was on this PC. Instead:

* **Converter versions.** The GitHub repo `fidalgojoao/Uncharted2-Multiplayer-Dev` (branch U3_Multiplayer) now ends at
  2026-10-27 23:45; its 9 paktools commits (10-26 13:13 .. 10-27 00:02) were extracted. The 10-31 tip `f525859` (used for
  the Lab conversion, gone from GitHub) is `tools/paktools` for 70 of 74 files; its `ndpak.py` was recovered from a full
  transcript Read (2026-11-01 07:09) and its `ps4writer.py` is git `b6eb7767` (6db6b8f..003b433), proven by the Lab
  rebuild being byte-identical. `texcodec.py` of f525859 (with `ps3_deswizzle_argb8`, `encode_bc3`, `gcm_remap`) is lost:
  the old `texcodec.py` is used, so the f525859 path fails on PS3 A8R8G8B8 textures (sky domes) and DXT remap 0xaae1.
* **`res/`** (mkplan survey): `lib/mkres.py` (the 10-31 scratchpad script, recovered from the transcript) over the retail
  pak23; it reproduces the 10-31 file (ps4tex.tsv byte size identical, 449 paks / 156 parse errors).
* **Search.** Every reference pak was converted with each version (`7d45c1f`, `003b433`, `f525859`), each PS3 source
  archive and option set, and compared by sha256 with the overlay files and with every recorded earlier state
  (block-patch md5s cube1/pf1/tt1/b40, inbox batch backups, work/backup-*, D:/u3-backups/*).
* **v30 option set** (`U3_STAGE_NO_FADE=1, U3_COLL_FILTER1=1, U3_STAGE_LEVELINFO_PS3=1`): the overlay-r3 stage paks of
  10-30 are f525859 without the fade records (added later by london-fade), with the old collision filter info (1 on every
  primitive) and with LEVEL_INFO_1 = the PS3 value. The last switch does not exist in any recorded version; it is the
  only change in `paktools-f525859/stage/ps4stage.py` (env-gated, default off). Example: coop-syria-area1-hallway =
  backup-london copy byte for byte, and with the london-fade step = the overlay file.
* **Per-level choices recovered from our own installed paks** (`data/overrides/overlay-r3/<level>.json`, made by
  `scratch/pak23-recovery/tmplsearch.py`): the cloud mkplan picked templates with scoring versions we no longer have
  (mkplan.py.pre-v2 .. pre-v23 / pre-watertex existed in the sandbox). The template of each material is identified by
  its effect hash in the installed pak and matched to the retail survey (`templates`: PS3 material -> retail pak +
  template name); then the material constants that still differ (`params`, floats), the retail pak a shared texture
  was copied from (`texsrc`), the texture caps when the sizes differ (`env`, the rebuild_bg / mapjob cap sets), and as
  a last resort at most 32 residual converter-decision bytes outside VRAM, e.g. the 0x100 own-lightmap instance flag
  (`bytes`, checked against our build before they are written). Names, numbers and flag bytes only, no game data.
  mkplan reads the first three through `U3_MKPLAN_OVERRIDE / _PARAMS / _TEXSRC` (`paktools-f525859/stage/mkplan.py`,
  env-gated, no effect when unset).
* **texcodec** (`paktools-f525859/texcodec.py`): `ps3_deswizzle_argb8`, `gcm_remap`, `encode_bc3` rebuilt; the colour
  part reproduces sky-air-multi-color bit for bit (float32 bounding-box BC1, as the 10-27 original shown in the
  assets-agent transcript); the alpha block rule is the analogous min/max fit (only all-zero alpha was checkable).
* **hud.pak** (`lib/hud_textures.py`): retail hud.pak + 2 full-resolution PS3 HUD textures (20 changed descriptor bytes).

## plan.tsv

One row per overlay pak: family, overlay, name, method, converter version, options (env `K=V`, `mb` = mapbuild's
paramfix + cubefix after ps4stage), PS3 archive (ps3index priority: dlc-patch18, dlc-patch13, dlc-map1..4, goty-patch,
goty-dlc, pak23, DATA2 *-7; `labsrc` = the Lab extraction of `lib/xl.py`), source name, sha256 the build must have,
status, reference sha256, note.

* `identical`: the build equals the reference overlay file; installed only when the sha256 matches.
* `base`: the build equals the recorded pre-session state (e.g. backup-london); the later recipe steps (london-fade,
  vram-gap, map-colors, ...) turn it into the reference.
* `equivalent`: no recorded converter state reproduces the file (the cloud converter changed between and after the
  recorded versions, and in-place batches 99..101m / 102 / 103 / h017c / h028 / tt1 / pf1 / cube1 / b40 patched the
  files); built with the nearest version, installed only when the overlay has no such file (never replaces a file),
  skipped with `--strict`.

Methods: `stage` (mkplan -> fxextract -> ps4stage), `ingame` (ps4ingame, IGNORE_OTHERS), `vischeat` (killvis),
`empty` (248-byte empty level: login page + empty VRAM table), `copy` (lab34-stage = lab-multi-14), `slice` / `havok`
(maps-batch Lab 34 pieces: slicepak.slice_pages + ps4ingame NEUTRALIZE_ARTS, collision.havok_bg_blobs).

## Files

| file | what |
|---|---|
| `levels.py` | driver (dry run by default, `--apply`, `--only`, `--strict`, `--jobs`); work folder `scratch/pak23-levels-work` |
| `plan.tsv` | the per-file plan (names, versions, options, hashes; no game data) |
| `paktools-f525859/` | f525859 files that differ from `tools/paktools`: ndpak.py, ps4writer.py, stage/ps4stage.py |
| `paktools-003b433/`, `paktools-7d45c1f/` | git snapshots (GPL v2, as tools/paktools); 7d45c1f gets f525859's stage/fxextract.py |
| `data/conv_anim_actors.json` | the 167 converted animated actor names of the 10-31 overlays (ps4ingame anim_arts) |
| `lib/` | mkres.py, ps3index.py, xl.py (Lab sources), convert_one.py, extract_one.py, empty_level.py, anim_actors.py, build_level_paks.py, slicepak.py (with patch7 slice_pages) |
