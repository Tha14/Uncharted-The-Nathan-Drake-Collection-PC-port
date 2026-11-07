# Uncharted 2 multiplayer asset inventory: PS4 collection vs PS3

Read-only research from 2026-11-07. Nothing was changed in any game folder and no game
was started. This file and `import-list.tsv` contain only names, counts, sizes and SHA-1 hashes.

## Sources compared

| Tag | Dump | What it holds |
|---|---|---|
| **PS4** | `CUSA02320/u2data` (Nathan Drake Collection) | U2 remaster, 4,369 files, 22.3 GB on disk. `shareddata/` has only logos and `chunks.txt`. `NATHANDRAKEPACK1/` is a licence stub (sce_sys only, no data). |
| **NPUA** | `PS3/NPUA80698/USRDIR` (PSN U2) | Base game plus `all-patches.psarc` (247.7 MB, 3,667 members), 19.1 GB |
| **BCUS** | `RPCS3/dev_hdd0/game/BCUS98123/USRDIR` (disc HDD data) | Partial base (`pak22.psarc` has 403 of 781 levels), the title updates `dlc-patch2/3/5/7.psarc` and **the two map packs `dlc-map1.psarc` and `dlc-map2.psarc`**, plus `promo1/*.edat` licences |

Notes:
- `PS3/Uncharted 2 - Among Thieves [BCES00757]` is a full EU disc copy (23.6 GB, extra speech languages). It was not compared, because it has the same base archives as NPUA.
- The NPUA copy does **not** contain the map packs. They exist only in the BCUS RPCS3 folder, so a player needs DLC that they own (`dlc-map1`, `dlc-map2`) for those maps.
- PS3 sizes below are uncompressed member sizes. PS4 sizes are file sizes. Both platforms use pak magic `0x0a79`, the same as U3, but PS3 files are big-endian (`00 00 0a 79`) and PS4 files are little-endian (`79 0a 00 00`). Every PS3 pak therefore needs the PS3 to PS4 pak conversion from `u3mp/docs/PAK-AND-MESH-FORMATS.md`, not a plain copy.

## 1. Directory structure

### PS4 `u2data/build/main` (platform files are in `*/ps4/` subfolders)

| Folder | Files | Size | Notes |
|---|---:|---:|---|
| pak22 (levels) | 780 | 5,468 MB | plus `pak22_ps4.txt` level registry (446 levels) |
| texturedict1 | 9 | 8,276 MB | streamed textures (`chunk-2x-tstrm*.pak`, `common-tstrm*`). This folder is new on PS4, and PS4 level paks are smaller because their textures moved here. |
| actor27 | 1,285 | 1,360 MB | |
| movie1 | 51 | 3,333 MB | `.bsf` replaces PS3 `.m2v`/`.divx` |
| sound1/music | 417 | 1,397 MB | loose `.xvag`. Tracks were renumbered, so PS4 `046_GenMoods_MOOD11` is PS3 `185_genmoods_mood11` |
| sound1/speech | 4 psarc | 1,553 MB | english/french/brazilian/sam-spanish `_ps4.psarc`, 17,114 lines each |
| sound1/banks | 362 | 101 MB | 181 `.bnk` + 181 `.sbf` |
| sound1/streams | 356 | 237 MB | |
| cubemap | 708 | 354 MB | new on PS4 |
| dc1 / dc2 | 113 / 68 | 18.5 / 12.5 MB | the PS3 `dc1/part-*.bin` files moved to `dc2/part-module/` |
| bpdata1 | 208 | 2.5 MB | `.json`, new on PS4 |
| core.psarc | 89 | 31 MB | `text1/ps4/*` (23 languages × common/networking/subtitles), LUTs, photo mode |
| bin.psarc | 640 | 13.5 MB | shaders and `boot1/` |
| uiextra.psarc | 240 | 106 MB | `gallery1/` |
| `../data.psarc` | 46 | 13 MB | fonts, post-processing textures, credits |

### PS3 NPUA80698 `build/main` (member counts, uncompressed)

| Archive | Members | Size | Notes |
|---|---:|---:|---|
| pak22.psarc | 781 | 7,800 MB | `pak22.txt` registry (407 levels) |
| actor27.psarc | 1,311 | 2,857 MB | |
| dc1.psarc | 183 | 26.5 MB | |
| text1.psarc | 51 | 4.4 MB | 17 languages × common/networking/subtitles |
| sound1/banks.psarc | 182 | 103 MB | |
| sound1/music.psarc | 446 | 2,222 MB | |
| sound1/streams.psarc | 357 | 800 MB | |
| sound1/speech/english.psarc | 17,127 | 1,446 MB | |
| movie1 (loose) | 59 | 8,495 MB | `.m2v` cutscenes and `.divx` bonus videos |
| gallery1, boot1, misc1, data/fonts | 240 / 32 / 1 / 8 | 123 MB | |
| all-patches.psarc | 3,667 | 416 MB | 37 actors (MP skins and Fort), 17 pak22 files (The Fort, `net.pak`, particle), 3,569 speech files (9 languages), 34 text, fonts, patched `pak22.txt` |

