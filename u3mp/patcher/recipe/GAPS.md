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
| overlay-r3 pak23 | 328 | 3 864 | 1 + 200 rebuilt (+36 base, +81 equivalent; + 10 fix-step side files) | (2026-11-06, recipe 1.7.0) **identical** 130 rows + **base** 36 (= a recorded pre-batch or pre-session state; `cloud-batches-r3` and the session steps make the reference) + **equivalent** 81. New: the cloud batches 102 / h017c / 103 / h017co / h028 are replayed by step `cloud-batches-r3` from their original scripts (PC inbox), their PS3 textures regenerated from the user's PS3 paks; replayed on the 66 recorded pre-batch states every pak reaches its pre-session state. 19 more rows exact this session (templates/params/texsrc/<=64 flag bytes recovered against the earliest recorded state; h018 paks = option set U3_STAGE_LEVELINFO_PS3=1). Remaining 81: per-pak table below (lost batches 99..101m / n014, Borneo water code, 8 sky paks, and 27 large paks whose search hit the 2 h limit). |
| overlay-r2 pak23 / dc1 / actor33 | 476 | 2 277 | 295 pak23 (+131 equivalent) + 33 actor33 | pak23 (step `convert-levels-r2`): identical 173 -ingame12 (f525859 or git 003b433 / 7d45c1f), 88 empty, 22 killvis (vischeat), 12 BG (ldn-train-multi-tc01..06, sky paks: git 003b433). Equivalent: 121 BG + 10 ingame12 of the 10-27 04:50 pass, made by a converter between git 003b433 and f525859 (other geometry layout: e.g. ldn-streets-multi-wide 618 vs 679/734 RAW_DATA items, caps 256); every one of them is shadowed by an overlay-r3 file of the same name, so the game never loads them. actor33 (step `convert-actors-r3`, 2026-11-06): 33 of 33 identical. dc1: other agents. |
| overlay-port pak23 / actor33 / dc1 | 843 | 1 096 | 138 pak23 (+1 base, +21 equivalent) + 680 actor33 | pak23 (step `convert-levels-port`): identical 76 -ingame12, 36 empty, 1 killvis, 24 BG (git 003b433 / 7d45c1f, first pass 10-26). 1.7.0: world-grave-mp-ingame12 identical with converter `tt1026` (the 10-26 _claude_transfer/tools ps4ingame/ps4writer/ndpak); gompa-multi-shade-npc-ingame12 = base (the backup-coop-audit state; shade-npc-paks makes the final). Equivalent: the 21 BG paks (all shadowed by r2/r3). actor33 (step `convert-actors-r3`, 2026-11-06): 680 of 680 identical (recipe test `--only actors-hk-tables,convert-actors-r3` + verify_output.py). dc1: other agents. |
| overlay-r3 actor33 | 538 | 284 | 538 (284 by convert-actors-r3 alone, the rest with the later actor steps) | (2026-11-06) step `convert-actors-r3` (`tools/fixes/actor33/build_actors.py`, jobs.tsv 1092 rows, section actor33 below): 284 byte-identical directly + 91 pre-session states (status base) that the later steps `wall-emblem-actors`, `actor-materials` (37), `hat-colour` (81 installed), `airport-igc2` (2) and `emblem-cap3` replace with the reference files; the 2026-11-06 recipe test of those steps gave 536 of 538 with the last two (pirate-armor-a-full-mp, ruby-garuda) missing, both now identical too. Needs numpy, the Havok tables (step `actors-hk-tables`) and runs about 7 minutes. |
| overlay-mp actor33 / pak23 / registry | 6 | 13 | 5 | 3 actor paks and `net.pak` byte-identical by step `convert-actors-r3` (2026-11-06; net.pak = `convert_net.py` of toolset 7d45c1f from the PS3 `net.pak` texture package), `net-ingame12.pak` = retail `cha-wide-roof-ingame12.pak` (matched); pak23_ps4.txt: row below. |
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

## actor33 (2026-11-06, all reproduced)

