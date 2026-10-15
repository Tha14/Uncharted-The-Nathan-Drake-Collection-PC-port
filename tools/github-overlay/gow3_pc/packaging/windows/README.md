# Building on Windows

The runtime builds with MSYS2's CLANG64 toolchain. Install [MSYS2](https://www.msys2.org/), open
the **MSYS2 CLANG64** shell and install:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
```

magic_enum, miniz and xbyak (not packaged by MSYS2) are submodules under `gpu/third_party/`;
FSR-Vulkan gets this project's patches (`gpu/patches/fsr-vulkan`) applied by `build.sh`.

Then, in the repository: `gow3_pc/build.sh`, and `tools/package_windows.sh` for the player
package (see the [main README](../../../README.md)).

`gow3-probe.rc` / `gow3-probe.manifest` give the runtime its icon (`uncharted.ico`) and Windows
settings (UTF-8 file paths, long paths, DPI awareness).
