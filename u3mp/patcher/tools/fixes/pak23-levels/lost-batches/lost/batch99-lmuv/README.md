# batch99-lmuv (fixer W): Desert Village black bands. The real cause is a false "own lightmap uvs" flag, not the shadow blockers

**Status: proven offline against the user's shots (see below). UNTESTED in game.** batch96 (blocker +0x24) was the wrong lead and stays reverted.

## Symptom (shots/v632/ours-desertvillage-1/-2 vs ps3-desertvillage-1/-2)
- Thick black bands around the window frames and along the crumbled wall edges.
- Black slabs on wall tops.
- The bands are flat, untextured colour, the same in sun and shade: mean RGB 23,19,12 with std ~5, which is just the distance haze over black.

## Proof that it is these submeshes (data, against the screenshots)
1. **Camera fit.** The installed desert-multi.pak (md5 c05ec808 = batch82-v22 + batch90 + batch92, rebuilt locally byte for byte) was rendered with a C rasterizer.
   - Camera found by a sky-mask search over 3,604 floor points, then a 7-point PnP fit on window corners and tower edges.
   - Reprojection error: 5.6 px (ours) and 3.8 px (PS3, from the same 3D points).
   - The render edges then line up with the left ruined wall, the tower, the low wall and the three-window wall in both shots (`evidence-desert-1.jpg`).
2. **The band is one submesh.** It is instance 58 submesh 5, material `des-plaster2crumble-001`: the crumble ring around the windows and the broken edges.
   - Its outline (cyan in the evidence image) traces exactly the black bands of the ours shot.
   - In the PS3 shot, the same pixels are textured plaster.
   - Per-submesh dark fraction (L < 37): **ours 0.47 vs PS3 0.05**. The main wall next to it (inst 58 sub 4, same material family, same lighting) is 0.06 vs 0.01.
3. **Shot 2 (rooftop, well at the right).**
   - Camera from a 6-point PnP; the fit is rougher (~14 px).
   - The roof is instance 196 and the well is instance 189. Both carry the false flag (`evidence-desert-2.jpg`, cyan outlines).
   - The black slab on the right parapet and the black shade behind it lie on 196 sub 4 (plaster2mudbricks: 50% of its pixels darker than L 37) and on the shaded part of 196 sub 2.
   - The sunlit roof top stays bright because direct sun is added on top of the black lightmap. Only the shaded / sun-facing-away parts go black, which is what both shots show.
4. **The shadow blockers are not involved.**
   - With all 270 blockers drawn, none of them is visible from this camera.
   - The blocker material and effect equal retail (`prime-shader.fx`, effect 2D7EB2A8145FF2B8).
   - So batch96 could not change anything here, which matches the user's test.

## Cause (converter bug, all converted BG maps)
- PS4 instance-submesh flag **+0x20 bit 0x100 = "this instance has its own lightmap uvs"** (per-instance stream slot 5, type 0x22). With the flag clear, the submesh is vertex lit (per-instance stream slot 8). This is how retail desert-village-1b does it: inst 47 (crumble) has no flag and slot-5 zeros; inst 48 has the flag and real uvs 0.0015..0.9995.
- ps4stage sets the flag whenever the PS3 instance has a 0x13 (uv2) stream pointer.
- **Vertex-lit PS3 blocks still carry a placeholder 0x13 stream:**
  - format 0x42 with scale 128*M*(1 + 2^-14);
  - bits all 0 or all 1;
  - so the decoded rows are only (0,0) or (M,M), M = 2..80.
  - Example: PS3 desert-multi prototype 22 block 13 = 380 rows of 0 and 3 rows of 16383.
- On PS4 the flagged submesh then samples the lightmap at an integer uv, i.e. the atlas corner. The desert `indirect.rgbmax` texel (0,0) is RGBM 123,255,189 with **alpha 0** = black. That gives a flat black surface, regardless of sun or texture.
- Real lightmap uvs are always inside [0,1] (scanned: 9,190 flagged instsubs on the PC; the only out-of-range non-integer ones are 1.002..1.029 overshoots, untouched).

## Fix
Clear bit 0x100 on every instsub whose slot-5 stream holds only integer rows outside [0,1]. The submesh is then vertex lit (its slot-8 stream is the PS3 per-instance vertex lighting), as on PS3 and in retail.
- **47 paks, 251 words, in place (no size change).** Scanned on the PC overlays (r3 > r2 > port > mp, first match) at 2026-10-29 22:40 UTC; md5-locked.
- Desert: desert-multi 11 (crumble rings of inst 15/16/57/58, the inst-196 roof set, sand-flow, wood), desert-multi-stage 5.
- Also, among others:
  - mon-coop-cloister 30, ldn-train-multi 26, graveyard-drydock-wide 11;
  - highrise-multi 10, museum-multi 10, mon-coop-stage 10, coop-airport-2 9, shambhala-multi 8;
  - grave-multi 7, temple-multi 7, yemen-multi-stage 7, coop-syria-area2-low 7, coop-ldn-atwood-station 7;
  - cha-multi / -shade 6, warzone-multi 6, air-multi / -shade 5, oasis-multi 5, fort-multi 4, old-quarter-multi 4.
- Per-pak counts and materials are in SWAP.tsv. Every write is listed with instance / submesh / material in patches/*.json.
- Gates on desert-multi: crashcheck clean, bindcheck 0, patched md5 = SWAP md5_after (local rebuild).

**Converter:** `paktools/stage/ps4stage.py` no longer flags a placeholder 0x13 stream (backup `ps4stage.py.pre-lmuv`; `U3_STAGE_NO_LMUVCHECK=1` = old behaviour). Future builds need no patch.

## Install
```
python3 apply_patches.py "<...>/U3 Multiplayer/work" <backup dir> --dry      # every pak should say "would patch"
python3 apply_patches.py "<...>/U3 Multiplayer/work" <backup dir>
python3 apply_patches.py "<...>/U3 Multiplayer/work" <backup dir> --revert   # undo
```
A pak whose md5 changed since the scan is skipped. Re-scan it with `tools/scanlm.py OUT.jsonl PAK...` (needs tools/ndpak.py, tools/ps4bg.py).

## Expected in game
- **Desert Village, same spots as ours-desertvillage-1/-2:** no black window frames and no black edge bands. The crumble rings show plaster and bricks like PS3. The inst-196 roof set (clumps / mudbricks / reeds) is no longer black.
- **Other maps:** the same kind of flat black pieces disappear where they exist (London train, Monastery cloister, Highrise, Museum, Shambhala, Graveyard ...).
- Shadows and the rest of the lighting are unchanged.
- If anything looks worse: `--revert`.

## Not changed
The general "darker shadow side" look (shadowed walls darker than PS3 by ~30%) is a separate issue. Retail sets the no-shadow-cast byte (+0x23) on 166 of 542 Desert-SP instsubs and the converter never does; no PS3 predictor is known yet.
