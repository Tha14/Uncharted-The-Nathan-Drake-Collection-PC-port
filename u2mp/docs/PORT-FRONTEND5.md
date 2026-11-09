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

Left out (not resolved this pass): the gallery picture `fort.dds` on the four perspective panels
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
