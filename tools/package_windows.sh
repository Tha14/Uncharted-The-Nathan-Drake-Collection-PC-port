#!/usr/bin/env bash
# Builds dist/Uncharted-windows/ and dist/Uncharted-windows.zip: a folder that runs on any Windows
# 10/11 x64 PC with a Vulkan GPU, without Python or MSYS2 installed.
#   Uncharted Launcher.exe   starter (tools/windows_starter) -> python\pythonw.exe launcher\...
#   python\                  Windows Python with Tkinter and Pillow (python_runtime below)
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
# Inside the MSYS2 shell (GitHub Actions) its root is "/": without the slash, "$msys/usr" stays a
# local path ("//usr/..." is a network share, each try timed out after ~11 s).
msys=${msys%/}
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
cp tools/probe_uncharted.py tools/apply_update.py \
    tools/game_files_102.json.gz "$stage/tools/"
# Uncharted 2 multiplayer (launcher -> Multiplayer while Uncharted 2 is chosen): u2mp/patcher, scripts only;
# the player's PC makes every game-derived file from the player's own game.
mkdir -p "$stage/u2mp"
cp -r u2mp/patcher "$stage/u2mp/patcher"
cp u2mp/README.md "$stage/u2mp/README.md"
mkdir -p "$stage/u2mp/docs"
cp u2mp/docs/import-list.tsv "$stage/u2mp/docs/import-list.tsv"   # the import's list (names, sizes, hashes)
find "$stage/u2mp" -name '__pycache__' -prune -exec rm -rf {} +
# Uncharted 3 multiplayer (launcher -> Multiplayer): the import (u3mp/patcher/u3mp_import.py), the overlay
# recipe and its tools (scripts and tables only), and the helper builds (our compiled code). The player's PC
# makes every game-derived file from the player's own copies.
mkdir -p "$stage/u3mp"
cp -r u3mp/patcher "$stage/u3mp/patcher"
# maintainer tools (recipe sync, release notes) are not needed by players
rm -rf "$stage/u3mp/patcher/maintainer"
find "$stage/u3mp/patcher" -name '__pycache__' -prune -exec rm -rf {} +
# Only the newest multiplayer build ships: the launcher (u3mp_release) and mpimage.py (latest_version) use the newest
# one, older ones only took space. A player's update drops the old folders (apply_update.py, installed-files.txt).
newest=$(ls -d "$stage/u3mp/patcher/build"/v*/ | xargs -n1 basename | sort -V | tail -1)
for d in "$stage/u3mp/patcher/build"/v*/; do [[ "$(basename "$d")" == "$newest" ]] || rm -rf "$d"; done
echo "multiplayer build shipped: $newest"
cp gow3_pc/LICENSE "$stage/u3mp/patcher/LICENSE.txt"
# The sound-bank encoder in C (sound-banks/native: the original vagenc.c; ~30x faster than vagenc.py), built
# here and checked byte-identical against vagenc.py; if either fails the import uses the Python encoder.
nat="$stage/u3mp/patcher/tools/fixes/sound-banks/native"
if clang -O2 -ffp-contract=off -fno-fast-math -shared -o "$nat/vagenc.dll" "$nat/vagenc.c" &&
        "$python_home/python.exe" "$nat/check_native.py"; then
    rm -f "$nat"/*.lib "$nat"/*.exp
else
    echo "Note: vagenc.dll not built or not identical: the multiplayer import uses the Python encoder" >&2
    rm -f "$nat/vagenc.dll"
fi
# The programs the import runs, from this PC's MSYS2 with their DLLs, when installed: ffmpeg (EULA pages,
# sound banks with MP3 samples; package mingw-w64-clang-x86_64-ffmpeg, whose DLLs the runtime ships) and GNU
# objdump (the Havok tables of the character conversion; mingw-w64-ucrt-x86_64-binutils). Without them the
# import still runs: the recipe skips or simplifies those steps and says so.
u3mp_bin=()
copy_program() {  # copy_program <exe> <folder>: the program and its DLLs from the same MSYS2 tree
    local exe=$1 dest=$2 tree
    tree=$(cygpath -u "$(dirname -- "$exe")")
    mkdir -p "$dest"
    cp "$exe" "$dest/"
    u3mp_bin+=("$exe")
    ldd "$exe" | awk -v t="$tree" 'index($3, t) == 1 {print $3}' | sort -u | while read -r dll; do
        cp "$dll" "$dest/"
    done
}
if [[ -f $msys/clang64/bin/ffmpeg.exe ]]; then
    copy_program "$msys/clang64/bin/ffmpeg.exe" "$stage/u3mp/bin"
else
    echo "Note: no ffmpeg.exe in $msys/clang64/bin: the multiplayer import runs without it" >&2
fi
if [[ -f $msys/ucrt64/bin/objdump.exe ]]; then
    copy_program "$msys/ucrt64/bin/objdump.exe" "$stage/u3mp/bin/binutils"
else
    echo "Note: no GNU objdump in $msys/ucrt64/bin (pacman -S mingw-w64-ucrt-x86_64-binutils): the" \
         "multiplayer import skips the Havok tables" >&2
fi
# PkgTool (LibOrbisPkg, LGPL-3.0): Advanced -> Install game update unpacks the player's own update PKG.
mkdir -p "$stage/tools/pkgtool"
cp tools/pkgtool/PkgTool.exe tools/pkgtool/LibOrbisPkg.dll "$stage/tools/pkgtool/"
# Every launcher module and data file.
cp launcher/*.py launcher/*.pyw "$stage/launcher/"
# the launcher's own native code (launcher/native: Add to Steam), built here with CLANG64
CLANG64="$msys/clang64" bash launcher/native/build.sh
mkdir -p "$stage/launcher/native"
cp launcher/native/uncharted_native.dll "$stage/launcher/native/"
# The update installer (launcher/native/apply.cpp): the launcher starts it after an update's download, and launchers up
# to 0.10.63 reach it through tools/apply_update.py. When the package drops Python, ship launcher/native/
# pythonw_bridge.exe as python/pythonw.exe (and small placeholders for launcher/uncharted_launcher.pyw,
# launcher/updater.py, tools/apply_update.py and tools/probe_uncharted.py): launchers up to 0.10.63 check for those
# files and start python\pythonw.exe -I tools\apply_update.py, which the bridge turns into tools\apply_update.exe.
cp launcher/native/apply_update.exe "$stage/tools/"
# The launcher's window (launcher-qt: Qt 6, C++ and QML; it runs launcher\qt_backend.py), built with CLANG64's
# Qt (mingw-w64-clang-x86_64-qt6-base, -qt6-declarative) and deployed with its Qt modules and every DLL it loads.
# Without Qt the package keeps the launcher's own window (the starter falls back to it).
qt_dlls=()
if [[ -f $msys/clang64/bin/windeployqt6.exe ]]; then
    qt_build=dist/stage/qt-build
    cmake -S launcher-qt -B "$qt_build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ > /dev/null
    ninja -C "$qt_build" UnchartedLauncher > "$qt_build.log" 2>&1 || { tail -40 "$qt_build.log" >&2; exit 1; }
    mkdir -p "$stage/launcher-qt"
    llvm-strip -o "$stage/launcher-qt/UnchartedLauncher.exe" "$qt_build/UnchartedLauncher.exe"
    # qmlimportscanner lives in CLANG64's share/qt6/bin
    PATH="$msys/clang64/share/qt6/bin:$PATH" windeployqt6 --qtpaths "$msys/clang64/bin/qtpaths6.exe" --release \
        --qmldir launcher-qt/qml --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime \
        "$stage/launcher-qt/UnchartedLauncher.exe" > "$qt_build-deploy.log" 2>&1 \
        || { tail -40 "$qt_build-deploy.log" >&2; exit 1; }
    # the QML modules' plugin DLLs windeployqt leaves out of CLANG64's layout (QtQuick.Controls ...)
    qml_src=$msys/clang64/share/qt6/qml
    (cd "$stage/launcher-qt/qml" && find . -type d) | while read -r dir; do
        for plugin in "$qml_src/$dir"/*.dll; do
            if [[ -f $plugin && ! -f $stage/launcher-qt/qml/$dir/$(basename -- "$plugin") ]]; then
                cp "$plugin" "$stage/launcher-qt/qml/$dir/"
            fi
        done
    done
    # the window always uses the Basic style: the other Quick Controls styles do not ship
    for style in FluentWinUI3 Fusion Imagine Material Universal Windows; do
        rm -rf "$stage/launcher-qt/qml/QtQuick/Controls/$style"
        rm -f "$stage/launcher-qt"/Qt6QuickControls2"$style"*.dll
    done
    rm -rf "$stage/launcher-qt/qmltooling"
    # its Qt files are its own folder's, never an installed Qt's
    printf '[Paths]\r\nPrefix = .\r\nPlugins = .\r\nQmlImports = qml\r\n' > "$stage/launcher-qt/qt.conf"
    # the CLANG64 DLLs Qt and its plugins load (windeployqt copies Qt's own only)
    find "$stage/launcher-qt" \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 -n 20 ldd 2>/dev/null \
        | awk '/clang64/ {print $3}' | sort -u | while read -r dll; do
        [[ -f $stage/launcher-qt/$(basename -- "$dll") ]] || cp "$dll" "$stage/launcher-qt/"
    done
    for dll in "$stage"/launcher-qt/*.dll; do
        qt_dlls+=("$(cygpath -m "$msys/clang64/bin/$(basename -- "$dll")")")
    done
    echo "launcher window: launcher-qt ($(du -sh "$stage/launcher-qt" | cut -f1))"
else
    echo "Note: no Qt 6 in $msys/clang64 (pacman -S mingw-w64-clang-x86_64-qt6-declarative): the package keeps" \
         "the launcher's own window" >&2
fi
# The package's version (tools/VERSION; the launcher's updates compare it with the releases).
cp tools/VERSION "$stage/VERSION"
cp launcher/assets/uncharted.ico launcher/assets/icon.png "$stage/launcher/assets/"
# The runtime's optimizations known to break Uncharted (GOW3_TOGGLE_FILE).
cp tools/defaults/disabled-optimizations.txt "$stage/analysis/uncharted-user/"
cp tools/README-Windows.txt "$stage/README.txt"
cp gow3_pc/LICENSE "$stage/LICENSE.txt"
cp tools/github/CHANGELOG.md "$stage/CHANGELOG.md"

# Licenses of the sources built into the runtime, and of every MSYS2 package whose DLL ships: its
# license files and, in THIRD-PARTY.txt, its version, license and source (some packages, FFmpeg
# among them, carry no license file).
licenses=$stage/licenses
mkdir -p "$licenses"
for dir in gow3_pc/gpu/third_party/* gow3_pc/third_party/*; do
    for file in "$dir"/LICENSE* "$dir"/COPYING* "$dir"/COPYRIGHT*; do
        [[ -f $file ]] || continue
        mkdir -p "$licenses/$(basename -- "$dir")"
        cp "$file" "$licenses/$(basename -- "$dir")/"
    done
done
head -n 24 gow3_pc/gpu/third_party/gcn/include/gcn/si_ci_vi_merged_offset.h > "$licenses/gcn-headers.txt"
# Opus (BSD), linked statically into the runtime for Uncharted 2 multiplayer voice chat.
if [[ -d $msys/clang64/share/licenses/opus ]]; then
    mkdir -p "$licenses/opus"
    cp "$msys"/clang64/share/licenses/opus/* "$licenses/opus/"
fi
# BearSSL (MIT): the TLS code inside u3_online.dll and the multiplayer helper
mkdir -p "$licenses/bearssl"
cp tools/licenses/bearssl/LICENSE.txt tools/licenses/bearssl/SOURCE.txt "$licenses/bearssl/"
mkdir -p "$licenses/liborbispkg"
cp tools/pkgtool/LICENSE.txt "$licenses/liborbispkg/LICENSE.txt"
echo "PkgTool and LibOrbisPkg 0.2.231 by Maxton, unchanged; source: https://github.com/maxton/LibOrbisPkg" \
    > "$licenses/liborbispkg/SOURCE.txt"
pacman=$(command -v pacman || echo "$msys/usr/bin/pacman")
# Three pacman calls in all (one per DLL took minutes on the CI runner).
dlls=()
for dll in "$stage"/gow3_pc/out/*.dll; do
    dlls+=("$(cygpath -m "$msys/clang64/bin/$(basename -- "$dll")")")
done
# The launcher window's Qt and CLANG64 DLLs (launcher-qt).
dlls+=("${qt_dlls[@]}")
# The multiplayer import's programs and their DLLs (u3mp/bin).
for file in "${u3mp_bin[@]}"; do
    dlls+=("$(cygpath -m "$file")")
done
for dll in "$stage"/u3mp/bin/*.dll; do
    [[ -f $dll ]] && dlls+=("$(cygpath -m "$msys/clang64/bin/$(basename -- "$dll")")")
done
for dll in "$stage"/u3mp/bin/binutils/*.dll; do
    [[ -f $dll ]] && dlls+=("$(cygpath -m "$msys/ucrt64/bin/$(basename -- "$dll")")")
done
mapfile -t packages < <({ "$pacman" -Qqo "${dlls[@]}" 2>/dev/null || true; } | sort -u)
{ "$pacman" -Qlq "${packages[@]}" | grep '/share/licenses/.*[^/]$' || true; } | while read -r file; do
    name=${file#*/share/licenses/}
    mkdir -p "$licenses/msys2/$(dirname -- "$name")"
    cp "$msys$file" "$licenses/msys2/$name"
