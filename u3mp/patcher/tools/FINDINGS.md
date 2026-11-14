# PS3 -> PS4 pak conversion findings (U3, pak version 0x0a79)

Source: pak-conversion research agent report (2026-10-26). Nothing here has been tested in-game yet.

## Key correction
- PS4 UI icons are looked up in a **material table** keyed by `sid(material name before ',')`, not by texture hash.
- `0x11b7600` sets UI+0x28 = level("net")->+0x150 once that level is in state 8, and UI+0x30 = level("menu")->+0x150. The icon code (0x7521a0, and the inlined f_97202c) scans UI+0x28.
- level+0x150 is built only from a GEOMETRY_1 item's materials bound against an EFFECT_TABLE. PS3 MATERIAL_TABLE_1 is ignored by the PS4 loader.
- So a PS4 net.pak needs EFFECT_TABLE + TEXTURE data + GEOMETRY_1 (materials), with login order EFFECT_TABLE, TEXTURE_TABLE, GEOMETRY_1.
- Icon ids are sids of PS3 material names, e.g. 0x7e2c2757 = sid("general/net/mode-buttons/customgame").

## Container
- Header (u32, file endianness): 0x00 u16 magic 0x0a79 + u16 flags; 0x04 hdrSize; 0x08 loginPage; 0x0c loginOffset; 0x10 pageCt; 0x14 pageEntryOff=0x2c; 0x18 numFixupTables=8; 0x1c fixupDirOff; 0x20 vramSize; 0x24/0x28 VRAM desc table page/offset; 0x2c PageEntry[] {fileOff,size,x}.
- Fixup entry {u16 srcPage, u16 dstPage, u32 off}: `*(page[src]+off) += base(page[dst])`.
- Page header: PS3 0x10 bytes `{u32 link,u32 size,u32 flags,u16 ?,u16 n}`; PS4 0x14 bytes `{u64 link,u32 pkgIdx,u32 size,u16 ?,u16 n}`; then n x 16-byte entries `{u64 name,u32 itemOff,u32 0x12345}`.
- Item header 0x20: `{u64 name,u64 type,u32 size,0x12345,u32 nameSid,u32 typeSid}`; the loader (0xdd4620) rebases the name/type pointers itself (not in the fixups).
- sid = CRC-32, poly 0x04C11DB7, MSB first, init 0, no final xor.
- PS4 page 1 holds only the login table (at 0x30) and the VRAM desc table, both unlisted; there is no VRAM_DESC_TABLE item type on PS4.

## Texture descriptor
| PS3 | field | PS4 | field |
|---|---|---|---|
| 0x04 | vramOffset u32 | 0x08 | vramOffset u64 |
| 0x08 | vramSize | 0x10 | vramSize |
| 0x0c | hash | 0x14 | hash |
| 0x14 | GCM format (06 DXT1, 08 DXT45, 05 ARGB8) | 0x1c | DXGI (0x47 BC1, 0x48 BC1 sRGB, 0x4d BC3, 0x50 BC4, 0x53 BC5) |
| 0x1c | mips | 0x24 | mips |
| 0x20/0x24 | w/h | 0x28/0x2c | w/h |
| 0x30 | path ptr | 0x38 | path ptr |
- PS4 +0x20 bytes {0,0,0x0f if sRGB,1}; +0x34 streamFlags (2 world, 3 UI).
- hash = sid(short path), same scheme on both.
- Texels: PS3 linear DXT, mips packed. PS4 per mip padded to 8x8 blocks, 8x8-block tiles row-major, Morton order inside a tile. Pure block shuffle, no re-encode.

## GEOMETRY_1 v0x10 (PS4)
- +0 version 0x10, flags (bit0 foreground); textures +0x38/+0x40 (0x28 each); materials +0x48/+0x50 (0x48 each: name, shader, params, effectHashHex, 0, bindings, nParams, nBind).
- Binding 0x28: {param ("g_tColorMap"), shortName, fullPath, i32 -1, u32 runtime, 0}.
- Background-level path: 0x10f6700 -> CreateGeometry 0xf48060 -> job 0xf52e70; foreground levels use 0xf534d0 (a mismatch traps).
- EFFECT_TABLE v7 {n, 7, entries[0x18] = {EFFECT*, fxIdx, u64 hash}}.

## Converter
- `convert_net.py ps3/net.pak out/net.pak --ref ps4/menu.pak --ingame12 ps4/menu-ingame12.pak out/net-ingame12.pak`
- `ps4check.py out/net.pak --want 7e2c2757 23e9be55 b675afdb [--png DIR]`
- Options: `--effect logo|edge`, `--foreground`, `--linear-color`, `--stream-flags N`.
- Output: 423 textures, 296 materials, effects copied from PS4 menu.pak (menu-logo effect `hud/textured-2d.fx`).

## Open
1. Runtime test (zero-mesh geometry, reused effect, TEXTURE_TABLE consumer unknown).
2. "net" level entry in the PS4 level list (level-def ReadTxtFile 0x1294bb0, packages via RequestPackage 0xdd6d10, path `%spak%i/%s.pak`); must load as a background level.
3. 171 materials used a PS3 shader with a different vertex program; small tint differences possible.
4. sRGB vs linear colour maps (`--linear-color` if wrong).
5. MP HUD icons (e.g. 0xc1df5e2e) need a merged PS4 hud.pak.

---
## 6. Merged PS4 hud.pak (multiplayer HUD materials) — `merge_hud.py`, output `out/hud.pak`

### Finding: the HUD materials are not missing, only their names are
* Retail PS4 hud.pak: 2 pages, 205 materials (all named `dir/file:variant,<guid>`), 205 EFFECTs, 347 textures. It still passes `ps4check`.
* **All 185 PS3 hud.pak materials have a PS4 equivalent.** Replacing the last `/` with `:` in the PS3 name gives the retail PS4 name, and no PS3 material has more textures than its PS4 version.
* The multiplayer code still searches the hud table (UI+0x20) for the **PS3 slash-form sids**. Example, PS4 0x62ea60:
  * the linear search at 0x62eb50 compares against `0xc1df5e2e = sid("general/hud/store/purchased")`
  * retail hud.pak only has `general/hud/store:purchased` = 0x658daf73
  * so the search returns -1 and the icon is blank.
* Slash-form hud/menu sids used in PS4 code:

  | sid | material | used at |
  |---|---|---|
  | 0xc1df5e2e | store/purchased | 0x5e7fc0, 0x62ea60, 0x62ef70, 0x62fb60, 0x630580, 0x76bbf0 |
  | 0x6e9d9193 | gradient-bar | 0x5da090 |
  | 0xd41f59a1 | medal-sheen | 0xc24df4 |
  | 0x7f89e267 | up-button-pulse-big | 0xc45e20 |
  | 0xd75be949 / 0x576ed245 | mode-buttons question-mark / notification | 0x64c510 |

  The ELF also contains store/locked and trophy outline/solid/blank sids.
