# Overlay rebuild recipe (patcher phase 1)

Goal: regenerate the four game-data overlays the launcher loads over the retail PS4 game
(`work/overlay-r3` > `overlay-r2` > `overlay-port` > `overlay-mp`) from a user's own game copies, so
the patcher ships only scripts. This folder is the recipe; `../tools/` holds every script it runs.

## Inputs (what the user points the patcher at)

| input | how it is recognised (region-agnostic, content only) | what is read |
|---|---|---|
| PS4 Nathan Drake Collection folder (`--ps4`) | `eboot.bin`, `sce_sys/param.sfo`, `u3data/build/main/{pak23_ps4.txt,core.psarc,dc1,pak23,actor33,sound1}`, `u2data/build/main/sound1` | retail paks, dc1, text, `u2data`/`u3data` sound streams, `param.sfo` TITLE_ID (for the PKG of phase 2) |
| PS3 Uncharted 3 data (`--ps3-u3`, repeatable, folders are scanned 4 levels deep) | **disc**: `build/main/pak23.psarc` or `actor33.psarc`; **update** (1.19): `dlc-patch18.psarc` / `dlc-patch13.psarc`; **data2** (written by the game/server): `net18.bin`, `build/main/*-7.psarc`; **dlcmaps**: `dlc-map1..4.psarc`. `PARAM.SFO` is only used to label the folder and to reject a non-Uncharted-3 title. | see the steps |
| PS3 Uncharted 2 data (`--ps3-u2`) | `build/main/pak22.psarc` / `actor27.psarc` / `dc1.psarc` | nothing today (0 overlay files come from U2; the folder is required because the patcher is for owners of all three games) |

On this PC: PS4 `D:/2. PS Games/PS4/Uncharted - The Nathan Drake Collection [CUSA02320]`; PS3 U3 disc
`D:/2. PS Games/PS3/Uncharted 3 - ... [BCES01670]/PS3_GAME/USRDIR` (GOTY 01.10), update
`RPCS3/dev_hdd0/game/BCES01670/USRDIR` (1.19), data2 `.../BCES01670DATA2/USRDIR`, dlcmaps
`.../BCES01176/USRDIR`; PS3 U2 `D:/2. PS Games/PS3/Uncharted 2 - Among Thieves [BCES00757]/PS3_GAME/USRDIR`
(01.00; RPCS3 also has `BCES00757` 1.09 and `BCUS98123` 1.33).

## Running it

```
u3mp/bin/u3import.exe --recipe --ps4 "<PS4 folder>" --ps3-u3 "<U3 disc folder>" --ps3-u3 "<RPCS3 dev_hdd0/game>" \
                      --out "<output folder>" [--allow-missing-u2] [--speech-langs all] [--continue-on-error]
u3mp/bin/u3import.exe --verify-output "<output folder>"   # compares with manifest.tsv, per family
u3mp/bin/u3import.exe --recipe --list                     # the step table
```

The driver is C++ (`u3mp/patcher/importer`: `recipe.cpp`, the former `rebuild_overlays.py` / `recipe_lib.py`); the
launcher's import (`u3import.exe` without `--recipe`) runs the same recipe after the multiplayer image. Every script
step runs `u3fix.exe` (next to `u3import.exe`); no step needs Python.

Output: `<out>/work/overlay-*` (the overlays), `<out>/work/backup-*` (what the fix steps replaced),
`<out>/scratch/<job>` (rendered job folders), `<out>/_recipe/state.json` + `log.txt`. Re-running on the
same `--out` skips steps whose recorded outputs are unchanged and steps whose files already equal
the reference manifest, so updates are incremental. ffmpeg for the steps that say so (`--list`, `needs`).

Hidden files (1.8.0): the game loads the overlays first-wins (r3, r2, port, mp), so a level or actor pak of a
lower overlay with the same path as one in a higher overlay is never read. 740 manifest files are hidden that way
(r2 pak23 179, port pak23 85, port actor33 452, r2 actor33 24; recipe.cpp). The driver writes
them to `<out>/_recipe/shadowed.tsv` and passes it as `RECIPE_SHADOWED`; the level and actor builders (u3fix levels / actors) skip
them (a hidden pak that a visible `copy` row needs is still built), the manifest-state check ignores them, and
`u3import --verify-output` counts them as `hidden` instead of `missing` and gives the match rate over the files the game
loads. `--all-files` builds them as well (only for comparing every file with the manifest). No later step reads or
patches a hidden pak (checked 2026-11-06: the only writers of those paths are the two builders).
Worker processes: `--jobs N`, default logical cores - 2 (`RECIPE_JOBS` for the u3fix actor and sound-bank
steps). The level step uses all logical cores by default; `RECIPE_LEVELS_JOBS` (or `--jobs` on `u3fix levels`)
tunes it.
The level converter (stage, ingame, vischeat, empty, hud, windoff) is C++ in `tools/fixes/dctools` (u3fix.exe levels);
the sky rows too (dctools/ps4stage.cpp sky_convert).

