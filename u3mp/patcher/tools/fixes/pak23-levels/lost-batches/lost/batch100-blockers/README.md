# batch100-blockers (fixer W): shadow-blocker shells drawn in the colour pass, plus 0/1 placeholder lightmap uvs

**Status:** built, and `--check` passed on the PC: every write was computed in memory against the installed files, and all 85 paks reproduce SWAP md5_after. Main is installing it now. **Not yet confirmed in game.** The tester re-shoots Fort turn3/turn4, Village spawn/floor/lobby stage and Plaza after the install.

## What it changes
85 paks, 2,690 four-byte words, in place, no size change. Every pak is md5-locked to the PC overlay-r3 files with batch99 installed (2026-10-29/30 scan).

| class | words | rule | why |
|---|---|---|---|
| A | 2,154 | blocker instsub with +0x20 bit 0x10000 (shadow only) but +0x24 != 0x101: +0x24 \|= 0x101 | Retail hidden blockers always carry +0x24 = 0x101 (lib-shadowblocker 38/38, ik-only 12/12). The instance builder 0xe9a490 sets pass bit 4 when byte4 == 0 and pass bit 0x10 when byte5 == 0, so a 0x10000-only blocker is still drawn (w005). These are the same words as batch96. batch96 showed no change in Desert because no blocker is visible from the user's Desert cameras (render check). |
| B | 278 | blocker instsub drawn (no 0x10000): +0x20 \|= 0x10000 (ik-only 0x1010000), +0x24 \|= 0x101 | ps4stage kept blockers drawn when the PS3 instance word +8 has 0x10000. Retail does that (yem-intro 5/5), but on our MP maps these are terrain-hugging shells, e.g. village-multi inst 969 and 3 in village-multi-stage. PS3 never shows a blocker. |
| C | 258 | lightmapped instsub (+0x20 bit 0x100) whose slot-5 lightmap uvs are all integers: clear 0x100 | Extends batch99. M = 1 placeholders decode to rows (0,0) / (1,1) only, which lie inside [0,1], so batch99 skipped them. They still sample the lightmap corners: village / ldn-streets / fort indirect.rgbmax corners have alpha 0-31, i.e. black. 291 instsubs, e.g. ldn-pub-light-fixtures, sham-multi-icicle-flat. |

Blocker = material name containing shadowblocker, ik-only or canopy-shadow. museum-multi(-stage) is left out (it never got batch92; main holds Museum).

## Evidence (offline, renders of the installed geometry)
Ground-level views: 30 to 60 random walkable points x 4 to 6 headings per map. The shares below are the pixels where a not-hidden blocker is the nearest surface, or lies within 0.3 m of it (z-fight):
- village-multi: blocker 969 (flags 0) in 30% of pixels. It is in front of the ground in only 0.9%, and within 5 cm in 8.3%.
- ldn-streets-multi: inst 0 sub 1 lib-shadowblocker (0x10000 only) in 19.5%.
- fort-multi: inst 1 sub 1 general/shadowblocker (0x10000 only, a 6,769-triangle proxy shell) in 15.3%, and inst 5 sub 1 in 1.8%.
- `evidence/render-*.png` are rough tiles (red = visible blocker). The Village tiles are degenerate views (eye inside a blocker box); use the numbers above.

**Caveat for Village.** Blocker 969 lies 0.3 to 0.8 m below the terrain (median -0.46 m at 298 terrain samples). The user's black ground (pitch black, stones looking as if they float) therefore means the terrain submesh (inst 2 sub 1, vil-pre-dirt-dry2grass-dry, effect 94D0F0E9C8D7B85C, the only user of that effect in village-multi and village-multi-stage) is either not drawn or drawn black. We see the black blocker underneath it. If Village shows a void instead of black after batch100, the terrain is not drawn; that is a separate fix (see NOTES-maps).

## Install / check / revert
```
python3 apply100.py "<...>/U3 Multiplayer/work" <backup dir> --check    # in-memory, compares with SWAP md5_after
python3 apply100.py "<...>/U3 Multiplayer/work" <backup dir>            # patch (backup taken once per pak)
python3 apply100.py "<...>/U3 Multiplayer/work" <backup dir> --revert   # undo
```
Needs numpy and tools/ (ndpak.py, ps4bg.py). A pak whose md5 is neither md5_before nor md5_after is skipped.

## Converter
paktools/stage/ps4stage.py (backup ps4stage.py.pre-b100):
- x8-0x10000 blockers are now hidden like every other blocker. `U3_STAGE_SB_KEEPX8=1` restores agent R's retail rule.
- The lightmap-uv placeholder test now treats any all-integer 0x13 stream as a placeholder. `U3_STAGE_LMUV_RANGEONLY=1` restores the batch99 test.
- +0x24 = 0x101 was already written by default (w005, `U3_STAGE_NO_SBVIS`).

## Update 2026-10-30: in-game result and class B revert
- The tester's run g7i (v6.43 + batch100) showed no visible change. The Village / Fort / Plaza black is batch101-detail's cause (the detail layer bound to the black flat-mask), not blockers.
- Class B deviates from retail and is dropped: `revertB/revertB.py <work> <backup dir> [--dry|--revert]` undoes only the B words. It covers 19 paks, md5-locked: md5_before = this batch's md5_after, md5_after = computed on the PC files.
- Classes A (1:1 retail +0x24 = 0x101) and C (0/1 placeholder lightmap uvs) stay.
- The converter keeps the retail x8 rule by default again; `U3_STAGE_SB_HIDEX8=1` = class B behaviour (opt-in).
