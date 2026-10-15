Uncharted: The Nathan Drake Collection - PC port (test build)
=============================================================

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
   High: 1440p 60. Ultra: 4K with the frame rate unlocked (fast GPUs; Uncharted 1 draws at
   1620p and upscales).
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