Recovered by reconverting every manifest actor pak with the git history of the converter (github fidalgojoao/Uncharted2-Multiplayer-Dev, 10 snapshots + recovered cloud variants in `tools/fixes/actor33/toolsets`, see `SOURCES.txt`), every PS3 source, retail template, mode, texture cap, env, post pass and template folder, comparing sha256 (`scratch/actor-rebuild/search.py`, `search2.py` (best-first), `probe_*.py`; results `scratch/actor-rebuild/found-all.tsv` + `found-2026-11-06/`). `tools/fixes/actor33/jobs.tsv` (1092 rows) lists every byte-identical job; step `convert-actors-r3` rebuilds them (`build_actors.py`).
* byte-identical builds: overlay-port 680 of 680, overlay-r2 33 of 33, overlay-mp 3 actor paks + net.pak, overlay-r3 284 directly + 91 pre-session states (status base) + Lab boxes/weapon packs, and the session steps (actor-materials, hat-colour, airport-igc2, wall-emblem-actors + emblem-cap3) give the rest of overlay-r3.
* causes found 2026-11-06 (all now reproduced): (1) the 10-27 assets-agent post pass `flatfix` (batch30-flatcolor: generated flat-color.tga VRAM_DESC 0x47 -> sRGB 0x48, +0x20 0x010f0000) on most r3 own-skeleton props -> `worker.py` post `flatfix` (+ `cubetype` = cubefix.fix_retail_types alone); (2) LOD paks without a PS3 skeleton use the base pak's (`PS4ACTOR_SKEL3`, assetbatch opt skel3=): mp-wrk-explode-tanker-lod1, t62-tank-mg-turret-lod1 -> jobs env `PS4ACTOR_SKEL3` `<source>/<name>.pak`; (3) overlay-port anim-soldier-mp / anim-npc-medium-mp: an older anim.py that writes the event key/value pointer also for empty lists -> toolset `animkv`; (4) **donor textures never worked on Windows**: ps4actor splits `PS4ACTOR_DONORS` on ':' so `D:/...` paths only resolved when the current directory was on that drive -> `worker.py` passes drive-less paths and changes to that drive (11 overlay-port hero parts then matched with donors such as mp-hero-french-hair); (5) mp-chloe-skel: kit with `skel_tmpl` mp-chloe-elena-hair (assetbatch opt skel=) -> jobs env `_skel`; (6) rename jobs: propane-tank-mp r3 = rename(propane-tank) + cb3fix (rename jobs now run post passes), all-pistols-mp / all-rifles-mp port = rename of retail teaser-yemen; (7) the rest were search-budget artefacts (wrong template/cap order). The PS3 DLC map psarcs (BCES01176 dlc-map1..4) hold no actor pak that differs from the disc/patch copies.
* the last 12 (2026-11-06 evening; were `scratch/actor-rebuild/gaps-actor33.tsv`), all byte-identical now (jobs.tsv rows at the end):
  * **no exact-match sort** (toolsets `pv1-nosort` = port-v1, `0a2-nosort` = 0a20361, each without the `users.sort(...)` line of the material loop): the earlier converter gave a template slot used by several PS3 materials the binds and name of the first one in `used_mats` order, not of the exact match. PS3 materials without an exact template material fall back onto the first template slot (the mouth of mp-hero-london-torso), so that slot binds the PS3 mouth / head textures by path (DXT1 normal -> 0x47) and takes that material's name, where the sorted converter copies the template's BC5 mouth textures. The "two textures with the same name" theory was wrong (the duplicates are only there because `tmpl_texture` caches by the .ndb path). overlay-port mp-hero-ngp-torso (disc), mp-hero-thug-e-torso (dlc-patch13 or goty-patch, same bytes), mp-hero-ramses-equip-torso (disc, `|solo`: md5 GUID for the bandolier material): pv1-nosort, mp-hero-london-torso, mp, cap 64; mp-kit-base-torso-in-skin-a-00: pv1-nosort, disc, mp-kit-base-neck-skin-a, mp, cap 256.
  * overlay-port mp-chloe-marlowe-jak-goggles, mp-chloe-marlowe-phantom-mask, mp-chloe-young-marlowe-jak-goggles: 0a2-nosort, dlc-patch13, mp-chloe-u2-legs, kit-spread (PS4ACTOR_SPREAD_MATS; mp + spread gives the same bytes), cap 128, donor mp-chloe-elena-hair (its marlowe-hair-spec-mask; the copies in mp-chloe-marlowe-hair / -young-marlowe-hair have other bytes). The drake-jak-goggles textures are the PS3 pak's own (first slot, no sort); marlowe-hair goes to the free chloe-pants slot (spread). Not a hat-colour / build_hat product.
  * overlay-port mp-hero-sullivan-part-sideburns: 0a20361, dlc-patch13, mp-hero-fat-sullivan-hair (or mp-hero-sullivan-hair), mp, cap 256, donor mp-hero-young-sullivan-hair, `|solo` (the reference has the md5 GUID of objects/characters/sullivan/mp-hair-young:default, not the retail one).
  * overlay-r3 + overlay-port ruby-garuda: the EFFECT / EFFECT_TABLE bytes are those of retail ruby-treasure-aa (= ruby-treasure-ab = emerald-treasure-aa, all at the same page offsets 0x1410 / 0x5e60); the 648 earlier variants used templates whose effect spans sit 0x10..0x60 later. 0a20361, dlc-patch13, ruby-treasure-aa, graft, cap 256; r3 + cubefix.
  * overlay-r3 + overlay-port pirate-armor-a-full-mp: 7d45c1f, disc, pirate-armor-all, mp + PS4ACTOR_SPREAD_MATS, cap 64, skeleton template pirate-torso-all (`_skel`; any retail pak with the same 124-joint table, e.g. agent-all, gives the same joint names: the reference's debugMeshJointNameLoc is 496 bytes, pirate-armor-all's own skeleton 12); r3 + cb3fix. The mp-kit-base-head-skin-b-00 material goes to the free acc-29 slot (spread, md5 GUID).
