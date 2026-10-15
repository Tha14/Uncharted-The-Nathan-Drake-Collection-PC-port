# Uncharted: The Nathan Drake Collection — PC port

Run the PS4 **Uncharted: The Nathan Drake Collection** (CUSA02344: *Drake's Fortune*, *Among
Thieves* and *Drake's Deception*) natively on Windows. The game's own x86-64 code runs directly
on the PC; a runtime supplies the PS4 system libraries, and a Vulkan renderer executes the PS4 GPU
work.

> **No game files are in this repository or its builds.** You need your own copy of the game
> (the dumped CUSA02344 folder). This project is not affiliated with Sony, Naughty Dog or
> Bluepoint Games.

## Status

| Game | Plays | Notes |
|---|---|---|
| Uncharted: Drake's Fortune | Yes | 60 FPS in the tested areas. Native rendering up to 2880x1620 (4K output through FSR 1). |
| Uncharted 2: Among Thieves | Yes | 1080p tested in play; 1440p and 4K start, play testing in progress. |
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
  | Ultra | 4K (Drake's Fortune: 1620p + FSR 1) | up to the monitor's refresh rate | very fast GPUs |

- **Display:** windowed, borderless or exclusive fullscreen; render resolution 720p, 900p, 1080p,
  1440p, 1620p or 4K; FSR 1 upscaling; RCAS sharpening; FPS counter.
- **Performance:** frame rate limit 30 / 60 / 120 / 144 / 165 / 240 / unlimited (the game's
  simulation follows the real frame time, so the speed stays right above 60); frames queued;
  background shader compilation; multithreaded rendering and faster GPU readbacks (experimental).
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
  aliasing, lazy CPU writes into GPU data (steady 60 FPS in heavy scenes), tolerant handling of
  garbage descriptors.
- Render resolution patch: the games' display settings and memory map are rewritten for the chosen
  resolution; cutscene movies are scaled to it and their conversion shader adjusted.
- FFmpeg-based video decoding (Videodec2), ATRAC9 audio (LibAtrac9), SDL3 window and controllers.

## Playing

1. Download the latest **Uncharted-windows** package from the
   [Actions](../../actions/workflows/build-windows.yml) tab (latest successful run → Artifacts),
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

## Building

GitHub Actions builds every push to `main` ([build-windows.yml](.github/workflows/build-windows.yml)).
To build locally, from an MSYS2 **CLANG64** shell:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
git clone --recursive https://github.com/bc160201983/uncharted-pc.git
cd uncharted-pc
gow3_pc/build.sh                 # the runtime: gow3_pc/out/gow3-probe.exe
tools/package_windows.sh         # dist/Uncharted-windows(.zip); needs a Windows Python 3.10+
                                 # with Pillow (PYTHON_HOME)
```

To run from the source tree: `Start-Uncharted.cmd` (launcher), or
`python tools/probe_uncharted.py --boot --seconds 0 --game <game folder>`.

## Repository layout

| Path | Contents |
|---|---|
| `launcher/` | The Uncharted launcher (Tkinter + Pillow). |
| `tools/probe_uncharted.py` | Prepares a game executable (patches, render resolution, update) and starts the runtime. |
| `tools/package_windows.sh` | Builds the Windows package; `tools/windows_starter/` its `.exe`. |
| `gow3_pc/` | The runtime: C runtime (`src/`), GPU library (`gpu/`: shadPS4 video core and shims), preparation scripts (`scripts/`). |
| `analysis/` | Compatibility notes (`STATUS.md`) and the plan (`PLAN.md`). |

## Credits and license

- The runtime started from the [gow3_pc](https://github.com/FelipePrado0/gow3_pc) project
  (whose internal names, such as `GOW3_*` settings, it keeps) and the
  [shadPS4](https://github.com/shadps4-emu/shadPS4) video core.
- Third-party code: FSR (AMD), imgui, magic_enum, miniz, xbyak, LibAtrac9, FFmpeg, SDL3.
- Licensed under the GNU GPL v2 or later (see `gow3_pc/LICENSE`).
