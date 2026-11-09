# Port job "hud": the multiplayer HUD in matches

Static analysis only (nothing built or run). PS3 = 1.10 EBOOT (NPUA80698), PS4 = Collection 1.02 image.
Code: `gow3_pc/src/u2mp/hud.cpp`, `install_hud(int &installed)`.

## 1. Why nothing showed

The multiplayer HUD is C++ in `game/hud/hud.cpp`, not a script. The HUD manager (`0x1130008`, PS3
`0x1026a88`; init PS3 `0x8a13e8` = PS4 `0x552c70`) creates one multiplayer element: PS3 ctor
`0x8ac330` = PS4 `0x55aa80`, stored at hud+0x320, element bits 0x2000, vtable PS3 `0xd87db8` = PS4
`0x103f550`:

| slot | PS3 | PS4 | state |
|---|---|---|---|
| 0 Update | `0x8ac1e8` (calls the sub-update `0x8abf00(+64)`, names `0x8aa448(+2080)`, ORs the HUD flags 0xe0000 while ngm state != 0 and not cinema, element fade +6368) | `0x55abe0` (all of it inlined: +0x40, +0x860, `or $0xe0000` at `0x55c51f`, fade +0x1920) | intact |
| 1 Draw | `0x8aef68` (0x1158 bytes) | **`0x55c5b0` = `ret`** | stripped |
| 3 Reset | `0x8a9b90` | `0x55abb0` | intact |

The HUD loop `0x554710` (inside the per-frame HUD update `0x554340` = PS3 `0x8a2b48`) calls slot 1
of every element whose bits are in the flags, so the element is drawn every frame, but slot 1 does
nothing. All the helpers only the Draw called are also missing from the PS4 image: `0x8ac3f0`,
`0x8ad018`, `0x8adb60`, `0x8a8ed8`, `0x8a9e48`, `0x8abc68`, `0x8ab600`, `0x8ab4d0`, `0x8a9bbc`.
Their text formats ("%i:", "%02i", "x%2.1f") and DC ids (`d38a7d18`, `e1167dfa`, `12ffe6ae`) are
not in the PS4 image. The HUD package, the DC widget data and the script natives are all present:
PS4 `dc1/ps4/hud-settings.bin` record `9353ed16` has the PS3 multiplayer widget set at the same index.

## 2. Ported (hud.cpp): the score / time panel of 0x8aef68

| PS3 | PS4 |
|---|---|
| IsMultiplayer `0x4384b0` | NET_SM `0x14ce97c` >= 2 |
| HUD flags `[[0x1026a88+56]]` | `[[0x1130008+0x60]]` (the PS4 flag code `0x554386..` / `0x55c506` uses it) |
| settings `0x151d4c(9353ed16)`, multiplayer set rec+0x58 | `0xa15e70(9353ed16)`, rec+0x90 (16 pointers at +0x20 on both: PS3 +0x20+4k = PS4 +0x20+8k; the SP timer `0x559770` reads +0x88 = PS3 +0x54) |
| set fields +4 / +8 / +24 / +28 / +32 / +36 / +44 | +0x08 / +0x10 / +0x30 / +0x38 / +0x40 / +0x48 / +0x58 (layout checked in both hud-settings.bin files) |
| text `0x8a0f04(widget, pos, scale, fmt, ...)` | `0x5555a0(widget, pos, fmt, float scale, ...)` (variadic, xmm0 = fixed scale; as `0x559770` calls it) |
| image `0x8a10fc(widget, pos, scale)` | `0x5559f0(draw ctx, widget, pos, scale)`; the ctx is slot 1's second argument |
| text `0x9294c4(0x109d3f0, id)` | TextDb::Find `0xcddc00(0x1066660, id)` |
| local tracker `0x4919d4` | ngm+0 |
| net game record `0x499908` | `*net-games*[ngm+0x1ca0]` |
| co-op test `0x4a0bcc` (type 4/8/9/12) | same |
| team tracker+300 | tracker+0x134 (the CheckGameOver team loop `0x63f4ae`) |
| team scores `0xf2efc4+15840+14+52*t` (u16) | `[0x104ae70]+0x3dee+0x34*t` (CheckGameOver `0x63f3f7`) |
| score limit +7220, time limit +7224, round start +6560, match start +6564, round +6580 | +0x21b4, +0x21b8, +0x1d5c, +0x1d60, +0x1d70 |

Logic (PS3 `0x8af000..0x8af960`): only competitive games (not co-op, not type 11), only with flag
0x40000:
- **type 10, elimination** (`0x8af648`): frame image, own and other team score, label `e1167dfa`,
  round ngm+0x1d70 + 1, time left from the match start;
