# Uncharted: The Nathan Drake Collection — Unofficial PC port

> **This is an unofficial, non-commercial fan project.** It is not made, endorsed or
> supported by Sony Interactive Entertainment, Naughty Dog, Bluepoint Games or any of their
> affiliates, and it is not affiliated with them in any way. *Uncharted: The Nathan Drake Collection*
> and the *Uncharted* games belong to their owners. You need your own legally obtained copy of the
> game; this project contains no game files and does not help anyone get them. See
> [Legal notice](#legal-notice).

[Development Plan](#development-plan) · [Contributing](CONTRIBUTING.md)

## About this fork

This repository is **João Fidalgo's fork** of
[BilalReloaded's original PC port](https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port).
The original port and its authors retain their credits and licensing below. A big thank you to
BilalReloaded: without his port, none of this would exist.

This fork will grow into a project of its own, step by step. The launcher and the code around it
will change over time, so that it slowly stops being a copy of the original and becomes our own
take, with our own look and features, built for the multiplayer. BilalReloaded's work stays
credited wherever it is still used.

**Fork date and license.** This repository was created on **5 November 2026**. It was forked from
the original at commit `e0c99f9` (*Release 0.8.3*). The original was then published under the
**GNU GPL v2 or later** (`gow3_pc/LICENSE`), which allows anyone to copy, modify and redistribute
it, provided the result stays under the same license with its source available. This fork follows
those terms. A license grant covers the code that was published under it, so a later change to the
original's license does not apply to this fork's copy.

This fork adds the experimental **Uncharted 3 multiplayer restoration** (beta) to the
launcher: the **MULTIPLAYER** tab imports it from your own PS3 and PS4 game copies, on your PC (see
[Uncharted 3 multiplayer](#uncharted-3-multiplayer-beta) below). Game assets, converted overlays and
patched game images are never distributed: the import makes them on the player's PC. The multiplayer
research source is in [u3mp/](u3mp/README.md).

Run the PS4 **Uncharted: The Nathan Drake Collection** (CUSA02344: *Drake's Fortune*, *Among
Thieves* and *Drake's Deception*) natively on Windows. The game's own x86-64 code runs directly
on the PC; a runtime supplies the PS4 system libraries, and a Vulkan renderer executes the PS4 GPU
work.

By **BilalReloaded** ([YouTube](https://www.youtube.com/@BilalReloaded/videos),
[GitHub](https://github.com/bc160201983)), October 2026. What changed and when:
[CHANGELOG.md](CHANGELOG.md).

> **No game files are in this repository or its builds.** You need your own copy of the game
> (the dumped CUSA02344 folder). This project is not affiliated with Sony Interactive
> Entertainment, Naughty Dog or Bluepoint Games.

## Status

| Game | Plays | Notes |
|---|---|---|
| Uncharted: Drake's Fortune | Yes | 60 FPS in the tested areas. Native rendering up to 2880x1620 (4K output upscaled from it, the HUD at 4K). |
| Uncharted 2: Among Thieves | Yes | Played at 1440p and 4K. 60 FPS and more; the heaviest view 99 FPS at 4K on an RTX 5080 (frame limit 120). |
| Uncharted 3: Drake's Deception | Yes | Played at 1080p and 1440p, 4K tested: the Cartagena crowd 90 FPS at 4K on an RTX 5080 (frame limit 120). |

The Collection menu, switching between the three games, saves, audio (music, speech, effects),
cutscene movies and controllers work. Details and open issues: [analysis/STATUS.md](analysis/STATUS.md);
plan and test list: [analysis/PLAN.md](analysis/PLAN.md).

## System requirements and the lowest PC that plays

- **Needs:** Windows 10 or 11 (64-bit); a CPU from **Intel's 4th-generation Core (Haswell, 2013)
  or newer, or any AMD Ryzen** (the games need AVX, F16C, BMI1, MOVBE and LZCNT); a graphics card
  or integrated GPU with **Vulkan 1.3** and a current driver; about **6 GB of free RAM** for the
  game (16 GB in the PC recommended).
- **Weakest GPU tested: the integrated AMD Radeon graphics of a Ryzen 7000/9000 desktop CPU**
  (2 compute units, about a quarter of a PS4's GPU). At **720p**: Uncharted 3 27-39 FPS,
  Uncharted 2 27 FPS, Drake's Fortune 24 FPS; at **540p** Uncharted 3 33 FPS. Playable, not
  smooth. Settings: preset **Low**, or resolution **1080p** with upscaling **FSR 1** at
  **Performance** (the game draws 960x540) or **Ultra Performance**, anti-aliasing off.
- **4 CPU cores:** 60 FPS in Drake's Fortune and Uncharted 3 (title screen and in play; a 4-core
  test with a fast GPU), with **Idle game threads: Sleep** (the default). On a 4-6 core CPU also
  try **Multithreaded rendering** (per game) and keep **Frames queued at 1**.
- **Players report:** Core i5-10400F + RTX 3050 6 GB plays Uncharted 2 and 3.

## Features

**Launcher** (`Uncharted Launcher.exe`)
- A card per game; settings are kept **per game**.
- Quality presets:

  | Preset | Resolution | Frame rate | For |
  |---|---|---|---|
  | Low | 1080p, FSR 1 Performance (draws 960x540) | 60 | weak GPUs |
  | Medium | 1080p (as on PS4) | 60 | most PCs |
  | High | 1440p | 60 | fast GPUs |
  | Ultra | 4K (Drake's Fortune draws 1620p, upscaled) | up to 120 (experimental) | very fast GPUs |

  Every preset queues 1 frame. 2 frames queued is faster in heavy scenes, but GPU results
  reach the game a frame later: keep it at 1.

  **Low-end PCs and laptops:** keep **Idle game threads** (launcher, **Performance** tab) on
  **Sleep** (the default). With **Spin** the games' six job threads keep their cores busy while
  they wait for work, as on the PS4 where each has its own core; on a 4-6 core CPU that took
  nearly all of it (a 4-core test: 19-27 FPS on the title screen, 60 with Sleep). Also turn on **Multithreaded rendering** (per game) and keep **Frames queued at 1**. The PS4's
  GPU commands are otherwise processed on one CPU core, which limits slower CPUs; with this option
  the Vulkan command recording runs on a second core. On fast desktop CPUs it makes no difference.
  If a game hangs while loading, turn that option off again. With a weak GPU also pick the **Low**
  preset (FSR 1 Performance at 1080p); integrated GPUs: FSR 1 **Ultra Performance**.

- **Display:** windowed, borderless or exclusive fullscreen; resolution 540p, 720p, 900p, 1080p,
  1440p, 1620p or 4K; aspect ratio 16:10, 16:9, 21:9, 32:9; **upscaling** FSR 1, FSR 3.1, FSR 4
  or NVIDIA DLSS: the game draws below the resolution (Quality 1/1.5, Balanced 1/1.7, Performance
  1/2, Ultra Performance 1/3 of its width and height) and each frame is upscaled to it before the
  HUD and menus, which stay sharp; FSR 3.1, FSR 4 and DLSS also at the resolution (Native AA,
  DLAA); anti-aliasing TAA (with upscaling off or FSR 1); RCAS sharpening; FPS counter. Upscaling
  saves GPU time only: where the CPU limits the frame rate (most PCs at 1080p and 1440p, Steam
  Deck) it changes little.
- **Performance:** frame rate limit 30 / 40 / 45 / 50 / 55 / 60 / 75 / 90 / 100 / 120 (60 is the
  PS4's; above 60 is experimental, and faster than 120 breaks the games' movement, so it is not
  offered; with VSync a limit at or above the monitor's refresh rate follows the monitor); frames
  queued; background shader compilation; multithreaded rendering (experimental); idle game
  threads (sleep or spin).
- **Keyboard and mouse:** mouse look (sensitivity, invert Y), right mouse aim, left mouse fire, WASD,
  and every action remappable per game in the **Controls** tab (two bindings per action, keys or
  mouse buttons). Controllers (DualSense, DualShock 4, Xbox) work at the same time.
- **Language** (Advanced tab): any of the 22 languages the games have text for; each game's main
  menu then offers it next to English (Square). Voices: the languages in your copy of the game.
- **Unlock all chapters** (Advanced tab, off by default): every chapter and the Brutal difficulty
  from the start in all three games; saves are untouched.
- **Game update 1.02** is used automatically when it sits next to the game folder (see below).
- Log and save folders, desktop shortcut, shader cache reset, game folder selection, links to the
  author's [YouTube](https://www.youtube.com/@BilalReloaded/videos) and GitHub.

**In game:** `Insert` or `LT + R3` opens the menu (display, anti-aliasing, draw distance, frame rate,
overlay, controls). Both shortcuts can be changed in its Controls tab (any key; any one or two
controller buttons).

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

- **Drake's Fortune:** the grenade launcher can kill the player (GitHub #6). Being looked at.
- **Slower CPUs:** the PS4's GPU commands are processed on one CPU core (3,000-5,600 draws per frame
  in the heaviest scenes); on older CPUs those scenes can drop below 60 FPS.
- **Multithreaded rendering is experimental:** its crashes with 1 frame queued are fixed. Drake's
  Fortune always runs without it (its guns flickered grey with it, GitHub #23). It is
  meant for low-end PCs and laptops (see the presets above) and gives no speed-up on fast CPUs;
  with "Frames queued 2" Uncharted 2 can still stop with the game assert `m_gfxEopTick`, so keep 1.
- **Recording with OBS Game Capture:** if OBS keeps losing and re-hooking the game, the game's
  memory use grows (each hook creates DirectX devices in the game). With an NVIDIA card set
  NVIDIA Control Panel -> Manage 3D settings -> Program settings -> Uncharted.exe ->
  **Vulkan/OpenGL present method: Prefer native**, or use OBS **Window Capture**.
- **Frames queued 2** is faster in heavy scenes, but GPU results reach the game a frame later
  (Uncharted 2's NPCs float, Nate's feet sink into the floor): keep it at 1 (every preset does).
- Drake's Fortune renders at most 2880x1620 (its memory layout); above it that size is upscaled
  to the resolution before the HUD (FSR 1, or the temporal upscaler chosen).
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

## Uncharted 3 multiplayer (beta)

Drake's Deception's multiplayer (competitive, co-op, LAN party) restored on top of this port. Nothing
game-derived ships with the launcher: the **MULTIPLAYER** tab converts the files from your own copies.

**What you need**

- The PS4 Collection as the launcher's game folder: **CUSA02320 (EU), version 1.02** (the multiplayer build
  is made for its Uncharted 3 executable; the import checks it by SHA-256 and says so when yours differs.
  A replaced `big3-ps4_Shipping.elf` is fine when the retail one is kept as `backup/big3-ps4_Shipping.elf`).
- Your PS3 **Uncharted 3 Game of the Year** (BCES01670 EU, BCES01692 UK, BCUS99086 US or BCAS25014 Asia; all have
  the same 1.19 code and the same game files apart from the speech languages): the game folder (disc `PS3_GAME/USRDIR` or an extracted
  ISO) and RPCS3's `dev_hdd0/game` folder with update **1.19**, the game data install (`BCES01670DATA2` /
  `BCES01692DATA2` / `BCUS99086DATA2` / `BCAS25014DATA2`) and the DLC map packs. Add both folders.
- The **decrypted 1.19 EBOOT** (`EBOOT.elf`) of your own copy of the game, for the characters' collision
  data. The import requires it; choose it on the Multiplayer tab.
- PS3 **Uncharted 2** is optional: no multiplayer file comes from it today.
- About 12 GB free for the multiplayer data folder (default
  `%LOCALAPPDATA%\Uncharted Launcher\U3 Multiplayer`, changeable on the tab).

**How to import**

1. Launcher -> **MULTIPLAYER** -> add your PS3 Uncharted 3 folders -> **IMPORT**. The tab shows the step
   (n/50) and the log (`import.log` in the data folder). The import first builds the multiplayer image from
   your Uncharted 3 executable with the port's own scripts and checks every stage by SHA-256 (seconds), then
   runs the overlay recipe (hours: maps and characters are converted). It keeps running if the launcher is
   closed; **RE-IMPORT / UPDATE** continues it or, after a launcher update, redoes only what changed.
2. Turn on **Start Uncharted 3 with multiplayer** and play Uncharted 3 as usual: display, controls, language
   and the other settings apply. Title screen: Right = Multiplayer, Right Right = Co-op; LAN Party: press
   Cross on your profile. Turned off, Uncharted 3 is the normal game again.

**Status**

| Part | State |
|---|---|
| Multiplayer image (helper v7.55) | Verified: the import reproduces the tested image byte for byte (SHA-256 `4ce34f24...`). |
| Overlay recipe | Runs whatever `u3mp/patcher/recipe/steps/ORDER.txt` holds; a failing step is reported on the tab and in `state.json`, the others go on. The map (pak23) and character (actor33) conversions are still being completed: until then the import reports those steps and the matching maps or characters are missing. |

A step that needs numpy is skipped when the launcher's Python has none; ffmpeg and GNU objdump are used from
the package's `u3mp\bin` when it carries them. Maintainers: `u3mp/patcher/maintainer/sync_recipe.py` copies a
new recipe in, `make_release.py` adds a helper build.

## Repository layout

| Path | Contents |
|---|---|
| `launcher/` | The Uncharted launcher (Tkinter + Pillow). |
| `tools/probe_uncharted.py` | Prepares a game executable (patches, render resolution, update) and starts the runtime. |
| `tools/package_windows.sh` | Builds the Windows package; `tools/windows_starter/` its `.exe`. |
| `tools/release.sh`, `tools/release-notes/` | Publishes a release. |
| `gow3_pc/` | The runtime: C runtime (`src/`), GPU library (`gpu/`: shadPS4 video core and shims), preparation scripts (`scripts/`). |
| `analysis/` | Compatibility notes (`STATUS.md`) and the plan (`PLAN.md`). |
| `u3mp/patcher/` | Uncharted 3 multiplayer import: `u3mp_import.py`, `mpimage.py` (image link), `recipe/` + `tools/` (overlay recipe), `build/v*/` (each multiplayer build's files and hashes; the online part is added by the release build, so a build from this source alone has no online play). |
| `u3mp/` (rest) | The multiplayer research source (helper C code, runner, pak tools, notes). |

## Development Plan

The roadmap tracks the launcher, multiplayer restoration for Uncharted 2 and 3, and crossplay
across PC, PS3, PS4 and PS5. See the [Development Plan checklist](DEVELOPMENT_PLAN.md).

## Contributing

Help is welcome. To keep the work organised, the project takes a small number of regular
contributors (one or two to start), and every change arrives as a pull request from a fork.

### What to work on

Contributions are limited to two areas:

- **Game-breaking glitches:** crashes, freezes, soft-locks, broken progression, missing or
  broken audio, controls that stop working.
- **Visual issues:** wrong colours, brightness or textures, missing geometry, broken effects,
  HUD and menu drawing problems.

Out of scope, and pull requests that touch these areas will be closed:

- **Online play, RPCN and matchmaking:** the sign-in, the online menus and any networking code.
  This part is maintained privately and is not open to contributions.
- **The build and release setup:** `.github/workflows/`, `tools/release.sh`, `tools/VERSION`,
  `launcher/updater.py` and `u3mp/patcher/maintainer/`.
- **Game files:** no game files, extracted assets, converted paks or patched game images, in any
  form. Changes ship as code and recipes that run on the player's PC.

If you are unsure whether something is in scope, open an issue and ask first.

### How to contribute

1. **Pick or open an issue** and comment that you are working on it. Wait for a reply before you
   start, so that two people never fix the same thing.
2. **Fork** this repository and create a branch for that one issue, for example
   `fix/airport-tpose`. Keep one fix per branch and one branch per pull request.
3. **Stay in sync.** Before you start, and again before you open the pull request, update your
   branch from this repository's `main`:
   ```bash
   git remote add upstream https://github.com/fidalgojoao/Uncharted-The-Nathan-Drake-Collection-PC-port.git
   git fetch upstream
   git rebase upstream/main
   ```
   Resolve conflicts on your side, keeping the changes already on `main`. Never force-push to this
   repository or rewrite its history; your own fork's branch is yours to rebase.
4. **Keep the change small and focused.** Don't reformat, rename or move code you are not fixing,
   so that changes from different people don't collide.
5. **Open the pull request against `main`** and describe:
   - the problem, with screenshots or a short clip;
   - the game, the map or chapter, and the steps to reproduce it;
   - what you changed and why;
   - how you tested it (your GPU and settings).
6. **Review:** the maintainer reviews, tests in game and merges. Please don't merge your own pull
   requests or push straight to `main`.

By contributing you agree that your contribution is licensed under the GNU GPL v2 or later, like
the rest of the project.

## Legal notice

- **Unofficial fan project.** This is a free, non-commercial, unofficial project made by fans. It is
  not made, approved, endorsed, sponsored or supported by Sony Interactive Entertainment, Naughty Dog,
  Bluepoint Games or any of their affiliates, and it is not affiliated with them in any way.
- **The games belong to their owners.** *Uncharted: The Nathan Drake Collection* was developed by
  Bluepoint Games and published by Sony Interactive Entertainment. *Uncharted: Drake's Fortune*,
  *Uncharted 2: Among Thieves* and *Uncharted 3: Drake's Deception* were developed by Naughty Dog and
  published by Sony Interactive Entertainment. All game code, art, audio, text and other content
  remain the property of their owners.
- **Trademarks.** "Uncharted" is a trademark of Sony Interactive Entertainment LLC. "PlayStation", "PS3",
  "PS4" and the PlayStation logos are trademarks or registered trademarks of Sony Interactive
  Entertainment Inc. "Naughty Dog" and "Bluepoint Games" are trademarks of their owners. All other
  trademarks belong to their owners. They are used here only to name the games and systems this
  project works with, not to suggest any connection or endorsement.
- **No game files, no piracy.** This repository and its releases contain no game files, no game code
  and no data taken from the games. Everything game-derived (converted files, patched game images)
  is made on each player's own PC from their own copies of the games, and is never shared by this
  project. You must own the games you use with it: the PS4 *Nathan Drake Collection* and, for the
  multiplayer, *Uncharted 3* on PS3. This project does not help anyone obtain games or bypass
  copy protection on games they don't own, and piracy is not supported.
- **How the multiplayer is built.** The multiplayer is restored by our own code, written from scratch:
  it reproduces how the PS3 version behaves, so the PS4 game can do what the PS3 game did. Apart from
  short instruction patterns it uses to find and check the places it patches in your own copy of the
  game, it contains no code from either game. The game data it needs is converted on your PC from your own
  PS3 and PS4 copies, and the patched game image is made there too; none of it is ever uploaded or shared.
- **No keys or secrets from the games.** A few values the game uses for its online profiles are part of the
  PS3 game. Releases do not contain them: the import reads them on your PC from your own decrypted copy of
  the PS3 game's executable, which the import asks for, and puts them only into the game image it builds for
  you. They never leave your PC.
- **Online play** runs on community RPCN servers, not on Sony's PlayStation Network (which no longer
  hosts these games' multiplayer). This project has no connection to PlayStation Network.
- **Nothing is downloaded from servers.** The launcher and the import download no game files or game data
  from any server: they use only the files already on your PC. The community server is used for online play
  only.
- **No money, ever.** This project is free. It takes no donations, sells nothing and shows no ads, and nothing
  connected to it is paid.
- **Contributions follow the same rules:** see [CONTRIBUTING.md](CONTRIBUTING.md).
- **No warranty.** The software is provided "as is", without warranty of any kind, as stated in the
  GNU GPL. Use it at your own risk.
- **Rights holders:** if you own rights in something here and have a concern, please open an issue on
  this repository and it will be looked at promptly.

## Credits and license

- **Uncharted port, launcher and game fixes: BilalReloaded**
  ([YouTube](https://www.youtube.com/@BilalReloaded/videos),
  [GitHub](https://github.com/bc160201983)), 2026. The fixes and their dates are listed in
  [CHANGELOG.md](CHANGELOG.md); the commit history shows each change.
- The runtime started from the [gow3_pc](https://github.com/FelipePrado0/gow3_pc) project
  (whose internal names, such as `GOW3_*` settings, it keeps) and the
  [shadPS4](https://github.com/shadps4-emu/shadPS4) video core.
- Temporal anti-aliasing for Uncharted takes the camera from the games' motion-blur velocity
  pass as SuperMedo's [Uncharted port](https://github.com/Supermedo/uncharted_pc) does (GPL v2;
  `CameraMotion::OnVelocityConstants` and the motion vector mode come from it).
- **NVIDIA DLSS:** DLAA uses NVIDIA DLSS technology: the NVIDIA DLSS SDK 310.9.16
  (`nvngx_dlss.dll`, under NVIDIA's license, in `licenses/nvidia-dlss`) through the DLSS bridge adapted from
  [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) (MIT).
  NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.
- AMD FSR 4: the v07 INT8 model as built by [Q2RTX](https://github.com/FireBurn/Q2RTX) from AMD's
  MIT-licensed FidelityFX SDK source.
- Third-party code: FSR (AMD), imgui, magic_enum, miniz, xbyak, LibAtrac9, FFmpeg, SDL3.
- Uncharted 3 multiplayer restoration (`u3mp/`, the launcher's Multiplayer tab): João Fidalgo's fork. Its pak
  tools come from the `U3_Multiplayer` branch of
  [fidalgojoao/Uncharted2-Multiplayer-Dev](https://github.com/fidalgojoao/Uncharted2-Multiplayer-Dev) (GPL v2);
  TLS by [BearSSL](https://bearssl.org) (MIT).
- Map and character visual fixes contributed by **33martyr** (Yeti mask, Outdoor Jacket, map lighting).
- **RPCS3 and RPCN:** online play (friends, invites, matchmaking) uses **RPCN**, the online service written
  for the [RPCS3](https://rpcs3.net) PlayStation 3 emulator, and talks to it the same way RPCS3 does.
  RPCN is by **RipleyTom** and its contributors ([RipleyTom/rpcn](https://github.com/RipleyTom/rpcn));
  RPCS3 is by the **RPCS3 team** and its contributors ([RPCS3/rpcs3](https://github.com/RPCS3/rpcs3)).
  A big thank you to both
  projects: the online play would not exist without them. This project is not part of, and is not
  endorsed by, RPCS3 or RPCN; please don't take its issues to them.
- The launcher window is built with [Qt 6](https://www.qt.io) (The Qt Company; LGPL v3, used as
  unmodified shared libraries); its settings run on [Python](https://www.python.org) (PSF License)
  with Tkinter and [Pillow](https://python-pillow.org) (HPND); the multiplayer import also uses
  [NumPy](https://numpy.org) (BSD). Uncharted 2 voice chat uses [Opus](https://opus-codec.org) (BSD).
  PS4 package reading uses liborbispkg (license in `licenses/liborbispkg`).
- Licensed under the GNU GPL v2 or later (see `gow3_pc/LICENSE`). The Windows package carries
  `LICENSE.txt` and a `licenses` folder with the license of every bundled library
  (`licenses/THIRD-PARTY.txt` lists each DLL package with its version, license and source).

### Reusing this work

You are welcome to use these fixes in your own port. The GPL asks that you keep the copyright and
license notices and publish your source code under the same license when you distribute your
build. Beyond that, a request: **credit this project and link back to it**
(`https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port`, by BilalReloaded) in your README or release
notes, so people can find where a fix came from.
