# Uncharted: The Nathan Drake Collection — PC port

Run the PS4 **Uncharted: The Nathan Drake Collection** (CUSA02344: *Drake's Fortune*, *Among
Thieves* and *Drake's Deception*) natively on Windows. The game's own x86-64 code runs directly
on the PC; a runtime supplies the PS4 system libraries, and a Vulkan renderer executes the PS4 GPU
work.

By **BilalReloaded** ([YouTube](https://www.youtube.com/@BilalReloaded/videos),
[GitHub](https://github.com/bc160201983)), October 2026. What changed and when:
[CHANGELOG.md](CHANGELOG.md).

> **No game files are in this repository or its builds.** You need your own copy of the game
> (the dumped CUSA02344 folder). This project is not affiliated with Sony, Naughty Dog or
> Bluepoint Games.

## Status

| Game | Plays | Notes |
|---|---|---|
| Uncharted: Drake's Fortune | Yes | 60 FPS in the tested areas. Native rendering up to 2880x1620 (4K output through FSR 1). |
| Uncharted 2: Among Thieves | Yes | Played at 1440p and 4K. Light scenes 60 FPS; the heaviest scenes 40-45 FPS (CPU-bound, see below). |
| Uncharted 3: Drake's Deception | Yes | 1080p tested in play; 1440p and 4K start, play testing in progress. |

The Collection menu, switching between the three games, saves, audio (music, speech, effects),
cutscene movies and controllers work. Details and open issues: [analysis/STATUS.md](analysis/STATUS.md);
plan and test list: [analysis/PLAN.md](analysis/PLAN.md).

## Features

**Launcher** (`Uncharted Launcher.exe`)
- A card per game; settings are kept **per game**.
- Quality presets:

  | Preset | Render resolution | Frame rate | For |
  |---|---|---|---|
  | Low | 720p + FSR 1 to the screen | 60 | weak GPUs (about twice as fast as 1080p) |
  | Medium | 1080p (as on PS4) | 60 | most PCs |
  | High | 1440p | 60 | fast GPUs |
  | Ultra | 4K (Drake's Fortune: 1620p + FSR 1) | up to 120 (experimental) | very fast GPUs |

  Every preset queues 1 frame. 2 frames queued is faster in heavy scenes, but GPU results
  reach the game a frame later: keep it at 1.

  **Low-end PCs and laptops:** turn on **Multithreaded rendering** (launcher, **Performance**
  tab, per game) and keep **Frames queued at 1**. The PS4's GPU commands are otherwise processed
  on one CPU core, which limits slower CPUs; with this option the Vulkan command recording runs on a
  second core. On fast desktop CPUs it makes no difference. With a weak GPU also pick the **Low**
  preset (720p + FSR 1). If a game hangs while loading, turn the option off again.

- **Display:** windowed, borderless or exclusive fullscreen; render resolution 720p, 900p, 1080p,
  1440p, 1620p or 4K; FSR 1 upscaling; RCAS sharpening; FPS counter.
- **Performance:** frame rate limit 30 / 40 / 45 / 50 / 55 / 60 / 120 (60 is the PS4's; 120 is
  experimental, and faster rates break the games' movement, so they are not offered); frames
  queued; background shader compilation; multithreaded rendering (experimental).
- **Keyboard and mouse:** mouse look (sensitivity, invert Y), right mouse aim, left mouse fire, WASD,
  and every action remappable per game in the **Controls** tab (two bindings per action, keys or
  mouse buttons). Controllers (DualSense, DualShock 4, Xbox) work at the same time.
- **Game update 1.02** is used automatically when it sits next to the game folder (see below).
- Log and save folders, desktop shortcut, shader cache reset, game folder selection, links to the
  author's [YouTube](https://www.youtube.com/@BilalReloaded/videos) and GitHub.

**In game:** `Insert` or `R3 + L2` opens the menu (display mode, frame rate, sharpening, overlay).

**Under the hood**
- Native execution of the three game executables; the Collection's game switching (LoadExec).
- shadPS4-derived Vulkan renderer with a persistent shader/pipeline cache, asynchronous shader
  compilation, and fixes for this game: GPU-written constants synced back to the CPU, MUBUF
  ADDR64 loads (character shadows), direct memory access for the tiled lighting, depth/stencil
  aliasing, CPU writes next to GPU data without stalls, GPU ray-cast results returned to the game
  (Drake's Fortune's bullet impacts, blood and decals), tolerant handling of garbage descriptors.
- Render resolution patch: the games' display settings and memory map are rewritten for the chosen
  resolution; cutscene movies are scaled to it and their conversion shader adjusted.
- FFmpeg-based video decoding (Videodec2), ATRAC9 audio (LibAtrac9), SDL3 window and controllers.

## Known issues

- **Frame rate in the heaviest Uncharted 2 scenes is about 50 FPS** at any resolution: the
  PS4's GPU command processing runs on one CPU core here (3,000-5,600 draws per frame). Lighter
  scenes reach 60. More optimization is planned.
- **Multithreaded rendering is experimental:** its crashes with 1 frame queued are fixed. It is
  meant for low-end PCs and laptops (see the presets above) and gives no speed-up on fast CPUs;
  with "Frames queued 2" Uncharted 2 can still stop with the game assert `m_gfxEopTick`, so keep 1.
- **Uncharted 3:** Sully's shirt in the bar can flicker at 60 FPS (not at 30).
- **Recording with OBS Game Capture:** if OBS keeps losing and re-hooking the game, the game's
  memory use grows (each hook creates DirectX devices in the game). With an NVIDIA card set
  NVIDIA Control Panel -> Manage 3D settings -> Program settings -> Uncharted.exe ->
  **Vulkan/OpenGL present method: Prefer native**, or use OBS **Window Capture**.
- **Frames queued 2** is faster in heavy scenes, but GPU results reach the game a frame later
  (Uncharted 2's NPCs float, Nate's feet sink into the floor): keep it at 1 (every preset does).
- Drake's Fortune renders at most 2880x1620 (its memory layout) and upscales to 4K; turn on FSR 1.
- Frame rates above 120 are not offered: the games' movement breaks (Drake's Fortune at 160 FPS
  dropped Nate through the world).
- Uncharted 3 has had less play testing than the other two: please report crashes with the log
  (launcher **Advanced -> Open log folder**, `boot.log`).

## Playing

1. Download **Uncharted-windows.zip** from the latest release on the
   [Releases](../../releases) page (or a newer build from the
   [Actions](../../actions/workflows/build-windows.yml) tab, latest successful run → Artifacts),
   or build it yourself (below). Extract it anywhere.
2. Start **Uncharted Launcher.exe**. On the first Play, select your game folder: the dumped
   `CUSA02344` folder (the one holding `eboot.bin` and `sce_sys`, often called `uroot`).
3. **Recommended: update 1.02** (Bluepoint's stability, movie, audio and gameplay fixes). Extract
   the update next to the game folder, named like it plus `-patch` (for example `CUSA02344-patch`,
   with or without its `uroot` folder). The launcher's Advanced tab shows the version in use.
4. Pick a game and a preset, press **PLAY**. The first start of each game prepares files and
   compiles shaders: expect a slow first load and some stutter until the cache has filled.

**Requirements:** Windows 10/11 64-bit, a GPU with Vulkan 1.3 and a current driver (NVIDIA, AMD,
Intel), 16 GB RAM recommended, a 4-core CPU or better. Saves, logs and settings stay in the
`analysis` folder next to the launcher.

**Updates** (from version 0.1.1): the launcher checks the releases when it starts and shows
**UPDATE TO x.y.z** when there is a newer one. It downloads the package, checks its SHA-256 against
the release, replaces the program files (saves, settings and the shader cache stay) and starts
again; if anything fails, the installed version is put back. Advanced tab: turn the check off, or
check now.

## Building

GitHub Actions builds every push to `main` ([build-windows.yml](.github/workflows/build-windows.yml)).
To build locally, from an MSYS2 **CLANG64** shell:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
git clone --recursive https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port.git
cd Uncharted-The-Nathan-Drake-Collection-PC-port
gow3_pc/build.sh                 # the runtime: gow3_pc/out/gow3-probe.exe
tools/package_windows.sh         # dist/Uncharted-windows(.zip); needs a Windows Python 3.10+
                                 # with Pillow (PYTHON_HOME)
```

To run from the source tree: `Start-Uncharted.cmd` (launcher), or
`python tools/probe_uncharted.py --boot --seconds 0 --game <game folder>`.

### Releases

`tools/release.sh 0.1.1` publishes version 0.1.1: write its text in
`tools/release-notes/v0.1.1.md` first. The script sets `tools/VERSION`, pushes the source and the
tag `v0.1.1`; GitHub Actions then builds the package and publishes the release with
`Uncharted-windows.zip` (about 20 minutes). The launcher offers newer releases as updates.

## Repository layout

| Path | Contents |
|---|---|
| `launcher/` | The Uncharted launcher (Tkinter + Pillow). |
| `tools/probe_uncharted.py` | Prepares a game executable (patches, render resolution, update) and starts the runtime. |
| `tools/package_windows.sh` | Builds the Windows package; `tools/windows_starter/` its `.exe`. |
| `tools/release.sh`, `tools/release-notes/` | Publishes a release (see above). |
| `gow3_pc/` | The runtime: C runtime (`src/`), GPU library (`gpu/`: shadPS4 video core and shims), preparation scripts (`scripts/`). |
| `analysis/` | Compatibility notes (`STATUS.md`) and the plan (`PLAN.md`). |

## Credits and license

- **Uncharted port, launcher and game fixes: BilalReloaded**
  ([YouTube](https://www.youtube.com/@BilalReloaded/videos),
  [GitHub](https://github.com/bc160201983)), 2026. The fixes and their dates are listed in
  [CHANGELOG.md](CHANGELOG.md); the commit history shows each change.
- The runtime started from the [gow3_pc](https://github.com/FelipePrado0/gow3_pc) project
  (whose internal names, such as `GOW3_*` settings, it keeps) and the
  [shadPS4](https://github.com/shadps4-emu/shadPS4) video core.
- Third-party code: FSR (AMD), imgui, magic_enum, miniz, xbyak, LibAtrac9, FFmpeg, SDL3.
- Licensed under the GNU GPL v2 or later (see `gow3_pc/LICENSE`). The Windows package carries
  `LICENSE.txt` and a `licenses` folder with the license of every bundled library
  (`licenses/THIRD-PARTY.txt` lists each DLL package with its version, license and source).

### Reusing this work

You are welcome to use these fixes in your own port. The GPL asks that you keep the copyright and
license notices and publish your source code under the same license when you distribute your
build. Beyond that, a request: **credit this project and link back to it**
(`https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port`, by BilalReloaded) in your README or release
notes, so people can find where a fix came from.