- **other types** (`0x8af220`, e.g. type 0 deathmatch): frame image, own and other team score,
  label `d38a7d18`, score limit, time left from the round start;
- **time left** (`0x49e0e8`, `0x8af878`): limit = time limit x 300 frames. When it is > 0,
  secs = (limit - elapsed) / 300, shown as minutes % 60 "%i:" and secs % 60 "%02i".

Install checks: the prologues of `0x5555a0` / `0x5559f0`, the ctor's vptr load `0x55aab2` and bits
store `0x55ab94`, the ret stub at `0x55c5b0` and the vtable slot value. Test switch:
`UNCHARTED_U2MP_HUD=0` leaves the stub. The log prints `hud: multiplayer draw #1 ...` at the first
call, then every 600th call (type, flags, team, scores, limits). That line proves the element is drawn.

## 3. The rest of 0x8aef68: status

The third pass (section 5) ported the parts listed here. Their PS3 order, gates and PS4 data:

| Part | PS3 | Gate | PS4 data | State |
|---|---|---|---|---|
| type 7 (Turf War) territory panel | `0x8af960..0x8afe6c`, `0x8aff14` | type 7, flag 0x40000 | set +0x60 / rect +0x68 / text +0x78; territories ngm+0x1d80 | ported (5.5) |
| objective carrier icon | `0x8af360..0x8af41c`, `0x8afe70..0x8b00b8` | score panel branch, not types 1 / 2 | ngm+0x1cc0 / +0x1cc8; set +0x18 / +0x20 / +0x28 | ported, two pieces left (5.5) |
| co-op type 9 money / wave | `0x8af4e0..0x8af5c0` | type 9, flag 0x8000, wave screen hidden | set +0xc0 / +0xc8 / +0xd0 | ported (5.4) |
| booster icons `0x8a9bbc` | `0x8af5c4` (film), `0x8af440` (loadout) | film: ngm+0x209c, flag 0x40000, cinema mode; loadout: competitive, flag 0x10 **set** | tracker +0x2a0 / +0x2a4; loadout 0x11474f8+0x4c | ported (5.4) |
| theater HUD (`0x8a8ed8` camera selector, `0x8ac3f0` camera name / progress / "x%2.1f" speed) | `0x8af620` | film, flag 0x100000, `[0xe1eb2c]` == 0 | element +0x1928 | not ported (5.6) |
| message list / kill feed | `0x8af424` | competitive, not type 11, flag 0x80000 | element +0x40 | ported (4.1) |
| names over heads `0x8aeeb0` -> `0x8adb60` | `0x8af4c4` | flag 0x20000 | element +0x860 | ported (5.2) |
| world markers `0x8ad018` | `0x8af4d8` | flag 0x20000 | element +0x1560 | ported (5.3) |
| centre message `0x8ab4d0` | `0x8af1e0` | none: drawn every frame (the doc said 0x20000; `0x8af1d4` branches around the names only) | element +0x1a90 | ported (5.3) |
| DrawText flush `0x73bf94(0)` | tail | - | not needed: PS4 HUD texts are queued in order | - |

Ammo, weapon, grenade count and reticle belong to the shared (single-player) HUD elements (vtables
`0x103f360..0x103f510`), see 4.3.

## 4. Second pass

### 4.1 Ported: the message list (kill feed, multi-kill / medal lines), PS3 0x8abc68 + 0x8ab600

The line layout below (score-widget copies in a 220-unit column) is replaced by section 6.3.

The data is intact on PS4. The list is at element+0x40: `{s32 count; entry[16] of 0x80 at +8}`. It is
filled by `0x55d140` (PS3 AddMessage; called by the net event Applies at `0x611160`, `0x612770`,
`0x61e710`, `0x61eb90`, `0x622d60`, `0x6236b0`, `0x625890`) and expired by the Update `0x55abe0`.

| entry field | PS4 | PS3 |
|---|---|---|
| text 1 (48 chars) / colour 1 | +0x00 / +0x30 | +0 / +48 |
| text 2 (48 chars) / colour 2 | +0x34 / +0x64 | +52 / +100 |
| icon pointer | +0x68 | +104 |
| start / end time (s; end = start + 7) | +0x70 / +0x74 | +108 / +112 |
| flags / count | +0x78 / +0x7c | +116 / +120 |