* Material lookup `0x11b89b0(u64 key = sid | table<<32, index)`:
  * table 1 = UI+0x28 (net) and 2 = UI+0x30 (menu), both searched by sid;
  * table 0 = UI+0x20 (hud) is accessed by index only; callers find the index by a linear sid search (e.g. 0x62eb38).

### What the merge does
* Pages 0 and 1 and the VRAM data are copied from retail **byte-for-byte**. All 12630 retail fixups, the item list, names and sids are unchanged.
* The only retail bytes changed are GEOMETRY_1 data +0x48 (material count 205 -> 390) and +0x50 (material array pointer). The pointer now targets page 2; its fixup changes from dst page 0 to 2.
* New page 2 holds 3 RAW_DATA items:
  * alias name strings;
  * private copies of the binding arrays (the runtime writes the texture index/hash into bindings);
  * the material array: the 205 retail entries in their original order, so indices are stable, plus 185 aliases.
* Each alias = the retail colon material's shader, params, effect hash and bindings (pointing at retail strings and textures), under the PS3 name. Aliases are built only for PS3 sids that are absent. `--all-slash` also aliases the 20 PS4-only (SP/photo-mode) materials; this is not used by default.
* No new textures or effects are needed; all of them already exist in the retail pak.
* **One GEOMETRY_1 only.** A background GEOMETRY_1 login (0x10f6700 / 0xf48060) reuses level+0xf0, and ConstructFromBoundMaterialTable (0xe4bf60) asserts `m_numMaterials == 0` and `m_aMaterial == NULL` on level+0x150. A second GEOMETRY_1 in the same level would trap, so the material array is extended instead.
* Login order (EFFECT_TABLE, TEXTURE_TABLE, GEOMETRY_1, TAG_FLOAT) is unchanged.
* Output: `out/hud.pak`, 29,886,064 bytes (retail 29,811,360), 3 pages, 15681 fixups.
  * `ps4check`: OK, 390 materials and 778 bindings resolved.
  * Checked by script: the first 205 materials are identical to retail, and all 185 aliases match their source except for the name.

### Emblem quad material
* The PS3 emblem renderer 0x27aa4c (spray-01 / emblem-16-drips) picks one of two materials, both in **hud.pak**:
  * `general/hud/alpha-mask-icon/default` (0xa0cf6221) when a layer has a mask;
  * otherwise `general/hud/alpha-only-icon/default` (0xd2e394f5).
  * The key is built via 0x97202c and the lookup is 0x971f64.
  * It then draws via 0x96b0c4 with the layer texture (r6 = tex+16) and an optional second texture, the mask (r7 = tex+16 or 0).
* PS3 texture slots:
  * alpha-only-icon: {default-color-white, default-trans}
  * alpha-mask-icon: {white, default-trans, <unbound runtime slot>}
* PS4 retail has both under colon names, and neither sid occurs in the PS4 ELF, i.e. the PS4 build removed the PS3 emblem renderer:
  * `alpha-only-icon:default` (0x4edc816b): ND_MENU3D + ND_TRANSPARENCY; ColorMap + TransparencyMap
  * `alpha-mask-icon:default` (0x3cf077bf): ND_MASK, ND_MENU3D, ND_TRANSPARENCY; Color + Transparency + MaskMap (bound to default-trans)
* The material with a single texture is `alpha-icon:default` (0xf518a1aa, ColorMap only, ND_TRANSPARENCY2), used by photo mode 0xc3f840. It is the likely cause of the earlier "missing second texture" crash.
* The merged pak now also answers the PS3 sids 0xd2e394f5 and 0xa0cf6221. An emblem helper should use those (or the colon sids) with table 0.

### Risks
* Not tested in game.
* The 185 aliases add 185 bound-material / effect-binding jobs at hud login. They are small, but memory use rises.
* Aliases share the retail param lists and effect strings, which are read-only. Binding arrays are private copies.
* The hud table on PS4 is reached by index. Retail indices 0..204 are preserved, so any cached index stays valid.
* The overlay must shadow the retail `pak23/ps4/hud.pak` with this file; hud-ingame12.pak is unchanged.

---
## 7. Boot crash with merged hud.pak, and `out/hud-min.pak`

### What the crash actually is
* The fault at 0x12c4917 is `movzx edx, WORD PTR [rsi+2]` with `rsi = *( *(0x1b24b80+0x48) + 0x20 )`, i.e. the **hud material table pointer (UI+0x20) is NULL**. It is not a NULL object from the 0x2c80 allocation: rbx is cleared by `xor ebx,ebx` at 0x12c4903, and rax = rdi = 0xe887da64 = sid("general/hud/bp-speedrun/bp-clock:default"), the lower-cased string it was about to search for.
* Boot order (0x569ac0):
  1. 0x56b1ea calls 0xc18900, which requests misc-fx, misc-fx-fma, decals, particle*, particle-override and then **hud** (callback 0xc18df0).
  2. 0x56b1ef calls 0x12bdef0 (Bluepoint UCC tweaks). It allocates 0x2c80 bytes and calls 0x12c4420, the speedrun clock init. That function unconditionally searches the hud table for bp-clock.
* 0xc18df0 runs only when the hud level is loaded and its level+0x150 table exists. It hands the table to the UI object (0x1805fa0 vtable+0x10 — this is what fills UI+0x20) and resolves pickup-indicator ids via 0xe4be10.
* With retail hud.pak the table is set by then. With the merged pak it was not, so **the hud level did not finish loading/logging in before this point.**

### Most likely mechanism: login-data allocations
* Everything allocated during login and the geometry/material jobs comes from the per-package `LevelMemAlloc`:
  * 0xdeba10 pushes it;
  * Allocate 0xdeb5e0 calls 0xdd7910, which calls Package::AllocateLoginData 0xdd9d20.
* AllocateLoginData first uses free tails and adjacent free 32 KiB sub-pages of the package's own loading-heap pages. Failing that, it takes a free run elsewhere or a whole 2 MB heap page (0xdcf3b0). If that also fails it asserts "Level login allocation has failed" (allocator-levelmem.cpp:0x4e) and traps (int 0x41).
* Per-package page slots (0x4b0) and the shader pool (dedup by hash, capacity 0x2000) are not limits here.
* Login data grows linearly with material count:
  * 0xf63000: 0x70·N bound materials (43,680 B for 390, i.e. more than one sub-page) and 0x20·N;
  * 0xe8e5a0: per material and technique 0xa0+0x90+0x90 bytes plus an ndgi object;
  * 0xe4bf60: 0x28·N plus per-material texture lists.
