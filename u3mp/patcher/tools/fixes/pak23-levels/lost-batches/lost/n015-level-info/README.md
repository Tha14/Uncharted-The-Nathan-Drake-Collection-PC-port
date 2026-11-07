# n015-level-info (fixer L): converter bug, the LEVEL_INFO_1 level flags are dropped (49 converted BG paks)

## Finding
- LEVEL_INFO_1 is one u32 of level flags. On every rosetta pair the retail PS4 value equals the PS3 value written little-endian:
  - cargo-plane-int-floor-b: PS3 0x20 = retail 0x20;
  - cha-forest-sinkhole, yem-chase-cistern-kitchen, yem-intro-temple-entrance: 0x100 = 0x100.
- ps4stage.py always wrote 0. A census of every PS3 MP pak in pak23.psarc that has an overlay pak found 49 paks where PS3 is nonzero and ours is 0:
  - 0x100: Syria co-op, Monastery co-op, desert-multi, facility-multi(-stage), syria-multi, ldn-train-multi-lit, coop-airport-2, ...
  - 0x108: ldn-train-multi, coop-airport-1/3a/3b.
  - 0x8: ldn-train-multi-tunnel / -tunnel-2, coop-ldn-atwood-station, coop-ldn-victorian-station, coop-airport-wide.
  - 0x28: ldn-train-multi-tc01..tc06 (the train cars).
  - 0x1: the -wide paks (air, cha, desert, facility, yemen).
- The LEVEL_INFO_1 login handler is at PS4 0x10e4913 (level+0x28 -> +0x240). These bits are read:
  - 0x1 = wide level, at 0x10f4d09. The engine also derives it from "-wide" in the name.
  - 0x10, at 0x10f4992. No converted pak has it.
  - 0x20 -> render-level flag 0x100, at 0x10f4d42. Only the London train cars (0x28) have it.
- I found no PS4 reader of 0x8 or 0x100, so this batch probably does not change the London tunnel lighting (n014 is a separate fix). It is a 1:1 data fix.

## Fix
- Data: one u32 per pak, set to the PS3 value. The batch is md5-locked against the PC overlay-r3 files as of 2026-10-30 00:40 UTC (SWAP.tsv).
  - Install: `python3 apply_patches.py "<work>/overlay-r3/u3data/build/main/pak23/ps4" <backup dir>` (`--dry`, `--revert`).
  - ldn-train-multi-tunnel is locked to the n014-installed file (617b033a), which is the current PC state.
- Converter: ps4stage.py now copies the PS3 LEVEL_INFO_1 value. The backup is ps4stage.py.pre-levelinfo, and `U3_STAGE_NO_LEVELINFO=1` gives the old behaviour.

## Test
- Load London Underground (intro, carriage = tc paks with 0x20) plus one of Desert Village, Facility or Syria co-op.
- Expect no change, or the train cars to look more like PS3. FAIL means an assert, a crash or missing geometry; in that case run --revert.
