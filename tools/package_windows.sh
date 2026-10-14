#!/usr/bin/env bash
# Builds dist/Uncharted-windows/ and dist/Uncharted-windows.zip: a folder that runs on any Windows
# 10/11 x64 PC with a Vulkan GPU, without Python or MSYS2 installed.
#   Uncharted Launcher.exe   starter (tools/windows_starter) -> python\pythonw.exe launcher\...
#   python\                  Windows Python with Tkinter and Pillow (tools/copy_python.py)
#   launcher\ tools\         the launcher and the start script (tools/probe_uncharted.py)
#   gow3_pc\scripts\         the preparation scripts; gow3_pc\out\ the runtime and its DLLs
# No game files ship: each player selects their own copy of the game in the launcher.
# Run after building the runtime (gow3_pc/build.sh), from Git Bash or an MSYS2 CLANG64 shell.
# PYTHON_HOME is a Windows Python 3.10+ with Pillow (default: the `python` on PATH).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
# The Windows Python (looked up before the CLANG64 tools go first on PATH: MSYS2 has its own).
python=${PYTHON:-python}
python_home=${PYTHON_HOME:-$("$python" -c 'import sys, os; print(os.path.dirname(sys.executable))')}
msys=${MSYS2_ROOT:-/c/msys64}
[[ -d $msys/clang64/bin ]] && export PATH="$msys/clang64/bin:$PATH"
[[ -f gow3_pc/out/gow3-probe.exe ]] || { echo 'Build the runtime first: gow3_pc/build.sh' >&2; exit 1; }

stage=dist/stage/Uncharted-windows
rm -rf -- dist/stage
mkdir -p "$stage/gow3_pc/out" "$stage/gow3_pc/scripts" "$stage/tools" "$stage/launcher/assets" \
    "$stage/analysis/uncharted-user"

# The runtime without debug information, and every DLL it loads from the CLANG64 tree.
llvm-strip --strip-debug -o "$stage/gow3_pc/out/gow3-probe.exe" gow3_pc/out/gow3-probe.exe
ldd gow3_pc/out/gow3-probe.exe | awk '/clang64/ {print $3}' | sort -u | while read -r dll; do
    cp "$dll" "$stage/gow3_pc/out/"
done

cp gow3_pc/scripts/*.py "$stage/gow3_pc/scripts/"
cp tools/probe_uncharted.py "$stage/tools/"
cp launcher/uncharted_launcher.pyw "$stage/launcher/"
cp launcher/assets/uncharted.ico launcher/assets/icon.png "$stage/launcher/assets/"
# The runtime's optimizations known to break Uncharted (GOW3_TOGGLE_FILE).
cp tools/defaults/disabled-optimizations.txt "$stage/analysis/uncharted-user/"
cp tools/README-Windows.txt "$stage/README.txt"

"$python_home/python.exe" tools/copy_python.py "$python_home" "$stage/python"

windres tools/windows_starter/starter.rc -O coff -o dist/stage/starter.res.o
clang -O2 -municode -mwindows tools/windows_starter/starter.c dist/stage/starter.res.o \
    -o "$stage/Uncharted Launcher.exe"

find "$stage" -name __pycache__ -prune -exec rm -rf {} +
# Nothing from the game or a player (game executables, saves, shader caches, logs) may ship.
forbidden=$(find "$stage" \( -iname eboot.bin -o -iname '*.elf' -o -iname sce_sys -o -iname savedata \
    -o -iname cache -o -iname '*.log' -o -iname 'probe*.ini' -o -iname '*.pkg' -o -iname '*.psarc' \
    -o -iname '*.pak' -o -iname 'boot*.bin' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi

rm -f dist/Uncharted-windows.zip
(cd dist/stage && powershell -NoProfile -Command \
    "Compress-Archive -Path Uncharted-windows -DestinationPath ../Uncharted-windows.zip")
rm -rf dist/Uncharted-windows
mv "$stage" dist/Uncharted-windows
du -sh dist/Uncharted-windows dist/Uncharted-windows.zip
