Uncharted: The Nathan Drake Collection - PC port (beta)
=============================================================
By BilalReloaded - https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port

Requirements
- Windows 10 or 11, 64-bit.
- A GPU with Vulkan 1.3 support and an up-to-date driver (NVIDIA, AMD or Intel).
- Your own copy of the game: the CUSA02344 folder with eboot.bin and sce_sys
  (the same files for all three games). No game files are included here.

How to play
1. Extract this whole folder anywhere (keep everything together).
2. Start "Uncharted Launcher.exe".
3. On first Play, select your game folder (CUSA02344). The launcher remembers it.
   Recommended: the 1.02 update (stability and visual fixes). Extract it next to the game folder,
   named like it plus "-patch" (e.g. CUSA02344-patch, with or without its uroot folder). The
   launcher shows the version it uses under Advanced.
4. Pick a game card and a preset, then press PLAY.
   Low: 720p + FSR 1 at 60 FPS (weak GPUs). Medium: 1080p 60 (as on PS4).
   High: 1440p 60. Ultra: 4K at up to 120 FPS (fast GPUs; Uncharted 1
   draws at 1620p and upscales).
   In game, Insert or R3 + L2 opens the menu (display, frame rate, overlay).
5. Keyboard and mouse work out of the box (WASD move, mouse look, right mouse aim, left mouse
   fire, Space jump, Left Ctrl cover, F melee, R reload). Change them per game in the launcher's
   CONTROLS tab. Alt+Tab or Insert frees the mouse.

Notes
- The first start of each game prepares files and compiles shaders: expect a slow first load
  and some stutter until the shader cache has filled.
- Settings are kept per game. Saves and logs are kept in analysis\ next to the launcher.
- Logs for troubleshooting: Advanced tab -> Open log folder (boot.log).
- Render resolutions above 1080p are new: if a game crashes there, choose Medium (1080p).
- Low-end PCs and laptops: turn on Multithreaded rendering (Performance tab, per game) and keep
  Frames queued at 1. The GPU commands then use a second CPU core; on fast desktop CPUs it makes
  no difference. With a weak GPU also pick the Low preset. If a game hangs while loading, turn it
  off again. (Experimental: with 2 frames queued Uncharted 2 can crash.)
- Changes: CHANGELOG.md.

Known issues
- The heaviest Uncharted 2 scenes run at about 50 FPS at any resolution (the PS4's GPU commands
  are processed on one CPU core). Lighter scenes reach 60.
- Uncharted 3: Sully's shirt in the bar can flicker at 60 FPS (not at 30).
- Frames queued 2 is faster in heavy scenes, but GPU results reach the game a frame later
  (Uncharted 2's NPCs float, Nate's feet sink into the floor): keep it at 1.
- Multithreaded rendering is experimental (see the low-end tip above): keep Frames queued at 1
  with it.
- Recording with OBS Game Capture: if OBS keeps losing and re-hooking the game, the game's memory
  use grows. With an NVIDIA card set NVIDIA Control Panel -> Manage 3D settings -> Program settings
  -> Uncharted.exe -> Vulkan/OpenGL present method: Prefer native, or use OBS Window Capture.
- Uncharted 1 renders at most 2880x1620 and upscales to 4K (turn on FSR 1).
- Frame rates above 120 are not offered: the games' movement breaks.

License
GNU GPL v2 or later (LICENSE.txt); the source is at the address above. The licenses of the
bundled libraries are in licenses\ (THIRD-PARTY.txt lists every DLL package, its version,
license and source). Not affiliated with Sony, Naughty Dog or Bluepoint Games.