* The merged pak added 185 materials and a new 50 KB page at boot, when several packages compete for the loading heap. A failed or trapped login leaves hud unloaded, so UI+0x20 stays NULL and the Bluepoint init faults.
* Not runtime-verified. If the log shows "Level login allocation has failed" or "Allocation failed: ... LevelMemAlloc", that confirms it.

### `out/hud-min.pak` (29,834,848 bytes, 2 pages)
* **14 materials renamed in place:** ':' becomes '/' in the material name string *and* in the matching submesh material-name string (same length, so no layout or size change). These are the ones whose colon-form sid is referenced nowhere in the ELF or the staged DC files (net / menu / modules):
  * gradient-bar, up-button-pulse-big, medal-sheen
  * trophy outline / solid / blank
  * mode-buttons question-mark / notification / warning / check-mark
  * flag-icons/treasure, treasure-chest
  * alpha-only-icon (0xd2e394f5) and alpha-mask-icon (0xa0cf6221)
* **2 aliases:** `general/hud/store/purchased` (0xc1df5e2e) and `store/locked` (0xea36b17f). Their colon sids 0x658daf73 / 0x7a8f9351 are still searched by PS4 code at 0x5b2740, so both names must exist.
  * The material array (205 retail entries in order, plus 2) is appended **to page 1**. It grows from 0xb80 to 0x4695 bytes, still inside its single 32 KiB sub-page, so no new loading-heap sub-page is needed.
  * GEOMETRY_1 +0x48 goes from 205 to 207, and +0x50 points to page 1. Page 1 is not freed after login: the loading-heap free calls are header/FreeLoginPages, ReleaseLoadedVramPages and Unload.
* **Retail delta:** page 0 has 28 renamed bytes plus the 2 geometry fields; page 1 has the appended data plus its size field. VRAM and all retail fixups are identical except the redirected +0x50. All 185 submeshes still resolve to a material.
* **Extra login data:** 2 materials (about 1% of the hud set). Bound-material array 0x70·207 = 23,184 B, which fits in one sub-page.
* `ps4check`: OK, 207 materials and 413 bindings. All 16 slash sids and both colon store sids resolve.
* **Remaining risk:** a colon-form name referenced by data that was not staged (hud DC, other `bin.psarc` modules) would no longer resolve for the 14 renamed entries. If that shows up, move that sid to the alias list (`merge_hud.py --minimal`, `MIN_SIDS`).

Tools:
* `merge_hud.py --minimal --scan <elf> <dc bins>` builds hud-min;
* `--min` aliases MIN_SIDS by appending to page 0, which adds 1 sub-page;
* the default mode is the full 185-alias build that crashed.

## 8. What is missing on PS4 (inventory, 2026-10-26)

- Actor packages: PS3 disc + 1.19 patch + DLC have 2758; PS4 has 2138. **679 missing**: 190 disc (custom-kit parts, co-op characters, accessories, MP map props/anims), 294 patch13 (1.13 accessories, cowboy kits, shirts, treasure items), 135 patch18 (masks/hats/shirts + 45 `multi-celebrate-*` taunts), 60 DLC map props/anims.
- Levels: PS4 keeps only MP sky domes, cargo-plane and a few `-multi` stubs. **All base MP maps and co-op maps are missing** (144 level definitions, 223 of 254 MP packages).
- Taunts: PS4 net.bin `0xb2b6e512` has the 15 disc taunts ({u32 anim sid, u32 text, u32 icon}); 1.19 has 44 (anim name as string). The 15 disc taunt anims are inside PS4 `anim-proto-multiplayer.pak`.
- MP hero names in net.bin: PS4 119, 1.19 330 (accessory variants).

## 9. PS3 foreground mesh format — decoded and verified (`ps3geom.py`)

Verified against PS4 versions of mp-hero-ramses-bandana, mp-kit-base-head-skin-a and mp-hero-talbot-torso: 42 segments, positions (<1e-4), UV, normal, tangent, triangle lists (exact) and skin weights (exact) all match. Vertex and triangle order are identical across builds.

- PM_FG_PROTOTYPES -> prototype (+6 u16 submesh count, +0x14 block table). FOREGROUND_SECTION_2 item -> prototype.
- One block per PS3 segment; PS4 concatenates consecutive segments of the same material into one submesh (in block order).
- Attributes are separate MSB-first bit streams. Fixed point `0x4N`: `a*(1+v*2^-21)+b`; `0x6N`: `a*(1+(4v+I)*2^-23)+b`; signed normalised `0x0N`: `v*2/(2^bits-1)-1`.
- Index compression: header {bits, nExplicit, nSlots, start}; 2-byte aligned streams: explicit indices, 2-bit configs (0: (p0,p2,new), 1: (p2,p1,new), 3: three new), 1-bit slot flags (0 = running counter).
- Skinning: palette = concatenated {first joint, count} ranges; vertices in groups of 4; per-group code byte (4/6 shared list, 2/10 per-lane rows (lane order 1,0,3,2), 0 repeat previous group, 8 new weights, 6/8/10 carry u8 weight rows); joint entries are `palIndex*48 (+4 = more follow)`.

PS4 target layout (GEOMETRY_1 v0x10, foreground): submesh 0x88 {name, flags, nverts, nindices, nstreams, streams*, index RAW_DATA*, material name, skin*}; stream 0x18 {u16 1, u16 stride, u32 size, fmt*, data*}; fmt {slot, 0, size, type (6 float3, 0x22 half2, 0x0a half4)}; slots 0 pos, 1 uv0, 2 normal(+w 1), 3 tangent(+w sign); skin 0x28 {0, total, counts u16*, offsets u16*, joints u16*, weights f32*}. PS4 adds a `lib-shadowblocker` submesh per prototype (PS3 has it too).

## 10. MP actor batches (assetbatch.py), hero/unlock tables, weapon skins, blocked formats (2026-10-26)

- Driver `assetbatch.py <batchNN> <jobs.tsv>` runs ps4actor/ps4rename, then ps4verify, ps4check (it ignores "unknown item type" and the env-reflection tiled-size warning that retail paks show too), and an item-type diff against the template. It publishes to outputs/assets/batchNN and appends READY.tsv.
- ps4actor changes:
  - `PS4ACTOR_SPREAD_MATS=1`: each PS3 material gets its own template slot, and hair materials go to the GLOBAL_HAIR slot.
  - `PS4ACTOR_DONORS=a.pak:b.pak`: textures the PS3 pak lacks (the hair `-trans` is usually a PS3 missingtexture) are copied from retail paks.
  - New texture kinds `-shift` (NoiseMap) and `-trans` (HairAlphaMap).
  - The page-0 entry cap fixes the effect span pinned behind a too-long entry table. ps4writer now raises an error there instead of padding forever (seen with pirate-armor-a-full-mp).
