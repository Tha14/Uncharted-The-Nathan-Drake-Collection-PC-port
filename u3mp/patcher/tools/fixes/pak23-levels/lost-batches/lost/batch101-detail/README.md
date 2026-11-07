# batch101-detail (fixer W): Village black ground, Fort black rubble, Plaza black plaster

**Status:** built and md5-locked against the PC files (staged read-only from the PC 2026-10-30 ~01:40 UTC, batch99 + batch100 installed). Gates were run offline:
- crashcheck clean, bindcheck 0, fidelity clean;
- ps4check shows the same 14 known items as before;
- the new textures decode as expected (Village detail mean 201, white flat 255);
- every original page is kept the same size.

**UNTESTED in game.**

## Cause (converter bug)
- In a prime-shader material, a colour layer with `COLOR_3_APPLY_MASK_TEXTURE` and no `COLOR_3_APPLY_COLOR_MAP` is a **detail multiply**: colour *= `g_tNdFetchMask3Map`.
- Retail always fills that slot with a near-white `-detail` map: 1,604 of 1,682 retail layer-3 mask bindings; yem-dirtyplaster-02-detail averages 240 (sRGB).
- mkplan/ps4stage gave the template param `g_tNdFetchMask3Map` the role 'mask'. The PS3 material has no 'mask' texture, so the converter bound the generated `flat-mask.tga`, which is **black**. Albedo x 0 renders pitch black whatever the lighting, which is why batch100 (blockers) changed nothing.

The in-game shots match these materials:

| map | material(s) | shot |
|---|---|---|
| Village (village-multi + -stage) | vil-pre-dirt-dry2grass-dry = the whole ground (inst 2 sub 1); vil-ceramic-red-2 | spawn / floor-down / lobby stage: ground black, stones float |
| Fort (fort-multi + -stage) | crumbled-brick-moss, darker-crumbled-brick-copy, eroded-brick-2-moss, fort-basement-*, fort-mlti-bricks-small-dk | turn3 / turn4: fallen masonry solid black |
| Plaza (warzone-multi + -stage) | war-interior-paint-blue-2, war-floral-lattice-paint-green, war-interior-paint-green (war-brick-worn-detail) | dark interior with a pitch-black wall |
| Molten Ruins (ruins-multi) | ruins-customs-plaster-dark-01, -plaster2crumble-wetnorain, -crumble-1-layer | (no shot) |
| also | gompa 4, yemen 3, warzone ground caustics (white), grave 1+3, temple 1, highrise 1, coop-airport-3a / -stage 1 | |

## Fix (per pak: one appended page, no existing page resized)
- Appends the PS3 material's own `-detail` texture, converted 1:1. It is PS3 DXT1, re-tiled to PS4 BC1 sRGB, the same format as retail's detail maps. `data/*.bin` holds the 12 converted maps:
  - Village: vil-ground-dirt-noise-2-detail. This is the same map Bluepoint's U2 retail binds for this exact terrain material.
  - Fort: bricks-dark-erosion-detail and fort-basement-water-detail.
  - Plaza: war-brick-worn-detail; Molten Ruins: ruins-streaks-002-detail.
  - others as listed in `data/targets.json`.
- Where the PS3 material has no detail map (vil-ceramic-red-2, ceiling-b-multi, grv-dusty-broken-window, grv-buoy, grv-deck-floors, war-ruin-ground-caustics, gompa-only), it appends a 64x64 **white** flat instead (neutral for a multiply).
- It adds them to TEXTURE_TABLE (hash-sorted), to a new VRAM_DESC_TABLE (the header points at it), and to the GEOMETRY_1 texture array. Texels go at the end of VRAM (256-byte aligned).
- It repoints **only** the `g_tNdFetchMask3Map` binding (short / full string) of the 39 target materials. flat-mask itself is untouched, because the blend maps of the other materials share it.

## Order and md5 locks
`SWAP.tsv` md5_before is the state **after `batch100-blockers/revertB/revertB.py`** for the paks batch100 class B touched (village-multi, village-multi-stage, fort-multi, highrise-multi, warzone-multi, warzone-multi-stage, yemen-multi; column base_state = after-revertB). For the others it is the batch100 state, which is the same before and after revertB. Install order:
```
python3 ../batch100-blockers/revertB/revertB.py "<...>/U3 Multiplayer/work" <backupB dir>     # drop batch100 class B, keep A + C
python3 append101.py "<...>/U3 Multiplayer/work" <backup101 dir> --check                         # in memory, compares with SWAP md5_after
python3 append101.py "<...>/U3 Multiplayer/work" <backup101 dir>
python3 append101.py "<...>/U3 Multiplayer/work" <backup101 dir> --revert                        # undo
```
Needs Python 3 only (tools/ndpak.py, tools/ps4writer.py; no numpy). A pak whose md5 is neither md5_before nor md5_after is skipped. Museum is not included (it is held).

## Converter
`paktools/stage/ps4stage.py` (backup ps4stage.py.pre-b101):
- When the donor template's own texture in a `g_tNdFetchMask<n>Map` slot is a `-detail` map, the slot takes the role 'detail' (the PS3 material's -detail map), else a white flat (`flat-white.tga`).
- `U3_STAGE_MASK3_ASMASK=1` restores the old behaviour.
