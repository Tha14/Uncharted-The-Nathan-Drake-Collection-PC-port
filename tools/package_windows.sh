#!/usr/bin/env bash
# tools/package_windows.sh - makes the player package: dist/Uncharted-windows/ and dist/Uncharted-windows.zip.
# The folder runs on 64-bit Windows 10/11 with a Vulkan GPU; players need neither Python nor MSYS2.
#
#   Uncharted Launcher.exe    the starter (tools/windows_starter)
#   launcher\, launcher-qt\   the launcher: its Python modules, its native code (launcher/native) and its Qt window
#   python\                   the Python it runs on (Tkinter, Pillow, numpy when installed)
#   tools\                    the game starter, the update installer, PkgTool, the base game's file list
#   gow3_pc\                  the runtime and its DLLs (out\), its preparation scripts, the FSR 4 models
#   u2mp\, u3mp\              the multiplayer imports (scripts, tables and our own compiled helpers)
#   licenses\                 every bundled component's license
# No game files ship; each player's own copy of the game is chosen in the launcher.
#
# Run it from Git Bash or an MSYS2 CLANG64 shell once the runtime is built (gow3_pc/build.sh).
# PYTHON / PYTHON_HOME: the Windows Python 3.10+ with Pillow to bundle (default: `python` on PATH);
# MSYS2_ROOT: the MSYS2 installation (default /c/msys64); NVIDIA_DLSS_LICENSE: the DLSS license file.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

# The Windows Python is looked up first, before MSYS2's own python comes ahead on PATH.
python=${PYTHON:-python}
python_home=${PYTHON_HOME:-$("$python" -c 'import os, sys; print(os.path.dirname(sys.executable))')}
msys=${MSYS2_ROOT:-/c/msys64}
msys=${msys%/}   # GitHub's runner has MSYS2 at "/": "//usr" would be a network path (each try timed out after ~11 s)
if [[ -d $msys/clang64/bin ]]; then
    export PATH="$msys/clang64/bin:$PATH"
fi
if [[ ! -f gow3_pc/out/gow3-probe.exe ]]; then
    echo 'Build the runtime first: gow3_pc/build.sh' >&2
    exit 1
fi
# FFmpeg: the small build (tools/build_ffmpeg.sh, only the decoders the port uses, no external libraries) instead of
# MSYS2's full one, which pulls in ~130 MB of codec libraries; its DLLs keep FFmpeg's names, so they are found first.
ffmpeg_bin=downloads/ffmpeg-8.1.1-min/bin
if [[ ! -f $ffmpeg_bin/ffmpeg.exe ]]; then
    echo 'Build the small FFmpeg first: tools/build_ffmpeg.sh' >&2
    exit 1
fi
export PATH="$PWD/$ffmpeg_bin:$PATH"
from_msys_or_ffmpeg='/clang64|ffmpeg-8.1.1-min/'   # the DLL paths ldd shows that ship

stage=dist/stage/Uncharted-windows
licenses=$stage/licenses
rm -rf -- dist/stage
mkdir -p "$stage/gow3_pc/out" "$stage/gow3_pc/scripts" "$stage/tools" "$stage/launcher/assets" \
    "$stage/analysis/uncharted-user"

# The MSYS2 files whose packages licenses\ has to cover (for licenses_of_msys2 at the end).
msys_files=()
msys_file() {   # msys_file <tree> <file>: the MSYS2 file a shipped copy came from (FFmpeg's are our own build)
    [[ -f $ffmpeg_bin/$(basename -- "$2") ]] && return 0
    msys_files+=("$(cygpath -m "$msys/$1/bin/$(basename -- "$2")")")
}

# ------------------------------------------------------------------------------------------ the runtime

# Stripped of debug information, with every DLL it loads from CLANG64 and the small FFmpeg.
llvm-strip --strip-debug -o "$stage/gow3_pc/out/gow3-probe.exe" gow3_pc/out/gow3-probe.exe
while read -r dll; do
    cp "$dll" "$stage/gow3_pc/out/"
