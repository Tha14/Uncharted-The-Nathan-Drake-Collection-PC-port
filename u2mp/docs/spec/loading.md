# Spec: the multiplayer loading screen (loading)

Clean-room description for a new implementation of `gow3_pc/src/u2mp/loading.cpp`. It states what
the code must do, as seen from outside. It does not prescribe structure, helper names or loop
shapes. All addresses are image offsets of the Uncharted 2 PS4 1.02 build unless they are called
"absolute".

Notation:

- `B` is `image.base` (`u2mp::image`). An image offset `X` is the absolute address `B + X`.
- `M` is the menu manager, image offset `addr::menu_manager` = `0x1136640`. `M+0xNN` is an image offset.
- `CTX` is `M + 0x10` (absolute `B + 0x1136650`): the menu draw context the game's item calls take.
- "u32", "s32", "s16", "u8", "u64", "f32" are little-endian host values of that size.
- "guest call F(a, b, ...)" means calling the game function at `B + F` with the x86-64 System V
  convention (`U2MP_GUEST`): integer/pointer arguments in rdi, rsi, rdx, rcx, r8, r9, then on the
  stack in order; float return in xmm0, integer return in rax.
- All floating point is IEEE single precision (`float`) unless "double" is said. Every operation is
  rounded on its own, evaluated in the order written (left to right, parentheses as shown). No fused
  multiply-add, no fast-math reassociation (the build has no `-march`).
- `sin`, `cos`, `tan`, `fmod`, `hypot`, `ceil` on floats mean the single-precision C runtime
  functions (`std::sin(float)` etc.). One place uses double `cos`; it is marked.
- "clamp(x, lo, hi)" means `std::clamp` semantics; "min" / "max" as `std::min` / `std::max`.
- "now" is the host monotonic clock (`std::chrono::steady_clock`). "Seconds since T" is the elapsed
  steady-clock duration converted to `std::chrono::duration<float>` (float seconds), unless "double" is said.

---

## 1. Purpose and hook points

The PS4 build has no multiplayer loading screen. The PS3 game draws one while a match loads: a black
screen, a slowly moving "table" of picture cards (a fort picture, player photos, map photos, money and
coins) seen through a slight perspective, two edge overlays, the two team rosters sliding in, a title
band with the map name and game description, and a tip line at the bottom whose letters fade in and
out one after the other. This module draws that screen in the PS4 build, in the menu's 1280 x 720
units, through the PS4 menu item calls and the deferred 2D arena.

Hook points:

1. **Show / hide requests.** Six TaskManagerOnline state functions (vtable slots) are wrapped. Two
   "Enter" states request "show"; four other states request "hide". A request is only recorded; it is
   applied once per frame (see 4.2), because the PS3 game polls a flag once a frame and so never sees
   the brief hide between LoadLevels::Exit and WaitReady::Enter that happen in the same frame.