done
{
    echo "Third-party packages whose DLLs ship in gow3_pc\\out (MSYS2 CLANG64 builds)."
    echo "Their sources: https://repo.msys2.org/mingw/sources/ and https://packages.msys2.org/"
    "$pacman" -Qi "${packages[@]}" | awk '/^Name /{print ""} /^(Name|Version|Licenses|URL) /{print}'
} > "$licenses/THIRD-PARTY.txt"

# DLAA and FSR 4 Native AA (tools/fetch_aa_files.sh), when fetched: NVIDIA's DLSS DLL and the
# bridge under their own licenses, and the FSR 4 native model. Without them the menus leave the
# two out.
if [[ -f gow3_pc/out/gow3_dlss.dll && -f gow3_pc/out/nvngx_dlss.dll ]]; then
    cp gow3_pc/out/gow3_dlss.dll gow3_pc/out/nvngx_dlss.dll "$stage/gow3_pc/out/"
    mkdir -p "$licenses/nvidia-dlss" "$licenses/gow3-dlss-bridge"
    dlss_license=${NVIDIA_DLSS_LICENSE:-}
    if [[ -z $dlss_license ]]; then
        for file in downloads/dlss-sdk-*/LICENSE.txt; do
            [[ -f $file ]] && { dlss_license=$file; break; }
        done
    fi
    [[ -f $dlss_license ]] || { echo 'Need the DLSS license (NVIDIA_DLSS_LICENSE).' >&2; exit 1; }
    cp "$dlss_license" "$licenses/nvidia-dlss/LICENSE.txt"
    cp gow3_pc/gpu/dlss_bridge/LICENSE.txt "$licenses/gow3-dlss-bridge/LICENSE.txt"