done < <(ldd gow3_pc/out/gow3-probe.exe | awk "$from_msys_or_ffmpeg {print \$3}" | sort -u)
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
# The programs the import runs, with their DLLs: ffmpeg (EULA pages, sound banks with MP3 samples; the small build,
# byte-identical output to MSYS2's 8.1.1 there) and, when installed, GNU objdump (the Havok tables of the character
# conversion; mingw-w64-ucrt-x86_64-binutils). Without objdump the import still runs and skips that step.
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
mkdir -p "$stage/u3mp/bin"
cp "$ffmpeg_bin/ffmpeg.exe" "$stage/u3mp/bin/"
while read -r dll; do
    cp "$dll" "$stage/u3mp/bin/"
done < <(ldd "$ffmpeg_bin/ffmpeg.exe" | awk "$from_msys_or_ffmpeg {print \$3}" | sort -u)
if [[ -f $msys/ucrt64/bin/objdump.exe ]]; then
    copy_program "$msys/ucrt64/bin/objdump.exe" "$stage/u3mp/bin/binutils"
else
    echo "Note: no GNU objdump in $msys/ucrt64/bin (pacman -S mingw-w64-ucrt-x86_64-binutils): the" \
         "multiplayer import skips the Havok tables" >&2
fi

# ------------------------------------------------------------------------------------------ the launcher

