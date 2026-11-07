# n012-museum-catchup (fixer L): Museum gets the converter fixes every other map already has, plus a test for the Museum freeze

## Why
Museum is the only map whose installed BG paks were left out of batch92, batch94 and batch96. Each of those batches was held for Museum or skipped it:
- museum-multi.pak (PC md5 9fd5549c = batch82-v22 + batch90 + batch97 water + batch99 lmuv):
  - 428 shadow-blocker submeshes (400 general/shadowblocker proxy shells, 26 lib-shadowblocker black, 2 ik-only) are still drawn in the main pass. On the other maps these were the black slabs.
  - 64 prototypes have no fade record. The engine then uses start 0 / end 1e-8, so those instances are faded out at every distance. On Fort this was the "floating pieces / white void".
- museum-multi-stage.pak (b8d02b53): 3 blockers drawn.
- museum-multi-wide.pak: no blockers and no fade prototypes, so it is not touched.

## What it writes
| pak | md5 before (PC overlay-r3, 23:30 UTC) | md5 after |
|---|---|---|
| museum-multi.pak | 9fd5549cf26df7ba524d9b6723d23d59 | 7a7c20e84188f9e0e854ef37a4972e08 (88617296 bytes) |
| museum-multi-stage.pak | b8d02b5338b55f8128bafcf4ea198761 | 6710d822419e8bdd2a32590abc5aa14d |

- batch92 words: the same 428 + 3 offsets and values as batch92's own museum patch. Each instance-submesh +0x20 gets 0x10000, or 0x1010000 for ik-only. The current words differ from batch92's base only where batch99 cleared 0x100 on other submeshes, and none of those overlap.
- batch96 words: +0x24 |= 0x101 on the same submeshes (fixer W's sbvis.py rule, rescanned on the patched bytes).
- batch94 fade: F3's museum recipe, unchanged (64 prototypes; fades 30/40 .. 80/90 m). It appends one page and re-lays out the header.

Checks:
- The output passes ndpak validate.
- All 898 submeshes have in-range indices; textures and VRAM are unchanged (324 textures, 0x2e80a00).
- All 64 fade pointers resolve; example record {40, 50, 0, 0}.
- The fade revert gives back the in-place bytes exactly.
- A dry run of apply_patches.py on copies of the PC files passes.

## Install / revert
`python3 apply_patches.py "<work>/overlay-r3/u3data/build/main/pak23/ps4" <backup dir>` (add `--dry` to check first). It is md5-locked: a file with any other md5 is skipped.
`--revert` copies the backups back.

Not included: batch91 v2.3 (Museum's re-template; a full pak on another base). batch91m-museum stays on HOLD.

## Museum freeze: static findings (item C)
1. **No data defect.** A census of all 145 overlay-r3 paks checked page validation, texture format, dims, mips and alignment, index range, NaN/Inf in vertex streams and stream counts. There were 0 problems anywhere. Museum is mid-pack: 898 submeshes, 1.01 M indices, max 7302 vertices per submesh, max texture 2048.
2. **VRAM is not the trigger by itself.** These are the match sets as they are actually loaded (from the run logs, using header vramSize):

   | map | set |
   |---|---|
   | Museum | 65.4 MB |
   | Temple | 63.7 MB |
   | Facility | 63.1 MB |
   | Highrise | 57.6 MB |

   Facility ran 85 s in **fullscreen 1920x1080** on v6.34 (run 1793309438) with no problem, which is only 2.3 MB less than Museum.
3. **Sky format is ruled out.** sky-village-pre's 2048x1024 texture (format 0x5b, 11.2 MB) is the same as Desert Village's sky-des-multi, and Desert runs fullscreen many times.
4. **The freeze has not been reproduced on any build since s014.**
   - Both freezes (09:59 and 11:21 runs) were v6.19x in fullscreen. Neither left a result.json and the process could not be killed.
   - The only later Museum run (v6.24, 12:37, windowed) passed.
   - Fullscreen Museum has never been run on v6.22 or later.
   - A guest fault in fullscreen does exit cleanly: run 1793314259 (22:54) exited with code 3. So the old freezes were a real host or driver hang, not a hidden guest fault.
5. "Unimplemented primitive type, using TriangleStrip" shows up in every map's run, so it is not Museum-specific.

## Proposed test (one session, fullscreen)
1. On the current build (v6.39 + n011), with **no** Museum data change: LAN TDM on Museum. Switch to fullscreen in the lobby, start the match and play 60 s. Then a second match on Facility, also fullscreen, as the control.
   - If Museum runs: the freeze is gone on current builds. Install n012 for the visuals, then repeat Museum fullscreen once.
2. If step 1 freezes: install n012 and repeat. If it still freezes, the next A/B is a VRAM cut below Facility's set. That means rebuilding batch93m's B/AB mip drops on the current museum-multi (batch93m was built on an older base, so its md5s no longer match). Ask fixer L or W for it.
3. Send boot.log, net-trace.jsonl and the Window lines (fullscreen or windowed) for each run.
