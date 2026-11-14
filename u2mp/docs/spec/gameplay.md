# Spec: gameplay group (scoreboard, rosternames, visibility, gameplay3, matchend, spawn, scripttime)

Behaviour description for a clean-room reimplementation of seven u2mp source files in `gow3_pc/src/u2mp/`.
It states what each file must do, as seen from outside: guest calls, image memory, log lines, install results.
It does not prescribe code structure, helper names or variable names.

## 0. Common ground

### 0.1 Conventions

- All addresses are image offsets of the Uncharted 2 PS4 1.02 image. "Image + X" means `image.base + X`.
- "u8/u16/s16/u32/s32/u64/f32" are little-endian values of that size in image memory.
- Pointers stored in game memory are absolute host addresses (the image is relocated). A vtable slot "holds X"
  means it holds the absolute address `image.base + X`.
- Every guest function is called with the x86-64 System V convention (`U2MP_GUEST`). Every host function the
  game reaches is a `U2MP_GUEST` function. Integer/pointer arguments go in rdi, rsi, rdx, rcx, r8, r9, then the
  stack. Float arguments go in xmm0, xmm1, ... independently of the integer registers.
- All float arithmetic below is IEEE single precision unless "double" is written. No fused multiply-add.
  "1/300 (float)" is the single-precision value with bits `0x3B5A740E` (0.0033333334...). "-1/300 (float)" is
  `0xBB5A740E`.
- Log lines go through `u2mp::log(format, ...)` (it adds the `u2mp: ` prefix and a newline). The format strings
  below are given exactly, between backticks. `%llx` arguments are 64-bit; `%u`/`%d`/`%x` arguments are 32-bit.
- "Installed counter": the `int &installed` argument of each `install_*` function. Each successful piece adds 1.

### 0.2 Facilities the files use (from `u2mp_internal.h` / `install.cpp`, unchanged)

- `image` (`base`, `size`, `segments`, `executable(off, len)`, `matches(off, bytes, len)`, `at<T>(off)`).
- `log(...)`, `near_stub(bytes)`, `retarget_call(offset, expected_target, host, name)`.
- `sid(const char *)` (CRC-32 string id, see header), `package_by_id(u32 id)` (session.cpp: a loaded package by
  id, or NULL; not a guest call, but a test may record it).
- `addr::menu_manager` = `0x1136640`.
- `retarget_call` prints its own log lines (`%s: call at 0x%llx -> C++`, and the failure lines) with the `name`
  passed in. That name is part of the contract and is given for each use below. Each successful
  `retarget_call` takes one 16-byte `near_stub`. Number and order of `near_stub` requests must match this spec.

### 0.3 Public interface (unchanged declarations)

In `u2mp_internal.h` (namespace `u2mp`), already declared:

```
void install_spawn(int &installed);
void install_visibility(int &installed);
void install_scripttime(int &installed);
void install_scoreboard(int &installed);
void install_rosternames(int &installed);
void install_matchend(int &installed);
void install_gameplay3(int &installed);
```

One more function, not in the header, defined by the visibility file and called by the matchend file. Both files
declare it themselves in namespace `u2mp`:

```
void visibility_kick();
```

No other symbol of these files is used elsewhere. All state is file-private.

### 0.4 Install order (install.cpp, unchanged)

`install_gameplay3` (early, after gameplay2, before cinema) ... `install_spawn`, (nullsites), `install_scripttime`,
(hud), `install_scoreboard` (after loading.cpp wrapped the 2D draw call), `install_rosternames`,
`install_matchend` (after session.cpp wrapped the Playing state slots), `install_visibility`.

### 0.5 Shared game facts

| Address / offset | Meaning |
|---|---|
| `0x113b760` | NetGameManager ("ngm"). ngm+0 = u64 pointer to the local player tracker (NULL when none). |
| ngm+`0x10` + i*`0x2c0`, i = 0..9 | the 10 player trackers (absolute `0x113b770` + i*0x2c0) |
| ngm+`0x1c9c` (`0x113d3fc`) | s32 team count |
| ngm+`0x1ca0` (`0x113d400`) | s16 index of the current *net-games* record |
| ngm+`0x1d5c` (`0x113d4bc`) | s32 round start (300 Hz frames) |
| ngm+`0x1d60` (`0x113d4c0`) | s32 match start (300 Hz frames) |
| ngm+`0x1d74` (`0x113d4d4`) | s32 rounds played |
| ngm+`0x1e80` (`0x113d5e0`) | s32 NGM state (2 = playing) |
| ngm+`0x1e84` (`0x113d5e4`), ngm+`0x1e88` (`0x113d5e8`) | s32 end state A / B (written by the EndGame Apply) |
| ngm+`0x21b4` (`0x113d914`) | s32 score limit |
| ngm+`0x21b8` (`0x113d918`) | f32 time limit, seconds |
| ngm+`0x21c0` (`0x113d920`) | s32 round limit |
| `0x14ce97c` | s32 NET_SM (network state machine; < 2 = not in multiplayer; 29 = NET_SM_RESULTS) |
| `0x1116180` | clock block. +`0x28` (`0x11161a8`) s32 frame number the collision probes compare with; +`0x104` (`0x1116284`) s32 frame counter used for menu time; +`0x204` (`0x1116384`) s32 realtime clock ticks (clocks[7], 300 Hz); +`0x210` (`0x1116390`) s32 clocks[7] delta ticks of this frame |
| `0xa15e70` | guest `LookupPointer(u32 id)`: script-data symbol lookup, returns a pointer or NULL |
| `0x3b9a067d` | sid of `*net-games*`: table {s32 count at +0; u64 records at +8}, record stride `0x90`; record +4 = u32 level id, record +0xc = s32 game type |
| `0x0dec97a4` | sid of `*levels*`: table {s32 count at +0; u64 records at +8}, record stride `0x38`; record +0 = u32 level id, +0x18 = char* name, +0x20 = u32 package id |
| `0x100ab80` | vtable of the Playing state: slot 2 (`0x100ab90`) Exit = `0x5f2350`, slot 3 (`0x100ab98`) Update = `0x5f14d0` |
| `0x1136640` | menu manager |

---

## 1. scoreboard (`scoreboard.cpp`)

### 1.1 Purpose

