# batch102-flatslots (fixer W): other texture slots bound to the converter's black flat-mask

batch101 fixed one class: Mask3 detail layers bound to the black `flat-mask`. This batch scans every converted BG and stage pak for every slot bound to a generated flat texture (`art/generated/u3mp/flat-*`). The PS3 material and the retail donor template were checked for each one.

`flat-table.tsv` has one row per binding (1099 rows from the 133 converted paks held locally; the 571 Blend / TintMask / Incandescence rows on the PC are all included). Columns: pak, material, slot, bound flat, retail donor texture, PS3 textures the converter left unplaced, verdict, batch102 texture.

## Verdict per class
| slot | rows | verdict |
|---|---|---|
| BlendMap | 37 | **fixed (A).** The PS3 material has a `-height` / `-blend2` map the role rules did not recognise. Retail binds `-height` maps in BlendMap too (des-vil-rubble-001-height, yem-stucco-05-height, ...). |
| BlendMap | 205 | open. The PS3 material blends by vertex colour only (no map), but the template is BLEND_MAP_AND_VERTEX_COLOR. PS4 has BLEND_FACTOR_USE_VERTEX_COLOR_ONLY, so this is a mkplan template choice and needs no data. |
| BlendMap | 212 | open. The PS3 leftover is a spec-mask / detail / colour map: not certainly the blend source. |
| ColorTintMask3 | 17 | **fixed (B).** The PS3 second `-blend` map is the layer-3 tint mask. PS3 brightness3 is 2.0, so black = plaster/paint never lightened. Retail tint masks are `-mask` / `-blend` maps. |
| ColorTintMask3 | 49 | open. The PS3 layer 3 is a `-detail` multiply, but the template is TINT_WITH_MASK. Black = no tint, which is about neutral on average. Retail never puts `-detail` maps in tint masks. |
| IncandescenceMask | 8 | **fixed (C).** The PS3 `-inc` / `-incan` / `-incand` mask was unplaced, so these lamps never glowed: dig lanterns and bulbs, dig floodlight, facility circular light, London distant lights. |
| IncandescenceMask | 28 | no effect (incandescence strength 0). |
| IncandescenceMask | 25 | open. A glowing PS3 material with no mask (retail uses default-mask = white for this). The strengths are up to 50 (war-prayer-flags), so this is not changed without a reference shot. |
| Mask3 | 41 | batch101 (installed). |
| Mask4 (spec) | 40 | correct. The PS3 material has no spec map. |
| Normal1/2/3 | 330 | neutral (flat normal). |
| Detail1/3 | 50 | neutral (normal-detail add; grey = none). |
| Color1/2 | 27 | open (grey colour layer, the PS3 material has no second colour map). |
| Foam / water | 19 | agent A's area. |

Actor paks: the converted MP kits' flat bindings are only Detail1/3 (normal-detail, neutral), Mask4 (spec), Normal and 9 Color1/2. No hair material uses a flat texture. The London No Intro white hair is the retail `mp-hero-french-hair` actor (fixer L's probe diagnostics n018 / n020), not a flat binding.

## Fix
`append102.py` works like batch101's appender. Per pak it appends one page, adds the PS3 texture (DXT1 converted 1:1 to PS4 tiled **BC1 UNORM** with linear flags, matching retail's height / inc maps; `data/*.bin`, 32 textures, 4.6 MB), and repoints only the 62 listed bindings (short + full string). Three village textures were already in the pak and are reused. Nothing else changes.

21 paks, all in overlay-r3: village-multi (19 bindings), village-multi-stage (9), fort-multi (4, basement brick/plaster), yemen-multi (4), dig-coop-ship (4), desert-multi (3), ruins-multi (3), ruins-multi-stage (2), dig-coop-dropdown (2), and one each in air-multi, air-multi-shade, dig-coop-approach, dig-coop-stage, facility-multi, ldn-streets-multi-wide, oasis-multi-wide, ruins-multi-wide, warzone-multi, warzone-multi-stage, yemen-multi-stage, yemen-multi-wide.

## Order and md5 locks
`SWAP.tsv` md5_before is the PC state of v6.44: after batch99, batch100 A/C, revertB, batch101 and batch101m. The locks were computed from the PC files themselves (staged 2026-10-30 03:2x). md5_after was built from those same bytes. Paks at any other md5 are skipped.

```
python3 append102.py "<...>/U3 Multiplayer/work" "<backup dir>" --check   # builds all in memory, compares md5_after
python3 append102.py "<...>/U3 Multiplayer/work" "<backup dir>"           # apply (backups written first)
python3 append102.py "<...>/U3 Multiplayer/work" "<backup dir>" --revert  # restore
```
Install only while the game is stopped.

## What to look at
- **Village** (match + lobby stage): boulder walls (`vill-boulder-wall-color-*`) should show stone vs plaster following the stone heights, not a flat vertex-colour split. Plaster and whitewash on the `vill-pre-*` walls, wood trims and stripped wood should be lighter where the blend mask says so.
- **Fort**: basement brick / plaster walls (`fort-basement-brick-plaster-slatted*`) should get their brick-to-plaster pattern.
- **Yemen**: rich-brick / concrete to stucco walls. **Ruins** (Molten Ruins): cracked customs plaster and pillar slabs. **Desert**: plaster to mud-brick walls. **Co-op Dig**: lanterns and bulbs glow.

## Converter fix (paktools/stage/ps4stage.py, backup .pre-b102)
- `_xrole()` gives names that `tex_role_final` calls 'color' their own role: `-height` -> height, `-blend<n>` -> blend, `-inc` / `-incan` / `-incand` -> inc. So a height map no longer becomes a colour layer (13 Color2 bindings showed a height map as colour).
- Binding fallbacks:
  - BlendMap uses the height map once the `-blend` maps are used up.
  - ColorTintMask<n> takes the `-blend` map after the ones BlendMap uses (PS3 order).
  - IncandescenceMask takes the inc map.
- These maps are encoded BC1 UNORM (linear), as in retail.
- `U3_STAGE_FLATSLOT_OLD=1` restores the old behaviour.
- Checked by replaying the binding rules on the 583 local rows: 61 of the 62 batch102 bindings come out the same. The exception is dig-floodlight's `flood-light-inc-color`, which the converter keeps as a colour map. There are 10 more bindings where a height map that used to be bound as a colour layer moves to BlendMap.
