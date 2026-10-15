# Runtime

The runtime that runs the Uncharted executables on Windows (see the [main README](../README.md)).
It started from the gow3_pc project, whose names it keeps internally (`gow3-probe.exe`, `GOW3_*`
settings, `gow3_*` files).

| Path | Contents |
|---|---|
| `src/` | The C runtime: loader (`probe.c`) and the PS4 system libraries the games use: memory, threads, files, save data, audio (ATRAC9, MP3), pad, video decoding, services. |
| `gpu/` | The GPU library: the shadPS4 video core (command processor, shader recompiler, Vulkan renderer, buffer and texture caches) and `gpu/shim/` (window, settings, in-game menu, fibers, Videodec2, movie scaling). |
| `scripts/` | Preparation of a game executable (`prepare.py`, `link_modules.py`, `content_profile.py`) and the update view (`game_update.py`). |
| `tests/` | Unit tests of runtime and GPU parts (CMake targets, built on demand). |

Build: `./build.sh` from an MSYS2 CLANG64 shell (packages: [packaging/windows/README.md](packaging/windows/README.md)).
The result is `out/gow3-probe.exe`; `tools/probe_uncharted.py` starts it with a prepared game.

Useful settings (environment, for diagnostics; the launcher sets the usual ones):

| Variable | Effect |
|---|---|
| `GOW3_GPU_ID=<n>` | Vulkan device to use (index in the log's GPU list). |
| `GOW3_ASYNC_READBACK=1` | GPU readbacks without waiting for the frame (launcher: Faster GPU readbacks). |
| `GOW3_FRAME_STATS=1`, `GOW3_FAULT_STATS=1`, `GOW3_PERF_DIAG=1` | Frame, fault and wait statistics every 5 s in the log. |
| `GOW3_GPU_PROFILE=1` | GPU time per render pass (slows rendering). |
| `GOW3_FAULT_TRACE=1` | The first 64 access violations, written straight to the log. |
| `GOW3_DUMP_SHADERS=1` | Shader binaries, IR and SPIR-V to the shader dump folder. |
