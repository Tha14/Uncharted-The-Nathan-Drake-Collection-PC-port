# batch98-water (fixer H, h006; continues fixer L's n005 water work): no swimming + clear/shiny water on converted maps (Old Quarter first)

**Supersedes batch97-water-layer. Do not install both.** batch98 includes all of batch97's collision fixes.

## Symptom (v6.32, shots/v632/ours-oldquarter-water.mp4)
The Old Quarter canal has a bright, shiny, transparent surface. The player sinks under it and walks on the basin floor, with no swimming.
On PS3 (ps3-oldquarter-water-dark.png) the water is dark and murky green-brown, and you swim at the surface.

## Cause 1: water collision in the wrong Havok layer (no swimming)
Our converter (paktools/collision.py) wrote collision filter info 1 on every BG primitive.

The retail Bluepoint rule sets filter 2, the water layer, for ND collision flag 0x4000. Fixer L checked it on 3.6 M primitives of 604 retail U3 paks with 0 mismatches. Retail grave-water and grave-03-docks-water show 2/0x4000 on every water primitive.

The water collision items (f35 = 1) in our paks were all filter 1. So the character doesn't stand on them, and nothing in the water layer starts swimming. The player falls through to the basin floor.

Fix: palette entry 1 -> 2 on the water collision mesh, in 19 paks:
- grave-water-multi gets 0x20000002, because its prims also carry 0x2000.
- Non-water BG collision is **not** touched. Its palette stays 1, as installed.

## Cause 2: wrong water material template (shiny clear surface)
The old mkplan water fallback picked the smallest retail pak's water in the same directory:
- Old Quarter got the clear Yemen market fountain (yem-into-market-fountain-cube-01: env-cube reflection, no diffuse);
- other levels got cruise-ship world-water-displacement (no water program, no textures).

PS3's Old Quarter water (levels/yemen/water/old-quarter-water-test/surface-puddlemtl) is an algae water. It uses the textures ug-water-algea-color and ug-water-algea-mask, with g_cRefracDepthColor (0.016, 0.024, 0.009), which is near-black green.

Fix: fixer L's new rule (paktools/stage/waterscore.py + water_templates.json) scores all 64 retail water.fx templates. Old Quarter now gets **retail ug-valve-room-water:surface-puddlemtl** (ldn-valve-room-2). That is the retail material built on exactly those two algae textures. The PS3 constants are applied on top, as before.

| Level | Old template | New template |
|---|---|---|
| old-quarter-multi | yem-into-market-fountain-cube-01 | ug-valve-room-water:surface-puddlemtl |
| oasis-multi (3 waters) | world-water-displacement | horse-canyon-water-cube-reflect-02 (river), san-cistern-collapsing-water (side pools), ug-valve-room-water (wide) |
| oasis-multi-stage | world-water-displacement | ug-valve-room-water |
| village-multi (puddles) | world-water-displacement | horse-canyon-water-cube-reflect-02 |
| warzone-multi | world-water-displacement x2 | ug-valve-room-water (streets), san-cistern-collapsing-water (pit) |
| dig-coop-stage | world-water-displacement | horse-canyon-water-cube-reflect-02 |

## What is in the batch
- `u3data/build/main/pak23/ps4/`: 6 full paks (old-quarter-multi, oasis-multi, oasis-multi-stage, village-multi, warzone-multi, dig-coop-stage) = installed overlay-r3 + new water template + water layer 2.
- `patches/` + `apply_patches.py`: 13 in-place patches (1 u32 each, water layer 2), md5-locked to the overlay-r3 files on the PC as of 21:37 UTC (batch96 reverted):
  - cave-multi-waterfalls, cha-multi-wide, coop-ldn-victorian-station, coop-syria-area2, coop-syria-connection, fort-coop-cistern, fort-multi, gompa-multi, grave-multi-moving-water, grave-water-multi, ldn-train-multi, museum-multi-stage, museum-multi.
- Install: `python3 apply_patches.py <overlay-r3>/u3data/build/main/pak23/ps4 <backup dir>` (`--revert` restores). A pak with another md5 is skipped and reported.
- `SWAP.tsv`, `MD5.txt`, `tools/` (build98.py, replanwater.py, mkold.py, waterlayer.py).

## How it was verified (data, before shipping)
- **The installed md5s were checked on the PC (21:3x UTC).** All 19 paks are the pre-batch96 overlay-r3 files, and they match every patch's md5_before.
- **Reproducibility.** The same pipeline that builds the 6 paks was run with the OLD water plan entries and filter 1 (`U3_STAGE_NO_FADE=1 U3_STAGE_NO_SBVIS=1 U3_COLL_FILTER1=1`, tools/mkold.py):
  - dig-coop-stage, oasis-multi-stage, oasis-multi and village-multi give the installed files **byte for byte**;
  - old-quarter-multi and warzone-multi differ from the installed files by one u32 each. A GEOMETRY_1 texture entry flag is 0x1000 vs 0x400 (default-mask.tga resp. default-env-reflection.dds, which the current ps4stage wrongly marks as a lightmap). It is restored to the installed 0x400 in the shipped paks, so those two reproduce byte for byte too.
  - So each shipped pak = the installed pak + only the water template change + the water layer.
- **Water layer.** waterlayer.py re-scan of every shipped pak: the water collision item has palette [0x2] with 0 wrong primitives. The BG items are unchanged (palette [0x1]).
- **Material.** In old-quarter-multi, the algae colour and mask textures are bound, and the PS3 constants are present (g_cRefracDepthColor r = 0.01608, g_fFresnelExponent 11.872, g_fRefractionBrightness 0.52).
- **Pak checks.** crashcheck, fidelity, holescan and bindcheck are all clean on all 6.
- **Not used:**
  - fade (batch94) and sbvis (+0x24, batch96, reverted) are off in these builds;
  - variant A (U3_STAGE_WATERPACK=1) differs from B by 11 bytes on Old Quarter, so only B ships.

## Test (one run, several maps)
1. **Old Quarter (TDM).** Walk into the canal.
   - Expect: you **swim** at the surface instead of sinking to the floor.
   - The surface is dark green-brown and murky, and you can't see the floor (ps3-oldquarter-water-dark.png), not bright and tiled.
2. **Oasis:** the river, the side pools and the wide water look like water (not grey/flat), and swimming works where the water is deep.
3. **Nepal Warzone:** the street water and the pit look right. **Village:** the puddles look right.
4. **Swimming where PS3 swims:** Fort, Sanctuary (Gompa) river, London (ldn-train), Museum, co-op Fort cistern, co-op Syria, co-op London, Chateau wide, Cave waterfalls, Graveyard water.
5. **If something looks wrong** (e.g. Oasis or Village water worse than before): `apply_patches.py --revert` restores the 13 patched paks; restore the 6 copied paks from the backup.