- Good templates for hat+hair kits: hero mp-hero-rescuer-hair (hair + hat slots); chloe mp-chloe-u2-legs (2 cloth slots, since no retail chloe pak has hair + accessory).
- net.bin tables (PS3 patch18 / PS4):
  - hero table 0xbd3bb3b9 (PS3 79 x 0x3c, PS4 57 x 0x70): {name, sid(name), sid2, slot ptrs @+0x18..+0x40, "proto", ...}.
    - slot = {count, ptr part-symbol sids, slot sid};
    - part symbol = {count, entries 0x2c/0x38 {sid, char* artgroup, ...}}.
  - unlock table 0xe2e8998e (PS3 0x1c, PS4 0x28 per entry): {itemId, category (1 kit, 2 hero, 4 weapon skin, 8 taunt, 9 emblem part), sid, price, ..., ptr conds @+0x18}.
    - conds = {count, ptr {u32 type, u32 sid, u32 value}}: type 1 = rank (sid 0x73baf4e0), 4 = flag sid, 6 = DLC pack index, 7 = ?.
  - weapon skins: 0xe77d8fba (long guns) and 0xa94f33c0 (p99) {sid, char* class, idx, ?, char* material path, ?, sid}.
  - 0xc56be227 = sid("multi-celebrate-laugh") (taunt).
- `wpnmods.py`: appends alias weapon-skin materials (clone of a retail sibling plus a PS3 colour texture) to a retail weapon pak. Retail bytes and indices are kept, and EFFECT_TABLE entries are appended (materials and effects pair up by index).
  - PS4 net.bin has no hk416-mod/jeremy entry in the skin table, so that skin also needs a DC entry.
- Blocked formats:
  - ANIM. PS3 ANIM = {6, n, ptr clip, ptr float-channel table, 0, skel sid}.
    - The float-channel table is a plain endian swap.
    - The clip is "CD\x01\x07" (BE) on PS3 and "AC\x01\x04" (LE) on PS4: similar header and sections, but relocated and much larger on PS4 (tense-run 6.8 KB -> 76 KB).
    - 605 matched pairs exist in anim-proto-multiplayer.pak (ros3 PS3 vs retail PS4) for RE.
  - COLLISION_DATA_FOREGROUND/BACKGROUND: Havok, PS3 96 B vs PS4 1872 B for propane-tank.
  - JOINT_HIERARCHY with more than 1 joint: no converter. graft only works when PS4 has a same-skeleton twin.

## 10. Background stage levels (`stage/ps4stage.py`, rosetta: ldn-train-stage, san-upper-multi-stage)
Converter verified against both retail stages with `stage/stagecmp.py` (geometry, instances, prototypes,
light table byte-exact; only cosmetic per-instance stream order differs).  Rules found:
- Signed-normalised (0x0N) attributes: nominal depth = min(type depth (normal/tangent 10, colour 8),
  widest normal/tangent 0x0N field of the block, 8 if none); narrower fields drop high zero bits.
  Instance constant 0x4e (0x0N) is unorm with its own widest field (k/3 for 2 bits). Colour unorm.
- PS3 prototype header byte0 = LOD count; PS4 keeps LOD0 only (groups until the first repeated material).
- Prototype flags[16]: [0] skinned (hdr byte1 0x02), [1] byte1 0x04, [2] byte1 0x08, [6]/[7] hdr+0x38
  bits 0x01/0x02, [10] bit 0x20.
  - flag[1] = draw-distance fade (agent F3, 2026-10-29). PS3 hdr+8 = u16 fade start, u16 fade end (metres). PS4 prototype
    +0x48 -> 16-byte RAW_DATA {f32 start, f32 end, 0, 0} (retail ldn-train-stage P9 260/265, P15 45/48, P18 40/45; instance
    builder 0xe9b3b5 copies it to inst+0x108; flag[13] would use floats [2]/[3]). With +0x48 NULL the engine uses 0 / 1e-8
    (0x144b1e0) = never drawn: the Fort "white void / floating pieces" bug. ps4stage writes it since F3 (.pre-fade backup);
    built paks: aga/maps/b94/fadefix.py / apply_fade.py.  cbuffer capacity +0x30 = 3 if any material has TRANSPARENCY_ALPHA_BLEND.
- Instance / instsub AABBs are prototype-local (skinned prototypes: AABB of the posed vertices).
  instsub +0x20 = 0x100 when the instance has its own lightmap UVs.
- Lightmapped stages bind every g_tLightMap / g_tLightMap1 to the stage's indirect.rgbmax /
  indirect.rgbmaxdelta (short name stays default-lightmap0/1); geometry texture entry +4 = 0x1000
  for lightmaps (0x400 typical), +0x14 = format class (BC1 4, BC3 5, BC4 3, BC5 6).
- TEXTURE_TABLE names are the short path incl. '/<HASH>.ndb' (sid(name) == hash).
- LIGHT_TABLE_1 full mapping in `convert_light_table` docstring (local lights 0x48 records, 0x3c
  records, sun, gobo list with texture-name pointer); byte-exact on 5 rosetta pairs.
- Retail stage VRAM is ~1.5 MB (64x64 resident mips, rest streamed); PS3-only textures are capped
  (U3_STAGE_MAX_TEX, default 256) since they have no stream.
- `stage/stagepipe.py` runs mkplan -> fxextract -> ps4stage -> ps4ingame -> stagecheck per stage.

## 11. Animations: PS3 ICE clip -> PS4 OrbisAnim clip, `anim.py` (2026-10-26)

**Status: solved.** `anim.py <ps3.pak> <out.pak> --check` converts whole anim packages.
- Validated against 664 clips in 9 rosetta paks that exist on both platforms: anim-proto-multiplayer (605), cha-wall-burn-doorway, loading-menu-cloth, propane-tank, capture-idol-gold, gold-treasure-ac, pistol-mp-base, silver-treasure-ae and turret-gatling-tripod.
  - Clip bytes are identical to retail, apart from 8 bytes of uninitialised converter memory at the end of the tail pad.
  - The clips cover 1, 2 and 4 groups and opcodes 0b 0c 0f 10 11 15 16 17 18 19 1a 1b 1c 1d 1e (0x17 added later, see below).
- Converting the whole PS3 disc anim-proto-multiplayer.pak and comparing item by item (pointers normalised) against retail gives:
  - ARTGROUP_DATA, LEVEL_PATS_1 and PACKAGE_ITEM_LOOKUP: exact.
  - ANIM items: same size and same pointers. The only difference is that PS3 4-byte channel padding is written as 00 where retail has aa.

