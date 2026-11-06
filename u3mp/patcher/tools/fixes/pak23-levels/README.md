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

## The cloud in-place batches of 2026-10-30 (step `cloud-batches-r3`, `cloud_batches.py`) - added 2026-11-06

The overlay-r3 BG paks were patched in place after the conversion. The batch folders were found intact in the PC inbox
(`D:/1. PC Games/Uncharted-windows/_claude_transfer/inbox/`): batch102-flatslots, h017c-bv-custom-fix, batch103-blendpolarity,
h017co, h028-spec-masks (h028sub = the same rows in chunks), each with the pak states it backed up before patching
(`b102/`, `b103/`, `h017c-backup/`, `h017co-backup/`, `h028-backup/`). `cloud-batches/<batch>/` holds the original scripts,
SWAP.tsv md5 locks and targets.json (material / slot / texture names and numbers); `cloud_batches.py` runs them in install
order (b102, h017c, b103, h017co, h028) on `work/overlay-r3`.

* The PS3 textures the batches appended (their data/*.bin) are not shipped: `data/binsrc.json` names the PS3 level pak and
  texture, the top mips dropped (h028 capped spec masks at 256) and the block size; the bytes are
  `texcodec.ps3_to_ps4_texels` of the user's PS3 texels and are md5-checked (b102 32/32, h028 54/54 identical).
  batch103 inverts the pak's own blend maps (tools/bc1inv.py), h017c/h017co write collision index words (rule: a
  degenerate (a,b,b,b) primitive becomes (b,a,b,b)); h028's scale constants (mean of the PS3 mask channel, 5 digits) are
  kept as written by the batch.
* Verified 2026-11-06 on the 66 recorded pre-batch states: every pak reaches its recorded pre-session state (backup-*,
  the manifest's .bak-leaf / .bak-jitter, or the final file).
* So a plan row of these paks is `base` with sha256 = the recorded pre-batch state (or the first session backup when no
  batch touched it); the template search (`scratch/pak23-recovery/RESUME.md`) recovers the converter choices against that
  state, not against the final file.

The batches 99 (lmuv), 100 A/C (blockers) + revertB, 101 (detail), 101m (museum), n012, n014 (London probes) and n015/n015b
(LEVEL_INFO) ran before batch102; their scripts are lost. Their effect is in the recorded pre-batch states; where the
search still differs, GAPS.md names the record (e.g. instance flag +0x21, LIGHT_TABLE_1 probe records).

Converter `tt1026`: `paktools-tt1026/` (the 10-26 `_claude_transfer/tools` ps4ingame / ps4writer / ndpak, put over the
f525859 tree; `OVERLAY_ON`) reproduces overlay-port world-grave-mp-ingame12 (REGION_TREE opcodes 0x3f, 8-byte tail).