fi
if [[ -f gow3_pc/fsr4_shaders/fsr4_model_v07_i8_native_1080_pre.spv ]]; then
    mkdir -p "$stage/gow3_pc/fsr4_shaders" "$licenses/fsr4"
    cp gow3_pc/fsr4_shaders/LICENSE-FSR4-v07.txt gow3_pc/fsr4_shaders/*.spv gow3_pc/fsr4_shaders/*.bin \
        gow3_pc/fsr4_shaders/*.json "$stage/gow3_pc/fsr4_shaders/"
    cp gow3_pc/fsr4_shaders/LICENSE-FSR4-v07.txt "$licenses/fsr4/"
fi

# The Python the launcher runs on (players need no Python installed): the interpreter and its DLLs, Tkinter (DLLs, tcl),
# the standard library without tests, tools and caches, Pillow, and numpy when installed (the multiplayer import's map
# and character steps).
python_runtime() {
    local home=$1 dest=$2
    mkdir -p "$dest/Lib/site-packages"
    local f
    for f in "$home"/python.exe "$home"/pythonw.exe "$home"/python3*.dll "$home"/vcruntime140*.dll "$home"/LICENSE.txt; do
        [[ -f $f ]] && cp -p "$f" "$dest/"
    done
    for f in DLLs tcl; do
        [[ -d $home/$f ]] && tar -C "$home" --exclude=__pycache__ -cf - "$f" | tar -C "$dest" -xf -
    done
    tar -C "$home" --exclude=site-packages --exclude=test --exclude=tests --exclude=idlelib --exclude=ensurepip \
        --exclude=turtledemo --exclude=__pycache__ --exclude=lib2to3 --exclude=venv --exclude=pydoc_data \
        --exclude='*.pyc' -cf - Lib | tar -C "$dest" -xf -
    [[ -d $home/Lib/site-packages/PIL ]] || { echo "Pillow is not installed in $home (pip install pillow)" >&2; exit 1; }
    tar -C "$home/Lib/site-packages" --exclude=__pycache__ -cf - PIL | tar -C "$dest/Lib/site-packages" -xf -
    if [[ -d $home/Lib/site-packages/numpy ]]; then
        for f in numpy numpy.libs; do
            [[ -d $home/Lib/site-packages/$f ]] && tar -C "$home/Lib/site-packages" --exclude=__pycache__ --exclude=tests \
                -cf - "$f" | tar -C "$dest/Lib/site-packages" -xf -
        done
    else
        echo "Note: numpy is not installed in this Python: the Uncharted 3 multiplayer import will skip the steps that need it"
    fi
    echo "Python copied from $home"
}
python_runtime "$python_home" "$stage/python"
# Python's own license is python\LICENSE.txt; Pillow's goes with the others.
for file in "$python_home"/Lib/site-packages/pillow-*.dist-info/licenses/* \
    "$python_home"/Lib/site-packages/pillow-*.dist-info/LICENSE*; do
    [[ -f $file ]] && { mkdir -p "$licenses/pillow"; cp "$file" "$licenses/pillow/"; }
done
# numpy (the multiplayer import's map and character steps), when python_runtime found it.
for file in "$python_home"/Lib/site-packages/numpy-*.dist-info/licenses/* \
    "$python_home"/Lib/site-packages/numpy-*.dist-info/LICENSE*; do
    [[ -f $file && -d $stage/python/Lib/site-packages/numpy ]] && { mkdir -p "$licenses/numpy"; cp "$file" "$licenses/numpy/"; }
done

windres tools/windows_starter/starter.rc -O coff -o dist/stage/starter.res.o
clang -O2 -municode -mwindows tools/windows_starter/starter.c dist/stage/starter.res.o \
    -o "$stage/Uncharted Launcher.exe"

find "$stage" -name __pycache__ -prune -exec rm -rf {} +
# Nothing from the game or a player (game executables, saves, shader caches, logs) may ship.
forbidden=$(find "$stage" \( -iname eboot.bin -o -iname '*.elf' -o -iname sce_sys -o -iname savedata \
    -o -iname cache -o -iname '*.log' -o -iname 'probe*.ini' -o -iname '*.pkg' -o -iname '*.psarc' \
    -o -iname '*.pak' -o -iname 'boot*.bin' -o -iname '*.dds' -o -iname '*.at9' \) -print)
# Converted overlay folders (overlay-r3 ...) must not ship either; the multiplayer recipe's own text files and
# folders are named after the overlays they describe (recipes/overlay-r3.json, data/overrides/overlay-r3/: names,
# hashes, options; sync_recipe.py refuses game data in them), so only folders holding game files count.
forbidden+=$(find "$stage" -type d -iname 'overlay-*' -not -path "*/u3mp/patcher/tools/*" \
    -not -path "*/u3mp/patcher/recipe/*" -print)
forbidden+=$(find "$stage/u3mp/patcher" -type d -iname 'overlay-*' -exec find {} -type f \
    \( -iname '*.pak' -o -iname '*.bin' -o -iname '*.psarc' -o -iname '*.dds' \) \; 2>/dev/null)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi

rm -f dist/Uncharted-windows.zip
"$python_home/python.exe" -m zipfile -c dist/Uncharted-windows.zip "$stage"
rm -rf dist/Uncharted-windows
mv "$stage" dist/Uncharted-windows
du -sh dist/Uncharted-windows dist/Uncharted-windows.zip