### Clip format
- The PS3 "CD\x01\x07" clip and the PS4 "AC\x01\x04" clip (PS4 code: orbisanim/commandblock.cpp, executor 0xf9fcd0, handler table 0x16e0870) share the same 0x30-byte header. Only endianness differs: u32 fields are swapped, and u16 pairs stay in order.
  - +08: total size
  - +0c: u16 group count
  - +10: keyframe-bitmask offset
  - +14: group table offset
  - +1c: fps
  - +20: u16 flags
  - +22: u16 frame count
  - +24/+28/+2c: floats
- Group table entry, 16 bytes: {u32 block offset, u16 block size, u8 joints, u8 floats, u32 frame-data offset, u16 bytes per frame, u16}.
- Masks:
  - On PS3, each group block starts with 16-byte channel masks (one for joints if b6 != 0, one for floats if b7 != 0, stored as BE u64 pairs). The command list follows them.
  - On PS4, all masks move into one table after the group table, and the block offset points directly at the commands.
- Commands: `u16 (argc<<8)|op` followed by argc u16 args.
  - arg1 is the evaluation context location: PS3 0x000c becomes PS4 0x0003 (location = offset | base index; base 0 = command list, base 3 = context).
  - arg2 is the const-data location in base 0. For op 0x1a it is unused, and PS4 writes 0.
- Const data: each region is re-emitted with the PS4 length (`_need`) and element swap (`_xform`). Regions are 16-aligned except the last one.
  - The source is the PS3 block, read as a buffer that runs up to the next group (or the first frame data) and is treated as 0xff beyond that. The PS4 converter evidently read past short PS3 regions and filled missing entries with 0xff.

  | op | data | length | swap |
  |---|---|---|---|
  | 0b / 0c | u8 bit widths | n or 4n if bit 15 set, else 1 | none |
  | 0f / 10 / 11 | u16 dest index | 2·align(n, 8/16/4) | 2 |
  | 15 | u16 idx + f32x3 | 14n | 2 / 4 |
  | 16 | u16 idx + half3 | 8n | 2 |
  | 18 | u16 idx + 48-bit quat | 8n | 2 / bytes |
  | 19 | u16 idx + f32 | 6n | 2 / 4 |
  | 1b | 2x u64 range + u32 format | 16n + 4n (or 4) | 8 / bytes |
  | 1c | u32 format words | 4n (or 4) | bytes |
  | 1d / 1e | s16 | 8n / 16n | 2 |

- Frame data is MSB-first bit streams per frame (stride bytes) and is copied verbatim.
- Layout: header, group table, mask table, blocks (each 16-aligned), frame data, then 16 zero bytes per group.
  - A frame-data offset that pointed at the PS3 end of clip maps to the end of the data. If no group has frames, it maps to the end of the clip.
- PS4 ANIM item (one item holds everything): {u32 6, u32 ver, ptr clip, ptr channel table, u64 0, u32 skel sid, aa, ptr events}, followed in order by:
  - the channel table {n, aa, ptr sids, ptr per-channel ptrs, aa aa}
  - per channel {u16 frames, u16 type, aa, ptr data}, with data as u16-swapped halves
  - events {sid, count, ptr} with entries {sid, f32 frame, count, aa, ptr kv}; kv is u32 pairs, swapped
  - the clip, 16-aligned
  - PS3 keeps the channel table in a separate ANIM_RAWCHANNELS item and the clip unlisted on another page; PS4 drops MATERIAL_TABLE_1 and VRAM_DESC_TABLE.
- ARTGROUP_DATA is converted entry by entry (PS3 hash buckets kept), with the X struct floats -1/1 set to 0. This matches retail exactly.

### Clip opcode 0x17 (2026-10-26)
- 0x17 = constant quaternions: u16 dest index[n] + f32 quat[n] (PS3 BE), the 4-component twin of 0x15.
  PS4 length 18n, swap 2 (indices) / 4 (floats); same 0xff-beyond-the-block rule as the others.
- Rosetta: retail PS4 clips using 0x17 were found by scanning all 191 PS4 anim-*.pak (anim-cha-spider,
  anim-cruise-ship-a, anim-lost-desert, anim-san-talbot-knife-melee, anim-sullivan, anim-syr-crypt).
  Against the PS3 disc paks: 11/11 0x17 clips byte-exact (+ the 8 tail bytes), and all 594 other clips
  of those 4 paks too. Used by every clip of anim-mp-underground-train (train track paths).

### Streamed animations (STREAMING_BUFFER + igc-stream-*.stm) (2026-10-26)
Rosettas (both byte-checked item by item, pointers normalised): loading-menu-cloth (1 stm, 15 streams,
59 chunks; SB exact, 942 ANIMs exact apart from the known pad/tail bytes) and anim-coop-syria (4 stm
files, 6 lists, 19 streams; whole pak same size as retail, every item exact apart from pad/tail and
7 runtime words in the SB, see below). PS3 stms: animstream4.psarc entries 23-93 (93 = multi-loadout).
- A streamed anim has a header-only clip in the pak (group-table offset 0, size 0x30). PS4 keeps the
  PS3 magic 0x43440107 and only swaps the header fields (same struct as a normal clip header).
- Chunk 0 of each stream is a normal ANIM item `<anim>-chunk-0` in the pak (login + artgroup).
- `.stm` file = blocks {16-byte tag (one u32 x4), mini PS3 pak: 1 page, login = 1 ANIM `<anim>-chunk-N`
  with its clip}; blocks are chunk-major (chunk 1 of every stream of the file in file order, chunk 2...).
  The chunk size tables in the SB are these block sizes (incl. the tag).
- PS4 has no stm: every chunk 1..N becomes a listed ANIM item `<anim>-chunk-N` of the pak (converted
  like any ANIM; not in login/artgroup), reached only through the SB chunk table.
- PS3 SB body: {u32* listCount[nLists], u32** listSids, u32** listSlot, u32 fps(30), u32* fileChunks
  [nFiles], u32 bufferSize, u32*** chunkSizes[list][anim]->u32[fileChunks], char** fileNames[nFiles],
  u32* fileStreams[nFiles], Runtime*}. A "list" is a set of anims that play together (an IGC's actors,
  max 4 per list; a file with 6 streams has 2 lists). Runtime = {nLists, u32* a, 0, u32* b, 0 x4,
  recs*, 0 x4}; recs = 3 x {id 0/1/2, 0, 0, buffer*, u32[nLists]*}; rec 1/2 buffers = 2 x bufferSize and
  end the item.