hud.cpp `draw_messages` uses the PS3 layout and animation (`0x8abcec..0x8abef0`): appear =
clamp((now - start) x 2.2222), vanish = clamp((end - now) x 2), x = 120 - 200 (1 - appear)^2, alpha =
appear x vanish, lines of 25 from y = 650 - sum(vanish x 25), newest on top; gate flag 0x80000,
competitive games other than type 11. PS3 drew with its low-level text and quad calls (DrawText
`0x73cf20`, quad `0x89ff3c`, weapon icon `0x8a0260`). None of them has a PS4 counterpart (DrawText exists
only inlined in `0x37a690`). So each name is a HUD text widget: a copy of the score widget (font), with the
entry colour and position, drawn with `0x5555a0`. That function adds the position and multiplies the
alpha by its float argument (`0x555719..0x555763`). Text 1 is right-aligned in a 220-unit box, and text 2
is left-aligned after it. The weapon icon is not drawn. A count is appended as "x<n>".

### 4.2 Medal and XP pop-ups: present on PS4, not part of the stripped Draw

The medal award `0x5779a0` (from NetEventEarnMedal Apply `0x61d1e0` and `0x61ee6c`) calls `0x5776c0`. That
function computes the money with the booster amount `0x5f6410` and adds a pop-up with `0x554ca0` to
the HUD manager's pop-up element at hud+0x310. That element is constructed at `0x553368` with vptr
`0x103f360` = PS3 `0xd87ca8`: Update `0x8a5340` -> `0x557d50`, Draw `0x8a51c0` (+ inlined `0x8a4a68`)
-> `0x557f10`, which keeps its three multiplayer branches. Nothing to port.

### 4.3 HUD element vtables, slot by slot

PS4 element vtables `0x103f2f0..0x103f760` (17 classes): every Update (slot 0) / Draw (slot 1) is a real
function except the multiplayer Draw `0x55c5b0`. The `ret` slots are destructors, the base no-ops
`0x557850` / `0x557840`, and the PS3 equivalents `0x89f848` / `0x89f84c` (4-byte `blr`). The PS3 table at
`0xd87c88..0xd87db8` has 9 of those classes; the others (weapon / ammo / grenade / reticle in
`hudweapon.cpp`, damage indicator) are in other PS3 tables. Their PS4 slots are not stubs. A
function-by-function comparison of their bodies was not done (the matcher pairs only 3 of them). The
PS4 Draw functions with a multiplayer branch keep it (`0x557f10`: 3 NET_SM tests).

### 4.4 (superseded by section 5)

The second pass found no PS4 counterpart for the PS3 low-level primitives. Section 5.1 maps them.

## 5. Third pass: the rest of the Draw

Static analysis only; nothing was built or run. `net_hud_draw` now follows the PS3 order of `0x8aef68`
(section 3). New install checks are in `layout_matches()`, `boosters_layout_matches()` and
`type7_layout_matches()`, about 40 byte checks. If any fails, the whole Draw stays the PS4 stub, as before.

### 5.1 PS3 primitives -> PS4

| PS3 | PS4 | Notes |
|---|---|---|
| textured quad `0x89ff3c` | `0x555c60(ctx, handle, index, &rgba, centred, global alpha, pos, size, dir, uv0, uv1)` | The image widget draw `0x5559f0` = `0x8a10fc` ends in it. Centred: pos is the centre and dir turns the quad; PS3 passes (0, -1) for upright (`0x8aa42c`). Global alpha: x `[0x104ed0c]` (PS3 `[0xe1a660]`). |
| handle `{id, table, index}` (`0x8a015c` sid, `0x8a0260` name) | same 12 bytes; `0x555140` by name | Table 0/1/2 = hud+0x68/+0x70/+0x78; index -1 = look it up. menuimages.cpp's `0x5558c0` port resolves PS3 sids in the net table. |
| material `0x8a1f88` | `0x5558c0` | Replaced by menuimages.cpp, so its prologue is not checked. |
| hudquad batch `0x8b0220` / `0x8b0de8` / `0x8b12b8`, rects `0x8b1600` + `0x8b1540` / `0x8b1598` | none | An axis-aligned rect becomes `rect()` = `quad()`, so callers pass the colour without the HUD alpha. A free quad becomes `quad4()` = `0x5558c0` + `0xbdce90(ctx, {material,0,0,0}, 4 vertices of 0x40)`, in the vertex layout of `0x555c60`. |
| DrawText `0x73cf20(state, text)` | `0xcc9a70(state, text)` | Only `0x5555a0` calls it. The state layout is in hud.cpp `draw_text`. PS3 +116 align/flags = PS4 +0x34 (4 = measure only, no glyphs, `0xcca558`). The box +120/+124 = +0x94/+0x98. Measuring writes the width to +0x9c (PS3 +128) and the height to +0x98. DrawText does not apply the HUD alpha. |
| projection `0x8a2388` | `0x5566b0(out of 0x30, &world, margin)` | The out block is pos, dir, angle, dist, scale, clamped. |
| ring meter `0x8a9e48`, edge arrow `0x8aa210`, ramp `0x89fd8c` | none | Reimplemented: `meter_8a9e48`, `edge_arrow`, `ramp`. |
| sin/cos `0xabc6ec(x, 0/1)`, fmod `0xabb130` | `std::sin` / `std::cos` / `std::fmod` | |
| clocks `[0xe60a00]` (112 B each from +12) | `[0x1116180]` (0x80 each from +0x04) | +124/+136/+236/+460 -> +0x84/+0x90/+0x104/+0x204 |
| HUD flags struct `[[hud+56]]` +1608 / +1612 | `[[hud+0x60]]` +0x408 / +0x40c | +0x40c is set at `0x8921bb` (= PS3 `0x61baa0` stb at `0x61c104`) |

