#!/usr/bin/env bash
# build.sh - builds uncharted_native.dll (the launcher's native code) with MSYS2 CLANG64, statically linked: it needs
# only Windows system DLLs. Output: launcher/native/uncharted_native.dll (tools/package_windows.sh runs this).
set -euo pipefail
cd -- "$(dirname -- "$0")"
CLANG64=${CLANG64:-/c/msys64/clang64}
export PATH="$CLANG64/bin:$PATH"
mkdir -p build
FLAGS=(-O2 -DNDEBUG -fno-ident -ffunction-sections -fdata-sections -std=c++17 -Wall -Wextra -Wno-unused-parameter -municode)
OBJECTS=()
for s in steam hang memstat config display controls system gamepads patcher starter runtime_env starter_files patch_tables prepare link start play updater gamefiles gameupdate; do
    clang++ "${FLAGS[@]}" -c "$s.cpp" -o "build/$s.o"
    OBJECTS+=("build/$s.o")
done
clang++ -shared -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -o uncharted_native.dll \
    "${OBJECTS[@]}" -lole32 -lwindowscodecs -luuid -ladvapi32 -luser32 -ldbghelp -ldxgi -lgdi32 -lshell32 -lbcrypt -lwinhttp
llvm-objdump -p uncharted_native.dll | grep "DLL Name" | sort -u
sha256sum uncharted_native.dll
# The launcher update's installer (apply.cpp: tools\apply_update.exe in the package) and the bridge that old launchers
# start as python\pythonw.exe once the package has no Python (bridge.cpp). Both stand alone (static, no DLL of ours).
for exe in "apply:apply_update" "bridge:pythonw_bridge"; do
    clang++ "${FLAGS[@]}" -mwindows -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -o "${exe#*:}.exe" "${exe%%:*}.cpp" \
        -luser32 -lgdi32 -lshell32
    llvm-objdump -p "${exe#*:}.exe" | grep "DLL Name" | sort -u
done