- PS4 SB: same tree, natural 8-byte alignment, every object 16-aligned with 0xaa fill, objects in the
  order: header(0x50), listCount, listSids ptrs, listSlot ptrs, [sids_i, slots_i] per list, fileChunks,
  fileStreams, name ptrs, strings, chunkSizes ptrs, per list {ptr[count], tables}, Runtime(0x60:
  +0 n, +8 a, +0x18 b, +0x40 recs, +0x48 chunk table), a, b, recs (3 x 0x20 {id,0,0,aa, buf*, arr*};
  only rec 0 keeps its array, 8-byte elements), chunk table: ptr -> ptr[nFiles] -> per file
  ptr[fileChunks+1] -> rows of fileStreams item-HEADER pointers (row 0 = chunk 0: all null), then
  rec 0's array, then the 2 buffers. Sizes, sids and buffer size are copied verbatim.
- Runtime words: retail anim-coop-syria has Runtime+0x10 = 2 and b = {3,3,3,3,1,-1}; loading-menu-cloth
  and every PS3 SB have zeros there. The converter writes zeros (the only 10-byte difference).
- `anim.py <ps3.pak> <out.pak> --stm <dir with .stm> --check`; selfcheck re-decodes chunk clips from the
  stm and verifies the chunk table rows. The chateau txt lists 2 outro streams but anim-mp-chateau.pak
  has no SB (the outros are plain clips there), so nothing streams there; same for anim-mp-train-wreck.
- Events: an event whose key/value count is 0 now gets a null kv pointer (PS3 points at the next list;
  retail writes null; seen in anim-coop-syria). Retail also has 3 apm clips where an empty event table
  holds a stale unrelocated pointer; we write null.

### Not covered
- Channel data length is the PS3 distance to the next block. Any PS3 pad bytes are kept (retail
  writes aa there; harmless).

## 12. Background collision: PS3 ND mesh -> Havok 2011.3 hkpBvCompressedMeshShape, `collision.py` (2026-10-26)

**Status: converter written and verified offline. Not yet tested in game.**
- `stage/ps4stage.py` now emits COLLISION_DATA_HAVOK_BG automatically whenever the PS3 stage has a mesh. Set `U3_STAGE_NO_COLL=1` to disable it.
- `stage/stagecheck.py` accepts the item and re-decodes the packfile.
- Stand-alone use: `collision.py <ps3.pak> <out.hkx> --check`.

### Rosetta check: ldn-train-stage
The PS3 disc stage has 6477 triangles; retail PS4 has 6324 primitives (153 of them quads) = 6477 keys.
- PS3 materials = the retail trailer material list, same order, friction 1.0.
- The per-triangle material flags match retail for every matched triangle.
- Vertices match within the PS3 16-bit quantisation.
- PS3 per-triangle u16 = Havok welding code: identical to retail for 85% of triangles. The rest were recomputed by Bluepoint's 2011 tools; the PS3 codes are kept as-is.

### PS3 format
PS3 reader: f_8dc0e8 -> f_8db848 (shape 0x70 bytes, vtable 0x11143c0); query f_b506c8; vertex decode f_b4d3c0; index decode f_b4d8f0.
- **Item** (32-bit):
  - +00 version 6
  - +0c count
  - +21 u8 flag (PS4 reads it at +35)
  - +2c sid[count]
  - +30 u32[count]
  - +40 mesh pointer
- **Mesh:**
  - aabb f32[6]
  - +1c chunk count, +20 group count, +24 material count
  - +28 groups {first, count}, +2c group aabbs, +30 chunk aabbs, +34 chunk records, +38 chunk quant boxes, +3c materials
- **Chunk record** (16 bytes): {ptr data, 0, u16 numIdx, u16 numVerts, u32 material}.
- **Chunk data:**
  - indices: 21 x 6-bit MSB-first per 16 bytes
  - vertices: u16x3 BE, decoded as box.min + (max-min)*q/65536
  - one u16 welding code per triangle
- **Materials:** {0, u16 0 + bf16 friction, u32 flags}.

### PS4 format
- **Item, 64-bit:** the PS3 struct with its pointers widened.
  - +18 count; +35 flag; +50 -> u32[count]; +48 sid list (null in retail); +70 -> trailer {ptr mats, ptr packfile, u32 size, u32 count}.
  - Packfile at item+0x180, 16-aligned.
  - mats {u32 1, f32 friction, u32 flags}.
  - Loader: HavokLoginLevel 0x1133bf0.
- **Packfile:** "hk_2011.3.0-r1", 64-bit LE.
  - 3 sections: classnames hkClass / hkClassMember / hkClassEnum / hkClassEnumItem / hkpBvCompressedMeshShape.
  - The only object is the root hkpBvCompressedMeshShape (0xf0 bytes), with one virtual fixup at 0.
- **Shape layout:**
  - +28 convexRadius 0; +2c weldingType 0 (anticlockwise); hasPerPrimitiveCollisionFilterInfo / hasPerPrimitiveUserData = 1
  - +30 collisionFilterInfoPalette [1]
  - +40 userDataPalette (material flags)
  - +50 hkcdStaticMeshTree:
    - +50 top nodes (Codec3Axis5)
    - +60 domain
    - +80 numPrimitiveKeys / bitsPerKey / maxKeyValue (key = sec<<8 | prim<<1 | quadHalf)
    - +90 sections (0x60 bytes each)
    - +a0 primitives u8[4] (tri = a,b,c,c)
    - +b0 sharedVerticesIndex u16
    - +c0 packedVertices u32 (11/11/10 bits: offset + scale*q, codecParms)
    - +d0 sharedVertices u64 (21/21/22 bits over the domain)
    - +e0 data runs, 8 bytes each: {u32 value = filterIdx | userIdx<<8 | welding<<16, u8 first, u8 count, u16 0}
  - Reflection is in the PS4 image: hkClassMember arrays at 0x164c000.., class ctor 0x122c40.
- **Section** (0x60 bytes):
  - nodes (Codec3Axis4), domain, codecParms f32[6], firstPackedVertex
  - sharedVertices / primitives / dataRuns as (offset<<8 | count)
  - numPackedVertices, numSharedIndices, leafIndex (top-tree node), page / flags 0
- **Node boxes:** relative to the parent's decoded box, nibble k -> span*k^2/226 (high nibble = min, low nibble = max).
  - Codec3Axis4: data bit0 = internal; data>>1 = number of left-subtree leaves (right child = i + 2*that) or the primitive index.
  - Codec3Axis5: hiData bit7 = internal, 15-bit value.

### Writer
- Splits the mesh into sections of at most 127 triangles and at most 255 vertices.
- All vertices go in the shared table (21/21/22 bits, error ~1e-5 m) when there are 65535 or fewer of them. Otherwise only vertices on section borders are shared, which keeps the mesh crack free.
- Boxes are encoded conservatively against the runtime decode; every primitive lies inside its leaf box (checked).
- Checked on 32 PS3 level paks with collision (up to 51k triangles): all decode back exactly. Large maps are slow (pure Python, about 2 minutes for 50k triangles).