### 5.2 Names over heads (PS3 0x8aeeb0 -> 0x8adb60)

The data is intact. The names sub-object is PS4 element+0x860 (PS3 +2080) and keeps the PS3 offsets:
- entries of 0xa0 at +0, count at +0xc80;
- handles at +0xca8 player-arrow, +0xcb4 player-ex, +0xcc0 triangle button, +0xccc knocked-down,
  +0xcd8 grapple, +0xce4 sid a2a60dcf and +0xcf0 sid 68951e57 (init `0x55d4f0` = PS3 `0x8a9340`).

The Update `0x55abe0` fills each entry with:
- +0 screen x / y and +8 distance scale s;
- +0xc side (0 above, 1 left, 2 right, 3 on screen);
- +0x10 arrow (0 player-ex, 1/2 arrow);
- +0x20 colour (team colour, name fade);
- +0x30 status (1 down, 2 grappled) and +0x34 fill;
- +0x38 name (64 chars) and +0x80 name colour.

The pointer grew past +0x90: +0x90 emblem pointer (PS3 +144), +0x98 talking (PS3 +148), +0x9c rank
(PS3 +152).

`draw_name` follows `0x8adb60`:
- **Measuring and placement.** The name is measured with flags 12 in a box of 99999 x 40, giving W and
  H. The icon is 50 s when there is an emblem or the rank is > 58; the talk icon is 40 s. The
  group [icon][name] is centred on x and clamped to 128 .. 1280 - 128.
- **Arrow.** Its place, size and angle come from the side: (cx, y - 25s - H) 1.5 pi/2; left edge - 10
  1.2 pi; right edge + 10 (+ talk/2 + 6 when the name shows) 1.2 0; (cx, y - 25s) 1.5 -pi/2. With a
  status it bobs by clamp(1 - |cos 8t|) x 20, and an on-screen entry with a status draws no arrow.
- **Status.** At (cx, y - 145s):
  - a glow (sid a2a60dcf) in red (down) or blue (grappled) at 0.75 alpha, sized 50s x lerp(3.5, 5,
    |cos 4t|) and turning with the clock;
  - a black one at 225s;
  - the ring meter (radii 30 / 50, white blink below 0.25 fill);
  - the triangle-button / knocked-down (flags +0x40c) or grapple icon, 60s square.
- **Rank icon.** Above 58: 8 x 8 atlas bda0f568 (f1b63553 above 63) at (left + icon/2 - 8s,
  y - 25s - H + icon/2 - 4s).
- **Name and talk icon.** The name is drawn at (left + icon, y - 25s - H) with flags 8, in the name
  colour x HUD alpha. The talk icon is ffa98ec5.

Left out: the emblem (PS3 `0x8ae2cc`). It is material 6927b434 with the emblem texture passed into the
hudquad begin `0x8b0220(material, emblem+16)`, and PS4 has no call that takes that texture. Its space
is kept.

### 5.3 World markers (PS3 0x8ad018) and the centre message (PS3 0x8ab4d0)

The world markers are in PS4 element+0x1560 (init `0x55c5c0`), which keeps the PS3 layout:
- up to 8 entries of 0x40 at +0x10: id, type, world position, colour and ring colour (0xAARRGGBB), fade
  +0x2c, fade target +0x30, ring fill +0x34, frames +0x38 / +0x3c;
- 11 type records of 0x24 at +0x210: icon, pulsing icon, size, dark flag, number label;
- the arrow handle at +0x39c and the ring handle at +0x3a8.

`0x555340` (= PS3 `0x8a9980`) adds or refreshes a marker every frame. Its callers include
DcNetDrawObjectiveIcon `0x669250`.

