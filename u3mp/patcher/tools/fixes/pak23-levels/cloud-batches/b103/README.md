# batch103-blendpolarity (fixer W): blend maps used with the wrong polarity (Village ground and 88 more bindings)

## Cause: the shader maths, decoded on both platforms
- **PS4.** The prime-shader permutations are the EFFECT items inside each level pak.
  - The material's hash string (material +0x18) is the EFFECT_TABLE key ({ptr, 0, u64 hash}, 0x18 bytes each).
  - The GCN code sits in RAW_DATA items: a header with the code offset at +4, then the code.
  - It was disassembled with `tools/gcn` (a GCN2 table built from llvm-mc's bonaire encodings).
- **PS3.** The NV40 fragment and vertex programs come from MATERIAL_TABLE_1 +0x1c, in 0x10-byte records indexed by the node's program id:
  - fragment program at +0xc, ucode at its header +0x10;
  - vertex program at +8, ucode at +0x30.
  - They were disassembled with `tools/fp.py` and `tools/vp.py`, which reproduce c011's decode of the emblem shader.

For vil-pre-dirt-dry2grass-dry, the Village ground (PS4 effect 94D0F0E9C8D7B85C, PS3 program 0x39; listings in `evidence/`):

| term | PS4 (our template) | PS3 |
|---|---|---|
| vertex term t | -(vc(1+2s) - 2s)/(2s+0.01) (VS param4) | identical (VP o[TEX6].x) |
| blend factor f | sat(t + **map**/(2s+0.01)) | sat(t + **(1-map)**/(2s+0.01)) |
| colour | lerp(c2·tint2·b2, c1·tint1·b1, f) | same (tex3 = grass, tex4 = dirt) |
| border tint | lerp(border, 1, abs(2f-1)) | same |
| layer 3 | × mask3·tint3·b3 | same |

- PS4 has the inverted form as the feature BLEND_FACTOR_INVERT_BLEND_MAP. The vil-rock2mud permutation computes t + R - R·map.
- PS3 picks the polarity per material. Bluepoint kept it: on the Desert Village retail exact twins, PS3 polarity = INVERT on 36 of 39.
- Our stage converter chose templates without regard to it.
- On the Village ground the grass-strand pattern comes out mirrored. Measured over the real blend map and vertex colours:
  - mean per-texel |Δf| = 0.25;
  - grass fraction 0.63 vs PS3 0.75;
  - dark strands become dirt (the hatched dirt on the lobby floor).
  - Mean albedo stays about the same, so the overall brightness is a separate issue (exposure).

## Fix
For each material where PS3 is inverted and the template is not, `append103.py`:
1. appends a block-wise inverted copy of the material's own blend texture (`tools/bc1inv.py`);
2. repoints only the BlendMap binding at it.

The result is map' = 1 - map, so the shader's result equals PS3's.
- BC1 4-colour blocks invert exactly (c0' = ~c1, c1' = ~c0, indices XOR 1). 3-colour blocks are re-fitted. Mean error on the ground map is 1.3/255.
- The texture is named `<blend>-polinv.tga`. Materials of the matching polarity keep the original texture.

**Targets:** 89 bindings in 25 paks, all overlay-r3. Per-pak counts are in SWAP.tsv.
- Village: ground ×2 (match + stage), boulder2rocks ground, snow2cliff, stone-block2plaster-invt, wood-lt-map.
- Fort: basement stones2moss, plaster-wet, fountain floor, moss2dirt, skeleton floor.
- London (ldn-train-multi): 3 paint-chipped crumble walls.
- London Streets ×6, Shambhala ×13, Oasis ×10, Grave ×15, Museum ×7, Ruins ×8, others.

**Selection.** The PS3 polarity is read from the program by `tools/pol.py`. Only the 'inv' reading is used: on 2188 retail exact twins it never reads 'inv' where retail lacks INVERT. Not touched:
- 'non' readings against an INVERT template. 71 template rows, but the detector misses about 20% of real inversions, so these are not certain.
- Materials whose blend map is a generated flat.
- `evidence/polarity-scan.json` has every converted blend material: pak, material, PS3 polarity, template INVERT, BlendMap.

## Locks and use
SWAP.tsv md5_before is the PC state after batch102 (v6.47 as installed). The locks were computed from the PC files themselves, which I staged and checked.

h017c-bv-custom-fix installs before batch103, so dig-coop-dropdown.pak and dig-coop-ship.pak also accept the state after it:

| pak | md5_before (after h017c) | md5_after |
|---|---|---|
| dig-coop-dropdown | f857452e | ffaf7987 |
| dig-coop-ship | 79a40b5f | 7132c0e1 |

These are the rows marked `state` = "after batch102 + h017c" in SWAP.tsv. To build them I ran H's apply_patches.py on the staged post-batch102 files first. I tested both states: check, apply, already-applied and revert.
```
python3 append103.py "<...>/U3 Multiplayer/work" "<backup dir>" --check
python3 append103.py "<...>/U3 Multiplayer/work" "<backup dir>"
python3 append103.py "<...>/U3 Multiplayer/work" "<backup dir>" --revert
```
Install only while the game is stopped.

## What to look at
- **Village** ground, match and lobby stage: grass clumps should follow the blend-map strands. Dark strands = grass, not dirt hatching, with more grass overall.
- Fort basement walls and floors. London Underground crumbling paint walls. Shambhala, Oasis and Grave as before, with the blend pattern swapped where it was wrong.

## Converter fix (paktools/stage/ps4stage.py, backup .pre-b103; U3_STAGE_BLENDPOL_OLD=1 = old behaviour)
`ps3blendpol.py` reads the PS3 material's polarity from its fragment program. When it reads 'inv' and the template lacks BLEND_FACTOR_INVERT_BLEND_MAP, the BlendMap gets an inverted copy of the PS3 blend texture: `convert_ps3_texture(..., invert=True)`, `bc1inv.py`.

## Still open: the 205 "vertex-only" rows
These are not vertex-only on PS3. In cha-hollowblock-alt-to-plaster, for example, PS3 takes the blend from another texture's .z channel (tex0) with the same softness maths. A data fix needs a derived texture per material (that channel copied into green) and the PS3 sampler-unit mapping. That is a follow-up.
