Uncharted: The Nathan Drake Collection - unofficial PC port (beta)
=================================================================
Source: https://github.com/fidalgojoao/Uncharted-The-Nathan-Drake-Collection-PC-port
Based on BilalReloaded's PC port (see Credits).

What you need
- 64-bit Windows 10 or 11.
- A graphics card with Vulkan 1.3 and a current driver (NVIDIA, AMD or Intel).
- Your own copy of the game: the CUSA02344 folder that holds eboot.bin and sce_sys. All three
  games run from it. This package contains no game files.

Getting started
1. Extract the whole folder wherever you like and keep its contents together.
2. Run "Uncharted Launcher.exe".
3. The first time you press Play, point the launcher at your game folder (CUSA02344); it is
   remembered from then on.
   The 1.02 game update is recommended (stability and visual fixes). Either install it from its
   .pkg (Advanced -> GAME UPDATE), or extract it beside the game folder with "-patch" added to the
   name (for example CUSA02344-patch, with or without a uroot folder inside). Advanced shows
   which update version is in use.
4. Choose a game card and a preset, then press PLAY.
     Low     720p with FSR 1 at 60 FPS, for weak graphics cards
     Medium  1080p at 60 FPS, like the PS4
     High    1440p at 60 FPS
     Ultra   4K at up to 120 FPS, for fast cards (Uncharted 1 renders at 1620p and upscales)
   During play, Insert or LT + R3 opens the in-game menu (display, anti-aliasing, frame rate,
   overlay, controls). Both shortcuts can be changed in its Controls tab.
5. Keyboard and mouse work right away: WASD to move, mouse to look, right button to aim, left
   button to fire, Space to jump, Left Ctrl for cover, F for melee, R to reload. Each game's keys
   can be changed in the launcher's CONTROLS tab. Alt+Tab or Insert releases the mouse.

Good to know
- Each game's first start prepares its files and builds shaders, so the first load is slow and
  there is some stutter until the shader cache is filled.
- Settings are stored per game. Saves and logs live in the analysis\ folder beside the launcher.
- For troubleshooting, Advanced -> Open log folder shows the logs (boot.log).
- Resolutions above 1080p are newer: if a game crashes there, go back to Medium (1080p).
- On laptops and low-end PCs, leave Idle game threads (Performance tab) at Sleep, the default.
  Spin keeps the games' six job threads busy while they wait, which takes a whole 4-6 core CPU.
  Turning on Multithreaded rendering (per game) with Frames queued at 1 moves the GPU commands to
  a second core; fast desktop CPUs gain nothing from it. Pair a weak graphics card with the Low
  preset. If a game hangs while loading, turn Multithreaded rendering off again. (Experimental:
  Uncharted 2 can crash with 2 frames queued.)
- What changed in each version: CHANGELOG.md.

Lowest tested hardware
- Windows 10 or 11 (64-bit); an Intel Core of the 4th generation (Haswell) or later, or any AMD
  Ryzen; a Vulkan 1.3 graphics card with a current driver; about 6 GB of free memory (16 GB
  installed).
- Weakest graphics tested: the AMD Radeon graphics built into Ryzen 7000/9000 CPUs. At 720p
  Uncharted 3 runs at 27-39 FPS, Uncharted 2 at 27 and Drake's Fortune at 24; at 540p Uncharted 3
  reaches 33. Use the Low preset at 540p or 720p with FSR 1 and anti-aliasing off.
- With 4 CPU cores Drake's Fortune and Uncharted 3 hold 60 FPS when Idle game threads is Sleep.
- Reported by players: a Core i5-10400F with an RTX 3050 6 GB plays Uncharted 2 and 3.

Known issues
- Uncharted 2's heaviest scenes stay around 50 FPS at any resolution, because the PS4's GPU
  commands are handled on a single CPU core. Lighter scenes reach 60.
- Frames queued 2 helps heavy scenes, but the game then gets GPU results one frame late (in
  Uncharted 2 characters float and Nate's feet sink into the floor). Keep it at 1.
- Multithreaded rendering is experimental (see the low-end note above); use it only with Frames
  queued at 1.
- OBS Game Capture: if OBS keeps losing and re-hooking the game, the game's memory use climbs.
  On NVIDIA cards set NVIDIA Control Panel -> Manage 3D settings -> Program settings ->
  Uncharted.exe -> Vulkan/OpenGL present method to Prefer native, or use Window Capture.
- Uncharted 1 renders at most 2880x1620 and upscales to 4K (turn FSR 1 on).
- Frame rates above 120 are not available, since the games' movement breaks beyond that.

Credits
- The original PC port, launcher and game fixes: BilalReloaded
  (https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port).
- DLAA uses NVIDIA DLSS technology (DLSS SDK 310.9.16). NVIDIA, RTX and DLSS are trademarks of
  NVIDIA Corporation. NVIDIA's license for nvngx_dlss.dll: licenses\nvidia-dlss\LICENSE.txt.
- AMD FidelityFX Super Resolution (FSR 1, FSR 3.1, FSR 4) by AMD; the FSR 4 model as built by
  Q2RTX from AMD's MIT-licensed source.
- Camera motion for anti-aliasing in Uncharted follows SuperMedo's Uncharted port.
- The runtime comes from the gow3_pc project and the shadPS4 video core.

License
GNU GPL v2 or later (LICENSE.txt); the source is at the address at the top. Not covered by the
GPL and licensed separately: the online components in u3mp\patcher\online\ (u3_online.dll,
u2_online.dll) and the multiplayer helper
u3mp\patcher\build\<version>\profile-helper.bin, which are separate works by this fork's author.
The bundled libraries' licenses are in licenses\ (THIRD-PARTY.txt lists every DLL package with its
version, license and source; BearSSL is in licenses\bearssl).
This is an unofficial fan project, not affiliated with, endorsed or supported by Sony Interactive
Entertainment, Naughty Dog or Bluepoint Games.
