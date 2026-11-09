# PORT-FRONTEND5: the PS3 multiplayer loading screen (trace) and the Profile image sizes (2026-11-09)

Static analysis only. PS4 1.02 image offsets; PS3 = GOTY NPUA80698 1.10. Run 43 rejected the
loading screen of PORT-FRONTEND4 ("very wrong"). This file is the PS3 trace that the port has to follow,
with the status of each element. The rejected version is now off by default (`loading.cpp`,
`UNCHARTED_U2MP_LOADING=1` shows it for comparison).

What was wrong with it (run 43 capture `1794188521436995300`, frames 15..18): the 2D arena takes
1280x720 menu units (the renderer's boxes in the log are already transformed), so the quads given in
1920x1080 units landed 1.5x too far: the map card filled the screen from (480, 75) and ran off the
right and bottom edges, and the band and all text were below the screen. And the layout itself was
invented, not PS3's.

No PS3 reference image of this screen is available locally (the RPCS3 screenshots are of another
title). A capture of the U2 PS3 loading screen (RPCS3 has U2 installed) is needed to check any port.

## 1. What PS3 draws

Caller: the per-frame net display `0x447248` (from the frame draw `0x76c80`; PS4 pair Draw `0x3fc0b0`)
runs `0x4adaf8(obj, f119c5)` and `0x4ae638(obj)` with the loading object `0xf1ff04`, while `0xf119c5`
is set: LoadLevels::Enter `0x4405c4` / WaitReady::Enter `0x43bd28` set it, LoadLevels::Exit
`0x438448` / WaitReady::Exit `0x443f00` clear it.

`0x4adaf8` (show / hide): on the first frame shown (obj+4 0 -> 1): `0x4ad600` (coin animation start)
and `0x4233c4(menu, PAGE_MULTIPLAYER_LOADING)` (page record +216 = 2: its lists refresh).

`0x4ae638` (draw, 0xad0 bytes), in order:

| # | PS3 | what | data | status |
|---|---|---|---|---|
| 1 | `0x4ae6a0..0x4ae6ec` | `0x158ff8(cdf70794, ee0494, obj+4, 30, 1.0)`: looks like a DC tunable read (default 30) | | callee not identified |
| 2 | `0x4aef94..0x4aefdc` | **background**: when `0x49e6e0(ngm)`, sprintf(`"loading-screens/%s"`, `"fort.dds"`) and `0x414968(menu+0x10, path)`: the gallery loader, `"%s/build/main/gallery1/%s"`, path at slot +444, request +180 = 1, +699 = 0, +700 = 10 | PS4 has the file: `uiextra.psarc: gallery1/loading-screens/fort.dds`; PS4 gallery loader inlined at `0x58b5c8` (slot `[widget+0x238]`: path +0x268, request +0x120, +0x367, +0x368 = 10; the widget gets the slot at construction `0x5c5fce` from `[rbp-0x158]`, most likely `menu+0x10` as on PS3) | traced, not ported |
| 3 | `0x4aeff8..0x4af06c` | obj+0 fade: += / -= frame time ([e60a00]+244), clamped 0..1 | | traced |
| 4 | `0x4ae724..0x4ae7ec` | colour transforms: identity `0x420f58`, copy `0x421548`, from packed colour `0x421018(0xff000000)`, compose `0x4212ec` (10 floats: 5 multipliers, 5 offsets); alpha = (obj+0 - 0.4) * 1.6667 clamped 0..1 | | traced |
| 5 | `0x4ae7f4..0x4ae850` | `0x4156cc(menu+0x10, xform, desc {type 0x1b, -1 ...}, rect {0, 0, 1280, 720})`: a full-screen element of descriptor type 0x1b | `0x4156cc` = PS3 item draw (one of the four callers of the converter `0x41fd18`) | pending: PS4 pair of `0x4156cc` |
| 6 | `0x4ae858..0x4ae92c` | `0x4ad124(out, obj, 0/1)` twice: the two moving element rects (motion: sin / cos of the clock, constants 0.4, 1.667, 1.2, 50, 0.75, 1.0 at `[r30-32264]` etc.) | | traced, not ported |
| 7 | `0x4af070 / 0x4af0bc` | `0x4adbd0(obj+32 / +868 / +1704 / +2540, rect)`: four panel records (836 bytes each) drawn from the gallery slot (`0x414ad8`) with transforms (`0x8b16f0`) | | pending: `0x4adbd0` (0xa68 bytes) not translated |
| 8 | `0x4ae944..0x4aead0` | `edge` (`6732636e`) twice: `0x8a015c` lookup, `0x8b1600` quad, `0x8b1598` submit: quad 1 at (0, 0) size 1280 x 720, quad 2 at (-10, -10) size 1290 x 730 (`[db2478]`, `[db2480]`) | PS4 net.pak `general/net/loading-screen:edge` | pending: quad API mapping (`0x8b1600` -> `0xbdce90`, to confirm against renderer case 3 on both) |
| 9 | `0x4aeb10..0x4aeb60` | **team rosters**: `0x423410(menu, PAGE_MULTIPLAYER_LOADING, alpha, pos)`: finds the page record, sets up a draw context, calls the page's update vfunc, then `0x408928` (layout) and `0x4062fc` (render) | PS4 menu.bin has the page (two action-23 lists, column set `dcae45f4`); PS4 page objects at `menu+0x370[i]` (id +0x10, 200); per-frame page draw `0x5d12f0` -> `0x5a6000(ctx, &desc, page, alpha)` + `0x5b48f0(ctx)` | pending: the draw context (rsi of `0x5d12f0`) |
| 10 | `0x4aeb68..0x4aec1c` | `title-back` (`270e3564`) band: quad at (128, 22) size 1152 x 278 (0x43000000, 0x41b00000, 0x44900000, 0x438b0000), alpha = fade x 0.75 (`[db2390]`): at the top of the screen | PS4 net.pak `general/net/loading-screen:title-back` | traced constants, not ported |
| 11 | `0x4aec24..0x4aed60` | **map name**: level record `0x499908` / `0x491f38` (+4 name text, TextDb `0x9294c4`), colour from DC `cac46a80` (`0x42159c`), text draw `0x4153d0` in the rect x 0, y 70, w 1280, h 100 (0x428c0000, r20 = 1280, r25 = 100), scale 1.4 (`[db2490]`) | | pending: `0x4153d0` -> PS4 `0x5b5450` argument mapping |
| 12 | `0x4aed68..0x4aee3c` | **game type** text: `0x49b5f0(ngm, buf, 128, rec, ngm+7220...)` then colour DC `f1758b4d`, `0x4153d0` in the rect x 0, y 120 (0x42f00000), w 1280, h 100 | | pending as 11 |
| 13 | `0x4aee44` | `0x4acc34(obj)`: the coin / money elements (`0x4ac9f4` x15, money `9595f0a7`, coins `b4b036fa`, started by `0x4ad600`) | PS4 net.pak `money`, `coins` | pending |
| 14 | `0x4aee4c..0x4aef08` | two filled bars `0x415294` from the display struct `0x10288a4` (+4, +8, +28; only when +36 < 719): letterbox bars | | likely nothing at 720p / 1080p; to confirm |

Mapping still needed before code: `0x4153d0 -> 0x5b5450` (match.pickle pair, arguments to align from
a PS4 widget call such as `0x57a8e8`), the other converter callers `0x4155a4 / 0x4156cc / 0x4157a8`
(PS4 `0x5b59ea / 0x5b5b2e / 0x5b5bd9` sites), the PS4 DC colour lookup equivalent of `0x3ec768`,
the gallery slot's draw (PS3 `0x414ad8` state 3, texture +184), and the page draw context.

## 2. Profile page image sizes

- Converter: same rule on both (size = texture size x scale, clamped to the caller's box); its four
  callers pass the scale from their own arguments on both (`0x4153d0 / 0x4155a4 / 0x4156cc / 0x4157a8`
  read `lfs f1,0(rX)`; PS4 `0x5b5603 / 0x5b599a / 0x5b5ac3 / 0x5b5b8f` read `[rbx] / [r15] / [rdx]`).
  The 0.5 for PS4 net.pak textures (PORT-FRONTEND4) does not reach the GOTY skins: their photos are
  net-u2mp (PS3 resolution). The Hero card of the carousel popup (run 43 frame 10) is about 390 menu
  units tall, more than any of these textures: the kind-8 widget passes its own scale or box.
- Run 43 has no size line for these images (the `draw type` lines come only from the ported types and
  were PAGE_MULTIPLAYER's party column). Added: every type 3 net image the converter sizes is logged once
  per (caller, id): `menu images: size id <id> from 0x<caller>: scale <s> box <w>x<h> texture <w>x<h>
  (PS4 net.pak | net-u2mp) -> <w>x<h>` (96 lines at most; converter `0x5bc290` prologue moved to a
  trampoline). Run 44 names the widget and its scale; the fix then goes at that call site, compared
  with its PS3 caller, as asked.

## 3. Port (2026-11-09, second pass): what loading.cpp now does

On by default again (`UNCHARTED_U2MP_LOADING=0` off; `UNCHARTED_U2MP_LOADING_TEXT=0` without the texts,
`UNCHARTED_U2MP_LOADING_ROSTERS=0` without the page draw). Everything in PS3's 1280x720 menu units.

Mappings resolved:

| PS3 | PS4 | how it was matched |
|---|---|---|
| colour transform (10 floats) `0x420f58 / 0x421018 / 0x421548 / 0x4212ec` | same layout | the PS4 widget composition at `0x57a80b` does `0x4212ec`'s arithmetic (multiply 0..4 and 7, add 5, 6, 8, 9); PS4 `0x5b5450` reads [0] scale and [8] [9] translation as PS3 `0x4153d0` |
| text in a rect `0x4153d0(ctx, xform, text, rect, mode, r8, flags, clip, byte)` | `0x5b5450(ctx, context, xform, text, rect, mode, [r8, flags, clip, byte, 0, 0, -1])` | match.pickle pair; argument order from the PS4 call `0x57a8a9..0x57a8e8` |
| descriptor in a rect `0x4156cc(ctx, xform, desc, rect, mode, r8)` | `0x5b5920(ctx, context, xform, desc, rect, mode, [r8])` | same structure (converter with box = rect size and scale = xform[0], then the item draw `0x5b56d0` = PS3 `0x414bb0`) |
| ctx `0xf0e810` | `menu+0x10` | the page-draw desc of `0x5d12f0` starts with `menu+0x10` and widgets pass `[desc+0]` as `0x5b5450`'s first argument |
| (no PS3 counterpart) | context = NULL | the renderer sends a NULL context to the deferred 2D arena (`0x5be5cd`), as menuimages.cpp's quads |
| MenuColor `0x3ec768` (table built by `0x3ebc18`) | values copied | `cac46a80` = 0xffcd4100, `f1758b4d` = 0xffdfcf8f (PS3 `AddColor` immediates at `0x3ebf8c`, `0x3ebfb0`) |
| game description `0x49b5f0(ngm, buf, 128, rec, ngm+7220, 0, 1, 1)` | `0x64abd0(ngm, buf, 128, rec, ngm+0x21b4, 0, [1, 1])` | match.pickle pair (`"%s - %s - %s"`), arguments as its PS4 caller `0x657ee2` |
| page in place `0x423410(menu, id, alpha, pos)` | page object `menu+0x370[i]` (id +0x10), `0x5a6000(context, desc, page, alpha)` | desc built as `0x5d12f0` does: `{menu+0x10, xform (alpha [4], position [8] [9]), {0, 0, 1280, 720} (0xe34290), frame / 300, menu state 1..2, menu+0x17a4}` |
| type 0x1b | renderer case `0x5c0c07`: a solid colour rect | |

Drawn, in PS3 order (constants from section 1): fade `fade += / -= frame time`, `a = clamp((fade - 0.4) x
1.6667, 0, 1)`; black fill (transform base o black) over `{0, 0, 1280, 720}`; the two edge quads,
colour a; the PAGE_MULTIPLAYER_LOADING rosters with alpha `a x clamp(t - 3, 0, 1)` and y `200 x (1 -
clamp((t - 2) x 0.6667, 0, 1))^2` (t = seconds since shown); the band `{128, 22, 1152, 278}`, colour a,
alpha `0.75 a`; the map name (base o scale 1.4 o MenuColor cac46a80, [5] = 3.0) in `{0, 70, 1280,
100}`; the game description (base o MenuColor f1758b4d, [5] = 3.0) in `{0, 120, 1280, 100}`. Show / hide
on all four PS3 points (LoadLevels::Exit added), plus Playing::Enter / Leave::Enter.

Left out (not resolved this pass; the cards are ported in section 7, the tip line traced in 7.6): the gallery picture `fort.dds` on the four perspective panels
(`0x4adbd0` x4, 0xa68 bytes of 4x4 matrix work, with the motion `0x4ad124`; the PS4 file and loader
exist, see section 1), the coin / money particles (`0x4ad600`, `0x4ac9f4` spawns, 15 slots), the tip
line (`0x4acc34`, DC `7f306129` tips), the PS3 page update call before the page draw and the list
refresh `0x4233c4`. Not verified in the game: the NULL-context page draw (`0x5a6000`) and `0x5b5920`
(no direct PS4 caller was found; it matches PS3 `0x4156cc` by structure).

## 4. Profile editor sizes (second pass)

The kind-8 control (rich rows / carousels, LIST_BOOSTERS, LIST_PREFERED_*) is class vtable `0x10072b0`
(control construction switch `0x5c5880`, case 8 `0x5c60ad`): update `0x58b410` (which also drives the
gallery slot `[this+0x238]`), draw `0x58b6f0`. Its direct converter calls are the panel (type 0xe, box =
desc +0x38 / +0x3c, scale 1.0 `[0xe61820]`) and the two arrows (type 6, scale = xform[0]); the item
pictures are drawn through the item list object (`[this+0x18]`, vtable +0x40). The call that sizes the
Hero card was not pinned down statically in this pass; the converter size log (section 2) stays and
names it in the next run.

## 5. Third pass: what 0x4adbd0 really is (not ported)

`0x4adbd0` is not a picture panel: it draws a **particle system**. The loading object holds four of
them (`obj+32`, `+868`, `+1704`, `+2540`, 836 bytes each: a count and up to 15 particles of 52 bytes,
spawned by `0x4ac9f4` with random positions from `0x90a900`, started by `0x4ad600`, moved by
`0x4ac7d8`). Per particle (`0x4ae0dc..0x4ae480`): a 4x4 transform is built (`0x3cf18` copy,
`0x3ee038` rotation by +8, `0x62e94` translation, `0x35780` multiplies, projection from `0xabbac0` /
`0x10644`), the four corners are transformed with AltiVec (`vmaddfp`), the UVs are rotated by the
particle's orientation (+36: 0..3), and the quad is submitted by its type (+24):
- 0: `0x8b16f0` corners + `0x8b1598` with the particle's own material (+12..+20: money / coins);
- 1: `0x414ad8(menu+0x10, 8, quad)`: texture slot 8 of the menu resources;
- 2: `0x414ad8(menu+0x10, 10, quad)`: slot 10 = the gallery slot, i.e. `fort.dds`.
So `fort.dds` is one more particle type of the same flying-card system, not a background. A faithful port
needs: the four systems' spawn / update / draw (`0x4ad600`, `0x4ac7d8`, `0x4ac9f4`, `0x4adbd0`), the PS4
gallery slot load (`0x58b5c8` pattern on `menu+0x10`) and its texture slots 8 / 10 (PS3 `0x414ad8`:
`obj+96+4*slot`, slot 10 valid when `+180 == 3`), the AltiVec permute masks (`v28` / `v30` are set
before this function, from the caller's frame) and a projected-quad draw on PS4 (`0xbdce90` takes
x, y, z, u, v per vertex: the corners can be projected on the CPU). Not done in this pass.

Also not done: the tip line `0x4acc34` and the list refresh. PS3 `0x4233c4` sets the page record +216 = 2;
PS4 `0x5cd2e0(menu, event, page)` is the page-event dispatcher, not that refresh, and the PS4 page object
field for it was not found.

## 6. Third pass: profile editor sizes (not fixed)

Kind-8 class `0x10072b0`: its update `0x58b410` calls the item list `[this+0x18]` vtable +0x40
(`0x58b5c2`, args (list, index, 4)) and uses the returned item to build a gallery path (`0x58b5c8`): the
kind-8 control can show gallery DDS pictures through its own gallery slot `[this+0x238]`. Its draw
`0x58b6f0` uses +0x40 only for the item texts (`0x58bba6`, `0x58bcc4` -> `0x5b5450`); the card picture of
the skin / booster carousels was not found in this class's direct calls (candidates: the other methods
`0x58b230`, `0x58b270`, `0x58b320`, `0x58b3d0`, `0x5a1320`, and the page's own image rows). The size
log of section 2 stays the way to name the call site.

## 7. Fourth pass: the card systems ported (loading.cpp, section "cards")

Corrections to section 5 (read from the code this pass): `0x4ac7d8` is not the update, it adds the black
frame; nothing moves the particles, a moving **camera** (`0x4ad124`) does; type 1 is the gallery
picture (slot 8) and type 2 a solid quad (slot 10 has no material); `v26..v31` are set inside
`0x4adbd0` (perm mask `0xaea7c0`, word masks), not by the caller; the orientation picks a quarter of a
2x2 sheet, it does not rotate the UVs. TOC base of all these functions: r30 = `0xdba260` (r2 =
`0xe183b8`).

### 7.1 Data

Loading object: +0 fade, +4 shown, +8 start (ticks of `[0xe60a00]+236`, 300 per second), +12 seed
(2 x rand), +16 tip index, +20 tip glyph counter, +24 / +28 tip time / duration, +32 / +868 / +1704 /
+2540 the four systems: `{s32 count; particle[16]}` (836 bytes), particle (52 bytes): +0 x, +4 y, +8
angle, +12..+20 material handle (`0x8a015c`), +24 type, +28 w, +32 h, +36 orientation (0..3, -1 whole),
+40 / +44 / +48 r g b.

Random: `0x90a900` / `0x90a56c` = Marsaglia multiply-with-carry (5115, 1776, 1492, 2111111111),
float = u32 x 2^-32. Ported as is (own seed: the PS3 state is global).

### 7.2 Start `0x4ad600` (on the first frame shown, `0x4adb88`)

| system | contents |
|---|---|
| A (+32) | the gallery card (type 1, centre (640, 360), 1280 x 720, white); the frame `0x4ac7d8`: four type-2 black rects (centre, size) (640, -500, 5280 x 1000), (-1000, 360, 2000 x 720), (640, 1220, 5280 x 1000), (2280, 360, 2000 x 720); `money` 512 x 512, spread pi/4 |
| B (+868) | 10 different player photos (`rand & 15`, up to 100 tries, table `0xb40630`: drake ... flynn) 256 x 256, spread 0.3 pi; 4 different map photos (`rand % 9`, table `0xb40600`: warzone ... train-wreck) 512 x 256, spread 0.3 pi |
| C (+1704) | 12 `coins` 100 x 100, spread 0.1, orientation 0, 1, 2, 3, ... |
| D (+2540) | empty |

Spawn `0x4ac9f4(sys, material, {w, h}, -, orient, spread)` (only while count <= 15): x = 50 + 1180 r1,
y = 50 + 620 r2, angle = spread (r3 - 0.5), r g b = 0.8 + 0.2 r (r4 r5 r6, stored b first).

### 7.3 Cameras `0x4ad124(out, obj, idx)`, out = {x, y, angle, zoom, alpha}

t = seconds since start, ph = fmodf(0.07 t, 1), k = 2.0923009 idx, b = 0.75 k + t + seed,
w = S(ph, 0.5, 0.6) - S(ph, 0.9, 1.0) (smoothstep `0x89fe2c`, window `0x89fe94`).
- idx 0: alpha = 1 - w; a = 0.20943953 b; (x, y) = (250 cos a, 150 sin a).
- idx 1: alpha = w; a = 0.15 b; m = 0.25 (1 - cos 2a) + 0.5; (x, y) = (250 m sin a, 150 m cos a/2).
- both: s = clamp(0.5 cos(0.1 t + k) + 0.5) (double), zoom = 1 - s + 1.25 s; angle = 0.15707964 sin(0.1 t + k).

Caller (`0x4ae858..0x4ae940`): alpha x= a, zoom x= (1 - c) 1.2 + c with c = clamp(1 - (1 - a)^2); when
fading out, y += 50 (1 - a)^2; all four systems drawn with camera 1 then camera 0 (each only when its
alpha is not 0). The fade `a` is the remapped one while fading in and the plain fade while fading out
(`0x4af028` -> `0x4ae750`); loading.cpp now does the same for every element.

### 7.4 Draw `0x4adbd0(system, camera)`

Per particle: M = R(angle) T(p + cam.xy - (640, 360)) R(cam.angle) S(cam.zoom) T(640, 360) (row
vectors; `0x3ee038` rotZ rows {c, s}, {-s, c}; `0x62e94` scale; `0x35780` dst = a x b), corners
(-w/2, -h/2) (w/2, -h/2) (w/2, h/2) (-w/2, h/2) x M (AltiVec `vmaddfp`), colour (r, g, b, cam.alpha),
UV quarter by orientation (0: u 0..0.5 v 0..0.5; 1: u 0.5..1; 2: v 0.5..1; 3: both), type 1 flipped in
v. Submit: type 0 `0x8b16f0` + `0x8b1598` with the material; type 1 / 2 `0x414ad8(menu+0x10, 8 / 10)`.
`0x414ad8`: slot 8 = material `ctx+132` with the gallery texture `ctx+184`+16, only when `ctx+180 == 3`;
slot 10 = no material, `0x8b0220(NULL)` falls back to the default material (solid colour).

Before the particles it stores a projection (transposed) in the menu matrix `0xf0e830` (= ctx+0x20,
the matrix the PS4 renderer copies into its text struct at `0x5be07e`), identity again after them:
G = rotX(0.01) x inverse(M0) x P, M0 = camera at z = -30 with x mirrored, P = perspective with
f = 1 / tanf(0.024674013), z' = 1.000001 z - 0.0001, w = z. Applied as the PS3 2D library applies its
matrix (`0x8b0de8`: v = (x - 640, 360 - y, z, 1) x M, x y scaled 2 / 1280, 2 / 720): screen =
(640 + f xc / W, 360 - f cos(0.01) yc / W), W = 30 + sin(0.01) yc. That is a 1.35 x zoom with the top
of the table receding (W 33.6 at the top edge, 26.4 at the bottom). Which PS3 path consumes
`0xf0e830` for these quads was not found (the 2D library's own matrix is `0x10272f0`, set by
`0x8b012c`); the projection is ported as built and can be switched off for comparison
(`UNCHARTED_U2MP_LOADING_PERSPECTIVE=0`). With it, the fort card covers the screen at the camera's
extreme offsets (left edge at x = -18 with offset 250, zoom 1.25); without it, black would show.

### 7.5 PS4 mapping

| PS3 | PS4 | how |
|---|---|---|
| `0x414ad8(ctx, quad, slot)` | `0x5b4e90(ctx, list, quad, slot)` | same body: slot 10 -> no material; slot 8 needs `ctx+0x128` and `ctx+0x110 == 3`, then {material, `ctx+0x118`, ...}; renderer case 0x1b calls it with slot 10 (`0x5c0d6e`) |
| `0x8b16f0` + `0x8b1598` | `0xbdce90(ctx, {material, 0, 0, 0}, quad)` | as menuimages.cpp |
| `0x414968(ctx, path)` gallery request | inlined `0x58b5c8`: slot = ctx - 0x10 (the menu manager): +0x368 = 10 (keep-alive frames), path `"%s/build/main/gallery1/%s"` (`0xe87590`, root `[0x104ad28]+0x408`) to +0x268, +0x367 = 0, +0x120 = 1 if 0 | the menu update `0x5b46dc..0x5b488a` sees these at ctx +0x358, +0x258, +0x257, +0x110 and loads into ctx+0x118 (state 3) |
| materials `0x8a015c(id)` | `menu_net_material(id)` | PS4 net.pak `general/net/loading-screen:money / coins`, `player-photos:*`, `map-photos:*` (net-u2mp for missing ones) |

A PS4 quad is affine; PS3 interpolates UVs perspective-correct. Textured cards are therefore cut into
n x n cells (n = projected edge / 96, at most 12) and every cell is projected; cells are clipped to
W >= 1 (only the frame rects get there, far off screen). A card whose material is missing is not drawn
(PS3 would draw it with the default material). The first 8 textured cards are logged with the gallery
state (`ctx+0x110`, `ctx+0x128`), so a run shows whether fort.dds loaded during the loading screen
(the PS4 menu update has to run while loading for that).

Switches: `UNCHARTED_U2MP_LOADING_PARTICLES=0` (no cards and no gallery request),
`UNCHARTED_U2MP_LOADING_PERSPECTIVE=0` (cards flat, no projection).

### 7.6 Tip line `0x4acc34` (ported, loading.cpp section "tip")

- Table `7f306129` (PS4 net.bin defines it: 34 records of 12 bytes {game type, generic flag, text id},
  PS3 45; the net-patch9 copy has 24-byte records, so `UNCHARTED_U2MP_NETPATCH=all` would change the
  layout). Game type = net game record +0xc on PS4 (`0x5dee67`; PS3 `0x49e4a0`, +12).
- New tip when obj+24 + obj+28 < now (both 0 on show, `0x4adb98`): n = rand % count + 1; walk from
  index 0, every eligible record (type == current or flag != 0) counts one down; the tip is the record
  after the n-th, wrapping, one more while that is the previous tip (PS3 spins when nothing else
  qualifies; bounded in the port). obj+24 = now, obj+28 = int(75 x strlen) ticks.
- Draw: printer `0x73cf20` = PS4 `0xcc9a70` twice: measure (mode 4) in width 600, then mode 1 at x =
  (1280 - w) / 2, y = 650 - h, measured width kept, height limit 9999, scale 0.75, white, font 1,
  glyph callback = OPD `0x4ad118`, obj+20 = 0 first.
- Callback convention. PS3: the line printer `0x73c494` calls callback(glyph) per character (`0x73c6d4`)
  with a copy of the struct (x / y / scale of the glyph, character +120), returns the advance; NULL =
  `0x73b2e0`, the glyph draw (it takes the glyph only). PS4: `0xccae50` calls `float cb(ctx, glyph)`
  (`0xccb602`: rdi = its state, rsi = the copy at -0x110, character at +0x94); NULL = `[0x104af58]` =
  `0xcc7cc0` (R_X86_64_RELATIVE in eboot.elf at 0x109eb40), the PS4 glyph draw: it reads x, y, scale
  +0x20/+0x24, colour +0x28, font +0x30, flags +0x34, clip +0x10/+0x14, matrix +0x50.., character
  +0x94 - the PS3 fields at their PS4 offsets - and returns the advance.
- `0x4acf2c(obj, glyph)`: a = `0x4acb44(obj, obj+20++)`; when a != 0, the glyph 1 unit lower with colour
  word int(128 a) (black, alpha 128 a); then the glyph with alpha int(255 a); returns that advance.
  `0x4acb44`: e = 0.013333 (now - obj+24) - 0.1 i, d = 0.0066667 obj+28, a = obj+0 (the fade) x
  (E(e, 0, 1) - E(e, d - 1, d)), E = cosine step `0x89fd8c` ((cos(pi t) - 1) x -0.5).
- Struct map (PS3 -> PS4): x y +0/+4 -> +0/+4; scale +8/+12 -> +0x20/+0x24; colour +16 -> +0x28 (bytes
  swapped: PS3 0xRRGGBBAA, the renderer swaps them for the printer at `0x5be4da..0x5be5c3`, so alpha
  is the top byte); value +20 -> +0x2c; clip +24..+36 -> +0x10..+0x1c; font +40 -> +0x30; mode +116 ->
  +0x34 (4 = measure, 1 = mode 1, bit for bit as the converters map them; PS4 0x10 = break at the width,
  which PS4 sets where PS3 passes its width: set for both calls); matrix +48 -> +0x50..+0x8f; width /
  height +120 / +124 -> +0x94 / +0x98 (the measure results land there on both); callback +132 -> +0xa0.
  The user pointer (+112 = obj) is not needed (the port's state is static); PS4 +0x38 stays 0.
- No draw list: as the renderer's text case (`0x5be5f1`): +0x90 = `[0x1050e58]`, `0xbda9c0(0x124a000,
  &s, text)` queues a copy (callback and string included: +0..+0x43, +0x50..+0xa7), then flags |= 4
  and `0xcc9a70` measures right away. The glyph flags carry the 4 in measure passes; the callback then
  calls the default draw and does not count the glyph.
- Inferred, to check in a run: the PS4 colour byte order (from the renderer swap) and the 0x10 flag.
- Switch: `UNCHARTED_U2MP_LOADING_TIP=0` (also off with `UNCHARTED_U2MP_LOADING_TEXT=0`).