`draw_markers` follows the PS3 steps in order:
1. Skips a type whose bit is clear in the mask `[0x104f4b0+0x14]`.
2. Zeroes a marker that was not set for 300 frames.
3. Moves the fade toward the target by +-4 dt and resets the target.
4. Projects the marker with margin 100.
5. Draws, in this order:
   - the pulsing icon, scaled by lerp(1.5, 2.5, |cos 4t|);
   - the icon, black when the dark flag is set;
   - the edge arrow when the point is clamped;
   - the ring meter at 1.75 s when the fill is not 0;
   - the "%i" label (DrawText scale 2s, box 400 x 400, centred).

The centre message is at element+0x1a90: text[128], start +0x80 (clock 2), colour +0x84. It is set
inline at `0x649b91` / `0x6470b9` and drawn every frame, centred in 800 x 20 at (240, 288) at scale
1.2. PS3 fades each glyph (callback `0x8a7a38`). The PS4 DrawText has no per-glyph hook, so the string
fades in with the first glyph's curve and out with the last glyph's. It is cleared when PS3 clears it.

### 5.4 Boosters, co-op type 9 panel

The booster icons (`0x8a9bbc`) are 76.8 x 38.4 quads at x = hud+0x340 + 90, y = 150 and 180.72, white
at fade x 0.75. They are drawn at two sites:
- **Film site:** ngm+0x209c, flag 0x40000, cinema mode `[0x113a990+0x678]`. It draws the watched
  player's boosters by index from tracker +0x2a0 / +0x2a4 (SetBooster `0x662dd0`).
- **Loadout site:** competitive, flag 0x10 set, boosters allowed (ngm+0x1e80, game record +0x6c). It
  draws the loadout ids 0x11474f8+0x4c / +0x50, looked up in the booster list 0x113a788.

The co-op type 9 panel needs flag 0x8000 and the wave screen float `[0x104f544+0x20]` == 0. It draws:
- the image set+0xd0;
- "x%i" from `0x64cbd0` in text set+0xc8, at set+0xc0;
- a ring meter of the wave time left (PS3 `0x49cae8` ported: wave table at game record +0x7c, ngm
  +0x1d64 / +0x1d68 / +0x1d6c), scale 0.475.

This corrects section 3 of the second pass: PS3 set +104 is an inline vec2 (PS4 +0xc0), so the text is
+0xc8 and the image +0xd0. PS3 +52..+64 is an inline rect at PS4 +0x68, and PS3 +68..+100 are pointers
at PS4 +0x78..+0xb8.

Unverified: no PS4 code reads `[0x104f544+0x20]`; it is inferred from the entry layout that
`0x668850` / `0x668890` write.

### 5.5 Type 7 territory panel and carrier icon

**Type 7 panel.** The territories are the ngm list +0x1d80: 5 handles of 0x10, count at +0x1dd0
(DcNetSpawnTerritory `0x669840`). The territory object fields are PS3's + 0xb4: owner +0x234,
capturer +0x238, index +0x250, gain / loss end +0x258 / +0x25c, lengths +0x268 / +0x26c.

`draw_type7_panel` draws the frame set+0x60, then the time left from the round start, then 5 slots in
the rect set+0x68, in reverse order for team 1. Each slot has:
- the capture ring (0.4, capturer colour from `0x64aeb0` out+0x10, alpha 0xe0) while the progress is
  below 0.99;
- the 12ffe6ae square in half the owner colour;
- its number in a copy of widget set+0x78 at scale 0.7, black at 0.565 and then coloured 2 units higher.

**Carrier icon.** `draw_carrier` resolves the carry object ngm+0x1cc0 / +0x1cc8 (`0x6237ab`) and finds
the carrier tracker by flag+0x440 == tracker+0x120. It draws the image set+0x18, +0x20 or +0x28 for
no carrier, own team or other team.

Left out:
- the PS3 fallback search through the players' held objects, which needs character +44 / +6696 / +7600
  on PS4, not mapped;
- the 0.5 s blink, because PS3 `0x2b75c` exists only inlined on PS4 (`0x63e439`, `0x649879`).

### 5.6 Not ported: the theater HUD (PS3 0x8af620)

The doc called this the "recording / multiplier indicator"; it is the film playback HUD.
- **`0x8a8ed8` is not a draw.** It is the camera selector update. It rebuilds the camera list in
  sub+256 from the entries at +12, reads pad buttons 4 / 8 through `0xaac38`, plays f57d53ce and
  switches the camera with `0x426dbc(0xf0e800, id)`.
- **`0x8ac3f0` draws.** It shows the camera name, the progress bar and the speed "x%2.1f".

The PS4 sub-object is at element+0x1928 (PS3 +6372 + 0x44). The ctor initialises it with `0x55d690`
(= PS3 `0x8a7c64`), and the Reset `0x55abb0` sets +0x1928 = 3, +0x192c = 3.0, +0x1a7c = 1.0.