## Files in this folder

| file | what |
|---|---|
| `manifest.tsv` / `manifest.json` | reference: overlay, path, size, sha256, mtime of all 15 541 overlay files (`recipe_tools make_manifest`, recipe_tools.cpp) |
| `provenance.tsv` | per file: class and source (`recipe_tools provenance`, from the hash tables below) |
| `lists/*.tsv` | copy lists of the copy steps (destination, archive, entry, size, sha256); `levels-*.txt`, `hat-colour-only.txt` name lists |
| `steps/ORDER.txt`, `steps/NNN-<id>.json` | the step registry (see `ADDING-A-FIX.md`); `make_steps.py` regenerates the initial set |
| `gather_tools.py` | tool collector (the driver and the verifier are C++: `../importer`, `u3import --recipe` / `--verify-output`) |
| `GAPS.md` | what cannot be regenerated and why |
| `retail-ps4-hashes.tsv`, `retail-ps4-psarc-entries.tsv`, `retail-ps4-u2data-sound.tsv`, `ps3-hashes/*.tsv`, `backup-hashes-*.tsv`, `transfer-hashes.tsv`, `stage-assets-hashes.tsv` | hash tables used for the provenance (names and hashes only, no data; `recipe_tools hash_tree`, `recipe_tools psarc_hash`) |
| `transcript-mining/` | how the session transcripts were mined for every overlay write (`SUMMARY.md`, `overlay-writes.tsv`, `inline-writes/`) |

## Families and provenance (reference overlays, 2026-11-03)

| family | files | MB | produced by | clean rebuild |
|---|---|---|---|---|
| overlay-mp `vtex1/ps4` | 882 | 124 | byte copies of PS3 UI textures: 710 loose `build/main/vtex1`, 134 `dlc-patch13`, 11 `dlc-patch18`, 25 `goty-patch`, 2 `lab-trench-v1-7` | 882 match |
| overlay-r3 `sound1/streams` | 11 980 | 379 | byte copies: 11 637 `english-mp.psarc`, 79 `goty-speech-english.psarc`, 65 `goty-dlc.psarc`, 199 PS4 `u2data` streams | 11 980 match |
| overlay-r3 `sound1/banks` | 128 | 167 | 4 retail copies; 124 PS3 banks converted by `sound-banks/convert_banks.py` | 128 match |
| overlay-r3 `pak23/ps4` | 328 | 3 864 | stage pipeline conversions (cloud) + 10 session fixes; rebuilt by `pak23-levels` (plan.tsv) | 1 + 200 match, 36 base (= recorded pre-batch / pre-session state; step cloud-batches-r3 replays the 10-30 cloud batches), 81 equivalent |
| overlay-r3 `actor33/ps4` | 538 | 284 | ps4actor conversions (cloud) + 5 session rebuild batches | 0 |
| overlay-r3 `dc1/ps4` | 281 | 6 | DC conversions (cloud) + 9 session edits; `skel-retargets.bin` from retail | 1 match + 72 by `convert-dc1` (tools/dcconv); the dctools edit steps reproduce every checkpoint exactly once the converted net.bin / menu.bin / modules.bin base exists (that base, the cloud "netfix" merge, is still missing) |
| overlay-r3 `text1/ps4` | 66 | 12 | PS4 text banks + goty-patch MP strings, `text1-merge/merge_text1.py` | 66 match |
| overlay-r3 `vtex1`, `dc2` | 10 | 10 | EULA renders, emblem, particle modules | 0 (2026-11-03; since then `eula-pages` 7, and `dc-recipes` / `convert-dc2-part-modules` the 3 dc2 part modules) |
| overlay-r2 (`pak23` 427, `actor33` 33, `dc1` 16) | 476 | 2 277 | second conversion pass (cloud) + shade-npc copies; pak23 by `pak23-levels` | pak23: 295 match, 131 equivalent (all shadowed by r3) |
| overlay-port (`pak23` 160, `actor33` 680, `dc1` 3) | 843 | 1 096 | first conversion pass (cloud) + shade-npc copies; pak23 by `pak23-levels` | pak23: 138 match, 1 base, 21 equivalent |
| overlay-mp `actor33`, `pak23` | 5 | 13 | `convert_net.py` + 3 actor conversions; `net-ingame12.pak` retail copy | 1 match |
| `pak23_ps4.txt` x4 | 4 | 3 | `mkregistry.py` + lab_reg / apply_maps / air_emblem / shade_npc; final state by `registry-final` (line provenance) | 4 match |
| **total** | **15 541** | **8 234** | | **12 869 match = 82.8 % of files, 495 MB = 6.0 % of bytes; 4 differ, 2 668 missing** |

