# h017c-bv-custom-fix (fixer H): Borneo co-op adventure freeze, corrected (in-place patches, 5 paks)

**Install with the game stopped, together with h017-coop-adv-worlds and h018-coop-adv-bg (both unchanged). Do NOT install h017b.**
Order: h017, h018, then h017c (apply_patches.py). batch102-flatslots may be installed before or after; see below.

## What went wrong with h017b
- Run baseline-1793332610288080100 froze on the **same** Havok assert. Guest base this run was 0x10400000. NdJobWorkerThread 1
  sits on the int3 at image 0x12fce80 with the same call chain as before (0x1c4e7 / 0x1c17b / 0xae938 ...). The other workers
  are parked inside the same mesh query code (0x12fbe0e, 0x12fbb78) or job code.
- h017b used the wrong rule. Disassembly of the primitive decoder (0x12fcb19 and the loop at 0x12fcc50) gives, for
  primitive bytes (i0,i1,i2,i3):
  - i2 == i3 and i1 == i2 -> **CUSTOM** (type 3) -> custom type from the u16 table at shape+0x68, > 4 = assert, int3 0x12fce80;
  - i2 == i3 and i1 != i2 -> TRIANGLE;
  - i2 != i3 -> QUAD.
  So CUSTOM means i1 == i2 == i3, not i0 == i1. Our converter writes triangles as (a,b,c,c), so a degenerate PS3 triangle (a,b,b)
  became (a,b,b,b) = CUSTOM. h017b rotated the (a,a,c,c) ones, which were already plain triangles, and left the real ones alone.
  h017b changed nothing that matters, and is harmless.
- Retail check: 0 primitives with i1 == i2 == i3 in 3,652,299 primitives of 297 retail u3 PS4 paks with BG collision
  (whole u3data pak23 scanned; 156 paks gave a parse error in the scanner, the same packfile variant, none of them maps we ship).

## Scan of everything installed (overlay-r3 > r2 > port > mp, v6.47 state, plus h017 and h018)
- h018 water paks: clean. h017 paks: clean. h018 coop-syria-area1-low: 4.
- Borneo: dig-coop-approach 8, -dropdown 3, -ship 1, -stage 2. These are exactly the paks Borneo has open when it freezes.
- Other maps (NOT in this batch; see ../h017c-other-maps-later): 136 primitives in 14 paks.

## Fix
- `patches/*.json`: one u32 per primitive, (a,b,b,b) -> (b,a,b,b). That is the same zero-area triangle with i1 != i2, so Havok
  takes it as a TRIANGLE. No other byte changes.
- dig-coop-* have two accepted states: after batch102 (the PC now) and before batch102 (from inbox/b102). The written values are
  the same; the offsets differ. If batch102 is installed later, install it first and then h017c, or batch102 will skip these 4 paks.
- coop-syria-area1-low: locked to the h018 file (2776d1cf...).
- Verified on the PC (read-only, temp copies): each post-102 dig pak reaches md5_after, and a rescan finds 0 CUSTOM primitives.
  The same for coop-syria-area1-low in the container.
- Install: `python3 apply_patches.py "<work>/overlay-r3/u3data/build/main/pak23/ps4" <backup dir>` (`--dry` first; `--revert`).
- Converter: paktools/collision.py now rotates (a,b,b) to (b,b,a) (all 64 index patterns of 0..3 checked: all TRIANGLE).
  `U3_COLL_ALLOW_CUSTOM=1` restores the old output.
- Tools: `tools/bvscan_c.py` (scanner, the correct rule), `tools/verify_c.py` (applies to a temp copy and rescans).

## Test
1. Install h017 + h018 + h017c (game stopped). Custom Co-op Adventure Borneo Ch.1: walk, turn the camera, run to the dropdown,
   play 3+ minutes. Expect no freeze (before: four freezes in five runs, +15 s to +44 s).
2. If it still freezes: take u3_threads dumps again. A worker on 0x12fce80 again would mean a CUSTOM primitive I did not find;
   any other RIP is a new cause.