Porting it means taking over camera switching and pad input in cinema mode. It needs about 8 more
unmapped PS3 functions (`0x420f58`, `0x421018`, `0x4156cc`, `0x462fbc`, `0x426dbc`, `0xaac38`,
`0x30fb28` / `0x7069c8`, `0x8a8da8`), and the gate `[0xe1eb2c]` has no PS4 counterpart yet. It belongs
with the cinema port (cinema.cpp).

### 5.7 Unverified

- (Placement: checked in section 6.2. Text, images and the projection all use 1280 x 720.)
- The DrawText state is built from `0x5555a0` and read from `0xcc9a70`, not tested.
- The HUD alpha `[0x104ed0c]` is taken as PS3 `[0xe1a660]` by analogy.
- The sids used here (a2a60dcf, 68951e57, 12ffe6ae, bda0f568, f1b63553, ffa98ec5) are PS3 net-table
  sids. menuimages.cpp's lookup finds them among the PS4 names, or in net-u2mp; if neither has one, that
  quad is skipped.

## 6. Fourth pass: why no HUD image drew, layout check (run 46)

Static analysis only; nothing was built or run. Run 46 (Village deathmatch, the core panel build) showed
the score texts in the right places but no frame / background image.

### 6.1 The cause: the draw context is NULL on PS4

The HUD manager's frame update is called as `0x554340(hud, 0, 0)` (`0x3fc529 xor esi,esi`,
`0x3fc52d call 0x554340`), and the element loop `0x554710` passes that rsi (r12) to every slot 1 Draw.
So the second argument of every PS4 HUD Draw is NULL. It is the draw list: `0x555c60` copies it into
`{list, 0, 0, 0, 0, 0}` for `0xbdce90`, and a NULL list is the deferred 2D arena (as `menu_draw_quad`
uses it). The SP timer `0x559770` hands it on unchanged. hud.cpp returned early on `!ctx` in
`frame_image`, `quad`, `quad4`, the ring meter, the boosters and the co-op panel, so not one HUD image was
drawn. The texts draw through `0x5555a0`, which takes no context, and so they showed.

Fix: no ctx test is left; NULL is passed through as PS4 passes it.

The images themselves are in the PS4 data:

| PS3 sid | PS3 name (g-net.pak) | PS4 net.pak / hud.pak | used by |
|---|---|---|---|
| (widget) | general/net/mini-score/default, /tow | net.pak `mini-score:default` (index 0 in the table dump of run 46), `:tow` | frames, carrier icons (set +0x18..+0x28, +0x58, +0x60) |
| (widget) | general/net/message-back/default | net.pak `message-back:default` | kill feed back (set +0xb8) |
| (widget) | general/net/coop-icons/meter-back | net.pak `coop-icons:meter-back` | co-op panel (set +0xd0) |
| a2a60dcf | general/net/icon-glow/circle | net.pak `icon-glow:circle` | names status glow |
| 68951e57 | general/net/coop-icons/meter | net.pak `coop-icons:meter` | ring meters |
| 12ffe6ae | general/net/flag-icons/territory | net.pak `flag-icons:territory` | Turf War squares |
| bda0f568 | general/net/rank-badges/default | net.pak `rank-badges:default` | rank icons 59..63 |
| f1b63553 | general/net/rank-badges/default2 | not in PS4; net-u2mp | rank icons above 63 |
| ffa98ec5 | general/net/mic/default | net.pak `mic:default` | talk icon |
| 2fb98f98 | general/net/headshot/default | net.pak `headshot:default` | kill feed headshot |
| 6927b434 | general/hud/alpha-icon/default | hud.pak `alpha-icon:default` | emblem (not drawn) |

PS4 names its materials "set:name" and gives them the CRC of that name as id; a PS3 sid is the CRC of
"set/name". An image widget loads its own id by name (`0x555140` turns the last '/' into ':'), so the
dc widgets find the PS4 ids. The sids built in code are found by `0x5558c0` (menuimages.cpp: PS4 id, then
the PS3 name, then net-u2mp). hud.cpp now resolves each handle once per material table (pointer, count)
and replaces a PS3 sid with the PS4 id and index (the PS4 asset is used). Each image that does not resolve
is logged once:
- `hud: image <part> id <id> table <n> (<hud|net>) index <i> not resolved: <table> table set|NULL, <n> materials`
  (code-built handles);
- `hud: image <part> (widget <material name>) id <id> table <n> ... not resolved ...` (dc image widgets,
  checked after `0x5559f0` cached its id and index).

A PS3 sid that maps to a PS4 id is logged once as `-> PS4 id`. The draw log (`hud: multiplayer draw #n`)
now also prints the draw list pointer, the message count and the hud / net material table sizes.