### Actor collision (COLLISION_DATA_FOREGROUND) is a different case
- It is a Havok *packfile* on both platforms: PS3 "hk_2010.2.0-r1" 32-bit BE vs PS4 2011.3 64-bit LE.
- Typical contents: hkpRigidBody + hkpConvexVerticesShape (+Connectivity) + hkpBoxMotion, sometimes hkpListShape / ConvexTranslate / Box / Cylinder, and hkd* breakables.
- The class signatures differ for RigidBody / ConvexVertices / List / Cylinder / ConvexTranslate, so this needs a reflection-driven packfile converter.
  - PS4 member tables are at 0x164c000.. (ctor args recovered by `coll/clsparse.py`).
  - PS3 has 24-byte BE hkClassMember records (about 2900 found).
- Rosetta pairs: propane-tank, capture-idol-gold, turret-gatling-tripod, cha-wall-burn-doorway, pirate-armor-all, ak74/hk416/machinegun/p99.

## 13. Actor collision: COLLISION_DATA_FOREGROUND, `hkconvert.py` + `fgcoll.py` (2026-10-26)

**Status: solved offline (not yet tested in game).** `fgcoll.fg_blob(pak, item)` returns the PS4 item Blob.
Check against retail: `fgcoll.py check <ps3.pak> <ps4.pak>` (retail mesh packfiles substituted, since the
BV tree builder differs). Self-check of the meshes: `fgcoll.py <ps3.pak>`.
- 22 retail pairs (5 light/, 4 weapons, 13 disc props staged from the PC: breakables, constraints, meshes,
  ropes, vehicles): 19 items byte-identical. The other 3 differ only in
  - hkpMotion.savedQualityTypeIndex (uninitialised in the PS3 source; 8 bytes, turret),
  - quaternion LSBs of a hkpConvexTransformShape (ship-piano, 3 bytes),
  - mesh material word 0, which retail sets to an unknown value when flag bit 16 is set
    (0x1841a700 -> 0x6c00000d, 0x08410313 -> 0x4c000001; otherwise 1, which we write; grave-col-buoy-tall).
- Packfiles alone: 127/128 byte-identical on the new pairs, plus 13/15 on the old ones.

