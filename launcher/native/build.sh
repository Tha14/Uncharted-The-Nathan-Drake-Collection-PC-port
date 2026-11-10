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
for s in steam hang memstat config display controls system gamepads patcher; do
    clang++ "${FLAGS[@]}" -c "$s.cpp" -o "build/$s.o"
    OBJECTS+=("build/$s.o")
done
clang++ -shared -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -o uncharted_native.dll \
    "${OBJECTS[@]}" -lole32 -lwindowscodecs -luuid -ladvapi32 -luser32 -ldbghelp -ldxgi -lgdi32 -lshell32
llvm-objdump -p uncharted_native.dll | grep "DLL Name" | sort -u
sha256sum uncharted_native.dll