### 6.2 Layout: what the data says

**The widget set is PS3's.** PS4 `dc1/ps4/hud-settings.bin` record 9353ed16 (+0x90) and PS3 hud-settings
record 9353ed16 (+0x58) hold the same values (positions, sizes, scales, colours, uvs):

| set PS4 (PS3) | widget | values (1280 x 720) |
|---|---|---|
| +0x08 (+4) | own score text | (555, 59) box 60, right, scale 0.85, colour 0.80 0.98 0.63 |
| +0x10 (+8) | other score text | (687, 59) box 60, left, 0.85, colour 1 0.69 0.60 |
| +0x30 (+24) | limit / round value | (770, 69) box 60, centre, 0.6 |
| +0x38 (+28) | label (GOAL / ROUND) | (770, 60) box 60, centre, 0.3 |
| +0x40 / +0x48 (+32 / +36) | minutes / seconds | (433, 62) right 0.7 / (494, 66) left 0.6 |
| +0x58 (+44) | frame image mini-score:default | (440, 50) 400 x 60.94, uv 0,0..1,0.6094, white alpha 0.5 |
| +0x60 (+48) | Turf War frame mini-score:tow | same place and size |
| +0x18..+0x28 (+12..+20) | carrier icons, mini-score sub-images | (627, 56) 49.92 x 37.44 |
| +0xb8 (+100) | kill feed back message-back:default | 350 x 26 |

- **Units.** All three use 1280 x 720, as PS3:
  - Text: `0x5555a0` passes widget pos + pos to DrawText as is, and run 46's texts sit where these
    numbers put them (e.g. "0:41" ends at x = 493 x 0.75 = 370 of 960).
  - Images: PS4 changed one hud widget (set 7) to a full-screen image of 1280 x 720 at (0, 0).
  - Projection: `0x5566b0` maps to (640, 360) +- (640, 360).
- **Fonts and scales.** Neither PS3 `0x8a0f04` nor PS4 `0x5555a0` reads the widget's font word (+0x28).
  Both use the default font, the widget scale (+0x14) and the box width (+0x08). The float argument of
  both is an alpha factor, not a scale. So the texts are drawn as PS3 drew them.
- **Quad orientation.** PS3 `0x89ff3c` and PS4 `0x555c60` turn a centred quad by 3pi/2 - atan2(-dir.y,
  dir.x) (PS3 TOC 4.712389 at `0x8a0054`, PS4 `0xe610b0`), so the PS3 dir arguments carry over.

The panel looked small and badly spaced in run 46 only because its frame was missing. The frame spans
440..840 x 50..111, and the numbers are placed inside it.

### 6.3 The kill feed line, ported from PS3 0x8ab600

The first port drew each name as a copy of the score widget, right-aligned in a 220-unit column at
scale 0.55. Those values were not from PS3. `draw_message_line` / `draw_messages` now follow PS3 (TOC
values at `0xdd0bxx`):
- **List (`0x8abc68`).** Per line:
  - measure it at scale 1;
  - s = 350 / width when it is wider than 350, else 1;
  - draw the back widget (set +0xb8) at (x - 8, y - 2) with the line alpha;
  - draw the line at scale s.

  The alpha includes the HUD alpha (`[0xe1a660]` -> `[0x104ed0c]`).
- **Line.** Each text is DrawText at scale 0.5 s, box 999999 x 40, flags 8 (12 to measure), in the entry
  colour. In order:
  - text 1;
  - a gap of 10 s;
  - the weapon icon;
  - with flag 8 the headshot icon (28 s at (x - 4 s, y + 10 - 14 s)), then 30 s;
  - text 2.
- **Count badge (`0x8ab8b0`).** With a count and flags & 6, a badge is drawn at (x - 58, y - 1): a 50 x 25
  box in 0x5495bb / 0xd73e60 (flag 2) with "%s%i" of TextDb 9b5b3c66 (or "") and the count, scale 0.4,
  centred.

Left out:
- the badge box (PS3 `0x4156cc` has no PS4 counterpart). The count is drawn in the box colour instead of
  black on the box;
- the weapon icon (`0x8a0260` by name from the record at +0x68, 0.45 s high; that record's PS4 layout is
  not mapped). Its width is then 0, as on PS3 for an entry without an icon.

### 6.4 Still to check in a test

- Any `hud: image ... not resolved` lines, and the `-> PS4 id` lines.
- That the frame now shows behind the score (440..840 x 50..111 at 1280 x 720).
- Names, markers, boosters, the co-op and Turf War panels in their own game types. They draw with the
  same quad path, so the ctx fix applies to them too.

## 7. Fifth pass (2026-11-09): weapon panel, kill feed icon and badge, theater HUD

