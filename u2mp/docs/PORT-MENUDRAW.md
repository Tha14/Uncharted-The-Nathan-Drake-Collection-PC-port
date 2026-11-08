# PORT-MENUDRAW: the 2D command arena (menudraw.cpp, install_menudraw)

Symptom: PAGE_MULTIPLAYER_PLAYER_OPTIONS (Profile) runs all its queries every frame, but only the
first items draw; without the k19 rank widget more draw, but a different subset each frame; over the
busy Multiplayer page nothing draws. PS3 1.10 draws the whole page.

## Cause

Every menu primitive is a deferred render command allocated from one linear arena, global
`0x124a000` (allocator at `+0x10`: `+0x10` top, `+0x18` end, `+0x20` base, `+0x28` size << 2 | flags,
`+0x30` high-water mark, `+0x40` spin lock, `+0x70` command list handed to the render frame at
`0x4b9a78`).

| user | site | bytes |
|---|---|---|
| textured quad `0xbdce90`, draw list NULL | material `0x3039` `0xbdcf48`, vertices `0x303a` `0xbdd2c1`, end `0x303c` `0xbdd518` | 0x50 + 0x140 + 0x48 = 0x1d0 per quad |
| text `0xbda9c0` (renderer text `0x5be615`) | `0xbda9eb` | 0xe0 + strlen |
| renderer `0x5bcd70` | `0x5c1b25`, `0x5c2941` | 0x50 |
| item tail `0x5b4e90`, frames / rows `0x5b3eb0`..`0x5b5de0` | `0x5b501d`, `0x5b50d8`, `0x5b53a2`, ... | 0x18 / 0x50 |
| HUD and other 2D (`0x55e160`, `0x5a6660`, `0xccc6f0`, ...) | | |

HeapArena::Allocate `0xb32170`: when `top + size > end` it returns NULL; with flag bit 0 (set by
Draw, `0x3fc434`) it skips the "HeapArena::Allocate: out of memory" print. Every caller skips the
primitive on NULL, so the page loses whatever is allocated after the arena fills; worker threads
share the arena (spin lock), so the lost set changes frame to frame.

Draw `0x3fc0b0` (Bluepoint `bp_ucctweaks.cpp`, called per frame from `0x3fb596`, after the frame
reset of the arena at `0x3faea1` in `0x3facb0`) gives it **0x30000 bytes (192 KiB)** of memory
context 0x33 each frame:

- `0x3fc38c` `mov edi,0x30000`
- `0x3fc391` `call 0xb33160` (Allocate(size, &context 0x33))
- `0x3fc3f3` `or rax,0xc0000` (size << 2 into `+0x28`)
- `0x3fc403` `add r14,0x30000` (end)

About 400 quads in all for everything 2D in a frame: enough for single-player menus, not for the
Profile page (rank widget, booster / portrait rows, settings) over the page behind it.
PS3 1.10 has no such arena (menus draw through GCM, renderer `0x41a790`); not compared further.

The ported image types (menuimages.cpp `draw_missing_type`) draw through the same `0xbdce90` path as
the type 3 case: 0x1d0 bytes per quad, 1 quad for a rank badge, 2 for a booster pair. Not an
outsized share.

## Fix

- `0x3fc391` retargeted to `arena_memory`: a request of the new size returns the next of 4 host
  buffers (ring, one per frame, so a buffer is not overwritten while the render side reads it);
  any other size goes to `0xb33160` as before.
- Immediates `0x3fc38c`, `0x3fc3f3`, `0x3fc403` patched: 0x30000 -> **0x200000** (2 MiB, ~4,500 quads),
  `+0x28` = 0x800000.
- Byte checks first: the three instructions, `lea r12,[0x124a000]` at `0x3fc39e`,
  `mov [r12+0x20],r14` at `0x3fc3e5`; the call target via retarget_call.
- Diagnostics: the call at `0x3fae9c` (`0x4b4f30`, right before the reset) is wrapped by
  `frame_begin`, which logs the previous frame's use / high-water when a new peak is reached
  (`menu draw: 2D arena peak N of 2097152 bytes ...`, noting when it passes the old 192 KiB).

## Left / to check

- In game: the Profile page draws completely, also over the Multiplayer page; the log shows the
  peak (if it stays under 196608 the arena was not the cause and the next suspect is elsewhere).
- Lifetime of context 0x33 memory was not traced; 4 buffers assume the render side is at most
  3 frames behind Draw.
- The lead wires `install_menudraw(installed)` into install.cpp (declaration in u2mp_internal.h).
