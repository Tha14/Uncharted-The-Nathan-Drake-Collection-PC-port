# PORT-NETANIM: remote character animation (snapshot clip phases, NetAnimCmdGenerator slot 4)

Static analysis only (nothing was run or built). PS4 1.02 image offsets; PS3 = GOTY NPUA80698 1.10.
File: `gow3_pc/src/u2mp/netanim.cpp` (new, `install_netanim`). Replaces PORT-GAMEPLAY3.md section 3,
whose conclusion ("`0x5f48d0` is reached only through the vtable, remote players animate through
`0x5f48e0`") was wrong on both counts.

## 1. What is called every frame

- NetCharacter anim setup `0x601c4a..0x601cc1` gives the engine anim layer (type 0x10, ctor `0x391370`,
  vptr `0xffca20`) the generator at NetCharacter+0x310 through layer slot 6 `0x3914f0` (layer+0x40).
  PS3: `0x45d100` -> `0x2c7f78(animctrl, 0x7efa5a21, 1, 5, NetCharacter+560)`.
- `0x391450` (layer code) calls generator slot 2 (CreateAnimCmds `0x5f3ca0`); layer slot 4 `0x391570`
  calls generator slot 6 (`0x5f3bd0`, time advance) and then **slot 4** with
  `layer+0x24 > [0xe5f550] ? events : NULL`. PS3 layer `0x2c24b8`: slot 7, then slot 5 with the same
  gate (layer+24). Slot 4 on PS4 is `0x5f48d0` = `ret`; PS3 slot 5 is `0x453c10`.

## 2. The snapshot stream lost the clip phases (the bigger gap)

| | PS3 | PS4 |
|---|---|---|
| writer FillAnimCmdBitStream | `0x480698`; clip (cmd 8): type 2 (2 bits), index, then phase = frame / (frames - 1) x 15 / 255 / 65535 in 4 / 8 / 16 bits (frames <= 8 / 9..64 / > 64) at `0x480fe0`, `0x481d4c`, `0x481b68` | `0x634970`; clip case `0x6361d3..0x6364bb`: type and index only (no float math in the whole writer after the header) |
| reader | `0x4da608`; anim case: index, lookup, phase bits (scale 1/15, 1/255, 1/65535), visitor slot 3 (anim, counter, phase), counter++ (`0x4dab1c..0x4dac08`) | `0x66b430`; anim case `0x66bcd0..0x66c04c`: index, lookup + assert, counter++; **no phase read, no slot 3 call** |

Effect on PS4: visitor slot 3 is unreachable. That is `0x5f5770` for the CreateAnimCmds visitor
`0x100ac80` (the only code that emits the clip command 8 {anim, slot, frame} for a remote character),
`0x5f5440` for the locator visitor `0x103fb10` (generator slot 3 `0x5f3f70`) and `0x5f5b40` for the
snapshot debug visitor `0x100acd0`. A remote character's anim command list gets blends but no clip.

Port: two call-outs (stub: red-zone skip, all caller-saved GPRs and xmm0-15 saved, aligned call):
- writer, at `0x6364b5` (`inc r10d; inc r14d; jmp 0x638000`, reached after the index write and from
  the index-skip `je` at `0x6363fb`): `write_phase(cmd = r12, stream = r15)` writes the PS3 bits with
  the PS4 bit writer's own sequence (MSB first, `0x634c62` pattern); cmd 8 = {+4 anim, +0xc index,
  +0x10 frame} (engine producer `0x51db12`: frame = instance+0xa0).
- reader, at `0x66c035` (anim in rax): `read_phase(anim, rbp)` reads the bits from the reader at
  rbp-0x150 and calls `visitor->slot3(anim, [rbp-0x180], phase)` (visitor at rbp-0x190), then the
  original counter++ and `jmp 0x66c250`.
The stream format changes: **both PCs need this build**; cinema films recorded before it will not
replay correctly.

## 3. NetAnimCmdGenerator slot 4 = PS3 0x453c10 (anim events)

PS3 `0x453c10` keeps a NetAnimList (16 x {key, anim, phase, dir, mark}, PS3 gen+1856 / count +2112),
builds the lists of the two snapshots around the generator time (`0x4539f8`: `0x451d50` pair search,
two `0x4da608` reads with the NetAnimList visitor `0xd705d8`), advances the kept phases by the time
since the older snapshot (x 30 frames, clamped) and fires each animation's events between the old and
the new phase: `0x7ab88` = PS4 **`0x4bcfd0`** (present, no caller on PS4) into the layer's event list
(PS4 `0x4bcd30` appends {id, from, to, duration, b, b}). These are the frame-keyed sounds / effects
of the animations (footsteps etc.), not the pose.

Ported whole (`net_anim_update`), branch by branch (rebuild `0x4540e0`, advance `0x454298`, advance
with unchanged snapshots `0x454544`, keep-or-clear `0x453fa4`); constants 30, 0.1, 0.15, 0.85, 1.0
from the PS3 TOC (`0xdb8dac`). Pieces:

| PS3 | PS4 |
|---|---|
| `0x451d50` snapshot pair | C++ (same as the inlined copies in `0x5f3f70`, `0x5f48e0`) |
| `0x4da608` | `0x66b430(visitor, snap, gen+0x28, a, b, [gen+0x24])` |
| visitor `0xd705d8`: 0 `0x4522d0`, 3 `0x452238`, 4 `0x452d98`, 1/2/5/6 empty | C++ table with the PS4 slot ABI; base reset `0x66b270`; layout +8/+0xc flags, +0x10/+0x20, +0x30 abort, list +0x38, last +0x40 |
| `0x45212c` AddAnim | C++ |
| `0x7ab88(evt, f1, f2, f3, r7 dir, r8 loop, r9 0, r10 1, list)` | `0x4bcfd0(evt, dir, loop, 0, 1, list, from, to, duration)` |
| anim +36 flags / +40 info (+34 frames, +44 rate) / +56 events (+0 id, +4 count) | +0x24 / +0x28 (+0x22, +0x2c) / +0x48 (checked: `0x39813e..0x3982c5`, `0x3a3602`) |
| gen +56 count, +64 snapshots x128 (time +4), +2124 time, +20, +24 | +0x68, +0x70 x0x90, +0x960, +0x24, +0x28 |

The list cannot live in the PS4 generator (reset `0x5f3250` clears +0x70..+0x954, time at +0x960), so
it is a side table keyed by the generator (mutex; the layer may update on job threads). It is cleared
when the generator has fewer than 3 snapshots, i.e. after a reset (PS3 `0x453b74` cleared it there).

## 4. Install checks, switch, log

Checked bytes: writer site, its `je`/`jae` into it (`0x6363f6`, `0x63646c`), reader site and its
`test rax; jne` (`0x66bffb`), prologues of `0x66b430` and `0x4bcfd0`, the whole `0x66b270`, the slot's
16 bytes (`ret` + padding), the layer's call `0x391590`. The stream part installs only when all its
checks pass; the slot only after the stream part.

`UNCHARTED_U2MP_NETANIM=0` leaves everything out (must then be set on every PC). Log `u2mp: netanim:`
`snapshot clip phases restored`, `generator slot called` (first 3), `snapshot stream full / ends before
a clip phase` (stream overflow or a peer with another build).

## 5. Unverified

- Not run. Needs a two-player run: remote characters should now play their clips (before: blends only)
  and their footstep / animation sounds and effects should play.
- The writer's stream capacity (r15+0xc) was not measured; the phases add 4-16 bits per clip.
- The meaning of the layer gate value `[0xe5f550]` and of the events list beyond `0x4bcd30`'s writes.