* Havok reflection tables (`hk2010_ps3.json`/`hk2011_ps4.json`) are rebuilt from the decrypted PS3 1.19 EBOOT and the decrypted PS4 eboot by step `actors-hk-tables` (byte-identical to the 11-01 tables); needs `RECIPE_PS3_ELF`, `RECIPE_PS4_ELF` and GNU objdump.
* wall-emblem-actors needs overlay-r3 pak23/ps4/air-multi.pak in the output (any state), i.e. convert-levels-r3 before it; emblem-cap3 then gives the reference hashes.
* skin-textures (11-01 skin-batch) was never installed: the r3 weapon paks are the 10-29 wpnmods builds (rebuilt identically).


## overlay-r3 level paks: per-pak status of the 100 former "equivalent" rows (2026-11-06)

"rebuilt against" = the state the template search compared with: the earliest recorded state (inbox b102 / b103 / h017co-backup /
h028-backup = before that batch; backup-* = before the first session step; final = the overlay file; .bak-jitter = the manifest's
pre-borneo-water-jitter state). Rule for every row: levels.py must reproduce that state; `cloud-batches-r3` then replays the
batches recorded after it (verified), and the session steps do the rest.

Cause classes of the remaining rows:
* **2026-11-06, recipe 1.9.0:** the 8 r3 sky paks and the 7 Borneo water paks reproduce exactly (scratch/pak23-recovery/sky/REPORT.md, water/REPORT.md). The lost batches 97-101m / n012 / n014 / n015 were recovered (lost-scripts.zip, n015-batches.zip); replaying them needs the cloud pre-batch states (work in scratch/pak23-recovery/chain/).
* **lost batches 99 (lmuv), 100 A/C + revertB (blockers), 101 / 101m (Mask3 detail / flat-white appends), n012, n014 (London probe records), n015/n015b**: ran before batch102; their scripts are lost. Seen in the diffs: instance flag byte +0x21 (own-lightmap 0x100) cleared on 25 instances of yemen-multi-stage (batch99 or 100); an appended page with flat-white bound to Mask3 (coop-airport-stage, batch101 rule "white flat where PS3 has no -detail map"; re-implemented with the batch102 appender the page is 3 152 bytes longer than batch101's, so its page layout differs); LIGHT_TABLE_1 probe records (ldn-train-multi-tunnel: 57 bytes, recorded as fix bytes, exact).
  * 2026-11-06: n015b found complete (scratch/pak23-recovery/found/n015b: 5 one-dword patches, LEVEL_INFO_1 = the PS3 pak's flags, little endian). It shows n015's rule; no pak in this table differs in LEVEL_INFO_1, so n015/n015b are already reproduced and are not a gap. The other lost scripts were written by the claude.ai cloud teammates and never reached this PC as text (scratch/pak23-recovery/lost-sources-search.md).
* **effect order / donor**: EFFECT_TABLE + EFFECT string indices differ (yemen-multi-stage, grave-multi-stage, highrise-multi-wide, air-multi-strip): the same effects from other donor paks than our mkplan picks; the search's refine step did not find them within 6..12 candidates. Size differences of 64..96 bytes (facility/fort/museum-multi-wide, fort-coop-tower-wide) are EFFECT items placed with their donor's 0x100 phase (ps4stage.py:725) = also a donor choice.
* **Borneo water**: dig-coop-*-water(-ss) and the water material in dig-coop-stage use water.fx with the PS3 dig water textures bound (dig-ship-water-01-normal, dig-algea-01 colour/mask + retail waterbump / horse-chase env maps). f525859 plans these materials without bindings and writes no VRAM (43 KB vs 677 KB); the binding code is fixer H's h018 / batch98-water work (build98.py), never on the PC or GitHub. Not reconstructible without that code.
* **sky paks (8)**: the sky template's own reflection map is kept in the ref (e.g. sky-drydock binds sky-grave-03-docks-reflection-color where we bind the PS3 flow map), other skies use sky-clouds.fx templates whose records our converter does not produce ("no candidate gives the record"): a sky conversion between 003b433 and f525859 (10-27 sky fix) that is not recorded.
* **not run**: the template search (1-3 h per large pak) did not run for 59 paks: the machine was at 100 % CPU (full build + actor agent). Method is ready: `scratch/pak23-recovery/RESUME.md`.

| pak | status 2026-11-06 | rebuilt against | remaining cause |
|---|---|---|---|
| air-multi | base | b102 | rebuilt exact 2026-11-07 (scratch/pak23-recovery/airstrip): cloud effect donors + EFFECT phases; air-multi/-shade from their pre-lost-batch state (lost-batches-r3), air-multi-strip at backup-orange (new step orange-wall, then excess-wall); end to end identical incl. the wall-emblem chain |
| air-multi-shade | base | b102 | rebuilt exact 2026-11-07 (scratch/pak23-recovery/airstrip): cloud effect donors + EFFECT phases; air-multi/-shade from their pre-lost-batch state (lost-batches-r3), air-multi-strip at backup-orange (new step orange-wall, then excess-wall); end to end identical incl. the wall-emblem chain |
| air-multi-stage | base | backup-mapcolor2 | - |
| air-multi-strip | base | backup-excess | rebuilt exact 2026-11-07 (scratch/pak23-recovery/airstrip): cloud effect donors + EFFECT phases; air-multi/-shade from their pre-lost-batch state (lost-batches-r3), air-multi-strip at backup-orange (new step orange-wall, then excess-wall); end to end identical incl. the wall-emblem chain |
| air-multi-wide | base | b103 | - |
| cave-multi | equivalent | backup-london | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| cave-multi-stage | equivalent | h028-backup | searched, no output kept |
| cha-multi | equivalent | h017co-backup | searched, no output kept |
| cha-multi-shade | equivalent | h017co-backup | searched, no output kept |
| cha-multi-stage | base | backup-mapcolor2 | - |
| coop-airport-2 | equivalent | backup-london | searched, no output kept |
| coop-airport-3a | equivalent | h028-backup | searched, no output kept |
| coop-airport-3b-low | identical | final | - |
| coop-airport-stage | equivalent | h028-backup | size+26128 / pages 11/12 (appended page) / ref-only tex 1 (flat-white.tga) / bindings differ: 1 (ceiling-b-multi:default) |
| coop-ldn-library-inner | base | backup-london | rebuilt exact 2026-11-07 (template search run 6, target = the recorded pre-lost-batch state); lost-batches-r3 replays it to the recorded end state |
| coop-ldn-station-entrance-outside | equivalent | h028-backup | searched, no output kept |
| coop-syria-area1 | equivalent | backup-london | searched, no output kept |
| coop-syria-area2 | equivalent | backup-london | searched, no output kept |
| coop-syria-area2-top | identical | final | - |
| coop-syria-turret2 | base | b103 | - |
| desert-multi | equivalent | b102 | searched, no output kept |
| dig-coop-approach | equivalent | b102 | searched, no output kept |
| dig-coop-approach-water | base | .bak-jitter | rebuilt exact 2026-11-06 (converter f525859w = f525859 + stage/water_templates.json; dig-coop-stage + override templates/bytes): the recorded pre-step state |
| dig-coop-approach-water-ss | base | .bak-jitter | rebuilt exact 2026-11-06 (converter f525859w = f525859 + stage/water_templates.json; dig-coop-stage + override templates/bytes): the recorded pre-step state |
| dig-coop-dropdown | equivalent | b102 | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| dig-coop-dropdown-water | base | .bak-jitter | rebuilt exact 2026-11-06 (converter f525859w = f525859 + stage/water_templates.json; dig-coop-stage + override templates/bytes): the recorded pre-step state |
| dig-coop-dropdown-water-ss | base | .bak-jitter | rebuilt exact 2026-11-06 (converter f525859w = f525859 + stage/water_templates.json; dig-coop-stage + override templates/bytes): the recorded pre-step state |
| dig-coop-ship | equivalent | b102 | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| dig-coop-ship-water | base | .bak-jitter | rebuilt exact 2026-11-06 (converter f525859w = f525859 + stage/water_templates.json; dig-coop-stage + override templates/bytes): the recorded pre-step state |
| dig-coop-ship-water-ss | base | .bak-jitter | rebuilt exact 2026-11-06 (converter f525859w = f525859 + stage/water_templates.json; dig-coop-stage + override templates/bytes): the recorded pre-step state |
| dig-coop-stage | base | b102 | rebuilt exact 2026-11-06 (converter f525859w = f525859 + stage/water_templates.json; dig-coop-stage + override templates/bytes): the recorded pre-step state |
| facility-multi | equivalent | b102 | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| facility-multi-stage | equivalent | h028-backup | searched, no output kept |
| facility-multi-wide | equivalent | b103 | size-64 |
| fort-coop-cistern | base | h017co-backup | template search exact 2026-11-06 (RUN 2): the recorded pre-batch state; cloud-batches-r3 and the session steps make the reference |
| fort-coop-courtyard | equivalent | h017co-backup | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| fort-coop-stage | equivalent | h028-backup | searched, no output kept |
| fort-coop-tower | equivalent | h017co-backup | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| fort-coop-tower-wide | equivalent | h028-backup | size+96 |
| fort-multi | equivalent | b102 | searched, no output kept |
| fort-multi-stage | equivalent | h028-backup | searched, no output kept |
| fort-multi-wide | equivalent | final | size-64 |
| gompa-multi | equivalent | h028-backup | searched, no output kept |
| gompa-multi-stage | equivalent | h028-backup | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| gompa-multi-wide | identical | - | - |
| grave-multi | equivalent | b103 | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| grave-multi-moving | equivalent | h028-backup | searched, no output kept |
| grave-multi-stage | equivalent | final | items differing RAW_DATA:409,VRAM_DESC:59,EFFECT:13,EFFECT_TABLE:1 |
| grave-multi-wide | equivalent | b103 | searched, no output kept |
| graveyard-drydock-anniversary | base | h017co-backup | - |
| graveyard-drydock-wide | identical | final | - |
| highrise-multi | equivalent | h028-backup | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| highrise-multi-stage | equivalent | h028-backup | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| highrise-multi-wide | equivalent | final | items differing RAW_DATA:582,VRAM_DESC:44,EFFECT:13,EFFECT_TABLE:1 |
| ldn-streets-multi | equivalent | b103 | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| ldn-streets-multi-stage | equivalent | b103 | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| ldn-streets-multi-wide | base | - | - |
| ldn-train-multi | equivalent | b103 | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| ldn-train-multi-tunnel | base | backup-london | - |
| ldn-train-multi-tunnel-2 | base | backup-london | - |
| mon-coop-church | equivalent | h017co-backup | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| mon-coop-cloister | equivalent | h017co-backup | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| mon-coop-courtyard | equivalent | h017co-backup | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| mon-coop-front | equivalent | h028-backup | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| mon-coop-stage | base | h028-backup | template search exact 2026-11-06 (RUN 2): the recorded pre-batch state; cloud-batches-r3 and the session steps make the reference |
| museum-multi | equivalent | b103 | searched, no output kept |
| museum-multi-stage | equivalent | h028-backup | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| museum-multi-wide | equivalent | h028-backup | size-80 |
| oasis-multi | equivalent | b103 | searched, no output kept |
| oasis-multi-stage | base | b103 | rebuilt exact 2026-11-07 (template search run 6, target = the recorded pre-lost-batch state); lost-batches-r3 replays it to the recorded end state |
| oasis-multi-wide | equivalent | b102 | searched, no output kept |
| old-quarter-multi | equivalent | b103 | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| old-quarter-multi-stage | base | backup-london | - |
| ruins-multi | equivalent | b102 | searched, no output kept |
| ruins-multi-stage | base | b102 | template search exact 2026-11-06 (RUN 2): the recorded pre-batch state; cloud-batches-r3 and the session steps make the reference |
| ruins-multi-wide | base | - | - |
| san-upper-multi-stage | equivalent | final | size+2379952 / ref-only tex 6 (ammobox-2-brownpaint-wood-spec-mask.tga, ammobox-3-blue-02-spec-mask.tga, detail-normal.tga, missingtexture, table-top-d-env-refeletion.dds, wt-spec-mask.tga) / template |
| sham-multi-wide | identical | - | - |
| shambhala-multi | equivalent | b103 | searched, no output kept |
| shambhala-multi-stage | base | b103 | - |
| sky-drydock | identical | final | rebuilt byte-identical 2026-11-06: recovered batch57 sky converter (method sky, lib/build_skies.py, paktools-sky) |
| sky-fort-coop-b | identical | final | rebuilt byte-identical 2026-11-06: recovered batch57 sky converter (method sky, lib/build_skies.py, paktools-sky) |
| sky-fort-multi | identical | final | rebuilt byte-identical 2026-11-06: recovered batch57 sky converter (method sky, lib/build_skies.py, paktools-sky) |
| sky-gompa-mp | identical | final | rebuilt byte-identical 2026-11-06: recovered batch57 sky converter (method sky, lib/build_skies.py, paktools-sky) |
| sky-grave-multi | identical | final | rebuilt byte-identical 2026-11-06: recovered batch57 sky converter (method sky, lib/build_skies.py, paktools-sky) |
| sky-ldn-streets-multi | identical | final | rebuilt byte-identical 2026-11-06: recovered batch57 sky converter (method sky, lib/build_skies.py, paktools-sky) |
| sky-oasis-multi | identical | final | rebuilt byte-identical 2026-11-06: recovered batch57 sky converter (method sky, lib/build_skies.py, paktools-sky) |
| sky-yemen-mp | identical | final | rebuilt byte-identical 2026-11-06: recovered batch57 sky converter (method sky, lib/build_skies.py, paktools-sky) |
| syria-multi | equivalent | b103 | searched, no output kept |
| temple-multi | equivalent | b103 | searched, no output kept |
| temple-multi-stage | base | h028-backup | - |
| train-wreck-multi | equivalent | b103 | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| train-wreck-multi-stage | base | h028-backup | template search exact 2026-11-06 (RUN 2): the recorded pre-batch state; cloud-batches-r3 and the session steps make the reference |
| village-multi | equivalent | b102 | searched, no output kept |
| village-multi-stage | equivalent | b102 | searched, no output kept |
| warzone-multi | equivalent | b102 | searched, no output kept |
| warzone-multi-stage | equivalent | b102 | searched, no output kept |
| yemen-multi | equivalent | b102 | template search hit the 2 h limit (2026-11-06, CPU capped); rerun without limit planned |
| yemen-multi-stage | base | b102 | - |
| yemen-multi-wide | base | b102 | - |

### Session steps on level paks (1.7.0)
* wall-emblem: `prep_p3.py` extracts PS3 air-multi(-ingame12).pak from the disc pak23.psarc into ./p3 and creates ./out; lib.py loads
  ps4ingame/navmesh from the f525859 tree (`scratch/pak23-levels-work/pt-f525859`) and ps4actor from the actor33 toolset 4c36373, as the
  11-01 run (gh2 paktools); `base-md5.json` (the two md5 locks = backup-wall-emblem state) is shipped. Checked on the recorded base: the
  ingame12 rebuild equals the overlay and the new air-multi.pak = backup-mapcolor state (6f8471a8). Needs convert-levels-r3 and the
  rendered actor33 job (convert-actors-r3 / actors-hk-tables) in the same output.
* map-colors2: `mk_p3idx.py` builds p3idx.pkl (identical to the session's x3.py index: 3 809 names, pak23 / goty-dlc / dlc-patch13 / dlc-patch18); `survey_ov.py` + the shipped `detect.py` regenerate ov.jsonl.gz / rows.jsonl (the session's material survey of the overlay before this step); a pak whose lightmap-flag lock fails (base not at the recorded state) is now skipped instead of aborting the step. Test: cha-multi-stage reaches the reference; air-multi-shade (still equivalent) is skipped.
* shade-npc-paks: the PS3 spawner comparison (pakcmp.py -> unshipped scratch/recovered/coop-regions/detect.py) is skipped when missing
  (verification only; source and targets are md5-checked). Needs overlay-r2 (convert-levels-r2) for its source pak.
* excess-wall: needs air-multi-strip.pak at the backup-excess state; air-multi-strip is still "equivalent" (effect donor), so the step
  stops there ("pak changed since the plan was built").

### Recipe test 2026-11-06 (1.7.0, fresh folder)
`rebuild_overlays.py --only convert-levels-port,convert-levels-r3,cloud-batches-r3,facility-collapse,wall-emblem,borneo-water-jitter,
map-colors-lightmap,borneo-grass-leaf,map-colors2,excess-wall,shade-npc-paks,london-fade,vram-gap` + `verify_output.py`
(report: test-results/verify-1.7.0-pak23-levels.tsv): overlay-r3 pak23 161 match / 87 differ / 80 not built (Lab, maps-batch and
r2-sourced files outside the selection); overlay-port pak23 139 / 21 / 0. All 15 r3 rows changed this session match the reference
end to end (13 directly, air-multi-stage and cha-multi-stage after map-colors2), and port world-grave-mp-ingame12. Expected
failures in this selection: wall-emblem (needs the rendered actor33 job of convert-actors-r3; verified separately on the recorded
base: air-multi.pak -> backup-mapcolor state, ingame12 = overlay), shade-npc-paks (needs overlay-r2), excess-wall (air-multi-strip
still equivalent). r3 run time 54 min with the machine at 100 % CPU.
