# Uncharted: The Nathan Drake Collection — Unofficial PC port

A free, non-commercial fan project that runs the PS4 *Uncharted: The Nathan Drake Collection*
(CUSA02344: *Drake's Fortune*, *Among Thieves* and *Drake's Deception*) natively on Windows, from
your own copy of the game. The games' own x86-64 code runs directly on the PC; a runtime supplies
the PS4 system libraries and a Vulkan renderer runs the PS4 GPU work. This fork adds a new launcher
and an experimental **Uncharted 3 multiplayer restoration** (beta).

> **No game files are included, and you need your own legally obtained copies.** This project is
> not affiliated with or endorsed by Sony Interactive Entertainment, Naughty Dog or Bluepoint Games.

## Legal status

- **No game content.** This repository and its releases contain no game files, no game code and no
  data taken from the games. The launcher and the import download no game files or game data from
  any server; they use only the files already on your PC.
- **Your own copies, your own PC.** You need legally obtained copies: the PS4 Collection to play, plus
  *Uncharted 3* on PS3 for the multiplayer import. Everything game-derived (converted files, patched
  game images) is made on your PC from those copies and is never shared by this project.
- **No piracy.** The project does not decrypt games and does not help anyone obtain games or bypass
  copy protection on games they don't own. It works with game files already dumped from your own
  copies; the launcher refuses encrypted store PKGs.
- **Own code.** The multiplayer is restored by code written from scratch that reproduces how the PS3
  version behaves. Apart from short instruction patterns used to find and check the places it patches
  in your own copy, it contains no code from either game.
- **No keys or secrets from the games.** A few values the game uses for its online profiles are part
  of the PS3 game. Releases do not contain them: the import reads them on your PC from your own
  decrypted copy of the PS3 game's executable, which the import asks for, and puts them only into the
  game image it builds for you. They never leave your PC.
- **Online play** runs on community RPCN servers, not on PlayStation Network (which no longer hosts
  these games' multiplayer). This project has no connection to PlayStation Network.
- **Not affiliated** with, approved, endorsed or supported by Sony Interactive Entertainment, Naughty
  Dog, Bluepoint Games or their affiliates. *Uncharted* is a trademark of Sony Interactive
  Entertainment LLC; "PlayStation", "PS3", "PS4" and the PlayStation logos are trademarks of Sony
  Interactive Entertainment Inc. All game content and other trademarks belong to their owners and are
  named here only to identify the games and systems this project works with.
- **Free and open source.** No donations, sales or ads. Licensed under the GNU GPL v2 or later, with
  the complete source of everything under that license in this repository. Provided "as is", without
  warranty, as the GPL states.
- **Rights holders:** if you own rights in something here and have a concern, please
  [open an issue](../../issues) and it will be looked at promptly.

## Status

All three games play: the Collection menu, game switching, saves, audio, cutscene movies and
controllers work. Uncharted 2 and 3 have been played up to 4K, above 60 FPS on fast GPUs; Uncharted 3
has had less play testing than the other two. Details: [analysis/STATUS.md](analysis/STATUS.md).
Changes: [CHANGELOG.md](CHANGELOG.md) and the [release notes](../../releases).

## Requirements

- Windows 10 or 11, 64-bit, and about 6 GB of free RAM for the game (16 GB installed recommended).
- A CPU from Intel's 4th-generation Core (Haswell) or newer, or any AMD Ryzen (the games need AVX,
  F16C, BMI1, MOVBE and LZCNT). 4 cores hold 60 FPS in Drake's Fortune and Uncharted 3.
- A GPU or integrated GPU with Vulkan 1.3 and a current driver (NVIDIA, AMD or Intel). The weakest
  tested, the integrated Radeon of a Ryzen 7000/9000 CPU, gives 24-39 FPS at 720p: use the **Low** preset.

## Install and update

1. Download **Uncharted-windows.zip** from the latest [release](../../releases) and extract it anywhere.
2. Start **Uncharted Launcher.exe**. On the first Play, pick your dumped `CUSA02344` game folder (the
   one holding `eboot.bin` and `sce_sys`).
3. Recommended: game update **1.02**. Choose its extracted folder in **SETUP -> step 2**, or put it
   next to the game folder as `CUSA02344-patch`.
4. Pick a game and a preset, then press **PLAY**. Each game's first start compiles shaders: expect a
   slow first load and some stutter.

Settings are kept per game; saves and logs stay in the `analysis` folder next to the launcher. In
game, `Insert` or `R3 + L2` opens the in-game menu. The launcher's hints explain each setting;
[tools/README-Windows.txt](tools/README-Windows.txt) covers controls and low-end PCs. **Updates:** the
launcher offers **UPDATE TO x.y.z** when a newer release exists, checks the package's SHA-256, keeps
saves, settings and the shader cache, and restores the installed version if anything fails.

## Uncharted 3 multiplayer (beta)

Drake's Deception's multiplayer (competitive, co-op, LAN party), restored on top of this port. The
**MULTIPLAYER** tab builds it on your PC from your own copies. You need:

- The PS4 Collection **version 1.02** as the game folder: CUSA02320 (EU) or CUSA02343 (US), whose 1.02 game
  executables are byte-identical.
- PS3 **Uncharted 3 Game of the Year** (BCES01670, BCES01692, BCUS99086 or BCAS25014): the game folder
  and RPCS3's `dev_hdd0/game` folder with update 1.19, the game data install and the DLC map packs.
- The decrypted 1.19 `EBOOT.elf` of your own copy, and about 12 GB of free space.

