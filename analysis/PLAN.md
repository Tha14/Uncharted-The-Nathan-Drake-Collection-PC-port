# Uncharted PC — plan for 4K, 100+ FPS on fast PCs, 60 FPS on weak PCs

15 October 2026 (night). Written from tonight's investigation: crash logs, reverse engineering of
all three executables (1.00 and 1.02), boot tests on this PC (Ryzen 9 9950X3D, RTX 5080, and the
CPU's integrated Radeon as a stand-in for a weak GPU), and research (GoldHEN patch repository,
shadPS4 issues for this game).

## 1. Done tonight (ready for you to test)

| What | Result |
|---|---|
| Private GitHub repo + Actions build | `bc160201983/uncharted-pc`. Every push to `main` builds the Windows package (about 14 min, artifact `Uncharted-windows`). First build passed. |
| Update 1.02 | Extracted from your `utndc.v1.02.pkg` to `game/CUSA02344-patch`. Found automatically next to the game folder; the launcher shows the version. It holds Bluepoint's fixes for 1.01 and 1.02 (stability, performance, movies, 5.1/7.1 audio, aiming, gameplay bugs; the U2 quit-to-menu crash GoldHEN patches for 1.00 is gone). All our code patches now match both versions by pattern. In 1.02 the 2-second GPU wait halts the game outright, so that patch matters even more. All three games boot on 1.02. |
| Frame rate above 60 | Confirmed in the code: the engine measures every frame with the CPU clock (`rdtsc` / TSC frequency) and clamps it to "Minimum Framerate". Game speed stays correct at 120+ FPS. |
| Resolution crashes, causes | **U2 1440p NaN look-at** and **U3 1440p corrupted pointer** both ran with asynchronous readbacks on (584 / 230 in flight), the same feature that broke U3 at 1080p ("dcb full"). They are likely not resolution bugs. **U1 4K** was a real resolution bug (below). |
| U1 above 1620p | Uncharted 1 encodes pointers to loaded data (animations) as 31-bit offsets into two fixed 2 GiB windows (Bluepoint `GenerateOffset`). At 4K the grown render-target heap no longer fits the GPU area, moves to the CPU area, and is created before the level/animation heaps, so it pushes them past 2 GiB, and the first animation load asserts. Nothing simple moves it (tried: the alignment order, another memory context: null heap). **U1 now renders natively up to 2880x1620** (its GPU area's limit) and FSR 1 scales to 4K. |
| U3 4K | 1.02 maps U3's CPU area 100 MiB larger than its table says; at 4K that crossed 4 GiB and wrapped to 36 MiB (out of memory at boot). Fixed with headroom. |
| Native resolutions, boot-tested | U1 1440p and 1620p, U2 4K, U3 4K: real passes at that size, no asserts, 60 FPS on the title screen. Gameplay needs you. |
| Green, tiled cutscenes above 1080p | Cause: the movie player reads every movie frame with the display size it renders at, but the movies are 1080p and its frame buffers are fixed at 4 MiB. **Partly fixed:** the decoder writes a copy scaled to the display size into separate memory, and the GPU reads that, so the colors are right. **Still wrong:** the conversion shader scales by the movie's own size, so above 1080p the movie fills only the top-left 1920x1080 of the image. Next step in 3.A. |
| Launcher | Presets: **Low** 720p + FSR 1 at 60 FPS, **Medium** 1080p 60, **High** 1440p 60, **Ultra** 4K (U1: 1620p + FSR 1) with the frame rate unlocked. New 1620p option. Your saved per-game presets were moved to the new meaning: U1 High → 1440p, U2 Ultra → 4K, U3 Low → 720p (choose Medium for the old 1080p). |

## 2. Please test (in this order)

1. **U3 on 1.02, Medium (1080p)**: play past the London pub (the old "dcb full" spot), and check the hair in sunlight (the flicker fix from last night).
2. **U2 on 1.02, High (1440p)**: play for 15+ minutes (the old NaN crash came after minutes) and pick up a treasure.
3. **U1 on 1.02, Ultra**: play to a checkpoint save (the old 4K crash point). It renders at 1620p now.
4. **U2 or U3 on Ultra (native 4K)**: any gameplay, a cutscene, a save.
5. **Ultra above 60 FPS**: does the game run at normal speed? Note the FPS in heavy scenes.
6. **Low (720p + FSR)**: does it look acceptable, and does gameplay work at 720p?

After each crash, the log is in `analysis/<game>/boot.log` (Advanced → Open log folder).

## 3. The plan

### A. Correct at every resolution (needed before 4K is the default)

1. **Cutscenes above 1080p, finish.** Patch the movie renderer to use the display size where it uses the movie's (the conversion shader's scale factor, in movie-rendering.cpp around U1 1.02 `0x4fb7b1`; the same code exists in U2/U3), or enlarge `kSizeFrameBuffer` (4 MiB, movie-player.cpp) and the movie pool so frames fit at the display size. With the scaled copy already in place, only the scale is missing.
2. **Native 4K for U1.** Make the render-target heap get created after the level heaps, or find a third memory area that accepts it. Until then: 1620p + FSR 1 (2.25x the pixels of 1080p).
3. **Safe asynchronous readbacks.** Read-protect the pages of every readback still in flight; a CPU read of such a page waits for that readback only. Then async readbacks can be on for all three games (they gave the temple its steady 60 FPS) without the stale reads that crashed U3 and likely U2 at 1440p.
4. **Windows red zone (seen in shadPS4's report on this game).** On Windows a page-protection fault overwrites the 128 bytes below the stack pointer, which the PS4 compiler's leaf functions use for data. Our runtime takes thousands of such faults. Red-zone use is rare in these games (about 340 blocks each, none at our crash sites), so first a counter: faults whose instruction is in a red-zone function. If it fires in play, rewrite those instructions as shadPS4 does.
5. **Player targeting reads the display size** (player-targeting.cpp; the U2 NaN was in the look-at code). If the U2 1440p test still crashes with async readbacks off, look there first.

### B. 100+ FPS on fast PCs (Ultra)

The game logic is not the limit. The title screen runs at about 200 FPS uncapped (U1), and U2 at native 4K reached 112 FPS on the title screen. Gameplay costs are in the GPU emulation:

1. **Measure first:** play with Ultra and the overlay; the performance log (on by default) records where the time goes every 5 s (GPU waits, readbacks, compiles).
2. **Async readbacks** (A.3): removes the per-submission wait on GPU writes (about 10 ms per frame in the temple without it).
3. **Multithreaded rendering** (already in the launcher, off): command recording on its own thread and draw preparation in parallel. Test per game; turn on where it is stable.
4. **Shader stutter:** the pipeline cache already persists between runs. Add a pre-warm on load (compile everything the cache knows before the first frame) so the first minutes are smooth too.
5. **DMA sync:** keep the per-pointer windows (last night: 490 → 75 ms per 5 s); measure again at 4K.
6. **Frame pacing above 60:** the present thread runs at the menu limit with a 480 Hz vblank. Check that frame times are even at 120/144 (worst-frame line in the log).

### C. 60 FPS on weak PCs (Low)

Measured on the integrated Radeon (weaker than a PS4): U1 title screen 21 FPS at 1080p, **44 FPS at 720p**. Rendering cost scales with pixels, so:

1. **Low = 720p + FSR 1** (done). Next: an **adaptive** option between 720p and 900p for GPUs between that and a GTX 1060.
2. **Async shader compilation** stays on for Low (fewer hitches; a draw can be skipped while its shader compiles).
3. Reduce emulation overhead on 4-core CPUs: the GPU command thread and the game's 6 job threads compete; set thread priorities/affinity so the GPU thread keeps a core.
4. Shadow map and post-effect sizes are fixed by the game; lowering them would need game patches (later, only if 720p is not enough).

### D. Remaining bugs

| Bug | Status / next step |
|---|---|
| Green cutscenes above 1080p | Partly fixed (colors). A.1 |
| U1 native 4K | 1620p + FSR for now. A.2 |
| U3 crash with async readbacks | Async off for U3. A.3 |
| U3 hair flicker in sunlight | Fix built last night; please test (2.1) |
| Treasure pickup | Fixed last night (garbage descriptors tolerated); please test (2.2) |
| U1 smoke blob, bullet-hit circles | Not started: needs a frame capture in a scene with smoke |
| Multithreaded rendering | Untested. B.3 |
| Windows red zone | Counter first. A.4 |
| Sound effects in U2/U3 | The U1 read-only sound config patch has no U2/U3 match; U2/U3 load sound differently. Please confirm footsteps and gunfire there |
| GoW names inside the runtime | Cosmetic, optional |