# PkgTool (LibOrbisPkg, LGPL-3.0, unchanged): unpacks the player's own game and update PKGs (gameupdate.cpp).
mkdir -p "$stage/tools/pkgtool"
cp tools/pkgtool/PkgTool.exe tools/pkgtool/LibOrbisPkg.dll "$stage/tools/pkgtool/"
# Every launcher module and data file.
cp launcher/*.py launcher/*.pyw "$stage/launcher/"
# the launcher's own native code (launcher/native: Add to Steam), built here with CLANG64
CLANG64="$msys/clang64" bash launcher/native/build.sh
mkdir -p "$stage/launcher/native"
cp launcher/native/uncharted_native.dll "$stage/launcher/native/"
cp launcher/native/game_starter.exe "$stage/launcher/native/"   # PLAY's game starter (game_starter.cpp)
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
    # Only what the window loads: the QML modules its files import (QtCore, QtQuick, .Controls with the Basic style,
    # .Effects, .Layouts, and what those build on), the Windows platform plugin, the widget style its file dialogs
    # use and the .ico reader (its images are PNG, which Qt reads itself). windeployqt adds more than that.
    qt=$stage/launcher-qt
    for module in QtQml/XmlListModel QtQuick/Dialogs QtQuick/LocalStorage QtQuick/NativeStyle QtQuick/Particles \
        QtQuick/Shapes QtQuick/VectorImage QtQuick/tooling; do
        rm -rf "$qt/qml/$module"
    done
    for plugins in generic tls networkinformation; do
        rm -rf "${qt:?}/$plugins"
    done
    find "$qt/imageformats" -name '*.dll' ! -name qico.dll -delete
    # its Qt files are its own folder's, never an installed Qt's
    printf '[Paths]\r\nPrefix = .\r\nPlugins = .\r\nQmlImports = qml\r\n' > "$stage/launcher-qt/qt.conf"
    # the CLANG64 DLLs Qt and its plugins load (windeployqt copies Qt's own only)
    find "$stage/launcher-qt" \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 -n 20 ldd 2>/dev/null \
        | awk '/clang64/ {print $3}' | sort -u | while read -r dll; do
        [[ -f $stage/launcher-qt/$(basename -- "$dll") ]] || cp "$dll" "$stage/launcher-qt/"
    done
    # Then only the DLLs something left loads, followed from the program and its plugins through their imports
    # (dxcompiler.dll, for example, is Direct3D 12's shader compiler; Qt Quick draws with Direct3D 11).
    declare -A present=() wanted=()
    for dll in "$qt"/*.dll; do
        name=${dll##*/}
        present[${name,,}]=$dll
    done
    queue=("$qt/UnchartedLauncher.exe")
    while IFS= read -r -d '' plugin; do queue+=("$plugin"); done < <(find "$qt" -mindepth 2 -name '*.dll' -print0)
    while (( ${#queue[@]} )); do
        file=${queue[0]}
        queue=("${queue[@]:1}")
        while read -r name; do
            key=${name,,}
            [[ -n ${present[$key]:-} && -z ${wanted[$key]:-} ]] || continue
            wanted[$key]=1
            queue+=("${present[$key]}")
        done < <(llvm-objdump -p "$file" | awk '/DLL Name:/ {print $3}')
    done
    for key in "${!present[@]}"; do
        [[ -n ${wanted[$key]:-} ]] || rm -f -- "${present[$key]}"
    done
    for dll in "$stage"/launcher-qt/*.dll; do
        qt_dlls+=("$(cygpath -m "$msys/clang64/bin/$(basename -- "$dll")")")
    done
    echo "launcher window: launcher-qt ($(du -sh "$stage/launcher-qt" | cut -f1))"
else
    echo "Note: no Qt 6 in $msys/clang64 (pacman -S mingw-w64-clang-x86_64-qt6-declarative): the package keeps" \
         "the launcher's own window" >&2
fi
cp launcher/assets/uncharted.ico launcher/assets/icon.png "$stage/launcher/assets/"

# The starter, Uncharted Launcher.exe (tools/windows_starter), with its icon and version resource.
windres tools/windows_starter/starter.rc -O coff -o dist/stage/starter.res.o
clang -O2 -municode -mwindows tools/windows_starter/starter.c dist/stage/starter.res.o \
    -o "$stage/Uncharted Launcher.exe"

# ------------------------------------------------------------------------------------------ texts and defaults

cp tools/VERSION "$stage/VERSION"   # what the launcher's update check compares with the releases
cp tools/README-Windows.txt "$stage/README.txt"
cp CHANGELOG.md "$stage/CHANGELOG.md"
cp gow3_pc/LICENSE "$stage/LICENSE.txt"
# runtime optimizations Uncharted can't take (read through GOW3_TOGGLE_FILE)
cp tools/defaults/disabled-optimizations.txt "$stage/analysis/uncharted-user/"

# ------------------------------------------------------------------------------------------ Python

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
    # DLLs without CPython's test modules, SQLite and the ssl module (nothing uses them; hashlib keeps OpenSSL's
    # libcrypto: its SHA-256 is ~6x faster than the built-in one on the imports' game files)
    for f in DLLs tcl; do
        [[ -d $home/$f ]] && tar -C "$home" --exclude=__pycache__ --exclude='_test*.pyd' --exclude='xxlimited*.pyd' \
            --exclude='_ctypes_test*.pyd' --exclude='_sqlite3*.pyd' --exclude=sqlite3.dll --exclude='_ssl*.pyd' \
            --exclude='libssl-*.dll' -cf - "$f" | tar -C "$dest" -xf -
    done
    tar -C "$home" --exclude=site-packages --exclude=test --exclude=tests --exclude=idlelib --exclude=ensurepip \
        --exclude=turtledemo --exclude=__pycache__ --exclude=lib2to3 --exclude=venv --exclude=pydoc_data \
        --exclude=sqlite3 --exclude='*.pyc' -cf - Lib | tar -C "$dest" -xf -
    [[ -d $home/Lib/site-packages/PIL ]] || { echo "Pillow is not installed in $home (pip install pillow)" >&2; exit 1; }
    # Pillow without the readers and tools the launcher doesn't use (AVIF, WebP, colour management, ImageMath,
    # morphology); it uses Image, ImageDraw, ImageFont (FreeType) and ImageTk
    tar -C "$home/Lib/site-packages" --exclude=__pycache__ --exclude='_avif*.pyd' --exclude='_webp*.pyd' \
        --exclude='_imagingcms*.pyd' --exclude='_imagingmath*.pyd' --exclude='_imagingmorph*.pyd' \
        -cf - PIL | tar -C "$dest/Lib/site-packages" -xf -
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

# ------------------------------------------------------------------------------------------ licenses

# The runtime's bundled sources: each third_party folder's license files.
for dir in gow3_pc/gpu/third_party/* gow3_pc/third_party/*; do
    for file in "$dir"/LICENSE* "$dir"/COPYING* "$dir"/COPYRIGHT*; do
        if [[ -f $file ]]; then
            mkdir -p "$licenses/${dir##*/}"
            cp "$file" "$licenses/${dir##*/}/"
        fi
    done
done
# the GCN register headers carry theirs in the file head
head -n 24 gow3_pc/gpu/third_party/gcn/include/gcn/si_ci_vi_merged_offset.h > "$licenses/gcn-headers.txt"
# Opus (BSD), linked statically into the runtime for Uncharted 2 multiplayer voice chat.
if [[ -d $msys/clang64/share/licenses/opus ]]; then
    mkdir -p "$licenses/opus"
    cp "$msys"/clang64/share/licenses/opus/* "$licenses/opus/"
fi
# BearSSL (MIT): the TLS code inside u3_online.dll and the multiplayer helper
mkdir -p "$licenses/bearssl"
cp tools/licenses/bearssl/LICENSE.txt tools/licenses/bearssl/SOURCE.txt "$licenses/bearssl/"
# FFmpeg (LGPL 2.1 or later), built by tools/build_ffmpeg.sh from the release's source, unchanged
mkdir -p "$licenses/ffmpeg"
cp "$ffmpeg_bin/../COPYING.LGPLv2.1" "$licenses/ffmpeg/"
{
    echo "FFmpeg 8.1.1 (https://ffmpeg.org/releases/ffmpeg-8.1.1.tar.xz), LGPL 2.1 or later, unchanged; built by"
    echo "tools/build_ffmpeg.sh in the port's source with only the decoders, formats and filters the port uses."
} > "$licenses/ffmpeg/SOURCE.txt"
mkdir -p "$licenses/liborbispkg"
cp tools/pkgtool/LICENSE.txt "$licenses/liborbispkg/LICENSE.txt"
echo "PkgTool and LibOrbisPkg 0.2.231 by Maxton, unchanged; source: https://github.com/maxton/LibOrbisPkg" \
    > "$licenses/liborbispkg/SOURCE.txt"
# Pillow's and numpy's (Python's own is python\LICENSE.txt).
for file in "$python_home"/Lib/site-packages/pillow-*.dist-info/licenses/* \
    "$python_home"/Lib/site-packages/pillow-*.dist-info/LICENSE*; do
    if [[ -f $file ]]; then
        mkdir -p "$licenses/pillow"
        cp "$file" "$licenses/pillow/"
    fi
done
for file in "$python_home"/Lib/site-packages/numpy-*.dist-info/licenses/* \
    "$python_home"/Lib/site-packages/numpy-*.dist-info/LICENSE*; do
    [[ -f $file && -d $stage/python/Lib/site-packages/numpy ]] && { mkdir -p "$licenses/numpy"; cp "$file" "$licenses/numpy/"; }
done

# Every MSYS2 package with a file in the package: its license files under licenses\msys2 and, in THIRD-PARTY.txt,
# its version, license and source (FFmpeg and some others have no license file). The runtime's DLLs, the Qt
# window's, the multiplayer import's programs and their DLLs; pacman is asked three times in all (once per file
# took minutes on GitHub's runner).
for dll in "$stage"/gow3_pc/out/*.dll; do
    msys_file clang64 "$dll"
done
msys_files+=("${qt_dlls[@]}")
for file in "${u3mp_bin[@]}"; do
    msys_files+=("$(cygpath -m "$file")")
done
for dll in "$stage"/u3mp/bin/*.dll; do
    [[ -f $dll ]] && msys_file clang64 "$dll"
done
for dll in "$stage"/u3mp/bin/binutils/*.dll; do
    [[ -f $dll ]] && msys_file ucrt64 "$dll"
done
pacman=$(command -v pacman || echo "$msys/usr/bin/pacman")
mapfile -t packages < <({ "$pacman" -Qqo "${msys_files[@]}" 2>/dev/null || true; } | sort -u)
while read -r file; do
    name=${file#*/share/licenses/}
    mkdir -p "$licenses/msys2/$(dirname -- "$name")"
    cp "$msys$file" "$licenses/msys2/$name"
done < <({ "$pacman" -Qlq "${packages[@]}" | grep '/share/licenses/.*[^/]$' || true; })
{
    echo "Third-party packages whose DLLs ship in gow3_pc\\out (MSYS2 CLANG64 builds)."
    echo "Their sources: https://repo.msys2.org/mingw/sources/ and https://packages.msys2.org/"
    "$pacman" -Qi "${packages[@]}" | awk '/^Name /{print ""} /^(Name|Version|Licenses|URL) /{print}'
} > "$licenses/THIRD-PARTY.txt"

# ------------------------------------------------------------------------------------------ DLAA and FSR 4

# Shipped when tools/fetch_aa_files.sh fetched them; without them the in-game menu does not offer the two.
if [[ -f gow3_pc/out/gow3_dlss.dll && -f gow3_pc/out/nvngx_dlss.dll ]]; then
    cp gow3_pc/out/gow3_dlss.dll gow3_pc/out/nvngx_dlss.dll "$stage/gow3_pc/out/"
    dlss_license=${NVIDIA_DLSS_LICENSE:-}
    if [[ -z $dlss_license ]]; then
        for file in downloads/dlss-sdk-*/LICENSE.txt; do
            if [[ -f $file ]]; then
                dlss_license=$file
                break
            fi
        done
    fi
    if [[ ! -f $dlss_license ]]; then
        echo 'Need the DLSS license (NVIDIA_DLSS_LICENSE).' >&2
        exit 1
    fi
    mkdir -p "$licenses/nvidia-dlss" "$licenses/gow3-dlss-bridge"
    cp "$dlss_license" "$licenses/nvidia-dlss/LICENSE.txt"
    cp gow3_pc/gpu/dlss_bridge/LICENSE.txt "$licenses/gow3-dlss-bridge/LICENSE.txt"
fi
fsr4=gow3_pc/fsr4_shaders
if [[ -f $fsr4/fsr4_model_v07_i8_native_1080_pre.spv ]]; then
    mkdir -p "$stage/$fsr4" "$licenses/fsr4"
    cp "$fsr4"/LICENSE-FSR4-v07.txt "$fsr4"/*.spv "$fsr4"/*.bin "$fsr4"/*.json "$stage/$fsr4/"
    cp "$fsr4"/LICENSE-FSR4-v07.txt "$licenses/fsr4/"
fi

# ------------------------------------------------------------------------------------------ check and zip

find "$stage" -name __pycache__ -prune -exec rm -rf {} +
# Nothing from a game or a player may ship: game executables and data, saves, caches, logs, settings files.
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
    printf 'Package holds files that must not ship:\n%s\n' "$forbidden" >&2
    exit 1
fi

rm -rf dist/Uncharted-windows dist/Uncharted-windows.zip
"$python_home/python.exe" -m zipfile -c dist/Uncharted-windows.zip "$stage"
mv "$stage" dist/Uncharted-windows
du -sh dist/Uncharted-windows dist/Uncharted-windows.zip