On PS4 nothing shows the in-match scoreboard page (`PAGE_MULTIPLAYER_SCOREBOARD`). The per-frame update and draw
of the net player tracker's overlay entries were removed. This file shows entry 0 (the scoreboard) when the player
presses a pad button during a match, fades it in and out, and draws its page every frame. It runs once per frame,
after the frontend 2D draw, by retargeting the call at `0x3fc52d` (chaining whatever that call already reaches,
normally loading.cpp's wrapper).

### 1.2 Interface

- `void install_scoreboard(int &installed)`.
- Entry point: the call instruction at `0x3fc52d` (`E8 rel32`). After install it reaches a host function H with
  three u64 arguments (rdi, rsi, rdx) that returns u64 (rax).

### 1.3 Game facts

| Fact | Value / meaning |
|---|---|
| `0x554340` | original target of the call at `0x3fc52d` (the per-frame 2D/HUD draw) |
| `0x3fc52d` | the call site (`E8 rel32`) |
| `0x5a2670` | guest page layout function `(page, desc*)` |
| Prologue check bytes | `55 48 89 E5` at both `0x5a2670` and `0x554340` |
| `0x104f544` | net player tracker overlay object ("tracker") |
| tracker+`0x08` (`0x104f54c`) | entry 0 shown flag, u8 |
| tracker+`0x0c` (`0x104f550`) | entry 0 fade, f32 |
| tracker+`0x10` (`0x104f554`) | entry 0 fade speed, f32 (1.0 in the shipped image) |
| tracker+`0x18` (`0x104f55c`) | entry 0 page id, u32, must be `0xf650325d` |
| `0xf650325d` | sid("PAGE_MULTIPLAYER_SCOREBOARD") |
| `0x14b7fb0` | u32 raw pad buttons of this frame (scePad bits) |
| `0x100000` | default button mask (touchpad; PS3 SELECT) |
| menu+`0x10` | menu draw context (its address is passed, not its value) |
| menu+`0x370` | array of 200 u64 page pointers; the list ends at the first NULL |
| menu+`0x17a4` | s32 copied into the draw descriptor |
| page+0 | page vptr. vtable slot 2 (+0x10) = Update(page, args*); slot 3 (+0x18) = Render(page, NULL, desc*) |
| page+`0x10` | u32 page id |
| page+`0xb0` | u32 page flags. Bit `0x80000`: scale x0 by 0.75. Bit `0x800`: item fade ignores the global scale |
| page+`0xf4` | f32 page fade |
| page+`0xf8` | s32; set to 2 when the scoreboard is shown (positions are set again on the next layout) |
| `0x1050e58` | f32 global "item fade" used during page layout/render |
| `0x104ed74` | f32 global fade scale |
| `0x14ce97c` | NET_SM |
| ngm+`0x1e80` | NGM state |
| `0x1116180+0x104` | frame counter for the descriptor time |

### 1.4 Behaviour

State kept by the module (initial values): shown = false; fade = 0.0; speed = 1.0; previous buttons = 0;
"first frame" = true; "page log pending" = true (once per process).

**Page lookup.** Walk the menu manager's page pointer array (menu+0x370, up to 200 entries, stop at the first NULL).
The result is the first page whose u32 at +0x10 equals `0xf650325d`, or none.

**Each time H is called (once per frame):**

1. Call the previous target of the call site (the address the call reached before install) with the same three
   arguments. Keep its u64 result; H returns it at the end.
2. Compute dt (float seconds) from a monotonic host clock: 0.0 on the first call; afterwards the time since the
   previous call, capped at 0.1. Remember the current time.
3. "Playing" = s32 NET_SM > 1 and s32 ngm+0x1e80 == 2.
4. Read the u32 pad buttons. "Pressed" = a bit is set now, was not set in the previous frame's value, and is in
   the button mask. Store the current value as the previous value (every frame, playing or not).
5. If not playing: request "hidden" with reason `not playing`.
   Else if pressed: set speed to 2.0 and write 2.0 (f32) to tracker+0x10 (on every press). Then request the
   opposite of the current shown state with reason `button`.
6. Fade: fade = clamp(fade + (shown ? dt : -dt) * speed, 0.0, 1.0). Write fade (f32) to tracker+0x0c every frame.
7. If fade <= 0.0, stop here. Look up the page; if none, stop here.
8. ease = shown ? fade*fade : 1 - (1-fade)*(1-fade). y = (1 - ease) * (shown ? 20.0 : -20.0). Draw the page
   (below) with alpha = fade, x = 0.0, y, and dt.

Notes: speed never returns to 1.0 once a button was pressed. The page keeps drawing while it fades out, also when
not playing.

**Request shown state (on, reason):** nothing happens if `on` equals the current shown state. Otherwise:
- set shown = on; write u8 1 (on) or 0 (off) to tracker+0x08;
- if on: look up the page; if found, write s32 2 at page+0xf8. Then, only the first time this happens in the
  process, look the page up again and log `scoreboard: page %08x %s` with `0xf650325d` and `found` or
  `NOT FOUND`;
- log `scoreboard: %s (%s)` with `shown`/`hidden` and the reason.

**Draw the page (page, alpha, x, y, dt):**

1. Read the page vptr once.
2. Call vtable slot 2 (the entry read from vptr+0x10) as `(page, &args)`. args is 16 bytes:
   +0 u64 = image + 0x1136640 (menu manager address), +8 s32 = 0, +0xc f32 = dt.
3. Build a descriptor of 0x50 bytes, 8-aligned, all zero, then:
   - +0x00 u64 = image + 0x1136650 (menu+0x10, the address);
   - f32 at +0x08, +0x0c, +0x10, +0x14 = 1.0; +0x18 = alpha; +0x1c = 0; +0x20 = 0; +0x24 = 1.0; +0x28 = x; +0x2c = y;
   - f32 at +0x30 = 0, +0x34 = 0, +0x38 = 1280.0, +0x3c = 720.0;
   - +0x40 f32 = (float)(s32 at 0x1116284) * 1/300 (float multiply);
   - +0x44 u8 = 1 (+0x45..+0x47 zero);
   - +0x48 s32 = s32 at menu+0x17a4 (`0x1137de4`); +0x4c = 0.
   - If page flags (u32 page+0xb0) have bit 0x80000: f32 at +0x08 = 1.0 * 0.75.
4. Save the f32 at `0x1050e58`. Write to it: (1 - f32 page+0xf4) * (flags bit 0x800 ? 1.0 : f32 at `0x104ed74`).
5. Call guest `0x5a2670(page, &desc)`.
6. Call vtable slot 3 (entry read from vptr+0x18 now, vptr from step 1) as `(page, NULL, &desc)`; the same
   descriptor buffer as step 5 (whatever the layout wrote into it).
7. Write the saved value back to `0x1050e58`.

### 1.5 Install

In this order:
1. If env `UNCHARTED_U2MP_SCOREBOARD_BUTTON` is set: parse it as hex like `strtoul(v, NULL, 16)` with a 32-bit
   unsigned long (Windows: "0x" prefix accepted, junk stops parsing, overflow gives 0xffffffff, empty or invalid
   gives 0). That value becomes the button mask. If it is 0: log `scoreboard: off (UNCHARTED_U2MP_SCOREBOARD_BUTTON=0)`
   and return (nothing installed).
2. Check `55 48 89 E5` at `0x5a2670` and at `0x554340`. Else log
   `scoreboard: page layout / 2D draw not as expected, not installed` and return.
3. Check u32 at `0x104f55c` == `0xf650325d` and f32 at `0x104f554` == 1.0. Else log
   `scoreboard: tracker 0x%llx is not as expected, not installed` with `0x104f544`, return.
4. Check `0x3fc52d` is executable for 5 bytes and its byte is `0xE8`. Else log
   `scoreboard: 0x%llx is not a call, not installed` with `0x3fc52d`, return.
5. The current target = `0x3fc532 + rel32` (as an image offset; it may lie outside the image when another module
   already retargeted the call). Remember its absolute address as the previous target.
6. `retarget_call(0x3fc52d, current target, H, name)` with name `scoreboard: 2D draw` when the current target is
   `0x554340`, else `scoreboard: 2D draw (chained)`. If it fails, return.
7. installed += 1. Log `scoreboard: PAGE_MULTIPLAYER_SCOREBOARD port installed (button mask 0x%x; PS3 0x4cbf00 / 0x4cbc10)`
   with the mask.

The image is not checked for NULL here.

### 1.6 Test scenarios

- Env unset; env `0`; env `0x200`; env `zz` (gives 0, off).
- Each install failure: wrong prologue at either function; page id or speed wrong in the tracker; no `E8` at the call
  site; call site already pointing elsewhere (chained name); call reaching `0x554340` (plain name).
- Per frame: first frame (dt 0); not playing (NET_SM 1; NGM state != 2); playing without press; press (rising edge),
  held button (no new press), press again (hide), release+press with another bit outside the mask.
- Page present / absent / list with NULL before it / list of 200 entries; page flags 0x80000 and 0x800 set and clear.
- Fade reaching 1.0 and 0.0; leaving "playing" while shown (hidden, fade out keeps drawing with y < 0).
- The once-only page log (first show only), with the page absent on the first show.
- dt comes from the host clock; a test that needs fixed values should only compare frames where dt is 0 (first
  call) or arrange equal timing, or replace the clock in both builds the same way.

---

## 2. rosternames (`rosternames.cpp`)

### 2.1 Purpose

Team lists (loading-screen rosters, in-match scoreboard, results) print each player's 0x40-byte display name.
On PS4 the code that wrote the display name is dead, so the names are empty. Before a team list builds its rows
and before it asks for a cell's text, this file copies each player's nick into an empty display name, in the
NetGameManager trackers and in both NetStatGame tables.

### 2.2 Interface

- `void install_rosternames(int &installed)`.
- Entry points: the team list vtable `0x100a280`. Slot 2 (`0x100a290`, row build, originally `0x5b0f70`) and slot 14
  (`0x100a2f0`, cell text, originally `0x5b1e90`) are replaced by host functions. Each host function takes six u64
  arguments (rdi, rsi, rdx, rcx, r8, r9) and returns u64.

### 2.3 Game facts

| Fact | Value / meaning |
|---|---|
| `0x100a280` | team list vtable |
| `0x5b0f70` | slot 2: row build |
| `0x5b1e90` | slot 14: cell text |
| `0x5b24fc` | check bytes `4B 8D B4 3C 18 39 00 00` (the cell text reads the name at record +0x3918) |
| `0x66c998` | check bytes `49 8D BC 1D 20 39 00 00` (NetStatGame start copies tracker +0x60 into the record) |
| tracker = `0x113b770` + i*`0x2c0`, i = 0..9 | +`0x20`: nick (C string, read up to 0x3f chars); +`0x60`: display name (0x40 bytes) |
| `0x1147bd8` | NetStatGame table 1 (current game) |
| `0x114d2ec` | NetStatGame table 2 (previous-game copy, `0x1147bd0+0x571c`) |
| table+0 | u32 record count |
| table + i*`0x80` + `0x38f8` | record i nick (read up to 0x1f chars) |
| table + i*`0x80` + `0x3918` | record i display name |
| max records | 10 |

### 2.4 Behaviour

**Fill names** (runs at the start of both hooks):
1. For each tracker i = 0..9 in order: if the nick's first byte is non-zero and the display name's first byte is
   zero, copy the nick into the display name: n = length of the nick but at most 0x3f; copy n bytes, then write
   a 0 byte at display+n. Count it.
2. For table 1, then table 2: read the u32 count. For i = 0 .. min(count, 10) - 1: the same rule with the record's
   nick and display name, but n at most 0x1f. Count it.
3. If at least one name was filled and this log line was printed fewer than 8 times so far in the process: log
   `rosternames: %d display name(s) filled from the nick (trackers +0x60, NetStatGame +0x3918)` with the count.

Display names that already hold text are never changed. No matchmaking prefix is added.

**Row build hook / cell text hook:** fill names, then call the original function (`0x5b0f70` resp. `0x5b1e90`)
with the same six arguments and return its result.

### 2.5 Install

1. If `image.base` is NULL, return silently.
2. Check the bytes at `0x5b24fc` and `0x66c998`. Else log `rosternames: team list / NetStatGame bytes differ, left alone`
   and return.
3. Slot 2, then slot 14, each independently:
   - If slot address + 8 > image size: skip silently.
   - If the slot does not hold the expected function: log
     `rosternames: team list vtable 0x%llx slot %u is not 0x%llx, left alone` (`0x100a280`, slot number, expected
     offset); skip.
   - Else write the host function's address into the slot, log `rosternames: team list %s 0x%llx -> C++ (vtable 0x%llx slot %u)`
     with name `row build` / `cell text`, the original offset, `0x100a280`, slot number; installed += 1.

### 2.6 Test scenarios

- Install: NULL image; each check-byte mismatch; slot 2 wrong, slot 14 right (and the reverse); both right (+2).
- Fill: empty nick; display already set; nick of exactly 0x3f / 0x40+ chars in a tracker (display gets 63 chars + 0);
  record nick of 31 / 32+ chars; count 0, 3, 10, 12 (only 10 used); both tables.
- Log limit: nine calls that each fill something (only eight lines); calls that fill nothing (no line, not counted).
- Both hooks pass all six arguments through and return the original's value.

---

## 3. visibility (`visibility.cpp`)

### 3.1 Purpose

Remote players are only "visible" (name tags, spotting) when a line-of-sight collision probe from the camera to the
player was kicked in this or the previous frame. On PS4 nothing kicks these probes any more. This file kicks one
sphere-cast probe per remote player per frame, after Playing::Update (via `visibility_kick()` from matchend), and
releases the probes when the Playing state exits.

### 3.2 Interface

- `void install_visibility(int &installed)`.
- `void visibility_kick()`: called by the matchend hook once per frame, right after the original Playing::Update.
- Entry point: Playing vtable slot 2 (`0x100ab90`, Exit, originally `0x5f2350`) is replaced by a host function
  `(state*) -> u64`.

### 3.3 Game facts

| Fact | Value / meaning |
|---|---|
| tracker = `0x113b770` + i*`0x2c0`, i = 0..9 | +`0x20` u8 used; +`0x120` u32 net id; +`0x26c` u8 excluded; +`0x1b0` probe object |
| ngm+0 (`0x113b760`) | u64 pointer to the local tracker (NULL = none); its +0x120 is the local net id |
| `0x1127dd0` (`0x1125370+0x2a60`) | u64; non-zero when the collision world exists |
| `0x104e820` | camera position, 4 x f32 |
| `0x151f048` | u64 default collision filter object |
| `0x151f058` | u64 kick argument A |
| `0x151f060` | u32 kick argument B |
| `0x151f068` | u64 kick argument C |
| `0x11161a8` | s32 probe frame number |
| `0x665f70` | guest player position `(vec4 *out, tracker*)` |
| `0x5566b0` | guest view test `(out*, f32, const vec4 *pos)`; out+`0x2c` s32 non-zero = rejected |
| `0x1004180` | sphere probe vtable (probe+0 must hold it) |
| probe vtable slots | 1 Init `0x4f1dc0`; 2 Kick `0x4f2130`; 5 Release `0x4f2490`; 7 SetRayDir `0x4f25b0`; 9 SetFilter `0x4f26c0`; 12 IsActive `0x4f2850`; 32 SetRadius `0x4f2600` |
| Init arguments | count 1, hits 32, flags 0, type 13, buffer NULL, size 0 |
| ray length / radius | 1.0 / 0.01 (f32 bits `0x3C23D70A`) |
| filter | {26, 0} with the default filter object |
| Playing::Exit | vtable slot `0x100ab90`, original `0x5f2350` |

Check bytes:

| Address | Bytes |
|---|---|
| `0x665f70` | `55 48 89 E5 41 57 41 56 41 54 53 48 81 EC 80 00 00 00 49 89 FC` |
| `0x5566b0` | `55 48 89 E5 41 56 53 48 83 EC 30 48 89 FB` |
| `0x66618c` | `4C 8D B0 B0 01 00 00 48 63 90 D4 01 00 00` |
| `0x662797` | `48 8D 05 E2 19 9A 00 48 89 83 B0 01 00 00` |
| `0x63e813` | `E8 48 79 02 00` |

### 3.4 Behaviour

State: a "kicked" flag per tracker (10, all false); a kick counter (0); the exit frame, a signed 64-bit value
starting at -1; "ready" (false until install succeeds).

**Probe virtual calls.** Every call to a probe slot reads the probe's vptr (probe+0) and the slot entry at that
moment, then calls the entry with the probe as the first argument.

**Release one probe:** only if probe+0 holds `image + 0x1004180`: call slot 12 `(probe)`; if it returns true (al
non-zero), call slot 5 `(probe)`.

**visibility_kick():**
1. Return if not ready, if s32 NET_SM < 2, or if the u64 at `0x1127dd0` is 0.
2. Return if the exit frame equals the s32 at `0x11161a8` (sign-extended). (So a frame value of -1 also returns
   while no Exit has happened yet.)
3. Read the local tracker pointer (u64 ngm+0). Return if NULL.
4. For each tracker i = 0..9 in order:
   a. Read its u32 net id and the local tracker's u32 at +0x120.
   b. If its u8 used is 0, or its u8 excluded is non-zero, or its net id equals the local net id: if the kicked flag
      is set, clear it and release the probe (rule above). Go to the next tracker.
   c. If probe+0 does not hold `image + 0x1004180`, go to the next tracker (kicked flag unchanged).
   d. target = 16 bytes, 16-aligned, zeroed. Call `0x665f70(&target, tracker)` (return value ignored).
   e. view = 0x80 bytes, 16-aligned, zeroed. Call `0x5566b0(&view, 0.0f, &target)`. If the s32 at view+0x2c is
      non-zero, go to the next tracker (kicked flag unchanged, nothing released).
   f. start = copy of the 16 bytes at `0x104e820`. delta[k] = target[k] - start[k] for all 4 floats (w too).
   g. Calls, in this order:
      1. slot 1 `(probe, 1, 32, 0, 13, NULL, 0)`: four u32 in esi, edx, ecx, r8d; NULL in r9; u32 0 on the stack.
      2. slot 7 `(probe, 0, &start, &delta, 1.0f)`: u32 0 in esi; pointers in rdx, rcx; 1.0 in xmm0.
      3. slot 32 `(probe, 0, 0.01f)`: u32 0 in esi; 0.01 in xmm0.
      4. slot 9 `(probe, 0, 26, 0, &obj)`: u32 in esi, edx, ecx; r8 = pointer to a local u64 copy of the value at
         `0x151f048`.
      5. slot 2 `(probe, A, B, C, 0, 1)`: rsi = u64 at `0x151f058`, edx = u32 at `0x151f060`, rcx = u64 at
         `0x151f068`, r8d = 0, r9d = 1.
   h. Set the kicked flag. Add 1 to the kick counter; if it is now 1, log
      `visibility: first probe kicked (tracker %d, net id %u)` with i and the net id (once per process).

**Playing::Exit hook (state):**
1. Exit frame = s32 at `0x11161a8` (sign-extended).
2. Release all: for each tracker i = 0..9 whose kicked flag is set: clear the flag; if the u64 at `0x1127dd0` is 0,
   skip (not counted); else release its probe (rule above) and count it (counted even when the vptr did not match
   or IsActive returned false).
3. If the count is non-zero, log `visibility: %d probe(s) released (%s)` with the count and `Playing::Exit`.
4. Call the original Exit (the slot's value before install, i.e. `image + 0x5f2350`) with state; return its u64.

### 3.5 Install

1. If `image.base` is NULL, return silently.
2. For slots 1, 2, 5, 7, 9, 12, 32 in this order: if `0x1004180 + 8*slot + 8` > image size, or the slot does not hold
   its function: log `visibility: probe vtable 0x%llx slot %u is not 0x%llx, not installed` (`0x1004180`, slot,
   expected offset) and return (first failing slot only).
3. If any of the five check-byte runs differs: log
   `visibility: player position / view test / reader / tracker ctor bytes differ, not installed`, return.
4. If `0x14ce97c + 4`, `0x104e820 + 16` or `0x151f068 + 8` exceeds the image size: return silently.
5. If slot `0x100ab90` does not hold `0x5f2350`: log `visibility: Playing::Exit slot 0x%llx is not 0x%llx, not installed`
   (`0x100ab90`, `0x5f2350`), return.
6. Remember the slot value as the original Exit; write the Exit hook's address into the slot; set ready; log
   `visibility: KickVisibilityJob (PS3 0x494c38) ready; Playing::Exit 0x%llx releases the probes` (`0x5f2350`);
   installed += 1.

### 3.6 Test scenarios

- Install: each slot mismatch (log names the first bad slot); each check-byte mismatch; Exit slot wrong; success.
- visibility_kick: not ready; NET_SM 1; no collision world; exit frame equal to the frame (also the initial -1 case);
  no local tracker.
- Trackers: unused; excluded; local net id; previously kicked then unused (release with IsActive true / false;
  vptr wrong); wrong vptr; view rejected (with and without an earlier kick); view passes (full call sequence,
  first-kick log only once); several remote trackers in one frame.
- Exit hook: kicked trackers with and without the collision world; none kicked (no log); a kick in the same frame
  after Exit is skipped, the next frame kicks again; the original Exit's return value passes through.

---

## 4. gameplay3 (`gameplay3.cpp`)

### 4.1 Purpose

At match end the game enters the ShowResults state. On PS4 its Enter function is a bare `ret`, so the in-game menu
stays open, NET_SM never becomes NET_SM_RESULTS, and the results end time is never written. ShowResults::Update
computes the time left as `end - now/300` with `end` folded to 0.0, so the host leaves the results screen at once.
This file supplies a ShowResults::Enter (close the in-game menu, store end = now + 10 s, NET_SM = 29) and patches
the Update's time computation to add the stored end time.

### 4.2 Interface

- `void install_gameplay3(int &installed)`.
- Entry points:
  - ShowResults vtable `0x100aa40`, slot 1 (`0x100aa48`, Enter; original `0x5f0ea0`) -> host function `(state*)`,
    no return value.
  - Code patch at `0x5f0b50` (inside ShowResults::Update `0x5f0b30`): a `jmp` to a stub that returns to `0x5f0b67`.

### 4.3 Game facts

| Fact | Value / meaning |
|---|---|
| `0x14ce97c` | NET_SM; 29 = NET_SM_RESULTS |
| `0x113a040` | u64 pointer to the net clock; clock+`0xc` = s32 frames (300 Hz) |
| `0x1136640` | menu manager; +`0x1014` u32 menu state (0, 5, 6 = nothing open / closing); +`0x1018` u32 state argument; +`0x101c` f32 fade |
| `0xccf830` | guest menu event init `(event*, u32, u64)` |
| `0x5cd5c0` | guest menu event `(menu*, event*, u32 code)` |
| `0xe62288` | f32 -1/300 read by ShowResults::Update (bits `0xBB5A740E`) |
| `0x100aa40` | ShowResults vtable [dtor, Enter, Exit, Update] |
| `0x5f0ea0` | original ShowResults::Enter (ret stub; cinema.cpp may later replace its body) |
| `0x5f0b50`..`0x5f0b67` | the 0x17 bytes of Update that load and scale the clock |
| results length | 10.0 seconds |

Check bytes:

| Address | Bytes |
|---|---|
| `0x5f0e90` (ShowResults::Exit) | `48 8B 47 08 C7 80 48 01 00 00 00 00 80 3F C3` |
| `0x5f1264` (menu-close code in Resetting::Enter) | `48 8D 1D D5 53 B4 00 8B 83 14 10 00 00 83 F8 06 77 0A B9 61 00 00 00` |
| `0x5f0af0` (ShowRoundResults::Enter) | `C7 05 82 DE ED 00 1E 00 00 00` |
| `0x5f128e` | a call (`E8`) whose target is `0xccf830` |
| `0x5f129e` | a call (`E8`) whose target is `0x5cd5c0` |
| `0x5f0b50` | `48 8D 05 E9 94 B4 00 48 8B 00 C5 FA 2A 40 0C C5 FA 59 0D 21 17 87 00` |

### 4.4 Behaviour

Module state: a 16-byte, 16-aligned constant block {f32 -1/300 (`0xBB5A740E`), f32 end = 0.0, 0, 0}. The countdown
stub reads it. Only "end" ever changes.

**Countdown stub** (in executable memory from `near_stub`, within rel32 range of `0x5f0b50`). On entry it computes,
using only rax, xmm0, xmm1:
- rax = u64 at (image + 0x113a040); xmm0 = (float) s32 at rax+0xc (low lane; `vcvtsi2ss xmm0,xmm0,mem` form so the
  upper lanes of xmm0 are kept);
- xmm1 = xmm0 * block[0] (`vmulss xmm1,xmm0,mem`);
- xmm1 = xmm1 + block[1] (`vaddss xmm1,xmm1,mem`);
- rax = address of the block (left in rax);
- jump to image + `0x5f0b67`.
Byte form used by the current build (51 bytes; any equivalent is acceptable if a test does not compare stub bytes):
`48 B8 <u64 image+0x113a040>`, `48 8B 00`, `C5 FA 2A 40 0C`, `48 B8 <u64 block address>`, `C5 FA 59 08`,
`C5 F2 58 48 04`, `FF 25 00 00 00 00`, `<u64 image+0x5f0b67>`.
With end = 0.0 the result equals the original code's.

**ShowResults::Enter hook (state):**
1. Close an open in-game menu: read u32 menu state at `0x1137654`. If it is 0, 5 or 6, skip this step. Otherwise:
   a. event = 0x100 bytes, 16-aligned, zeroed.
   b. Call `0xccf830(&event, 0, 0)`.
   c. Call `0x5cd5c0(image + 0x1136640, &event, 3)`.
   d. Write u32 5 at `0x1137654`, u32 0 at `0x1137658`, f32 1.0 at `0x113765c`.
   e. Call `0x5cd5c0(image + 0x1136640, &event, 0)` (the same event buffer).
2. now = (float)(clock frames) * 1/300 (float multiply, `0x3B5A740E`). Clock frames = 0 if the u64 at `0x113a040` is
   NULL, else the s32 at that pointer + 0xc.
3. end = now + 10.0 (float). Store it in the block.
4. Write s32 29 to `0x14ce97c`.
5. Log `ShowResults::Enter: NET_SM_RESULTS, results end at %.2f s (now %.2f)` with end and now as double.
6. Call the guest function at the fixed address `0x5f0ea0` with state (whatever its body is then: the ret stub or
   cinema.cpp's replacement).

### 4.5 Install

1. If `image.base` is NULL, return silently.
2. If `0x100aa40 + 0x20` or `0x14ce97c + 4` exceeds the image size: return silently.
3. If the bytes at `0x5f0e90`, `0x5f1264` or `0x5f0af0` differ: log
   `gameplay3: ShowResults / menu anchors differ, nothing installed`, return.
4. If `0x5f128e` / `0x5f129e` are not `E8` calls to `0xccf830` / `0x5cd5c0`: log
   `gameplay3: menu event calls differ, nothing installed`, return.
5. Countdown patch (its result does not stop the rest):
   a. Bytes at `0x5f0b50` differ: log `results countdown (5f0b50): bytes differ, left alone`.
   b. Else f32 at `0xe62288` != -1/300 (float compare): log `results countdown (5f0b50): scale constant 0xe62288 differs, left alone`.
   c. Else request one `near_stub(64)`. If NULL, or `stub - (image + 0x5f0b55)` does not fit in s32: log
      `results countdown (5f0b50): no stub memory in range`.
   d. Else write the stub; write at `0x5f0b50` `E9 <rel32 to the stub>` followed by 18 bytes `90`; log
      `results countdown (5f0b50): end time restored (PS3 0xf12504)`; installed += 1.
6. If slot `0x100aa48` does not hold `0x5f0ea0`: log
   `ShowResults::Enter: vtable 0x%llx slot 1 is not 0x%llx, left alone (countdown stays at 0)` (`0x100aa40`,
   `0x5f0ea0`), return.
7. Write the Enter hook's address into the slot; log `ShowResults::Enter: vtable 0x%llx slot 1 -> C++ (PS3 0x43ec08)`
   (`0x100aa40`); installed += 1.

### 4.6 Test scenarios

- Install: each anchor mismatch; each call-target mismatch; countdown bytes differ / constant differs / no stub
  (the Enter is still installed); Enter slot wrong (countdown may still be installed: +1); everything right (+2).
- Enter: menu state 0, 5, 6 (no menu calls), 1, 4, 7, 0xffffffff (close sequence); clock pointer NULL and set (several
  frame values, negative too); log text; the call to `0x5f0ea0` with the same state pointer.
- Stub: run the patched code path (or the stub alone) with end 0 and after Enter; xmm1 = frames*(-1/300) + end.

---

## 5. matchend (`matchend.cpp`)

### 5.1 Purpose

The PS4 game-over checks were never seen to fire. This file wraps Playing::Update to log the match-end inputs every
10 s and whenever the end state changes, optionally forces a short time limit for test runs, and calls
`visibility_kick()` after every update.

### 5.2 Interface

- `void install_matchend(int &installed)`.
- Uses `void visibility_kick()` (section 3).
- Entry point: Playing vtable slot 3 (`0x100ab98`, Update). It must already hold something other than `0x5f14d0`
  (session.cpp's wrapper). It is replaced by a host function `(state*) -> u64`.

### 5.3 Game facts

| Fact | Value / meaning |
|---|---|
| `0x100ab98` | Playing::Update slot; original PS4 function `0x5f14d0` |
| `0x1116384` | s32 realtime frames (300 Hz) |
| `0x104ae70` | u64 pointer; pointer + `0x3dee` + i*`0x34` = u16 score of team i |
| `0x104f4f8` | u8 debug flag; non-zero = time limit off |
| `0x11475d0` | s32 cash (NetPlayerData `0x11474f8` + `0xd8`) |
| `0xa15e70` | guest LookupPointer |
| `0x3b9a067d` | *net-games* (record +0xc = game type) |
| ngm fields | see 0.5 (+0x1c9c teams, +0x1ca0 game, +0x1d5c, +0x1d60, +0x1d74, +0x1e80/84/88, +0x21b4, +0x21b8, +0x21c0) |

### 5.4 Behaviour

State: test time limit (f32, 0 = off); next log time (u64 ms, 0); last end A, last end B, last state (s32, all -1);
original Update.

**Game type** (used in the log): call guest `0xa15e70(0x3b9a067d)` (always, first). Read s16 game at `0x113d400`.
If the result is NULL or game < 0: -1. Else count = s32 at result+0, records = u64 at result+8; if game >= count or
records is NULL: -1. Else the s32 at records + 0x90*game + 0xc.

**Condition log (reason):** one line
`match end watch (%s): type %d, %.1f s of round (match %.1f s), time limit %.0f s%s, score limit %d, teams %d scores %s, rounds %d/%d, ngm state %d end %d/%d, NET_SM %d, cash %d`
with, in order:
- reason;
- game type (above);
- (double)(s32 frames at `0x1116384` - s32 ngm+0x1d5c) / 300.0 (32-bit wrapping subtraction, then double divide);
- the same with ngm+0x1d60;
- (double) f32 ngm+0x21b8;
- `" (debug off)"` if u8 at `0x104f4f8` is non-zero, else `""`;
- s32 ngm+0x21b4; s32 teams (ngm+0x1c9c);
- scores: if the u64 at `0x104ae70` is NULL, or teams <= 0: `-`. Else the u16 scores of teams 0 .. min(teams, 8)-1
  as decimal, joined with `/` (e.g. `12/7`);
- s32 ngm+0x1d74; s32 ngm+0x21c0; s32 ngm+0x1e80; s32 ngm+0x1e84; s32 ngm+0x1e88; s32 NET_SM; s32 cash.

**Playing::Update hook (state):**
1. If the test time limit is > 0 and the f32 at `0x113d918` != the test limit: log
   `match end watch: time limit %.0f s -> %.0f s (UNCHARTED_U2MP_TIME_LIMIT)` (old value, new value, as double),
   then write the test limit there.
2. Call the original Update (state). Keep its u64 result.
3. Call `visibility_kick()`.
4. now = a monotonic host clock in milliseconds.
5. Read A = s32 ngm+0x1e84, B = s32 ngm+0x1e88, S = s32 ngm+0x1e80.
   - If any differs from the last values: store them, write the condition log with reason `end state changed`,
     next log time = now + 10000.
   - Else if now >= next log time: condition log with reason `10 s`, next log time = now + 10000.
   - Else nothing.
6. Return the result of step 2.

So the first call always logs (`end state changed` unless A, B and S are all -1, then `10 s`).

### 5.5 Install

1. If `image.base` is NULL or `0x100ab98 + 8` exceeds the image size: return silently.
2. If env `UNCHARTED_U2MP_TIME_LIMIT` is set: v = `strtof(value)`; if 10.0 <= v <= 3600.0 the test limit is v. Log
   `match end watch: UNCHARTED_U2MP_TIME_LIMIT=%s -> %s` with the raw value and `used` or `ignored (10..3600)`.
3. If the slot is 0 or holds `0x5f14d0`: log
   `match end watch: Playing::Update slot is not wrapped by session.cpp yet, left alone`, return.
4. Remember the slot value as the original Update, write the hook's address, log
   `match end watch: Playing::Update slot 0x%llx wrapped` (`0x100ab98`), installed += 1.

### 5.6 Test scenarios

- Install: slot 0; slot = `0x5f14d0`; slot wrapped; env unset / `30` / `5` / `4000` / `abc` / `10` / `3600`.
- Update: with test limit, time limit already equal (no log/write) and different; without test limit.
- Change detection: first call; unchanged state within 10 s (no log); each of A, B, S changing.
- Log contents: lookup NULL; game -1; game >= count; records NULL; teams 0, 3, 9 (8 scores); score table NULL;
  debug flag set; negative frame differences.
- `visibility_kick` is called after the original Update (check the order of recorded calls).
- The 10 s timer uses the host clock; compare only the first call or change-triggered lines, or control the clock
  the same way for both builds.

---

## 6. spawn (`spawn.cpp`)

### 6.1 Purpose

GetSpawnLocator (`0x662fa0`) asks `0x644130` for the package of the net game's level record ("<map>-pak") and then
binary-searches that package's spawner group for a spawner id. Those packages are often not installed, so the lookup
asserts. This file retargets that call: it returns a package whose spawner group holds the id, searching more
widely, and otherwise a package of its own whose group holds one entry {id, fallback spawner}. The game then builds
the locator from that spawner as usual. It never asserts.

### 6.2 Interface

- `void install_spawn(int &installed)`.
- Entry point: the call at `0x662fd0` (originally `call 0x644130`) reaches a host function
  `(void *ngm, u32 id, s32 mode) -> const u8 *package` (rdi, esi, edx; result in rax). At that point rdi = ngm,
  esi = spawner id, edx = mode (GetSpawnLocator's own arguments). The game then reads the u64 at result+0x9a0.

### 6.3 Game facts

| Fact | Value / meaning |
|---|---|
| `0x662fc9` | `48 8D 3D <rel32>` with `0x662fd0 + rel32 == 0x113b760` |
| `0x662fd0` | `call 0x644130` |
| `0x662fd5` | `4C 8B 88 A0 09 00 00` (mov r9,[rax+0x9a0]) |
| `0x644130` | original target (level record package) |
| `0x1130390` | package table. +`0x10` u64 bucket array; +`0x4c` s32 bucket count. bucket[count] = end marker node. Node: +0 u64 next, +`0x10` u32 package id, +`0x18` u64 package |
| package+`0x9a0` | u64 pointer to its spawner group |
| spawner group | +`0x08` u64 pointer to an array of spawner pointers ("all"); +`0x10` s32 count; +`0x18` u64 pointer to sorted entries; +`0x20` s32 sorted count. Entry (16 bytes): +0 u32 id, +8 u64 spawner, sorted by id ascending |
| spawner | +0 position; +`0x10` quaternion (4 f32); +`0x20` char* name; +`0x28` u32 id; +`0x60` parent; +`0x68` u64 pointer to tag list; size `0x90` |
| tag list | +0 u64 pointer to entries; +8 s32 count. Entry (16 bytes): +0 u32 key, +8 u64 value. Value: +0 u16 type (1 = int), +2 u16 size, +8 u64 pointer to s32 data |
| `0x5bfae217` | tag key online-spawner (value 1 = yes) |
| `0x33364361` | tag key start-spawner (value 1 = yes) |
| `0xa15e70` | LookupPointer; `0x3b9a067d` *net-games*, `0x0dec97a4` *levels* (layouts in 0.5) |
| sanity limits | group counts 1..100000; tag count 1..4096; package bucket count 1..1048576; at most 4096 nodes per bucket |

### 6.4 Behaviour

State kept across calls: turn (unsigned 32-bit, 0); last logged id (`0xffffffff`); last logged level name (empty
string); the own package, own group, own entry and own origin spawner (static memory, reused).

**Find id in a package:** group = u64 at package+0x9a0. No result if the group is NULL, sorted entries NULL, or
sorted count <= 0 or > 100000. Otherwise find the first entry whose id is >= the wanted id (binary search, lower
bound). The result is that entry's spawner pointer if its id equals the wanted id; else none. An entry with a NULL
spawner counts as not found.

**Package walk:** if the bucket array is NULL or the count is <= 0 or > 1<<20, no packages. Else, for each bucket
0..count-1 in order, follow nodes from bucket[b] while the node is non-NULL, not the end marker (bucket[count]), and
fewer than 4096 nodes of this bucket were visited. Nodes with a NULL package are skipped. The first package for which
the test succeeds ends the walk.

**Level record:** call `0xa15e70(0x3b9a067d)`, then `0xa15e70(0x0dec97a4)` (both always, in this order). Fail if
either is NULL, either records pointer is NULL, the s16 game (`0x113d400`) is < 0 or >= the net-games count. Level id
= u32 at net-games records + 0x90*game + 4. Scan levels records 0..count-1; at the first record whose u32 +0 equals
the level id, take name = char* at +0x18 and package id = u32 at +0x20; success only if name is non-NULL (a later
matching record is not tried). No match: fail.

**Int tag of a spawner (key):** tag list = u64 at spawner+0x68. Default 0 if the list is NULL, entries NULL, or count
<= 0 or > 4096. At the first entry with that key: default if the value is NULL, its type != 1 or its data NULL; else
the s32 data. No entry with that key: default.

**Spawner name:** char* at spawner+0x20, or `?` when NULL.

**Online pick (package, turn, start):** none if the package is NULL, its group is NULL, "all" is NULL, or count <= 0
or > 100000. If start: first try the spawners (non-NULL entries of "all", in order) with online-spawner == 1 and
start-spawner == 1. If there are none (or not start): the spawners with online-spawner == 1. When a candidate set of
size n > 0 is used, return candidate number (turn mod n) (0-based, in array order). If no set has candidates: none.

**Map pick (package, turn):** none under the same group conditions. For the prefixes `pickup-`, `npc-`, `online-`,
`continue-player` in this order: candidates = non-NULL spawners whose name starts with the prefix. The first prefix
with n > 0 candidates gives candidate (turn mod n). If no prefix matches: return all[0] (may be NULL = none).

**Origin spawner:** the own 0x90-byte spawner buffer, zeroed on every use, with quaternion (0, 0, 0, 1.0) at +0x10
and +0x20 = pointer to the string `u2mp-origin`. Its id (+0x28) is 0.

**Host function (ngm, id, mode):**
1. Level record (above). If it succeeded: pak = `package_by_id(package id)`; else pak = NULL.
2. Map name: if the record succeeded, the name without a trailing `-pak` when the name is longer than 4 characters
   and ends with `-pak`; otherwise the whole name. If the record failed, the map name is empty.
   map package = NULL if the map name is empty, else `package_by_id(sid(map name))`.
3. If the record succeeded and the name differs from the last logged level name: remember it and log
   `spawn: level %s %s (%d spawners), map %s %s (%d spawners)` with: name; `loaded`/`missing` (pak); the s32 count
   (+0x10) of pak's group, or 0 when pak or its group is NULL; map name; `loaded`/`missing` (map package); the same
   count for the map package. Then, if pak is NULL, log
   `spawn: %s holds the multiplayer spawners (online-spawner-*); it is not installed, players spawn at fallback spawners`
   with the name. (An empty name equals the initial empty string, so it is not logged the first time.)
4. If pak finds the id: return pak. Else if the map package finds the id: return it. Else walk all packages; return
   the first that finds the id.
5. Fallback. t = turn; turn += 1 (only here). start = (mode == 0).
   a. Online pick in pak (t, start). Method text `online spawner of the level`.
   b. If none: walk the packages, the first non-none online pick (t, start). Method `online spawner of a loaded package`.
   c. If none: map pick in the map package (t). Method `map spawner`.
   d. If none: the origin spawner. Method `own spawner at the origin`.
6. If id differs from the last logged id: remember it and log
   `spawn: spawner %08x (mode %d) is in no loaded package; using %s %s (%08x)` with id, mode, method, the spawner's
   name, and the u32 at spawner+0x28.
7. Fill the own entry = {id, 0, spawner}; the own group = all zero except sorted = &entry and sorted count = 1; the
   u64 at own package+0x9a0 = &group (the rest of the own package, 0x9b0 bytes, stays zero). Return the own package.

### 6.5 Install

1. Read the s32 at `0x662fcc`. If the bytes at `0x662fc9` are not `48 8D 3D`, or `0x662fd0 + that value` != `0x113b760`,
   or the bytes at `0x662fd5` differ: log `spawn: GetSpawnLocator (662fa0) bytes differ, left alone`, return.
2. `retarget_call(0x662fd0, 0x644130, host function, "GetSpawnLocator spawner package (662fd0)")`. On success log
   `spawn: GetSpawnLocator (662fa0) looks the spawner up in every loaded package, with a fallback` and add 1.

There is no NULL-image check.

### 6.6 Test scenarios

- Install: each byte mismatch; lea target wrong; call target wrong (retarget_call's own failure); success.
- Level record: lookups NULL; game -1 / out of range; no matching level; matching level with NULL name; name
  `gompa-multi-pak`, `-pak`, `abc` (no suffix), empty string.
- Found in pak; found in map package only; found in another table package only; matching entry with NULL spawner;
  entries with duplicates/unsorted ids (lower-bound behaviour); group counts 0 and 100001.
- Fallback a/b/c/d each reached; mode 0 with and without start spawners; mode 1; several calls (turn rotates,
  shared across all fallback calls); tag edge cases (missing key, wrong type, NULL data, count 4097).
- Map pick: each prefix; none matches (all[0]); all[0] NULL (origin).
- Logs: level log once per name change; missing-pak line; spawner log once per new id (same id twice: one line).
- Package walk: end-marker node, NULL package nodes, bucket chains longer than 4096.

---

## 7. scripttime (`scripttime.cpp`)

### 7.1 Purpose

State scripts spawned as net timers make `SsTrackGroup::GetDeltaSeconds` (`0xa477c0`) hit an assert and `int 0x41`
(crash in Warzone). The net-timer case must return the frame delta of the realtime clock (clocks[7]) in seconds.
This file rewrites the assert block in place. No host code runs at game time.

### 7.2 Interface

- `void install_scripttime(int &installed)`.
- No host entry point. Code patch inside `0xa477c0`.

### 7.3 Game facts

| Fact | Value / meaning |
|---|---|
| `0xa477c0` | GetDeltaSeconds |
| `0xa47853` | flag test (process +0x168 bit 8), then `je 0xa47893` |
| `0xa4785c`..`0xa47893` | the net-timer assert block (0x37 bytes) |
| `0xa47893` | the game-clock path |
| `0xa4789f` | `vmulss xmm0,xmm0,[1/300]; pop rbp; ret` |
| `0x1116180` | clock block; +`0x210` = s32 clocks[7] delta ticks |

Expected bytes at `0xa47853` (86 bytes, up to `0xa478a9`):

```
F6 80 68 01 00 00 08  74 37
48 8B 05 15 67 B2 00  48 85 C0  74 27
48 8D 3D 0A 20 48 00  48 8D 35 A4 DA 45 00  48 8D 15 DA DA 45 00  48 8D 0D F0 CA 3A 00
41 B8 9B 02 00 00  45 31 C9  FF D0  CD 41  5D C3
48 8D 05 E6 E8 6C 00  C5 FA 2A 40 50  C5 FA 59 05 F5 FC 41 00  5D C3
```

### 7.4 Behaviour / install

1. If `0xa47853` is not executable for 86 bytes, or the 86 bytes differ: log
   `GetDeltaSeconds net timer (0x%llx): bytes differ, left alone` (`0xa477c0`), return.
2. Write 17 bytes at `0xa4785c`:
   `48 8D 05 1D E9 6C 00` (lea rax,[rip+0x6ce91d] = image+0x1116180),
   `C5 FA 2A 80 10 02 00 00` (vcvtsi2ss xmm0,xmm0,dword [rax+0x210]),
   `EB 32` (jmp short to `0xa4789f`).
3. Fill `0xa4786d`..`0xa47892` (38 bytes) with `CC`.
4. Log `GetDeltaSeconds (0x%llx): net-timer scripts use the realtime clock delta (clocks[7], PS3 net clock)`
   (`0xa477c0`); installed += 1.

Result at game time: net-timer delta = (float) s32 at `0x1116390` * f32 1/300 (the constant the function already
uses). The flag test, the game-clock path and the rest of the function are unchanged.

### 7.5 Test scenarios

- Bytes match (image after install equals the exact bytes above, +1); one byte differs (no change, log); region not
  executable (no change, log).
- Optional: execute the patched function with the flag set and a delta value, compare xmm0.

---

## 8. Items for the tester

- Pointer arguments to buffers on the host stack (scoreboard args/descriptor, visibility target/view/start/delta/
  filter copy, gameplay3 event) should be compared by content, not by address.
- Host clocks: scoreboard dt and the matchend 10 s timer read a monotonic host clock. Old and new must read it at the
  same points (scoreboard: once per frame after the previous draw returns; matchend: once per update after
  `visibility_kick`), but values differ between runs.
- `near_stub` requests: gameplay3 one 64-byte request (only when the bytes and constant checks pass); scoreboard and
  spawn one 16-byte request each through `retarget_call`. The image bytes written at `0x5f0b50`, `0x3fc52d` and
  `0x662fd0` depend on the stub addresses.
- Statics last for the process: once-only logs (scoreboard page log, visibility first kick, rosternames 8 lines,
  spawn level/id logs, spawn turn). Tests that reuse one process must expect this or reset both builds alike.

## 9. Clarifications found by the differential test

- scoreboard dt (1.4): the frame interval in nanoseconds converted to float and divided by 1e9f in single precision,
  then capped at 0.1f (not computed in double and rounded at the end).
- matchend install log (5.5): "used" when the stored test time limit is above 0 after this environment value was
  applied (a limit stored by an earlier install counts), else "ignored (10..3600)".