Press **IMPORT**; the tab shows each step and its log. Converting maps and characters takes hours and
continues if the launcher is closed. The map and character conversions are still being completed: the
tab reports any step that fails. Research source and notes: [u3mp/](u3mp/README.md).

## Known issues

- Drake's Fortune: the grenade launcher can kill the player (issue #6); it renders at most 2880x1620.
- On slower CPUs the heaviest scenes can drop below 60 FPS. Multithreaded rendering (experimental)
  helps low-end PCs. Keep **Frames queued at 1**: with 2, GPU results arrive a frame late.
- OBS Game Capture can make the game's memory grow; use Window Capture, or set NVIDIA's
  "Vulkan/OpenGL present method" for Uncharted.exe to **Prefer native**.
- Report crashes with `boot.log` (launcher **TOOLS -> Open log folder**). Full list:
  [analysis/STATUS.md](analysis/STATUS.md).

## Contributing

Help is welcome from a small number of regular contributors, through pull requests from a fork. In
scope: **game-breaking glitches** (crashes, freezes, soft-locks, broken audio or controls) and **visual
issues** (wrong colours or textures, missing geometry, broken effects or HUD). Closed as out of scope:

- **Online play, RPCN and matchmaking.** This part is maintained privately and is not open to
  contributions.
- **The build and release setup** (`.github/workflows/`, `tools/release.sh`, `tools/VERSION`,
  the launcher's self-update, `u3mp/patcher/maintainer/`).
- **Game files** in any form. Fixes ship as code and recipes that run on the player's PC.

Comment on an issue and wait for a reply before you start. Keep one small fix per branch, rebase on
`main` (`git remote add upstream https://github.com/fidalgojoao/Uncharted-The-Nathan-Drake-Collection-PC-port.git`,
then `git fetch upstream && git rebase upstream/main`), and describe the problem, how to reproduce it,
what you changed and how you tested it. The maintainer reviews, tests and merges. Every pull request
follows [CONTRIBUTING.md](CONTRIBUTING.md) and is licensed under the GNU GPL v2 or later.

## Credits and license

- **BilalReloaded** ([YouTube](https://www.youtube.com/@BilalReloaded/videos),
  [GitHub](https://github.com/bc160201983)) made the
  [original PC port](https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port):
  the Uncharted port (the game runtime), the game fixes and the launcher are his. This fork's
  launcher is his launcher's code converted from Python to C++. Without his port, none of this
  would exist.
- The runtime started from [gow3_pc](https://github.com/FelipePrado0/gow3_pc) (whose `GOW3_*` names it
  keeps) and the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core.
- Temporal anti-aliasing takes the camera from the games' motion-blur velocity pass, the approach of
  SuperMedo's [Uncharted port](https://github.com/Supermedo/uncharted_pc) (GPL v2).
- **NVIDIA DLSS:** DLAA uses NVIDIA DLSS technology: the NVIDIA DLSS SDK 310.9.16 (`nvngx_dlss.dll`,
  under NVIDIA's license, in `licenses/nvidia-dlss`) through the DLSS bridge adapted from
  [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) (MIT).
  NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.
- Third-party code: FSR (AMD), imgui, magic_enum, miniz, xbyak, LibAtrac9, FFmpeg, SDL3,
  [Qt 6](https://www.qt.io) (LGPL v3, unmodified shared libraries), [Python](https://www.python.org)
  (PSF) with [Pillow](https://python-pillow.org) (HPND) and [NumPy](https://numpy.org) (BSD),
  and [Opus](https://opus-codec.org) (BSD).
- Uncharted 3 multiplayer restoration (`u3mp/`), including its pak tools: João Fidalgo. TLS by
  [BearSSL](https://bearssl.org) (MIT).
- Map and character visual fixes by **33martyr** (Yeti mask, Outdoor Jacket, map lighting).
- **RPCS3 and RPCN:** online play uses **RPCN**, the online service of the [RPCS3](https://rpcs3.net)
  emulator, and talks to it as RPCS3 does. RPCN is by **RipleyTom** and contributors
  ([RipleyTom/rpcn](https://github.com/RipleyTom/rpcn)); RPCS3 by the RPCS3 team and contributors
  ([RPCS3/rpcs3](https://github.com/RPCS3/rpcs3)). Thank you to both. This project is not part of, and
  is not endorsed by, RPCS3 or RPCN; please don't take its issues to them.

**Fork and license.** This repository was created on **5 November 2026**, forked from the original at
commit `e0c99f9` (*Release 0.8.3*), which was then published under the **GNU GPL v2 or later**
([gow3_pc/LICENSE](gow3_pc/LICENSE)). That license lets anyone copy, modify and redistribute the code
if the result stays under it with its source available; this fork follows those terms, and a later
change to the original's license does not apply to this fork's copy. The Windows package carries
`LICENSE.txt` and every bundled library's license (`licenses/THIRD-PARTY.txt` lists each DLL package
with its version, license and source). The packages also carry the online components and the
multiplayer helper, separate works by this fork's author not covered by the GPL
([tools/README-Windows.txt](tools/README-Windows.txt)); a build from this source alone has no online play.

### Reusing this work

You are welcome to use these fixes in your own port. The GPL asks that you keep the copyright and
license notices and publish your source code under the same license when you distribute your build.
Beyond that, a request: **credit this project and the original port and link back to them**
(`https://github.com/fidalgojoao/Uncharted-The-Nathan-Drake-Collection-PC-port` and BilalReloaded's
`https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port`) in your README or release
notes, so people can find where a fix came from.