2. **Per-frame draw.** The call instruction at `0x3fc52d` (inside the frame's Draw, it calls the HUD
   manager's frame update `0x554340`) is retargeted to a host function. That function first calls
   `0x554340` itself, then applies a pending request, then draws the screen while it is shown or still
   fading out.
3. **Tip glyph callback.** A host function is placed in the tip's text struct as its per-glyph callback;
   the game's text printer calls it for every character (see 4.9).

Not in scope (nothing is drawn for them): the PS3 letterbox bars (only below 719 lines), a PS3 tunable
read at the start of the PS3 draw, and the PS3 condition "draw rosters only while a certain menu field
is 0".

## 2. Interface

### 2.1 Declarations other files use

- `void install_loading(int &installed);` in namespace `u2mp`. Declared in `frontend3.h` and in
  `u2mp_internal.h`. Called once from `u2mp_install` (install.cpp) after `install_unlocks` and before
  `install_dlc`. Nothing else in this module is used by other files.

### 2.2 Functions this module uses from other modules

- `uint8_t *menu_net_material(uint32_t id);` (menuimages.cpp): a net image material by id, or NULL.
  It may print its own log lines; call order therefore matters (see 4.4).
- `void menu_draw_quad(uint8_t *material, float x, float y, float w, float h, float u0, float v0,
  float u1, float v1, float alpha, float rgb);` (menuimages.cpp): a textured quad into the deferred 2D
  arena, colour (rgb, rgb, rgb, alpha).
- `retarget_call`, `image.matches`, `log`, `sid` (install.cpp / u2mp_internal.h).

### 2.3 Entry points the game reaches

| Entry | Where | Convention | Expected before install |
|---|---|---|---|
| Per-frame draw | call at `0x3fc52d` retargeted via `retarget_call(0x3fc52d, 0x554340, host, "loading screen draw (Draw 0x3fc52d)")` | `u64 f(void *tables, u64 a, u64 b)`, System V | an `E8` call whose target is `0x554340` (checked by `retarget_call`) |
| LoadLevels::Enter | u64 at `0x100ac08` (vtable `0x100ac00` + 0x08) | `u64 f(void *state)` | `B + 0x5f2e50`, or a host pointer |
| LoadLevels::Exit | u64 at `0x100ac10` (vtable `0x100ac00` + 0x10) | same | `B + 0x5f2e30`, or a host pointer |
| WaitReady::Enter | u64 at `0x100abc8` (vtable `0x100abc0` + 0x08) | same | `B + 0x5f2d90`, or a host pointer |
| WaitReady::Exit | u64 at `0x100abd0` (vtable `0x100abc0` + 0x10) | same | `B + 0x5f2830`, or a host pointer |
| Playing::Enter | u64 at `0x100ab88` (vtable `0x100ab80` + 0x08) | same | `B + 0x5f23e0`, or a host pointer |
| Leave::Enter | u64 at `0x100a9c8` (vtable `0x100a9c0` + 0x08) | same | `B + 0x5f00d0`, or a host pointer |
| Tip glyph callback | stored at text struct +0xa0 | `float f(void *printer_state, uint8_t *glyph_struct)`, System V, float in xmm0 | n/a |

"A host pointer" = a non-NULL value outside `[B, B + image.size)`: another module's wrapper installed
earlier (cinema.cpp wraps WaitReady::Exit and Leave::Enter before this module runs). It is chained.

Show / hide meaning of the six slots, with the name used in log lines (exact text):

| Slot | Request | Name |
|---|---|---|
| `0x100ac08` | show | `LoadLevels::Enter` |
| `0x100ac10` | hide | `LoadLevels::Exit` |
| `0x100abc8` | show | `WaitReady::Enter` |
| `0x100abd0` | hide | `WaitReady::Exit` |
| `0x100ab88` | hide | `Playing::Enter` |
| `0x100a9c8` | hide | `Leave::Enter` |

Order of the six slots (install checks and writes them in this order): the table order above.

Other modules that touch the same places: scoreboard.cpp (installed later) reads the current target
of the call at `0x3fc52d` (this module's stub) and chains it. This module must therefore leave a plain
`E8` call there (which `retarget_call` does).

## 3. Game facts

### 3.1 Guest functions called

| Offset | Meaning | Arguments as called here |
|---|---|---|
| `0x554340` | HUD manager frame update (original target of the call at `0x3fc52d`) | `(tables, a, b)` passed through; returns u64 |
| `0xa15e70` | DC table lookup by id | `(u32 id)` -> pointer to `{s32 count; s32 pad; u8 *records}` or NULL |
| `0xcddc00` | text lookup | `(B + 0x1066660, u32 text id)` -> `const char *` or NULL |
| `0x64abd0` | game description text (`"%s - %s - %s"`) | `(B + 0x113b760, char *buf, int 128, u8 *net game record, B + 0x113b760 + 0x21b4, int 0, int 1, int 1)`; the last two on the stack |
| `0x5b5450` | menu item: text in a rect | `(CTX, NULL, f32 *xform10, const char *text, f32 *rect4, int 1, int 1, int 0, f32 *clip4, int 0, int 0, int 0, int -1)`; arguments 7..13 on the stack |
| `0x5b5920` | menu item: descriptor in a rect | `(CTX, NULL, f32 *xform10, u8 *desc, f32 *rect4, int 1, int 1)`; the last one on the stack |
| `0x5a2670` | page layout | `(u8 *page, u8 *page_desc)` |
| page vtable +0x10 | page update | `(u8 *page, u8 *update_args)` |
| page vtable +0x18 | page item render | `(u8 *page, NULL, u8 *page_desc)` |
| list vtable +0x00 / +0x30 / +0x38 | team list getters: current / first / last row | `(u8 *list)` -> s32 |
| `0x5b4e90` | menu slot quad draw | `(CTX, NULL, u8 *quad, int slot)`, slot 8 (gallery picture) or 10 (no material: solid colour) |
| `0xbdce90` | material quad draw | `(u8 *ctx48, u64 *info4, u8 *quad)` |
| `0xcc9a70` | text printer | `(u8 *text_struct, const char *text)` |
| `0xcc7cc0` | default glyph draw | `(printer_state, u8 *glyph_struct)` -> f32 advance |
| `0xbda9c0` | queue deferred text (copies the struct and the string) | `(B + 0x124a000, u8 *text_struct, const char *text)` |

### 3.2 Data addresses and fields

| Address / field | Type | Meaning |
|---|---|---|
| `0x113b760` | object | NetGameManager (`ngm`) |
| `0x113b760 + 0x1ca0` | s16 | current net game index |
| `0x113b760 + 0x21b4` | object | ngm overrides (passed to `0x64abd0`) |
| `0x1066660` | object | text database (first argument of `0xcddc00`) |
| `0x104ad28` | u64 pointer | data root; the C string at (root + 0x408) is the base directory |
| `0x1116284` (`0x1116180 + 0x104`) | s32 | frame counter |
| `0x1050e58` | f32 | item fade (written and restored around the roster draw; also read for tip +0x90) |
| `0x104ed74` | f32 | fade scale |
| `0x144e648` (`0x144e640 + 8`) | u64 pointer | font 1; NULL when not loaded |
| `0x124a000` | object | deferred 2D arena |
| `0x1007910` | vtable | class vtable of a team list control (item kind 10) |
| `M + 0x370` | u64[200] | page object pointers; the list ends at the first NULL |
| page +0x10 | u32 | page id |
| page +0xb0 | u32 | page flags (0x80000: draw at 0.75 scale; 0x800: no fade scale) |
| page +0xb4 | s32 | item count |
| page +0xb8 | u64 pointer | item pointer array |
| page +0xf4 | f32 | page fade |
| page +0xf8 | s32 | layout snap counter (2 = place items without animation for the next layouts) |
| item +0x00 | u64 | item vtable (absolute) |
| item +0x10 | s32 | list condition (logged) |
| item +0x18 | u64 pointer | list object |
| `M + 0x17a4` | s32 | menu value copied into the page desc +0x48 |
| `M + 0x120` (= CTX + 0x110) | s32 | gallery slot state (0 idle, 1 requested, 3 loaded) |
| `M + 0x138` (= CTX + 0x128) | u8 | gallery loaded flag (logged) |
| `M + 0x268` | char[0x100] | gallery path; last byte `M + 0x367` |
| `M + 0x368` | s32 | gallery keep-alive frames |

### 3.3 DC tables

| Id | Record size | Fields used |
|---|---|---|
| `0x3b9a067d` (net games) | 0x90 | +4 u32 level id; +0xc s32 game type |
| `0x0dec97a4` (levels) | 0x38 | +0 u32 level id; +4 u32 name text id |
| `0x7f306129` (tips) | 12 | +0 u32 game type; +4 u32 generic flag; +8 u32 text id |

### 3.4 Ids

| Id | Name |
|---|---|
| `0xb9df67d6` | page `PAGE_MULTIPLAYER_LOADING` (= `sid("PAGE_MULTIPLAYER_LOADING")`) |
| `0x6732636e` | `general/net/loading-screen/edge` |
| `0x270e3564` | `general/net/loading-screen/title-back` |
| `0x9595f0a7` | `general/net/loading-screen/money` |
| `0xb4b036fa` | `general/net/loading-screen/coins` |

Player photos, in table order (index 0..15), all `sid("general/net/player-photos/<name>")`:

| # | name | id | # | name | id |
|---|---|---|---|---|---|
| 0 | drake | `0x05a8ba5f` | 8 | soldier-2b | `0x26fd249e` |
| 1 | elena | `0x01ef2812` | 9 | soldier-3a | `0xf9a7c39b` |
| 2 | chloe | `0x01f45731` | 10 | lazarevic | `0xeabed4cf` |
| 3 | sullivan | `0x87f4823f` | 11 | lieutenant | `0x4ea5310c` |
| 4 | rescuer | `0xa6da247b` | 12 | merc | `0x6477b06a` |
| 5 | soldier-1a | `0x59555d94` | 13 | jeff | `0x3a459fd0` |
| 6 | soldier-1b | `0x54167b4d` | 14 | schafer | `0x09a8d7c7` |
| 7 | soldier-2a | `0x2bbe0247` | 15 | flynn | `0xf69a236c` |

Map photos, in table order (index 0..8), all `sid("general/net/map-photos/<name>")`:
0 warzone `0x83b0526a`, 1 warzone-coop `0x4409a1a8`, 2 shambhala `0x7c3df559`, 3 gompa `0xed98d85e`,
4 treasure `0x1e1227ac`, 5 ice-cave `0x308b816c`, 6 village `0x00ce50f3`, 7 village-coop `0xae5c9059`,
8 train-wreck `0x9d998d63`.

### 3.5 Colour transform (10 floats)

Layout: [0] scale, [1] [2] [3] [4] colour multipliers r g b a, [5] [6] adds, [7] multiplier,
[8] [9] translation x y.

- Identity: [0] [1] [2] [3] [4] [7] = 1, the rest 0.
- "Set colour 0xAARRGGBB": [1] = R x k, [2] = G x k, [3] = B x k, [4] = A x k, where k is the float
  `1.0f / 255.0f` and each byte is converted to float first (multiply by the reciprocal; do not divide).
- "Compose(d, s)": [i] = d[i] x s[i] for i in 0, 1, 2, 3, 4, 7; [i] = d[i] + s[i] for i in 5, 6, 8, 9.
- Colours: map name `0xffcd4100`; game description `0xffdfcf8f`; black fill `0xff000000`.

### 3.6 Fill descriptor (0x2c bytes, passed to `0x5b5920`)

All bytes 0 except: +0x00 u32 = 0x1b (solid colour rect); +0x1c s32 = -1; +0x24 s32 = -1;
+0x28 f32 = 1.0. Buffer aligned to 16.

### 3.7 Page draw descriptor (0x50 bytes, aligned to 8)

| Offset | Value |
|---|---|
| +0x00 | u64 = absolute `CTX` |
| +0x08..+0x2f | the 10-float transform: identity with [4] = alpha, [8] = x, [9] = y; then [0] multiplied by 0.75 when page flags & 0x80000 |
| +0x30..+0x3f | f32 {0, 0, 1280, 720} |
| +0x40 | f32 = float(s32 at `0x1116284`) x (`1.0f / 300.0f`) (multiply by the float reciprocal) |
| +0x44 | u8 = 1; +0x45..+0x47 = 0 |
| +0x48 | s32 = s32 at `M + 0x17a4` |
| +0x4c | 0 |

Page update arguments (16 bytes): +0 u64 absolute address of `M`; +8 s32 = 0; +12 f32 = frame time `dt`.

### 3.8 Menu quad (0x110 bytes, aligned to 16)

Four vertices of 0x40 bytes, then +0x100 u32 = 0 and 12 zero bytes. Vertex: +0x00 x, +0x04 y,
+0x08 z (= 0), +0x0c u, +0x10 v, +0x14..+0x1f zero, +0x20..+0x2f colour r g b a, +0x30..+0x3f second
colour (all 1.0 for slot draws via `0x5b4e90`; all 0 for material draws via `0xbdce90`). All four
vertices carry the same colour.

`0xbdce90` context: 48 zero bytes (aligned to 16). Info: four u64 {absolute material pointer, 0, 0, 0}.

### 3.9 Text struct (0xb0 bytes, PS4 layout)

| Offset | Field |
|---|---|
| +0x00 / +0x04 | f32 x / y |
| +0x10..+0x1f | clip f32 {0, -1, 0, -1} (= no clip) |
| +0x20 / +0x24 | f32 scale x / y = 0.75 |
| +0x28 | u32 colour; alpha is the top byte (default 0xffffffff) |
| +0x2c | f32 = 1.0 |
| +0x30 | s32 font |
| +0x34 | u32 flags: 4 = measure only, 1 = mode 1, 0x10 = break at the width limit |
| +0x50..+0x8f | 4x4 f32 matrix, identity (1.0 at +0x50, +0x64, +0x78, +0x8c) |
| +0x90 | f32 (1.0 by default; set to the item fade before queueing) |
| +0x94 / +0x98 | f32 width / height (the measured size is returned here by the printer) |
| +0xa0 | u64 glyph callback (0 = default glyph draw) |

Every other byte is 0. "A fresh text struct (w, h, flags, font)" below means: all 0xb0 bytes zero, then
the fields above with the given width, height, flags and font.

### 3.10 Random generator

A multiply-with-carry generator with five u32 words of state: x0..x3 and carry c. Initial state
(once per process, never reseeded): x0 = 0x2545f491, x1 = 0x9e3779b9, x2 = 0x7f4a7c15,
x3 = 0x6a09e667, c = 0x01234567.

One step: S (64-bit unsigned) = 2111111111 x3 + 1492 x2 + 1776 x1 + 5115 x0 + c. Then x3 = old x2,
x2 = old x1, x1 = old x0, x0 = low 32 bits of S, c = high 32 bits of S. The step returns the new x0.

"Random unit" = float(step result) (u32 to float, round to nearest) times the float constant whose
bits are 0x2f7ffffe (the literal `2.3283061589829401e-10f`).

The cards and the tip share this one generator; the order in which they consume it matters.

### 3.11 Float constants (bit patterns)

| Value | Bits | Use |
|---|---|---|
| 0.78539819 | 0x3f490fdb | money card spread |
| 0.94247788 | 0x3f71463b | photo spread |
| 0.1 | 0x3dcccccd | coin spread, several factors |
| 2.0923009 | 0x4005e842 | camera phase step per index |
| 0.20943953 | 0x3e567751 | camera 0 angle factor |
| 0.15707964 | 0x3e20d97c | camera tilt amplitude |
| 0.024674013 | 0x3cca2128 | projection half angle |
| 1.6666666 | 0x3fd55555 | fade-in remap |
| 0.6666667 | 0x3f2aaaab | roster slide |
| 0.013333334 | 0x3c5a740e | tip glyph time scale |
| 0.0066666668 | 0x3bda740e | tip duration scale |
| 3.14159274 | 0x40490fdb | pi for the cosine step |

Derived projection constants (computed once, in float): f = 1.0f / tan(0.024674013f); s = sin(0.01f);
c = cos(0.01f).

## 4. Behaviour

### 4.1 Module state

Initial values: shown = false; fade = 0; pending request = none; map name = NULL; description text
= empty (128-byte buffer); cards started = false; four card systems empty; card seed = 0; tip index,
tip glyph counter, tip time and tip length = 0; card log budget = 8; two "log the page" flags false;
"have a previous frame" = false. The tip tick clock base is taken when the module is loaded (static
initialisation), not at install.

Ticks: int32 of (double seconds since the tick base x 300.0), truncated toward zero.

Threading: the state hooks may run on another thread than the draw. The pending request and the
shown flag are atomic. The map name and description are read and written under one lock. The card
systems, card start and the generator are under a second lock; the tip takes that second lock only
around its one generator step.

### 4.2 Show / hide requests (state hooks)

Each hooked slot calls the function that was in the slot at install time with the same `state`
argument and returns its u64 result unchanged.

- For a "hide" slot: record the request first, then call the original.
- For a "show" slot: call the original first, then record the request.

Recording a request (`on` = show or hide, `why` = the slot's name):
- If the module was turned off by the environment, nothing happens (unreachable in practice).
- Store `why` as the latest reason, store the request (replacing any pending one), and log
  `loading screen: %s requested (%s)` with `show` or `hide`, and the name.

### 4.3 Per-frame entry (the retargeted call at `0x3fc52d`)

1. Guest call `0x554340(tables, a, b)` with the three arguments received. Keep its result.
2. Frame time `dt`: 0 on the first call ever; otherwise min(seconds since the previous call, 0.1).
   Remember now as the previous call time.
3. Apply a pending request (below).
4. If shown, or fade > 0: draw the screen with `dt` (4.4).
5. Return the result of step 1.

Applying a pending request: take the pending value and clear it (atomically). If there was none, or
it equals the current shown state, stop (no log; the request is consumed). Otherwise:
- On show, in this order: prepare the texts (4.5); set the "shown at" time to now; if cards are on
  and ready, start the cards (4.6); set tip time and tip length to 0 (tip index is kept); refresh the
  roster page (4.7).
- Set shown to the new state.
- Log `loading screen: %s (%s)` with `shown` or `hidden`, and the latest reason.

Hiding does nothing else: the screen fades out over the following frames.

### 4.4 Drawing the screen (one frame)

Let `target` = shown.

1. If target and cards are on and ready: request the gallery picture (4.8).
2. Fade: if target, fade = min(fade + dt, 1); else fade = max(fade - dt, 0). If fade <= 0, stop here.
3. Alpha `a`: if target, clamp((fade - 0.4) x 1.6666666, 0, 1); else `a` = fade.
   `t` = seconds since "shown at".
4. Base transform = identity with [4] = a.
5. **Black fill.** Transform = Compose(base, identity with colour 0xff000000). Guest call `0x5b5920`
   with that transform, the fill descriptor (3.6) and rect {0, 0, 1280, 720}. Not affected by the text
   switch.
6. **Cards.** If cards are on and ready: draw the cards with `a` and `target` (4.6).
7. **Edges.** m = menu_net_material(0x6732636e). If m is not NULL: menu_draw_quad(m, 0, 0, 1280, 720,
   0, 0, 1, 1, alpha a, rgb a), then menu_draw_quad(m, -10, -10, 1290, 730, 0, 0, 1, 1, a, a).
8. **Rosters.** slide = 1 - clamp((t - 2) x 0.6666667, 0, 1). Draw the roster page (4.7) with
   alpha = a x clamp(t - 3, 0, 1), x = 0, y = (slide x slide) x 200, and `dt`.
9. **Band.** m = menu_net_material(0x270e3564). If not NULL: menu_draw_quad(m, 128, 22, 1152, 278,
   0, 0, 1, 1, alpha a x 0.75, rgb a).
10. Take the map name pointer and a copy of the 128-byte description under the text lock.
11. **Map name.** Transform = Compose(Compose(base, identity with [0] = 1.4), C1) where C1 = identity,
    colour 0xffcd4100, then [5] = 3.0. Draw text (below) with the map name in rect {0, 70, 1280, 100}.
12. **Game description.** Transform = Compose(base, C2), C2 = identity, colour 0xffdfcf8f, then
    [5] = 3.0. Draw text with the description copy in rect {0, 120, 1280, 100}.
13. **Tip.** If the tip is on, the text switch is on, and the tip is ready: draw the tip (4.9).

Draw text: skipped when the text switch is off, or the string is NULL or empty. Otherwise guest call
`0x5b5450` (3.1) with a copy of the transform, the string, the rect {x, y, w, h} and a clip of
{0, 0, 0, 0}. Rect, clip and transform buffers are aligned to 16.

### 4.5 Preparing the texts (on show)

1. Look up the net games table (`0xa15e70(0x3b9a067d)`), then the levels table
   (`0xa15e70(0x0dec97a4)`). Both lookups always happen, in this order.
2. Read the net game index (s16 at `0x113b760 + 0x1ca0`).
3. Under the text lock: map name = NULL, description = empty string (first byte 0).
4. If the net games table is NULL, or its records pointer is NULL, or the index is < 0 or >= count:
   stop (no further calls).
5. Record = records + 0x90 x index. Level id = u32 at record +4.
6. If the levels table and its records are not NULL: for every level record i from 0 to count - 1
   (0x38 bytes each), if its u32 +0 equals the level id, map name = text(u32 at +4). All matches are
   visited; the last match wins. text(0) gives NULL without a guest call; text(id) is guest call
   `0xcddc00(B + 0x1066660, id)`.
7. Guest call `0x64abd0` (3.1) with the 128-byte description buffer and the record. Then force the
   buffer's last byte (index 127) to 0.

The guest writes the description directly into the module's 128-byte buffer (aligned to 16).

### 4.6 Cards

#### Card record

Each card has: x, y, angle, material (pointer or NULL), type (0 = own material, 1 = gallery picture
via slot 8, 2 = solid colour via slot 10), w, h, orientation (0..3 = one quarter of a 2x2 sheet,
-1 = whole), r, g, b. A system holds a count and up to 16 cards.

#### Spawning a card into a system (material, w, h, orientation, spread)

If the system already has more than 15 cards, nothing happens (and no random number is drawn).
Otherwise append a card, drawing six random units r1..r6 in this order:
x = r1 x 1180 + 50; y = r2 x 620 + 50; angle = spread x -0.5 + spread x r3; type 0; the given
material, w, h, orientation; r = r4 x 0.2 + 0.8; g = r5 x 0.2 + 0.8; b = r6 x 0.2 + 0.8.
A card with a NULL material is still added (it is skipped at draw time but still logged).

#### Frame rectangles

Adding the frame to a system: if it has more than 12 cards, nothing. Otherwise append four type-2
cards, angle 0, material NULL, orientation -1, colour (0, 0, 0), centre and size:
(640, -500) 5280 x 1000; (-1000, 360) 2000 x 720; (640, 1220) 5280 x 1000; (2280, 360) 2000 x 720.

#### Start (on show, cards on and ready)

Under the card lock, in this order:
1. Card start time = now.
2. Seed = r + r, r = one random unit.
3. All four systems emptied.
4. System A: one type-1 card at (640, 360), angle 0, material NULL, 1280 x 720, orientation -1,
   colour (1, 1, 1). Then the frame rectangles. Then spawn money: material =
   menu_net_material(0x9595f0a7), 512 x 512, orientation -1, spread 0.78539819.
5. System B, ten player photos: for each, draw k = (step result) & 15, retrying while photo k was
   already used, but at most 100 draws (the 100th draw is taken even if used); mark k used; spawn
   menu_net_material(photo id k), 256 x 256, orientation -1, spread 0.94247788. The material lookup
   happens after k is chosen and before the spawn's random draws.
6. Clear the "used" marks for indexes 0..8 only. Four map photos the same way with
   k = (step result) mod 9 (unsigned): spawn menu_net_material(map id k), 512 x 256, orientation -1,
   spread 0.94247788.
7. System C: one lookup coins = menu_net_material(0xb4b036fa); twelve spawns i = 0..11 with that
   material, 100 x 100, orientation i & 3, spread 0.1.
8. System D stays empty. Cards started = true (never set back to false).

#### Smoothstep and window

S(x, e0, e1): 0 if x < e0; 1 if x > e1; else tt = clamp((x - e0) / (e1 - e0), 0, 1) and the result is
(tt x tt) x 3 + ((tt x tt) x tt) x -2.
Window W(x) = S(x, 0.5, 0.5 + 0.1) - S(x, 1.0 - 0.1, 1.0) (the bounds computed in float).

#### Cameras (time t = seconds since card start, index idx = 0 or 1)

- ph = fmod(t x 0.07, 1); k = float(idx) x 2.0923009; b = k x 0.75 + (t + seed); w = W(ph).
- idx 0: alpha = 1 - w; ang = b x 0.20943953; px = cos(ang); py = sin(ang).
- idx 1: alpha = w; ang = b x 0.15; amp = (1 - cos(ang + ang)) x 0.25 + 0.5; px = amp x sin(ang);
  py = amp x cos(ang x 0.5).
- x = px x 250; y = py x 150.
- cz = float of the **double** cos(double(t) x 0.1 + double(k)) (0.1 as a double). s = clamp(cz x 0.5
  + 0.5, 0, 1). zoom = s x 1.25 + (1 - s).
- angle = sin(t x 0.1 + k) x 0.15707964.

#### Drawing the cards (alpha `a`, `target`)

Under the card lock. If cards have never been started, nothing. Otherwise:
1. t = seconds since card start. Camera 0 and camera 1 at t.
2. Both alphas multiplied by a. inv = 1 - a; sq = inv x inv; kk = clamp(1 - sq, 0, 1);
   zf = (1 - kk) x 1.2 + kk. Camera 1 zoom x= zf, camera 0 zoom x= zf.
3. If not target (fading out): camera 1 y += sq x 50, camera 0 y += sq x 50.
4. If camera 1 alpha != 0: draw systems A, B, C, D in order with camera 1.
5. Then, if camera 0 alpha != 0: draw systems A, B, C, D with camera 0.

#### Drawing one system with a camera

For each card i from 0 while i < count and i < 16, in order:

1. Texture range: by orientation 0: u 0..0.5, v 0..0.5; 1: u 0.5..1, v 0..0.5; 2: u 0..0.5,
   v 0.5..1; 3: u 0.5..1, v 0.5..1; any other: u 0..1, v 0..1. Call them u0, u1, v0, v1. For type 1,
   swap v0 and v1.
2. Flat placement of a local point (lx, ly):
   dx = (cam.x + card.x) - 640; dy = (cam.y + card.y) - 360; pc = cos(card.angle),
   ps = sin(card.angle); cc = cos(cam.angle); cs = sin(cam.angle).
   X = (lx x pc - ly x ps) + dx; Y = (lx x ps + ly x pc) + dy;
   X2 = X x cc - Y x cs; Y2 = X x cs + Y x cc;
   flat point = (X2 x zoom + 640, Y2 x zoom + 360).
3. Projection of a flat point (x, y) when perspective is on: xc = x - 640; yc = 360 - y;
   W = yc x s + 30; screen = (640 + (f x xc) / W, 360 - ((f x yc) x c) / W). When perspective is off,
   screen = flat point. Depth of a flat y: perspective on: (360 - y) x s + 30; off: 1.
4. hw = w x 0.5; hh = h x 0.5. Colour = (card r, g, b, camera alpha).
5. Cell count n: 1 for type 2 or when perspective is off. Otherwise project the four corners
   (-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh) (flat placement, then projection); edge = the largest of
   hypot(dx, dy) of the four sides (corner k to corner k+1, wrapping), starting from 0;
   n = clamp(int(ceil(edge / 96)), 1, 12).
6. For each cell row gy = 0..n-1 (outer) and column gx = 0..n-1 (inner):
   - a0 = float(gx) / float(n); a1 = float(gx + 1) / float(n); b0, b1 the same with gy.
   - Local corners in order: (-hw + (2 x hw) x a0, -hh + (2 x hh) x b0), (… a1, … b0),
     (… a1, … b1), (… a0, … b1); cell texture fractions in the same order: (a0, b0), (a1, b0),
     (a1, b1), (a0, b1).
   - For each corner: flat point (x, y); u = u0 + (u1 - u0) x ua; v = v0 + (v1 - v0) x vb;
     d = depth(y) - 1.
   - Clip the four corners against d >= 0, going around the edges k -> k+1 (wrapping): output corner
     k when its d >= 0; then, when exactly one of corner k and corner k+1 has d >= 0, output the edge
     point at fr = d_k / (d_k - d_{k+1}): x, y, u, v each = value_k + (value_{k+1} - value_k) x fr.
     This gives m points (0..8). If m < 3, the cell is skipped.
   - Project every output point (step 3).
   - Submit a fan of quads: for j = 1, 3, 5, … while j + 1 < m: the quad of points
     (0, j, j + 1, min(j + 2, m - 1)) with their screen positions and u, v. With 4 points this is one
     quad (0, 1, 2, 3); with 3 points (0, 1, 2, 2); with 5 points (0, 1, 2, 3) then (0, 3, 4, 4).
7. Submitting a quad (3.8, colour from step 4):
   - Type 0: if the material is NULL, nothing. Otherwise guest call `0xbdce90(ctx48, info, quad)`
     with the second colour left 0.
   - Type 1 or 2: second colour (1, 1, 1, 1) on all vertices, guest call
     `0x5b4e90(CTX, NULL, quad, 8)` for type 1 or `(…, 10)` for type 2.
8. After the card's cells: if the card log budget is > 0 and the type is not 2, decrement the budget
   and log `loading screen: card type %d material %p %.0fx%.0f at %.0f,%.0f, %dx%d cells, gallery state %d loaded %d`
   with type, material pointer, w, h, card x, card y (as doubles), n, n, s32 at `M + 0x120`, u8 at
   `M + 0x138`. The budget is 8 for the whole process.

### 4.7 Roster page

Finding a page by id: walk the u64 pointers at `M + 0x370` + 8 i for i = 0..199; stop at the first
NULL; the first page whose u32 +0x10 equals the id is the result.

Refresh (on show): find page 0xb9df67d6. If found, write s32 2 at page +0xf8. Log
`loading screen: rosters page %08x %s%s` with the id, `found, refreshed (+0xf8 = 2)` or `NOT FOUND`,
and `` (empty) or ` (rosters off)` (when the roster switch is off). Then set both "log the page"
flags (first frame, visible) to true, also when the page was not found.

Draw (alpha, x, y, dt), every frame the screen is drawn:
1. If the roster switch is off: nothing. Find the page; if not found: nothing.
2. Guest call page vtable +0x10 (vtable = u64 at page +0) with (page, update args 3.7).
3. Build the page desc (3.7) with alpha, x, y. Read the page flags (u32 +0xb0) after the update
   call; if flags & 0x80000, desc transform [0] x= 0.75.
4. Save the f32 at `0x1050e58`. Write it as (1 - f32 page +0xf4) x (1.0 if flags & 0x800, else the
   f32 at `0x104ed74`).
5. Guest call `0x5a2670(page, desc)`. Then guest call page vtable +0x18 with (page, NULL, desc)
   (the vtable pointer is the one read before step 2; its +0x18 entry is read only now, after the update and layout calls).
6. Restore the saved f32 at `0x1050e58`.
7. If the "first frame" flag is set: clear it and log the page with `updated and drawn (first frame)`.
   Then, if alpha >= 1 and the "visible" flag is set: clear it and log the page with
   `updated and drawn (visible)`.

Logging the page (with a `when` text):
- n = s32 page +0xb4; items = u64 page +0xb8. Log `loading screen: rosters page %s: %d items` (when, n).
- If items is not NULL: for i = 0 while i < n and i < 8: item = items[i]; skip a NULL item. If the
  item's vtable (u64 +0) is not `B + 0x1007910`, log `loading screen:   item %d vtable 0x%llx`
  (i, vtable - B, as unsigned 64-bit). Otherwise list = u64 item +0x18; current, first, last = -9;
  if list is not NULL, guest calls in this order: list vtable +0x00 (current), +0x30 (first),
  +0x38 (last), each with (list). Log
  `loading screen:   item %d: team list (cond %d), list %p: rows %d..%d (current %d)`
  (i, s32 item +0x10, list, first, last, current). Note the three spaces after `screen:`.

### 4.8 Gallery request (every frame drawn while shown, cards on and ready)

1. Guest call `0xa15e70(0x3b9a067d)`; read the s16 net game index. If the table is NULL, or the index
   is < 0 or >= count: stop. (The records pointer is not checked here.)
2. root = u64 at `0x104ad28`; if NULL, stop.
3. path = `"%s/build/main/gallery1/%s"` formatted with the C string at root + 0x408 and
   `loading-screens/fort.dds`, into a 512-byte buffer (snprintf truncation).
4. Write s32 10 at `M + 0x368` (always, once step 2 passed).
5. If the string at `M + 0x268` differs from path (strcmp): copy path into `M + 0x268` with strncpy
   semantics for 0x100 bytes (zero-fill the rest of the 256 bytes), then write byte 0 at `M + 0x367`;
   then if the s32 at `M + 0x120` is 0, set it to 1 (otherwise leave it).

### 4.9 Tip line

Cosine step E(x, lo, hi): 0 if x < lo; 1 if x > hi; else tt = min((x - lo) / (hi - lo), 1), then
tt = max(tt, 0); r = (cos(tt x 3.14159274) - 1) x -0.5; result = min(max(r, 0), 1).

Glyph alpha for glyph number i: e = float(ticks() - tip time) x 0.013333334 + float(i) x -0.1 (the
subtraction in 32-bit integers, ticks read at that moment); d = float(tip length) x 0.0066666668;
alpha = fade x (E(e, 0, 1) - E(e, d - 1, d)). `fade` is the module's raw fade (not `a`).

Glyph callback (called by the printer with (state, glyph struct)):
- If the glyph struct's flags (u32 +0x34) have bit 4: return the guest call `0xcc7cc0(state, glyph)`
  with the original struct; the counter is not touched.
- Otherwise alpha = glyph alpha for the current counter value; then counter += 1.
- If alpha != 0: make a copy of the first 0xa0 bytes of the glyph struct; add 1.0 to its y (+0x04);
  set its colour (+0x28) to (low 8 bits of int32(alpha x 128)) << 24 (black, all other bits 0);
  guest call `0xcc7cc0(state, copy)` and ignore the result.
- Then make a fresh copy of the first 0xa0 bytes of the original glyph struct; replace the top byte
  of its colour with the low 8 bits of int32(alpha x 255), keeping the low 24 bits; return the result
  of guest call `0xcc7cc0(state, copy)`.
- int32(...) is truncation toward zero; negative values are possible (short tips) and give their
  low 8 bits. The copy buffer is 0xb0 bytes, aligned to 16; only its first 0xa0 bytes are defined.

Draw the tip (one frame):
1. tips = `0xa15e70(0x7f306129)`. If NULL, or records NULL, or count <= 0: stop.
2. Guest call `0xa15e70(0x3b9a067d)`. Game type = s32 at net game record +0xc when that table and its
   records are not NULL and 0 <= index < count; otherwise 0.
3. now = ticks().
4. If tip time + tip length < now (32-bit signed): choose a new tip:
   - Take one generator step r (under the card lock).
   - old = tip index; index = 0; n = (r mod count, unsigned) + 1.
   - Repeat at most count x 8 times: if the record at index is eligible (s32 +0 == game type, or
     u32 +4 != 0), n -= 1; advance index by one, wrapping to 0 after count - 1; if n is now 0: if index
     == old, set n = 1 and go on; otherwise stop. (So the chosen tip is the record right after the
     n-th eligible one, whatever its own eligibility; the previous tip is avoided. When the repeat
     limit is reached, the index stays where it is.)
   - s = text(u32 +8 of record index) (4.5 rules). tip time = now; tip length =
     int32(float(strlen(s) or 0 when NULL) x 75).
5. s = text(u32 +8 of record index) again (a second lookup, also right after choosing). If NULL or
   empty: stop.
6. font = 1 when the u64 at `0x144e648` is not NULL, else -1.
7. Measure: fresh text struct (w 600, h 9999, flags 4 | 0x10, font). Guest call `0xcc9a70(struct, s)`.
   Read width W = f32 +0x94 and height H = f32 +0x98.
8. Draw: fresh text struct (w W, h 9999, flags 1 | 0x10, font); x = (1280 - W) x 0.5; y = 650 - H;
   callback (+0xa0) = the glyph callback; glyph counter = 0; +0x90 = the f32 at `0x1050e58`. Guest
   call `0xbda9c0(B + 0x124a000, struct, s)`. Then set flag bit 4 in the struct (flags = 0x15) and
   guest call `0xcc9a70(struct, s)` (the callback then sees measure passes and counts nothing).

The struct buffer is 0xb0 bytes aligned to 16 and is reused for the three calls.

## 5. Install-time behaviour

`install_loading(installed)`, in this order:

1. If the environment variable `UNCHARTED_U2MP_LOADING` is exactly `0`: mark the module off, log
   `loading screen: off (UNCHARTED_U2MP_LOADING=0)`, return. Nothing is patched.
2. Switches, each turned off only when its variable is exactly `0`: `UNCHARTED_U2MP_LOADING_TEXT`
   (texts and tip), `UNCHARTED_U2MP_LOADING_ROSTERS` (roster page), `UNCHARTED_U2MP_LOADING_PARTICLES`
   (cards and gallery request), `UNCHARTED_U2MP_LOADING_PERSPECTIVE` (projection, cell split and
   clipping), `UNCHARTED_U2MP_LOADING_TIP` (tip). Each defaults to on.
3. For `0x5b5450`, `0x5b5920`, `0x5a2670`, `0x64abd0` in this order: the bytes must be
   `55 48 89 e5`. At the first mismatch log `loading screen: 0x%llx is not a function start, not installed`
   (the offset) and return.
4. If cards are on: cards ready = all of these match (checked in this order, stopping at the first
   mismatch):
   - `0x5b4e90`: `55 48 89 e5 41 57 41 56 41 55 41 54`
   - `0xbdce90`: `55 48 89 e5 41 57 41 56 41 54 53`
   - `0x58b5c8`: `41 c7 85 68 03 00 00 0a 00 00 00 b9 08 04 00 00`
   - `0x5b46dc`: `41 83 bd 10 01 00 00 02`
   If not ready, log `loading screen: card calls not as expected, cards off`. If cards are off, cards
   ready stays false and nothing is logged.
5. If the tip is on (regardless of the text switch): tip ready = all of these match, in order:
   - `0xcc9a70`: `55 48 89 e5 41 57 41 56 41 55 41 54`
   - `0xcc7cc0`: the same 12 bytes
   - `0xbda9c0`: `55 48 89 e5 41 57 41 56 41 54 53`
   - `0xccb602`: `48 8b bd b0 fe ff ff 48 8d b5 f0 fe ff ff ff 95 b8 fe ff ff`
   If not ready, log `loading screen: tip calls not as expected, tip off`.
6. For each of the six slots in table order (2.3): if the u64 equals `B + expected function`, fine.
   Else if it is 0 or inside `[B, B + image.size)`: log
   `loading screen: state vtable slot 0x%llx holds 0x%llx, not 0x%llx: not installed` (slot offset;
   the value minus B when the value >= B, else the raw value; the expected function offset) and
   return. Otherwise (a pointer outside the image) count it as chained.
7. `retarget_call(0x3fc52d, 0x554340, per-frame entry, "loading screen draw (Draw 0x3fc52d)")`. If it
   fails, return (it logs its own line). The slots are not touched in that case.
8. For each slot in order: remember its current u64 as that slot's original, then write the absolute
   address of the matching host wrapper.
9. Log `loading screen: PS3 port installed (%zu state hooks, %d chained; text %s, rosters %s, cards %s, perspective %s, tip %s)`
   with 6, the chained count, and `on`/`off` for: text switch; roster switch; cards on and ready;
   perspective switch; tip on and ready.
10. `installed` += 1. (It is incremented only on this path.)

## 6. Test scenarios

Time: the screen uses the host steady clock in four places (frame time, "shown at", card start, tip
ticks). Scenarios that compare float arguments need a controlled clock or the same clock source
for old and new; otherwise compare only call sequences and integer data.

Install:
1. `UNCHARTED_U2MP_LOADING=0`: off log, no patch, `installed` unchanged.
2. Each of the four prologues broken in turn: the "not a function start" log for that offset, no patch.
3. Card patterns: each of the four broken in turn -> "card calls" log, install still succeeds with
   `cards off`. `UNCHARTED_U2MP_LOADING_PARTICLES=0` -> no card log, `cards off`.
4. Tip patterns: each of the four broken -> "tip calls" log, `tip off`. `UNCHARTED_U2MP_LOADING_TIP=0`.
5. Slots: all original; one slot NULL; one slot pointing elsewhere inside the image (not installed log);
   two slots holding host pointers (chained = 2, the originals called through).
6. Call at `0x3fc52d` not an `E8`, or calling elsewhere: retarget fails, slots unchanged, no install log.
7. Switch combinations in the install log (`TEXT=0`, `ROSTERS=0`, `PERSPECTIVE=0`).

Show / hide:
8. Show slot then a frame: original called before the request log; on the frame: text preparation,
   card start, roster refresh, "shown" log. Hide slot: request log before the original.
9. Hide then show in the same frame while shown: no restart, no applied log. Show while already shown:
   consumed silently. Hide while hidden: consumed silently.
10. Return values of the originals passed through.

Text preparation:
11. No net games table; NULL records; index -1; index = count: no description call, no map name.
12. Levels table NULL; no matching level; two matching levels (last wins; text called for each);
    name text id 0 (no text call).
13. Description buffer filled with 128 non-zero bytes by the fake: byte 127 forced to 0.

Frame drawing:
14. First frame after show (dt = 0): only the gallery request; fade stays 0, nothing drawn.
15. Fading in: `a` remapped (fade 0.3 -> a 0; fade 0.7 -> a 0.5); fading out: `a` = fade; fade reaching
    0 stops drawing; dt capped at 0.1.
16. Edge and band materials present / missing (quads skipped).
17. `TEXT=0`: fill, cards, edges, rosters, band drawn; no text calls, no tip.
18. Empty map name / empty description: the text call is skipped.

Rosters:
19. Page not found (refresh logs NOT FOUND, no draw calls); page found: update, layout, render order;
    flags 0x80000 and 0x800 combinations (desc [0], item fade value), fade value restored afterwards.
20. Page log: first-frame log once; visible log only once alpha >= 1; items NULL; n > 8; NULL item;
    non-list item vtable; list item with NULL list (-9 values) and with a list (getter order).
21. `ROSTERS=0`: refresh log has ` (rosters off)`, no page calls.

Cards:
22. Start with all materials present; with some photo materials NULL (cards still spawned and logged,
    not drawn); generator sequence identical (check spawned positions through the quad calls).
23. Photo selection with a fake table where duplicates occur (retry path).
24. Camera alphas: ph inside and outside the window (one camera or both drawn; alpha 0 skips a pass).
25. Fading out: cards sink by sq x 50.
26. Perspective on: cell counts 1..12, a frame rectangle partly behind W = 1 (clipped fan with 3 and 5
    points), a cell fully clipped (skipped). Perspective off: one quad per card, no clipping.
27. Card log: only the first 8 non-type-2 cards over the whole process.
28. Gallery request: data root NULL; first request (path written, zero-filled, state 0 -> 1); state
    already 2 (left alone); same path again (only +0x368 written).

Tip:
29. Tips table missing / empty. Text missing or empty (nothing drawn, length 0).
30. Tip choice: several eligible records, one generic record, choice landing on the previous tip
    (n reset to 1), no other eligible record (repeat limit reached).
31. Font pointer NULL (font -1) and set (font 1).
32. Callback with flag 4 (pass-through, counter unchanged); with alpha 0 (no shadow); with alpha > 0
    (shadow then glyph, colours as specified); negative alpha for a 1-character tip.
