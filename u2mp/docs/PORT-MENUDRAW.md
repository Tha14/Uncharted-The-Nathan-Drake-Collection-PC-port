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

## Second limit: dynamic vertex / index streams (run 32)

Run 32: arena peak 213432 bytes, more than the old 196608 bytes, and then an assert at `0xbdc65a`
(sibling `0xbdc5c2`): dynamic-stream-mgr.h SetPointer `m_pCurrent - m_pBufferStart < m_bufferSize`.

- Writer `0xbdc190(ctx, vertices, alpha xmm0)`, the render-side execution of the 0x303a command
  (command list executor `0xbdaca0`, `call` at `0xbdad3c`; immediate path `jmp` at `0xbdd16c`):
  4 vertices of 0x40 bytes at the 64-aligned current pointer of the vertex stream `[ctx+0x10]`,
  6 u16 indices (vertex number = offset / 0x40) to the index stream `[ctx+0x18]`. It writes first
  and asserts afterwards.
- Stream object, 0x28 bytes: `+0` buffer object, `+8` per-frame view, `+0x10` start, `+0x18` current,
  `+0x20` u32 size.
- Stream manager `0x13d2ee0`: 4 frame slots of 0x370 bytes, set up once by `0xc79d40` (from
  `0xc8edb0` at render init). Each frame `0xc7a3e0` (frame params `+0x98`, `0xb6315b`) takes every
  stream's memory from the frame heap (`0xc12150` -> `0xb333f0`, context 0x46, or 0x35 for kind 4)
  with the size at `+0x20`. Each stream also has a buffer object made by `0xc16820` from
  `{u32 bytes, u32 kind}`, which has to match the size.
- Contexts `0xccf830(ctx, type)`: type 0 = vertex `+0x00` / index `+0xf0` (the 2D pass, `0x4bab4c`,
  command list run at `0x4bad05`); 1 = `+0x50` (14 MiB) / `+0x140` (4 MiB); 2 = `+0xa0` / `+0x280`;
  3, 4 = `+0x28` / `+0x118`.
- 2D sizes: vertex 0x40000 (1,024 quads), index 0x2000 (8 KiB = 682 quads; the old arena allowed
  about 420 quads, so this was never reached on PS4).

Fix (`install_streams`, called first in `install_menudraw`):

| site | instruction | old | new | streams |
|---|---|---|---|---|
| `0xc79d70` | `movabs rax` | 0x800040000 | 0x800180000 | size `+0x00`, `+0x28` |
| `0xc79d8b` | `movabs rax` | 0x100040000 | 0x100180000 | buffer `+0x00`, `+0x28` |
| `0xc79ed3` | `movabs rax` | 0x800002000 | 0x800012000 | size `+0xa0`, `+0xf0` |
| `0xc79ee8` | `movabs rax` | 0x100002000 | 0x100012000 | buffer `+0xa0` |
| `0xc79f33` | `movabs r12` | 0x200002000 | 0x200012000 | buffer `+0xf0`, `+0x118` |
| `0xc79f68` | `movabs rax` | 0x800002000 | 0x800012000 | size `+0x118` |

- Vertex goes to 1.5 MiB (6,144 quads, 24,576 vertices, inside the u16 index range). Index goes to
  72 KiB (6,144 quads). Neighbouring streams share the immediates, so they grow as well. This is
  about 2.8 MiB more per frame from the frame heap, and the memory stays guest frame-heap memory
  (GPU path unchanged).
- All six byte patterns are checked before any is written. The patch only takes effect if it is
  installed before render init runs `0xc79d40`.
- Overflow guard `quad_write`, on both `0xbdad3c` (call) and `0xbdd16c` (jmp through a near stub):
  - It checks the vertex stream (64-aligned + 0x100), the index stream (+0xc) and the u16 limit
    the same way `0xbdc190` does.
  - If they do not fit, it drops the quad and logs (`vertex/index stream full, quad dropped (N so
    far ...)`, the first 4, then powers of two).
  - Otherwise it calls `0xbdc190`. Streams that are NULL go to the original, which asserts as before.
  - Dropping is safe: the flush `0xbdc670` (0x303c) draws (index count - batch start) and skips a
    draw of 0.

## Left / to check

- In game: the Profile page draws completely, also over the Multiplayer page; the log shows the
  arena peak and the `dynamic streams per frame` line; no `quad dropped` lines expected.
- Lifetime of context 0x33 memory was not traced; 4 arena buffers assume the render side is at
  most 3 frames behind Draw.
- Frame heap capacity (contexts 0x33 / 0x34 / 0x35) was not measured. If the larger streams
  exhaust it, `0xb333f0` fails at frame start: then lower kVertexNew / kIndexNew, since the guard
  alone already prevents the assert.
- Other stream writers were not guarded: text (`0xcc28d2`.., `0xcc3472`..), `0xcd0748`. If a
  text-heavy page asserts in those, the same kind of guard is needed there.