### BCUS98123 DLC and title-update archives (members)

| Archive | Members | Size | Content |
|---|---:|---:|---|
| dlc-map1.psarc | 49 | 233 MB | `facility-multi` (+wide/sky/npc), DLC `ruins-multi` (78 MB vs 33 MB base) + wide/sky/npc, 22 actors (`fac-*`, `mp-ruins-*`, plank bridge), banks, 4 streams |
| dlc-map2.psarc | 81 | 359 MB | `highrise-multi-2` (+wide/npc), `museum-multi`, `museum-multi-eva` (+wide/sky/npc), a reshipped `ice-cave-multi`, updated `*-multi-npc` for 7 maps, 28 actors (`hi-*`, `mus-*`, `treasure-set-*`, generator, mp heli) |
| dlc-patch2.psarc | 49 | 142 MB | The Fort (same as NPUA patch), `mp-hero-pinkerton`, `mp-hero-rika`, dc updates (`net.bin`, `net-beta.bin`, `ss-multiplayer`) |
| dlc-patch3.psarc | 1 | 0.1 MB | `menu.bin` |
| dlc-patch5.psarc | 72 | 128 MB | dc parts for facility/highrise/museum/ruins, `part-multi-character`, `hud.bnk`, text |
| dlc-patch7.psarc | 80 | 286 MB | last update: all `part-*-multi` dc, `net.bin`, `net.pak`, levels `siege-v1-*` and `wt-v1-*`, two "remake-menu" music tracks |
| sound1/speech/*.psarc (8) + speech-update | 356 each | ~2–3 MB each | only `pnk-*` and `rka-*` lines (Pinkerton and Rika DLC characters) |

## 2. Multiplayer content found

**Maps.** The MP strings in `text1/*.networking` name: The Plaza, The Village, The Sanctuary, The Ice Cave, Nepal Warzone, The Lost City, The Train Wreck, The Temple, The Stage, Fort, Ruins and Dig. The `net.bin` playlist pak names are:

| Playlist pak | Level paks | Source | On PS4? |
|---|---|---|---|
| warzone-multi-pak | warzone-multi, sky-multi-warzone, warzone-multi-npc | base | yes |
| village-multi-pak | village-multi, -wide, -tank, -npc, sky-village-multi | base | yes |
| gompa-multi-pak | gompa-multi, -wide, -sky, -npc | base | yes |
| ice-cave-multi-pak | ice-cave-multi | base (and reshipped in dlc-map2) | yes |
| shambhala-multi-pak | shambhala-multi, sham-multi-wide, -npc | base | yes |
| train-wreck-multi-pak | train-wreck-multi, -tdm | base | yes, except 2 actors (see below) |
| treasure-multi-pak | treasure-multi | base | yes |
| stage-multi-pak | stage-multi, stage-multi-pak | base | yes |
| (other) | dig-multi, sniper-towers-multi, world-multiplayer | base | yes |
| co-op | 64 `*-coop` levels (war-*, village-*, gompa-*) plus war/village/gompa-wide-coop | base | yes, except `war-wide-coop.bnk` and 6 co-op skins |
| the-fort-multi-pak | the-fort-multi | patch | **main pak only**. Missing: -wide, -wide-2, sky-fort-multi, -npc-ingame10, 7 actors, bank, 3 streams |
| ruins-multi-pak | ruins-multi (DLC version), -wide, sky-ruins-multi, -npc | dlc-map1 | base-version pak only. PS4 registry already lists the DLC actors, which PS4 does not have |
| facility-multi-pak | facility-multi, -wide, sky-facility-multi, -npc | dlc-map1 | **no** |
| highrise-multi-2-pak | highrise-multi-2, -wide, -npc | dlc-map2 | **no** |
| museum-multi-pak | museum-multi, -eva, -wide, sky-museum-multi, -npc | dlc-map2 | **no** |
| (siege/wt) | siege-v1-*, wt-v1-* | dlc-patch7 only | no. The PS4 `net.bin` does not list them |

**Level registry.** `u2_levels.py check` reports that every base MP and co-op level listed in `pak22_ps4.txt` has its packages on PS4. The only missing PS4 dependencies are the ones in the table above. There are 21 in total: Fort pieces, ruins DLC actors and bank, `mp-wrk-crash-boxcar`, `mp-wrk-explode-tanker`, `war-wide-coop.bnk`, and 6 co-op skins. Two `-ingame10` paks are missing on both platforms (`sniper-towers-multi`, `train-wreck-multi-tdm`), so this is harmless.

**Characters and skins (`actor27/mp-hero-*.pak`).** The PS3 `net.bin` (patch 7) lists 66 skins and the PS4 `net.bin` lists 58. PS4 ships only 15 of them, all re-encoded: chloe-variation, divesuit, eddie, elena, elena-divesuit, merc, navarro, pirate, red, roman, skeleton-1-glow, skeleton-1-platinum, sullivan-blue/red/winter, udf. **43 skins listed in the PS4 `net.bin` are PS3-only**, including the default `mp-hero` (Drake). Six of the missing skins are also loaded by co-op wide levels: chloe, chloe-ponytail, chloe-winter, elena-winter, rescuer and sullivan. Three names in the PS3 `net.bin` exist in no dump: desert, sir-francis and skeleton-1-redz. The PS3 `net.bin` lists 8 names that the PS4 one drops: desert, flynn-frosty, frosty, lazarevic-fat, nd, pirate-nd, sir-francis and skeleton-1-redz. `nd-frosty` and `pirate-nd` are still imported for completeness.

**dc scripts.** PS4 has every MP dc file in PS4 format: `net.bin` (100 KB vs PS3 140–200 KB), `ss-multiplayer`, `vox-mp`, `anim-net-player`, `hit-reactions-coop`, `multiplayer-wet-characters`, `menu.bin`, `store.bin`, `medals.bin`, `unlockables.bin`, and all `part-*-multi.bin` / `part-*-coop.bin`. That includes **the DLC maps** (`part-facility-multi`, `part-highrise-multi`, `part-museum-multi`, `part-the-fort-multi`, `part-ruins-multi`) and `part-multi-character`. Missing on PS4: `net-beta.bin`, plus SP-only `time-trials`, `grenade-director` and `phys-sounds`. The older duplicate `part-train-wreck-multi.bin` is also missing; `part-trainwreck-multi` is present.

**Menus and UI.** The PS4 `dc1/menu.bin` still has the full MP page set: `PAGE_MULTIPLAYER`, `PAGE_LOBBY`, `PAGE_MULTIPLAYER_MATCHMAKING`, JOIN/CONNECTING/LOADING, SCOREBOARD, RESULTS/ROUND_RESULTS, STORE/STORE_ITEMS, FRIENDS_LIST, MUTE_PLAYERS, CINEMA_LIST, `MENU_TITLE_PARTYUP`, `PAGE_MENU_LEADERBOARD`, `PAGE_MULTIMEDALS` (148 matches vs 140 on PS3).

**Text.** PS4 `core.psarc:text1/ps4/N.networking` exists for 23 languages and contains all mode and map names (Plunder, Gold Rush, Elimination, Deathmatch, King of the Hill, Turf War, map names above). Nothing needs importing. The PS3 patch-7 `*.networking.crypt` (4 languages) is optional.

**Sound.**

| Item | Status on PS4 |
|---|---|
| MP banks | present, except fort-multi, ruins-multi, war-wide-coop, facility-multi, highrise-multi, museum-multi and museum-multi-phys |
| Streams | 12 Fort and DLC streams missing |
| Music | 24 MP/co-op tracks missing (`mp_win1-3`, `mp_lose1-3`, 18 `*_coop1-3` tracks, 60 MB). By stem, PS3 has 57 tracks PS4 lacks and PS4 has 28 renamed or new ones |
| Speech | PS4 has every base line, including MP vox. Missing: the DLC character voices `pnk-*` (217) and `rka-*` (139), 2.3 MB per language |

## 3. What the PS4 `big2-ps4_Shipping.elf` still expects

- **DLC loading is still in the code:** `%s/dlc-map%d.psarc`, `MountDLCArchives`, `g_NumDLCArchives < kMaxDLCArchives`, `GetDlcRequired`, `Fake DLC Entitlement`, `ndlib\net\entitlements.cpp`, and the PS3 DLC ID `NPUA70051`. PS3 used `build/main/dlc-map%i.psarc`, `all-dlc.psarc` and `promo1/map-dlc%i.edat`.
- **File paths with no matching file in the PS4 data:** `build/main/patch5-speech-whitelist.txt`, `build/main/sid1/sidbase.bin`, `dc%u/ss-audio-precache.txt`, `%s/dlc-map%d.psarc`. The first three are debug or optional.
- **Network code is reduced.** PS4 keeps the gameplay-side net layer: `NetGameManager` (coop id, teams, spawners), `NetEvent*` (PlayerDeath, PlayVox, EmitProjectileBullet, StartNpcFight), `AddNetPickup`, `AddOrUpdateNetPlayer`, `GetRandomNetGameIndexPair`, 29 `DcNet*` script functions (same set as PS3), and the debug options "Force Matchmaking Min Players" and "Force Show Matchmaking Vote". The session layer is **missing from the string table**: the 21 `NET_SM_*` state names (START_MATCHMAKING, SERVER_LOBBY, CLIENT_JOIN_GAME_WAIT, VOTE_SCREEN, PARTY_*...), `NetPlayerData` (TUS/HDD stats), `Callback_Multiplayer`, `SceNpMatching2`, `cellVoice` and leaderboard calls. Net-ish strings: 326 shared, 482 PS3-only, 144 PS4-only. This is a Shipping build, so log strings may have been compiled out, but the missing `NET_SM_*` enum names suggest the PS3 lobby state machine was not ported.
- Menu pages are data-driven from `menu.bin`. The PS4 executable contains only 7 `PAGE_*` literals and the PS3 one contains none, so the page set comes from dc data, which PS4 has.

## 4. Minimal import list (PS3 → PS4)

The full table with SHA-1 for every file is `u2mp/docs/import-list.tsv` (3,785 rows). Sizes are PS3 uncompressed. Every pak needs PS3 to PS4 conversion.

| Group | Files | Size | Source the player needs |
|---|---:|---:|---|
| A. MP skins (`mp-hero*`, incl. default Drake) | 49 | 333.8 MB | base `actor27.psarc` + patch / dlc-patch2 (Pinkerton, Rika) |
| B. The Fort completion (wide, wide-2, sky, npc-ingame10, 7 actors, bank, 3 streams, registry fragments) | 20 | 23.2 MB | `all-patches.psarc` or `dlc-patch2.psarc` |
| C. Map pack 1: Facility + DLC Ruins | 38 | 227.8 MB | `dlc-map1.psarc` (owned DLC) |
| D. Map pack 2: Highrise, Museum, Museum-EVA, npc updates | 73 | 271.5 MB | `dlc-map2.psarc` (owned DLC) |
| E. Base MP/co-op extras: mp-wrk props, war-wide-coop and ruins banks, 24 music tracks, pnk/rka voices | 3,588 (English only: 588) | 94.7 MB (English only: 72.6 MB) | base + patch |
| **Required total** | **3,768** | **950.9 MB** (928.9 MB with English voices only) | |
| F. Optional, not referenced by PS4: siege-v1, wt-v1, lost-city-storm bank, `*.networking.crypt` | 17 | 188.1 MB | `dlc-patch7.psarc` |

Without the DLC (groups A, B and E only), about 430 MB comes from the base game plus its free patch. That covers all 12 base and patch maps, co-op and all base skins.

**Already on PS4 and not needed from PS3:**
- all base MP and co-op level paks (~160) and their streamed textures
- all MP dc data, including the DLC map dc parts
- MP menu pages and MP text in 23 languages
- MP voice lines, weapons, and MP anims (`anim-mp-*`, `anim-proto-multiplayer`, `anim-npc-multiplayer`)
- `net.pak`, 15 skins, and most MP banks

**Never copy:**
- `promo1/*.edat` and `.rap` (licences)
- EBOOT
- movies
- fonts

## Tools (`u2mp/tools/`)

| Tool | What it does |
|---|---|
| `u2_list.py` | file and psarc-member listing (TSV) |
| `u2_compare.py` | PS3 (several dumps) vs PS4 key match, with source precedence (dlc-patch > patch > dlc-map > base) |
| `u2_levels.py` | parse, diff and dependency-check `pak22*.txt` registries |
| `u2_strgrep.py` | count or show short matching strings in files or psarc members |
| `u2_importlist.py` | build the grouped import list with SHA-1 |
| `psarc_lib.py` | copied from u3mp |

Intermediate listings (`ps3_list.tsv`, `bcus_list.tsv`, `ps4_list.tsv`, `compare_all.tsv`, executable string sets, `check_*.tsv`) are in `.../U3 Multiplayer/scratch/u2mp/`.

## Open points

- PS4 `ruins-multi.pak` (24.9 MB) does not tell us whether it is the base or the DLC version. The PS4 registry lists the DLC actor set but the actors are missing. Check this by parsing the pak's level data.
- Many PS4 `*-multi-npc-ingame10.pak` sizes match the dlc-map2 versions more closely than the base versions.
- The PS4 executable may need its session/lobby layer replaced, as was done for U3.
