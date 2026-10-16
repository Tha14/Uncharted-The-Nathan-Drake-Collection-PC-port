# Changelog

Uncharted: The Nathan Drake Collection PC port, by **BilalReloaded**
([YouTube](https://www.youtube.com/@BilalReloaded/videos), [GitHub](https://github.com/bc160201983)).
Technical details of every item: [analysis/STATUS.md](analysis/STATUS.md).

## 16 October 2026 (0.1.2)

- **Uncharted 2: blurry background while aiming fixed.** The aiming depth of field reads its focus
  distance from a 1x1 texture the GPU renders; it was never copied back to the game (the distance
  stayed 0, so everything behind Nate was out of focus). Tiny GPU-rendered textures now come back
  with the frame's "done" fence (Uncharted 2 only; the other two games do not need it). Above 1080p
  the game also measured the depth at the wrong spot (the crosshair's 1080p HUD position over the
  larger render): that position is now scaled to the render size, so it is right at every
  resolution.
- **Uncharted 2: GPU hang ("Device lost") after a long session fixed.** The treasure pickup effect
  takes some buffer descriptors from table slots that hold other data; one that pointed into mapped
  memory with ~1e9 records hung the GPU. Such descriptors (unaligned, huge stride or over 64 MiB)
  are now ignored for its two shaders.
- **Multithreaded rendering:** the Uncharted 3 crash within minutes (flips overtook the frame's
  fences; the GPU-written range set was read and changed by two threads) and the Uncharted 2
  floating NPCs (fences did not wait for their readbacks on the recording thread) are fixed;
  Uncharted 3 ran 8 minutes crash-free. Still experimental.
- **Faster heavy scenes:** optimizations that were off since the grey-world and floating hunts
  (threaded recording, draw preparation, prepared resources, deferred stream copies, parallel
  copies) are on again by default: heaviest Uncharted 2 view 43.8 -> 49.0 FPS, Uncharted 3 about 9%
  less GPU-command-thread work.
- **74-85% less GPU readback in all three games:** windows the CPU stopped writing into cool down
  after 3 s, and small GPU writes come back on their own instead of with the whole 64 KiB around
  them (the effect shaders' data the game never reads made up nearly all of it). Uncharted 2
  2.9 -> 0.46 GB per 5 s, Uncharted 3 3.1 -> 0.48, Drake's Fortune 0.63 -> 0.16.
- **Frame rate limits 40, 45, 50 and 55** in the in-game menu and the launcher.
- Profiler (`GOW3_PROFILE`): no longer freezes or crashes the game (own lock-free stack unwinder).
- New diagnostics: `GOW3_READBACK_STATS=1` (readback bytes per window, why and who wrote them),
  `GOW3_TRACE_PS=<hashes>` (render targets, viewport, textures and constants of a shader's draws).

- **Uncharted 3: crash in the Cartagena museum and characters flying around fixed.** GPU results
  were copied back to the game without protection: a late copy could land on data the game had
  just written (garbage pointers, a broken GPU command list). New GPU data is now read-protected
  until it is back; the game's reads of it wait, everything else runs on.
- **Uncharted 2: NPCs coming from the sky fixed.** The game places characters on the ground with
  GPU ray casts and reads the results once a GPU "done" counter is set. That counter was written
  when the command was recorded, before the GPU had run it, and a shortcut from 15 October threw
  nearly every result away. The counter is now written once the results are back in game memory,
  in strict order with the frame's other counters (as on the console).
- **Uncharted 2: NPCs floating in the air, Nate's feet sinking into the floor and NPC legs
  flickering while they walk fixed** (also in 0.1.1). The game reads back data that a GPU copy
  shader writes every frame; the port returned it with the next submission, a frame late. Those
  small copies now come back before the frame's "done" fence the game waits for (no FPS cost; the
  shader's large copies, which the game never reads, are left out). Every preset queues 1 frame
  again; the launcher moves preset settings back from 2 to 1.
- **Faster heavy scenes:** guest-memory copies for the GPU no longer take a lock (10-15% of the GPU
  command thread in heavy Uncharted 2 scenes: 7.9 -> 6.4 microseconds per draw); GPU checkpoints
  (crash diagnosis, ~2%) only on request (`UNCHARTED_GPU_CHECKPOINTS=1`).
- Protected copy-back without the cost: pages where the game reads its own data beside GPU results
  and results the game reads every frame (such as scene brightness) stay readable; Uncharted 2
  went from 33-46 to 60 FPS with protection on. A deadlock that hung Uncharted 3 while loading
  with protection on is fixed (uploads copy through an unprotected view of the same memory).
- Tracking of small GPU writes no longer grows over a session.


- **First release, 0.1.0 (beta).** The package now carries `LICENSE.txt`, `CHANGELOG.md` and
  the licenses of all bundled libraries (`licenses/`).
- **Faster heavy scenes with every preset: 2 frames queued** (Low and Medium had 1). With 1 the
  CPU waited for the GPU to finish the previous frame every frame; in Uncharted 2's museum
  (1080p) that was 44-47 FPS, with 2 it is 56-59. The launcher moves existing settings over
  (not for a game with multithreaded rendering, which Uncharted 2 needs with 1).
- The game and launcher windows name the port's author in their title bar.
- **Launcher updates:** at start the launcher looks for a newer release and offers
  **UPDATE TO x.y.z**; it downloads the package, checks its SHA-256 against the one GitHub publishes,
  installs it with the new version's own installer (`tools/apply_update.py`: saves, settings and the
  shader cache stay; on any error every replaced file is put back) and restarts. Advanced tab: the
  check can be turned off or run now. Tested against a local fake release (install, restart,
  cleanup) and a locked file (full rollback of 1,077 files).
- **Automatic releases:** `tools/release.sh X.Y.Z` pushes the tag `vX.Y.Z`; GitHub Actions builds
  the package and publishes the release (marked Latest) with the notes from `tools/release-notes/vX.Y.Z.md`.
- Profiler: call stacks (`profile_*_stacks.txt`), also of the game's own thread that submits the
  frames, so a wait shows who waits.
- Packaging: the license step no longer hangs on GitHub Actions (the MSYS2 root `/` made
  `//usr/bin/pacman` a network path).
- **Uncharted 2: Nate floating above the ground or invisible fixed.** A CPU shortcut for one of
  the game's small GPU copy shaders (6a6422a9) copied the player's data before the game had
  finished it; that copy runs on the GPU again.
- **Uncharted 2: crash with multithreaded rendering fixed** (game assert `m_gfxEopTick` in
  `FrameBegin` after 1-3 minutes): GPU memory copies and fills no longer overtake the frame's
  end-of-pipe timestamp. Still open with 2 frames queued, see the known issues.
- **Heavy scenes: stalls removed.** CPU writes into ring memory whose old GPU data was still being
  read back waited for the GPU (a third of each frame with multithreaded rendering); the
  per-draw stream buffer grew from 128 to 512 MiB (it filled up with 2 frames queued);
  "Frames queued 2" is now the High preset's default. Heavy Uncharted 2 scenes: about 33 -> 40-45
  FPS (4K, multithreaded off).
- Built-in sampling profiler for the GPU threads (`GOW3_PROFILE=<seconds>`).
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