Built, not run yet.

### 7.1 Weapon panel on a weapon switch

The weapon panel (icon, ammo, grenades) is the hudweapon.cpp element at HUD loop index 13: vtable
`0x103f760`, Draw `0x563a20`, element bits 0x8, widget `0x8632c9ad` of hud-settings `9353ed16`. It draws
while HUD flag 0x8 is set. Only the weapon controller's ShowWeaponHud sets it: player+0x2768, vtable
`0x1026d40` slot +0x2f0 = `0x954740` (PS3 `0x69e538`, the same code: booster `2bbd20fa` check, OR 0x8,
mark the element, store the fade). Its callers are player states (RunNGun `0x8fe490`, `0x8b5660`, ...),
which pass -100.0 or 0.0.

In matches the panel never showed on a weapon switch. hud.cpp now calls `0x954740(controller, -100.0)`
from the multiplayer element's Draw (loop index 9, before the panel at 13) for 3 s after the local
player's current weapon changes (player vtable slot +0x260 = `0x71dec0`, the inventory entry at
+0x4b8; local player = handle `0x115bd20`, id at object +0x44). A probe on vtable slot `0x1027030` counts
the game's own calls and logs their callers, and a probe on the panel's Draw slot `0x103f768` counts its
draws:
`hud: weapon panel: N switches, ShowWeaponHud N by the game (from ...) / N here, panel draws N, flags 0x...`.
Switch: `UNCHARTED_U2MP_WEAPON_HUD=0`.

Checked and ruled out: the HUD frame update `0x554340` zeroes the flags in net games only when the NGM
state (ngm+0x1e80) is not 2 (PS3 `0x8a2c10` does the same); the panel's layout data is stock PS4.

### 7.2 Kill feed: weapon icon and count badge

- Weapon icon: the message entry +0x68 points at a record of the hud-settings list `9353ed16` +0x40
  (+0x68 next; filled by `0x646560` from the weapon id). The icon element `0x562990` loads its image with
  `0x555140(record +0x10 name, 0)`; size +0x18 / +0x1c; PS3 +28 / +36 = PS4 +0x28 / +0x30. Drawn at
  0.45 s x size after text 1, advance = width x +0x30 / +0x28.
- Badge: PS3 `0x4156cc` (rounded box) has no PS4 pair; the box is the net table square `12ffe6ae`
  tinted with the badge colour, the count black on it.

### 7.3 Theater HUD (supersedes 5.6)

hud.cpp `draw_theater`, called from the multiplayer Draw while a film plays (ngm+0x209c) with HUD flag
0x100000 (square in RePlaying). Selector as PS3 `0x8a8ed8` (filter by cinema mode, L1 / R1 = PS3 pad bits
4 / 8, Cross = confirm, DoCallback `0x5c8840(menu 0x1136640, id, 0)`, release id when Cross is let go,
smoothing 0.5, fade 0.7 / 0.3). The draw is a simplified layout (PS3 layout primitives have no PS4 pair):
panel, camera icons 64 x 32 every 72 units, the selected name, position / length (m:ss) and a progress
bar. Position = (clock +0x204 - cinema +0x6a0) / 300 (PS3 `0x462fbc`), length = cinema +0x1d8 (header
+0x10, PS3 +456). Left out: the camera change sound and the "x%2.1f" speed line. Pad: raw OrbisPadData
buttons `0x14b7fb0`. Switch: `UNCHARTED_U2MP_NO_THEATER=1`.

### 7.4 Carrier blink, theater speed and sound (same day)

- Carrier icon: PS3 `0x8afec0` blinks the icon (shown while fmod(clock / 300, 1) < 0.5) when `0x2b75c(flag)`
  is true. That is the flag's tag `e9a3dd67`; PS4 inlines the lookup at `0x63e439` (tag table
  [[process+0x68]] = {entries of 0x10: u32 tag, +8 value; u32 count}, value {u16 type 1, +8 -> u32}).
  Ported. Still left out: the fallback search through the trackers' characters when the ngm carry handle
  is dead (PS3 `0x8af3ac`: character +44 bits 0x200 / 0x1000, held handle +7600 (`0x45da80`), object +6696
  slot 116 with type `310f465e`); those PS4 character fields are not mapped.
- Theater HUD: the speed line "x%2.1f" when the replay speed (PS3 `[0xe1558c]` = PS4 `0x104f1bc`,
  compared with 1.0 by RePlaying::Update `0x5e735d`) is not 1; the camera change sound `f57d53ce` through
  PS4 `0x3dd780` + `0x9b5c20` (PS3 `0x30fb28` + `0x7069c8`, the same pair `0x4ae0cc` uses).
