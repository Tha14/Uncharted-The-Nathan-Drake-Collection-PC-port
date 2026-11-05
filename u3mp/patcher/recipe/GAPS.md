# Gaps: what the recipe cannot regenerate yet, and why

State of 2026-11-05 (recipe 1.1.0; banks, text1, EULA, map-colors restored 11-05). Numbers are files of the reference overlays (`manifest.tsv`,
15 541 files, 8 234 MB). "Match" means sha256-identical after a clean rebuild from the game inputs.

| family | files | MB | match | reason / what is missing |
|---|---|---|---|---|
| overlay-mp vtex1 | 882 | 124 | 882 | - |
| overlay-r3 sound1/streams | 11 980 | 379 | 11 980 | - |
| overlay-r3 sound1/banks | 128 | 167 | 128 | Converter: `tools/fixes/sound-banks` (`bnkconv.py` SBlk v4/v5 BE -> v13 LE, `convert_banks.py`, step `convert-banks`). All 124 converted banks byte-identical (verified 2026-11-05 with ffmpeg 8.1.1). MP3/PCM16 samples are re-encoded by `vagenc.py`, an exact port of the original encoder `third_party/vagenc-oct27-src/.../sound/c/vagenc.c` (supplied by the user 2026-11-05); the envelope table `adsr_env.json` + nearest-pair fallback are the original's too. Needs ffmpeg for the 95 banks with MP3 samples. |
| overlay-r3 dc1 (281) + dc2 (3), overlay-r2 dc1 (16), overlay-port dc1 (3) | 303 | 6.4 | 303 (by recipe test) | (2026-11-05) All reproducible: `convert-dc1` converts 72 whole-file conversions exactly (+69 .dci; tools/dcconv/dcconv.py, layouts learned from the PS3/PS4 pairs); `dc-recipes` rebuilds the other 161 files byte for byte (the merged modules net.bin / menu.bin / modules.bin / medals / looks / scenes / vox* / weapon-* / net-always-loaded(.dci) / ss-coop-adv-fort / the 3 dc2 part modules, overlay-r2 and overlay-port net / medals / modules / render-settings, and the 80 conversions the converter misses, with their .dci) from provenance recipes (`tools/dcconv/dc-recipes.json.gz`, 1.25 MB): copy ops over the user's PS4 retail and PS3 1.19 modules (raw, 32-bit-swapped, 16-bit-swapped, opcode-renumbered) + the retail weapon actor paks for the skin material names, the pointer slots, and 5947 literal bytes in all (short string-id fragments). Sources are sha256-checked, outputs must hit the reference sha256; a different-region game copy whose modules differ would make a recipe refuse (it then reports which source). `skel-retargets.bin` = airport-igc3. The cloud merge scripts themselves (dcsynth/dcgraft2/dcmod4/ngconv2/netfix.py) remain lost: only fragments are in the 10-27 transcripts. dctools (common/dcedit/dcsyms, originals) and every session dctools step are verified 29/29 from their backups. |
| overlay-r3 text1 | 66 | 12 | 66 | Merge rewritten 2026-11-05: `tools/fixes/text1-merge/merge_text1.py` (step `convert-text1`). Source of the PS3 strings is the disc `goty-patch.psarc` (`build/main/text1/`), not `text1.psarc`/`dlc-patch18`: 47 fixed keys replaced/added where they differ, in hash order, plus the helper message 0x4a93e6b9 "Your party does not have enough players..." appended to every `.common`. |
| overlay-r3 vtex1 (eula) | 7 | 10 | 7 | 7 pages p1..p7 (not 6 + emblem). Source: the JPEGs BCES-EN-1..7 in the user's `DATA2/USRDIR/http-cache` (matched by sha256); Pillow flip + JPEG q95, ffmpeg lanczos mips, DXT5: `eula-batch/make_eula.py` (step `eula-pages`) reproduces all 7 byte for byte with Pillow 13.0.0 + ffmpeg 8.1.1; without ffmpeg equivalent. A user without the cached pages (never online in RPCS3) cannot build them. |
| overlay-r3 pak23 | 328 | 3 864 | 1 + 195 rebuilt (+22 base, +100 equivalent; + 10 fix-step side files) | (2026-11-05, `tools/fixes/pak23-levels`, steps `convert-levels-r3`, `levels-lab`, `levels-maps-batch`; per-file `plan.tsv`) **identical**: all 63 Lab files (the 10-31 stagepipe run replayed: xl.py, mkres.py, f525859 converter), the 7 maps-batch level paks, 93 -ingame12 (f525859 ps4ingame; the 15 h017 worlds included), 12 BG paks direct (v30 option set or plain f525859), hud.pak (retail + 2 PS3 HUD textures), and 19 BG paks whose template choices / constants / texture source paks / caps / <=16 flag bytes were recovered from the installed pak (`data/overrides`; 11 more of these are among the base rows); 8 of them re-checked through the shipped job (levels.py) byte for byte; **base**: 22 (21 BG + air-multi-ingame12) equal the recorded pre-session state (backup-london, -mapcolor, -mapcolor2, -sun, -excess, -wall-emblem); the session steps then make the final (london-fade checked end to end on coop-syria-area1-hallway). **Equivalent** (100 BG, built with f525859 + v30 options, installed only when missing): paks the cloud batches 99..101m / 102 / 103 / h017c / h028 / b40 / cube1 / pf1 patched in place (appended pages; the batch scripts 99..101m are lost, the others lock on md5s of those states), the 6 Borneo water paks + 3 h018 placeholders (h018 water converter newer than f525859), 5 sky paks with other template records, and big stage paks whose search did not finish. Evidence per file: plan.tsv, scratch/pak23-recovery/ts/*.log. |
| overlay-r2 pak23 / dc1 / actor33 | 476 | 2 277 | 295 pak23 (+131 equivalent) | pak23 (step `convert-levels-r2`): identical 173 -ingame12 (f525859 or git 003b433 / 7d45c1f), 88 empty, 22 killvis (vischeat), 12 BG (ldn-train-multi-tc01..06, sky paks: git 003b433). Equivalent: 121 BG + 10 ingame12 of the 10-27 04:50 pass, made by a converter between git 003b433 and f525859 (other geometry layout: e.g. ldn-streets-multi-wide 618 vs 679/734 RAW_DATA items, caps 256); every one of them is shadowed by an overlay-r3 file of the same name, so the game never loads them. dc1/actor33: other agents. |
| overlay-port pak23 / actor33 / dc1 | 843 | 1 096 | 137 pak23 (+23 equivalent) | pak23 (step `convert-levels-port`): identical 76 -ingame12, 36 empty, 1 killvis, 24 BG (git 003b433 / 7d45c1f, first pass 10-26). Equivalent 21 BG (shadowed by r2/r3) + gompa-multi-shade-npc-ingame12 (overwritten by shade-npc-paks) + world-grave-mp-ingame12. actor33/dc1: other agents. |
| overlay-r3 actor33 | 538 | 284 | 0 | re-conversions (cloud `as1` batches) + the session rebuilds: `actor-materials/build.py` (37 paks), `hat-colour/batch_hats.py` (81 installed of 249), `airport-igc2` (2), `wall-emblem/mkemblem.py` (7), `skin-batch` (weapon skins). The build scripts are shipped; they need numpy, the PS3 actor extracts and run for hours; their outputs (`out/`) are derived data and are not shipped. Not verified in this session. |
| overlay-mp actor33 / pak23 / registry | 6 | 13 | 1 | 3 actor paks and `net.pak` (`convert_net.py` from the PS3 `net.pak` texture package: shipped, runnable, not verified), `net-ingame12.pak` = retail `cha-wide-roof-ingame12.pak` (matched). |
| pak23_ps4.txt (4) | 4 | 3 | 4 | (2026-11-05) step `registry-final` (`tools/fixes/pak23-registry`): each registry is replayed from line-provenance recipes over the user's retail pak23_ps4.txt, PS3 1.19 pak23.txt and two DLC/Lab level .txt files; the only literal lines are our own (emblem actors, lab34-*, san-multi-hunter-pak header). 4/4 sha256-identical. Runs last, so earlier registry edits do not matter. |

## Lost or unavailable tools

* ~~`dctools/common.py`, `dcedit.py`, `dcsyms.py`~~ restored 2026-11-05 (originals, see the dc1 row).
* The PS3 -> PS4 DC converter: rewritten 2026-11-05 (`tools/dcconv/dcconv.py`, 72/152 exact); every other DC file is rebuilt exactly by `dc-recipes`. The cloud merge scripts (netfix, menu, modules, ssfix) remain lost.
* Sound-bank sample encoder: solved 2026-11-05 with the original source (vagenc.c, README of
  `third_party/vagenc-oct27-src/vagenc-oct27`). The rule my searches missed: the shift comes from the open-loop
  peak with limit (7 << (12-sh)) - 1, both sh and sh-1 are quantised closed-loop with lrint, the lowest squared error
  wins (strict <), and the double-precision reconstruction is the state. MP3 data is read from sample+0x80 (not the
  header length) and decoded with `-ac 1 -ar <rate>`; ffmpeg 8.1.1 gives the same PCM as the original 6.1.1 for
  all 2084 MP3 samples. The approximate encoder of the earlier passes is kept as `vagenc_approx.py`;
  the rejected hypotheses are in `scratch/restore-bte/enc-rev/`.
* Dependencies (exact versions verified 2026-11-05): numpy 2.5.4 (Python 3.13.18) for `map-colors-lightmap`,
  `map-colors2`, `wall-emblem`, `actor-materials`, `excess-wall`, `hat-colour`, `london-fade` (and the paktools
  converters ps4actor/texcodec/bggeom); Pillow 13.0.0 (Python 3.14.10) for `eula-pages` (and airport-igc2/skin-batch
  build scripts); ffmpeg 8.1.1 (msys2 clang64 build) for `eula-pages` (byte-identical pages) and `convert-banks`
  (MP3 decode, 95 banks). Steps declare `python:numpy` / `python:PIL`; ffmpeg is found via `RECIPE_FFMPEG`, PATH or
  `C:/msys64/clang64/bin/ffmpeg.exe`. `C:\Python315` (driver default) has neither numpy nor Pillow: the patcher
  must bundle one Python with numpy + Pillow and an ffmpeg.exe.
* `apply_vo.py` (Fort co-op voice) is not idempotent: run on an overlay that already has the later `coop-voice-all`
  change it stops with "old entries changed". The driver skips it when the files already match the reference.

## Facts worth keeping

* Uncharted 2 PS3 data contributes **no file** to the overlays (0 of 15 541 match any U2 psarc or loose file:
  `BCES00757` disc, RPCS3 `BCES00757`/`BCUS98123` installs, their `dlc-map*`/`dlc-patch*` psarcs were all hashed).
  The 79 "U2 flashback" speech files come from the U3 disc's `goty-speech-english.psarc`; the Flashback-map DLC is
  `BCES01176` (= Uncharted 3 DLC Map Pack install), not Uncharted 2. The driver still requires `--ps3-u2` because the
  patcher is for owners of all three games, and `{{PS3_U2}}` is available to future steps.
* 199 "PS3" sound effects are in fact the PS4 Collection's own `u2data/build/main/sound1/streams` files (batch44).
* The 11 637 speech lines of `english-mp.psarc` are language-specific; 5 019 of them are byte-identical in
  `dutch-mp.psarc` (grunts/efforts). `--speech-langs all` copies every `<lang>-mp.psarc` the disc has into
  `speech/<lang>/` (the game picks the folder by its language setting; not part of the reference manifest).
* Region: the recipe reads nothing by title id. PS3 folders are classified by content (`pak23.psarc`,
  `dlc-patch18.psarc`, `net18.bin`, `dlc-map1.psarc`), PS4 by `eboot.bin` + `u3data/`. Only the EU GOTY disc
  (`BCES01670` 01.10 + 1.19 update) and the EU Collection (`CUSA02320`) were available to test; a US/JP disc with the
  same 1.19 update should contain the same archives (the file names inside `english-mp.psarc` etc. are not
  region-specific), but this is unverified: a user of another region can confirm by running
  `rebuild_overlays.py --only copy-ps3-vtex1,copy-ps3-speech,copy-ps3-sfx,copy-ps4-retail` and `verify_output.py`.
  The PKG (phase 2) must be built for the user's own `TITLE_ID` from `sce_sys/param.sfo` (`Inputs.ps4_title_id`).

## actor33 (2026-11-05, recipe 1.4.0)

Recovered by reconverting every manifest actor pak with the git history of the converter (github fidalgojoao/Uncharted2-Multiplayer-Dev, 10 snapshots + 3 recovered cloud variants in `tools/fixes/actor33/toolsets`), every PS3 source, retail template, mode, texture cap, env (spread/notmpl/donors), post pass (cubefix/cb3fix) and template folder, comparing sha256 (`scratch/actor-rebuild/search.py`, results `found-all.tsv`). `tools/fixes/actor33/jobs.tsv` lists every byte-identical job; step `convert-actors-r3` rebuilds them (`build_actors.py`).
* byte-identical builds: overlay-port 574 of 680, overlay-r2 25 of 33, overlay-mp 2 actor paks + net.pak of 4, overlay-r3 207 + 48 Lab boxes/art groups + 7 weapon packs (wpnmods) directly, and the session steps (actor-materials 37/37, hat-colour 249/249 with 229 installed, airport-igc2 1/1, wall-emblem-actors 8/8 + emblem-cap3) give the rest: a recipe test produced 254/254 reference r3 files from these steps alone.
* not reproduced yet (`scratch/actor-rebuild/gaps-actor33.tsv`, 192 at hand-back; the search was still running): mostly props with their own skeleton + Havok collision (fort gates, rails, vehicles, breakables), some anim packs whose clips used an older anim.py (anim-soldier-mp, anim-npc-medium-mp), a few kits with donor textures, overlay-mp mp-hero-ramses-bandana (an ps4actor state between c6b291a and de13aa1). build_actors.py never installs these (status not identical).
* Havok reflection tables (`hk2010_ps3.json`/`hk2011_ps4.json`) are rebuilt from the decrypted PS3 1.19 EBOOT and the decrypted PS4 eboot by step `actors-hk-tables` (byte-identical to the 11-01 tables); needs `RECIPE_PS3_ELF`, `RECIPE_PS4_ELF` and GNU objdump.
* skin-textures (11-01 skin-batch) was never installed: the r3 weapon paks are the 10-29 wpnmods builds (rebuilt identically).