Everything in the "cloud" column was converted between 2026-10-25 and 10-30 in a Claude cloud sandbox
with `u3mp/paktools`; the command lines never reached this PC (details and remedies in `GAPS.md`).
The session fixes of 2026-10-31 .. 11-03 are all shipped as steps; those that only patch an existing
converted pak run once the conversion exists.

## Step order (steps/ORDER.txt)

1. `copy-ps3-vtex1`, `copy-ps3-speech`, `copy-ps3-sfx`, `copy-ps4-retail` (byte copies, sha256-checked)
2. `convert-levels-port`, `convert-levels-r2`, `convert-levels-r3`, `convert-actors-r3`, `convert-dc1`,
   (recipe 1.17.0: the stage rows convert with the level fixes the old in-place batches made, see
   `tools/fixes/pak23-levels/README.md`; the replay steps `lost-batches-r3` and `cloud-batches-r3` are retired;
   1.18.0: a Game of the Year disc without the DLC map packs gives `gompa-multi-shade-npc-ingame12` from goty-dlc.psarc,
   whose build plan.tsv accepts as `goty_sha256` in the row's note: before, a fresh GOTY-only import failed
   `convert-levels-port` on it; 1.19.0: every stage and sky row gets the map-colors2 pass as it is built, so PS3
   normal-map blends, gloss and stripes reach every level that needs them, not only the 8 paks the old `map-colors2`
   step was locked to; that step is retired; the Airstrip steps `orange-wall` and `excess-wall`, which edited
   air-multi-strip.pak by byte offset in an older converter's output and fail on the latest one, are retired too: the
   Airstrip strip level (the take-off cutscene scenery) is to be fixed in the converter)
   `convert-dc2-part-modules` (runnable since 1.10.0: `tools/fixes/part-emitters`, the recovered batch45/batch48
   particle converter); `convert-banks`, `convert-text1` (runnable since 1.1.0)
3. `registry-pak23` (mkregistry)
4. session fixes in application order: `facility-collapse`, `lab-registration`*, `elim-pic`*,
   `classic-netgames`*, `skins-net-names`*, `maps-batch`*, `rpcn-menu-page`*, `skin-textures`, `wall-emblem`,
   `borneo-water-jitter`, `map-colors-lightmap`, `borneo-grass-leaf`, `coop-voice-fort`, `hair-key`*, `eula-pages`,
   `actor-materials`, `coop-desc-fix`, `hat-colour`, `airport-igc2`,
   `enemy-stall-option-defaults`, `emblem-cap3`, `coop-audit-option-defaults`, `shade-npc-paks`, `coop-voice-all`,
   `london-fade`, `airport-igc3`, `map-lighting`, `vram-gap`, `hud-mod-mips` (recipe 1.18.0: the weapon mod icons of the in-match HUD keep their two largest mip levels, as sharp as on PS3) (* = dctools steps: restored 2026-11-05, verified sha256-identical from their backups with `tools/dcconv/verify_dctools_chain.py`; maps-batch is now `maps-batch-dc` + `maps-batch-files`, and `eula-menu-box` follows `rpcn-menu-page`)