### Havok packfile converter (hkconvert.convert_packfile)
- Objects are written in Havok packfile-writer order: depth first from the contents object, following pointers in
  PS4 member order (pointers inside arrays at the array's member position); each object's arrays and strings
  follow it, 16-aligned (strings too). Global fixups are emitted in that same traversal order (not sorted).
- PS4 member tables that are built at run time (empty in hk2011_ps4.json) are given in OVERRIDE4:
  hkpConstraintInstance (data 0x18, entities 0x28/0x30, priority 0x38, name 0x50, userData 0x58),
  hkdBreakableShape (physicsShape 0x10 / material 0x18 from hkpBreakableShape, typeAndFlags 0x20,
  majorAxisSpace 0x30, comAndMass 0x40, inertia hkHalf8 0x50, children 0x60, graphicsShape 0x70,
  objectProperties 0x78, userData 0x88; geometry is gone), hkdGraphicsShape (type 0x0c, numChildTransforms 0x10),
  hkpConstraintMotor.type 0x0c, hkpBvTreeShape.bvTreeType 0x18. Same-size pointer-free classes (constraint atoms)
  take the PS3 table; missing PS4 parents (atoms -> hkpConstraintAtom) are taken from PS3.
  Note: 2011 64-bit fields may start at +0x0c inside hkReferencedObject.
- 2010 -> 2011 model changes (transform_objects):
  - each hkdBreakableShape gets a new hkpSimpleBreakableMaterial {strength = shape strength, typeAndFlags 0x11}
    written right after its physicsShape;
  - hkpBreakableBodyController.breakingImpulse = -1.0;
  - hkpConvexTransformShape: hkTransform -> hkQsTransform (translation, quaternion, scale; unit scale snapped to 1.0)
    plus extraScale 0.
  - hkdBreakableBody keeps its rigid body, but it moves after the shape tree, because the 2011 member order is
    controller, breakableShape, physicsBody.

### ND wrapper (fgcoll.py docstring has the full table)
- item {ver 0x1004, nParts, parts**, nExtra, extras**, flags, nCons, cons**}.
- part {xf f32[8], joint, hk*}.
- hk block {size, type, trailer*, f32 x2, mesh* | piece count, packfile}. The type is PS3 3/5/1 -> PS4 2/4/0
  (body / breakable / constraint).
  - A body block's +0x24 may point to an ND collision mesh. This becomes a 2011 hkpBvCompressedMeshShape packfile +
    mats {1, friction, flags} + {mats*, pf*, size, nmats}, emitted before the block header.
- trailer (body) = 3 arrays (records 0x40 / 0x38 / 0x28, mostly floats with u16 pairs at fixed word indices) + 3
  bounding-sphere trees (node {sphere, kids**, u16 n, u16 tag}, kids inline, leaf tag = record index).
- breakable trailer = (piece count) records {trailer*, u16 pair, u32 bitfield}. The bitfield's bit order is
  reversed on PS4 (0x40000000 -> 2).
- Padding: 0xaa after array counts and part/hk pads, as retail. Item end is padded to 8.
- Constraint blocks (nCons) carry hkpConstraintInstance packfiles (ragdoll / limited hinge / breakable) between
  body indices (u16 pair).
- All 46 FG items of the props on the to-do list convert, and every mesh decodes back exactly (max error 3e-6).

## 14. Multi-joint skeletons: JOINT_HIERARCHY `skel.py`, ps4actor `--own-skel` (2026-10-26)

**Status: converter byte-exact on 17 retail pairs. Batch22 published: 55 map props. Nothing tested in game.**
- Retail differs only in uninitialised bytes: B+0x20 is a stale tool pointer, and the last 8 bytes of the blob are
  garbage.

### Skeleton blob: PS3 "JH\x01\x0a" -> PS4 "AH\x01\x06" (skel.convert_blob)
- Same data in a new frame. The PS4 header is 0x50 bytes:
  - +0c u16 nJ, u16 nJ; +10 0xff010000; +14 u16 0, u16 groups; +18 groups; +1c 1; +28 pose offset 0x50.
  - +30 inverse bind, +34 A, +38 B, +40 C, +4c D.
- Then, in order:
  - pose: nJ x 0x30 floats;
  - inverse bind: nJ x 0x30;
  - A (0x20): {0, 0, u16 0, u16 nJ, u8 g, u8 g, 0, 1, u32 nJ};
  - B: the PS3 per-group bytes {u8 count, 0, 1, group} + swapped u32 first joint, padded to 16;
  - C (0x10): {u16 0, u16 cmdSize, u32 cmdOffset};
  - the command list: u16-swapped. Only u16[2] changes, 0x000c -> 0x0003: the context location, as in the anim clips;
  - D: {u16 0, u16 size, u32 0x20 (sid offset), ..., +0x14 parent-table offset}, then sids (16-aligned) and the
    parent table (nJ x 0x10, u32 swap), then 0x20 zero bytes.
- The PS3 header positions are: +0c nJ<<16, +14 group bytes, +28 parent table, +2c pose, +44 inverse bind,
  +48 group table, +4c {cmdSize, cmdOff}, +50 sids.

### Item
- {8, nJ, blob*, B*, names*, 0, D*, skelSid}.
  - names: {sid, aa, char*}, strings 16-aligned with aa fill.
  - D: a constant template (PS3 is always {3, zeros}).
  - B: PS4 keeps only S* at +0x50; the PS3-runtime arrays are dropped.
  - S: {u16* per group, float** per group, u8** per group}.
    - floats: count = sum of the two bytes of the group's u16;
    - bytes: the group's joint count, rounded up to even.
- PS4 item order: header, names, strings, D, B, S tree, blob.

### Actor assembly (ps4actor --own-skel; assetbatch mode `own`)
- skeleton id = PS3 FG section +8; hierarchy id = blob hash; joint names = the joint sids; njoints = nJ.
- JOINT_HIERARCHY, ANIM (anim.py) and COLLISION_DATA_FOREGROUND (fgcoll) are converted from the PS3 pak. The template
  supplies only materials and effects.
- COLLISION_DATA_BACKGROUND / EMPTY and SKELETON_FLIPDATA have no PS4 item: the BG meshes live inside the FG
  collision.
- Fix that applies to graft too: ARTGROUP_DATA entries now point at the item headers, as retail does (the FG entry
  stays null). Before, every entry was null.
- Other changes:
  - `PS4ACTOR_SKEL3` / opt `skel3=`: LOD paks without a skeleton take it from the parent PS3 pak.
  - `notmpl` (PS4ACTOR_NO_TMPL_TEX): typed slots get flat textures instead of the template's. Untyped bindings
    (env maps, overlays) are still copied (force=True).
  - PAGE_LIMIT was lowered to 0x180000: tables filled after item() pushed page 0 past 0x1c0000.
- Templates used: rover-2d-civ-hvk (vehicles, train wreck, village buildings), crate-wood-1x1x1-a (fort/gompa/vil
  props), cloth-frontgate-b (flags, banners), crate-wood-1x1-break-ship, rover-4d-agent-hvk, san-elevator-main,
  cha-wall-burn-doorway.
  - pirate-armor-a-full-mp was done as a kit on the sol-armor-body-01 skeleton (0x2628a5dc).
- Limitations:
  - Materials map onto template slots (8 at most with rover), so props with more PS3 materials share slots
    (mp-wrk-crash-boxcar has 15).
  - The 64-texel cap (tex<=64) applies to the larger props.
  - Templates with big effect spans (syr-bldng-break-a, grave-pirate-ship) overflow page 0.

## 15. Level packages: NAV_MESH_1, FEATURE_DB_2, empty paks; DLC map Cave end to end (2026-10-26)

**Status: stages batch06 holds the full cave-multi package set (11 paks). Nothing tested in game.**

### What a playable MP map is
PS3 dlc-map1 cave-multi.txt lists `package cave-multi`, `package cave-multi-ingame` and a sound bank. The full pak set is:
- `<map>.pak`: main BG level, with VIS_INFO_1;
- `<map>-ingame12.pak`: spawners, regions, NAV_MESH_1 xN, FEATURE_DB_2, splines;
- `-stage`, `-waterfalls` and similar: sub-level BG paks, each with its own ingame12;
- `-pak`, `-comp-pak`: empty BG paks, plus an ingame12 with the mode-specific spawners/regions;
- `-killvis`: VIS_CHEAT.

Retail PS4 still ships one complete MP map, Sanctuary (san-upper-multi*). Its ingame12 has NAV_MESH_1 x21 + FEATURE_DB_2,
and its killvis is a 244-byte stub. That 244-byte stub is how PS4 ships every empty pak (~190 of them, with
uninitialised header words). Our empty paks are 248-byte valid PS4 paks with an empty login table.

### NAV_MESH_1 (navmesh.py): byte-exact on 27/27 retail pairs (Sanctuary, chateau, cargo plane)
- The 0xd0/0xe0-byte PS3 header is reordered into a 0xf0/0x100-byte PS4 header (table in the module docstring).
  - The two u16-pair words swap places: PS4 +0x08 <- PS3 w39 and PS4 +0xdc <- PS3 w10.
  - Fillers are 0x12341234.
- PS4 order: header, vertices, raw data bytes, name (aa-padded), polys, id bytes.
- Poly: PS3 0x50 -> PS4 0x60, made of 32 zero bytes + 4 vec4.
  - v0.w keeps its link bytes, but byte 2 is a flag byte with its bit order reversed.
  - v1.w is copied raw.
  - v2.w = the PS3 poly float; v3.w = the PS3 poly u16 pair.

### FEATURE_DB_2: exactly the FG-collision trailer3 layout (fgcoll.read_tr3/w_tr3)
- Cover/edge records 0x40, 3 sphere trees (empty trees have a NaN radius). 6/6 byte-exact, including Sanctuary's 350 KB one.
- ps4ingame now converts both (CONVERT_NAV). stagepipe reports only LEVEL_OFFSET_1 as dropped.
- Splines with content are still unsupported: the cave-multi ones are empty. Retail pair for later: cha-forest-drop-ingame12.

### Sizes
- cave-multi.pak: 58.2 MB (43.3 MB pages + 14.7 MB vram), against 53.8 MB on PS3.
  - PS3 geometry lives in VRAM, so its 13 MB of pages is not comparable to ours.
- Whole cave set: 70.2 MB (PS3 76.3 MB). Retail Sanctuary set: 74.5 MB (san-upper-multi.pak alone 64.8 MB).
  - Retail SP levels go up to 75 MB; the median level pak is 4 MB.

### Not converted yet
- VIS_INFO_1: occlusion. Retail pair available: cha-entrance-fire-low.
- The second COLLISION_DATA_HAVOK_BG of cave-multi.pak: ps4stage takes only the first.
- Non-empty SPLINE_TABLE, LEVEL_OFFSET_1.
- The level registration (package list / DC level definition). PS4 pak23 has no .txt manifests.
