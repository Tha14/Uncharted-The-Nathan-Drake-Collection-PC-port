# Changelog

Uncharted: The Nathan Drake Collection PC port, by **BilalReloaded**
([YouTube](https://www.youtube.com/@BilalReloaded/videos), [GitHub](https://github.com/bc160201983)).
Technical details of every item: [analysis/STATUS.md](analysis/STATUS.md).

## 15 October 2026

- **Drake's Fortune: bullet impacts, bullet holes and blood.** The game traces its bullet and
  decal rays on the GPU (`Cs_RayTrace`, `Cs_RayTraceGatherHits`) and reads the hits once the
  frame's end-of-pipe event fires. That event was signalled before the GPU had produced the
  hits, so every shot read as a miss. The results are now copied back asynchronously with their
  pages read-protected until they arrive.
- **Drake's Fortune: cutscene movie colours at 1620p** (grey picture with diagonal coloured
  lines). The GPU reads the movie's colour plane with rows padded to 64 texels (1472 at 1620p);
  the scaled frame copy now writes its rows the same way.
- **Frame-rate limit capped at 120.** At 140-160 FPS Drake's Fortune's movement broke (Nate fell
  through the world). The launcher and the in-game menu offer 30 / 60 / 120; older, faster
  settings are read as 120.
- **Launcher and in-game menu settings stay in step.** Closing the in-game menu no longer
  overwrites a change made in the launcher while the game runs, and the launcher shows what the
  in-game menu saved.
- **Uncharted 2: crashes at treasure pickups and in the Treasures screen fixed.** CPU-rewritten
  per-frame data (skinning tables) was corrupted by a byte-compare merge of GPU data; the merge
  is gone, and resource descriptors that cannot be real are ignored instead of crashing the GPU.
- **Uncharted 2: frame rate no longer falls over time.** In a busy scene it slid from about 42 to
  31 FPS within minutes: a per-page list of GPU writes grew every frame and was scanned on every
  buffer binding. Found with a new built-in sampling profiler (`GOW3_PROFILE`); the frame rate now
  stays level.
- **Frame drops in heavy scenes reduced.** CPU writes next to GPU data in flight no longer wait
  for the GPU; scalar copy shaders run on the CPU; GPU readbacks are asynchronous.
- **Uncharted 2: torch follows Nate** (launcher option, Advanced tab). *In test.*
- **Update 1.02** used automatically when it sits next to the game folder.
- **Render resolution above 1080p:** Uncharted 2 and 3 up to 4K, Drake's Fortune up to 1620p with
  FSR 1 (its memory layout); cutscene movies scaled to the render size.
- Keyboard and mouse: mouse look and remappable controls per game; display modes fixed.

## 14 October 2026

- **Grey/black ground and unlit Nate fixed:** constants the GPU writes are synced back to the CPU.
- **Sound effects** (gunfire, footsteps, ambience) fixed.
- **Dark interiors:** fire and flashlight light fixed; bright hair and cobwebs fixed (MUBUF ADDR64
  loads).
- Uncharted 2 and 3 start from the Collection menu; the Uncharted 2 new-game freeze fixed.
- The Uncharted launcher: per-game settings, quality presets, render resolution.

## 13 October 2026

- Gameplay reached: the Collection's game code runs natively on Windows.