5. `registry-final` (pak23_ps4.txt of the four overlays, line provenance), `remove-lab` (recipe 1.15.0: no Lab maps on
   PC; deletes the Lab level / actor paks and Lab map pictures older imports have and strips the Lab blocks of
   pak23_ps4.txt; `levels-lab`, `levels-maps-batch` and `maps-batch-files` are retired, convert-actors skips the 48
   labsrc jobs, the DATA2 Lab psarcs are never read), `remove-rpcn-page` (1.16.0: no RPCN page in the game's menus;
   the launcher has the RPCN section), then `netgame-order-119` (recipe 1.14.0,
   `tools/fixes/netgame-order/netgame_order.cpp`, with_jobs dctools): the overlay net.bin netgame table and playlists
   rebuilt in the order of the player's DATA2 `net18.bin` (exactly PS3's netgames: count, order, names, maps, types;
   PS3 scalar values; PC-only netgames dropped; a PS3 map the PC lacks added with +0x94 = 1), modules.bin net size
   word by module name. Last, after every other net.bin step; those steps exit 0 on a table in this order
   (`dctools/ng119.py`: classic.py and apply_maps.py check it, the others' own guards already hold).
   Since 2026-11-12 these three steps are C++: `u3fix.exe <step>` (sources `tools/fixes/dctools/u3fix.cpp`, `dc.cpp`
   and the `.cpp` in each step's job folder), built into `u3mp/bin` by `tools/package_windows.sh`; the importer puts
   `u3mp/bin` on PATH. Their output is byte-identical to the Python versions they replaced (recipe version unchanged: no player needs a rerun).
   Since 2026-11-13 these steps are C++ too, each `u3fix.exe <name>` with its .cpp in the step's job folder: emblem-cap3,
   eula-menu-box (eula-box), orange-wall, excess-wall, enemy-stall-option-defaults (enemy-stall), coop-desc-fix
   (coop-desc), vram-gap, airport-igc3 (igc3-retargets + igc3-install), convert-text1 (text1-merge),
   coop-audit-option-defaults (coop-audit), registry-pak23 (registry-levels), registry-final, hair-key. Shared code:
   `dctools/psarc.cpp` (PSARC archives, the PS3 DC index) and `launcher/native/json.h` / `inflate.h`. Each was run
   against its Python on copies of a saved import: the same printed lines and byte-identical files. Differences, all
   on error or rerun paths: a step that finds a problem stops before writing anything (the scripts had sometimes
   written part); orange-wall and excess-wall count as applied on a rerun when their own backup is the plan's input
   (later steps rebuild the pak, so the scripts failed there); an LZMA block in a PSARC stops with a message (the
   scripts tried lzma; Uncharted 3 archives use zlib).

Per-file order where several fixes touch one file (from the transcripts): `net.bin`: netfix -> lab_reg ->
elim_pic -> classic -> skins -> apply_maps -> hair_key -> coop_desc_fix -> coop_vox -> netgame_order (rerun
`classic` after any net.bin rebuild; netgame_order last). `modules.bin`: lab_reg -> elim_pic -> skins size table ->
apply_maps -> mk_rpcn -> apply_vo -> coop_vox -> netgame_order (net size word). `air-multi.pak`: air_emblem -> mapcolor -> mapcolor2 -> london. `air-multi-strip.pak`:
excess -> london -> vram_gap. `facility-multi.pak`: collapse -> london. `ss-multiplayer/*.bin`: ssfix ->
option_defaults -> option_defaults_2. `vox*.bin`: apply_vo -> coop_vox.

## Timing (this PC, Python 3.15, NVMe)

* **Fresh import 1.8.0 (2026-11-06, through u3mp_import.py like a player, 12 threads, 10 workers): 56 min** for the
  recipe (3 337 s), about 99.3 % of the 14 801 files the game loads byte-identical (verify_output; 740 hidden files
  not built). Per step: convert-levels-r3 2 120 s (before the level fast paths: 247 paks), convert-banks 280 s (Python
  encoder; ~30x less with vagenc.dll), convert-actors-r3 153 s, convert-dc1 120 s, london-fade 111 s, hat-colour
  ~105 s, map-colors2 79 s, levels-lab 69 s, cloud-batches 42 s, port 27 s and r2 20 s (were 529 s and 6 146 s in
  the 1.7.0 full build, which built the hidden paks), every other step under 30 s.
* Level fast paths: 11 large/medium r3 paks in 43 s instead of 90 s (same machine, 10 workers), all identical.

* Clean rebuild of everything that can run: **96 s** (copy steps 13 s for 12 868 files / 495 MB; the rest is
  script steps failing fast for lack of converted bases). A second run on the same output: 37 s (copies
  verified by hash and skipped).
* Update-mode run on a copy of the reference overlays (8.2 GB): 56 s the first time (the manifest-state check
  hashes the files a step owns, up to 3.9 GB for the pak23 steps), 21 s the second time. Result: 15 537 of
  15 541 files untouched and identical; every script step reported "already applied" / "already at the
  reference state"; the 4 `pak23_ps4.txt` differ only because `registry-pak23` ran once before the
  reference-state check existed (it drops the lab34 lines that `lab-registration`/`maps-batch` add; on a
  complete overlay the step is now skipped). Test outputs were on `D:/1. PC Games/Uncharted-windows/u3-recipe-test*`
  and were deleted; the verify reports are in `test-results/`.
* Not measured (not runnable here): the conversion pipelines. From the batch timestamps the cloud passes took
  about a day each; `batch_hats.py` and `actor-materials/build.py` run for hours.

## Dependencies (recipe 1.1.0, verified 2026-11-05)

| what | version used | steps |
|---|---|---|
| Python | 3.15.1 (driver default `C:\Python315`), 3.14.10, 3.13.18 all work | all |
| numpy | 2.5.4 | `map-colors-lightmap` (verified: 14/14 paks = recorded md5_after), `map-colors2`, `wall-emblem`, `actor-materials`, `excess-wall`, `hat-colour`, `london-fade`; paktools `ps4actor`/`texcodec`/`bggeom` |
| Pillow | 13.0.0 | `eula-pages` (JPEG flip + q95 re-save is part of the reference bytes) |
| ffmpeg | 8.1.1 (msys2 clang64) | `eula-pages` (lanczos mips; 7/7 identical), `convert-banks` (MP3 decode for 95 banks; 8.1.1 and the original 6.1.1 give identical banks) |

A step whose `python:` need is missing in the driver's interpreter is skipped with a message; run the driver with a
Python that has numpy + Pillow (the patcher should bundle one). ffmpeg is looked up in `RECIPE_FFMPEG`, PATH and
`C:/msys64/clang64/bin/ffmpeg.exe`; without it `eula-pages` writes equivalent pages and `convert-banks` skips the MP3 banks.
`gather_tools.py` no longer re-gathers `map-colors`, `eula-batch`, `sound-banks`, `text1-merge` (maintained in `tools/fixes`).


## DC tools (2026-11-05)

* `tools/fixes/dctools/common.py`, `dcedit.py`, `dcsyms.py`: the original DC reader / append-only editor / symbol inserter (the project's own u3mp/dctools; the originals are kept in `tools/dctools-recovered/` with the other 20 scripts of that folder). Steps that import them name `with_jobs: ["dctools"]` (the driver renders that job too and puts it on PYTHONPATH).
* `tools/dcconv/dcconv.py`: PS3 -> PS4 DC converter. `learn` aligns PS3/PS4 pairs object by object and keeps a layout per object key; `convert` lays a PS3 module out from the learned schemas; `test` compares with reference files. `schema-oldconv.json` (shipped) holds the layouts of the 2026-10-27 cloud conversions; the retail schema is learned on the user's PC from the PS3 disc and PS4 Collection DC modules (about 10 minutes, cached in `work/_dcconv/`).
* `tools/dcconv/verify_dctools_chain.py --project <P> --test <dir on D:>`: reruns every dctools step from its backup and compares with the next checkpoint (29 checks). `make_convert_list.py` regenerates `fixes/dcconv/dc1-convert.tsv`.
* `dc-recipes` (step 085, `tools/fixes/dcconv/apply_recipes.py`): rebuilds every DC file `convert-dc1` does not reproduce - the merged modules (net.bin, menu.bin, modules.bin, medals, looks, scenes, vox*, weapon-*, net-always-loaded(.dci), ss-coop-adv-fort, dc2 part modules, overlay-r2 / overlay-port DC) and 80 ss / ai / render-settings conversions - from `tools/dcconv/dc-recipes.json.gz` (`dcrecipe.py`: copy ops over the user's PS4 retail and PS3 1.19 files, pointer slots, 5947 literal bytes in all). `make_recipes.py` regenerates the recipes from the reference overlays. With convert-dc1 and the option-default fixes this gives 302/303 dc1+dc2 files sha256-identical (+ skel-retargets.bin from airport-igc3).
