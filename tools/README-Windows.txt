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
4. Pick a game card and a preset, then press PLAY.
   In game, Insert or R3 + L2 opens the menu (display, frame rate, overlay).

Notes
- The first start of each game prepares files and compiles shaders: expect a slow first load
  and some stutter until the shader cache has filled.
- Settings are kept per game. Saves and logs are kept in analysis\ next to the launcher.
- Logs for troubleshooting: Advanced tab -> Open log folder (boot.log).
- Render resolutions above 1080p are experimental and known to crash in gameplay.
