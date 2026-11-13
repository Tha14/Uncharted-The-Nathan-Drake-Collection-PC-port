#!/usr/bin/env bash
# package_windows.sh - assembles the Windows release: dist/Uncharted-windows/ and its zip, dist/Uncharted-windows.zip.
# Players need 64-bit Windows 10 or 11 and a Vulkan graphics card; no Python or MSYS2 of their own.
#
# What the package holds:
#   Uncharted Launcher.exe   what players start (tools/windows_starter/starter.cpp)
#   launcher-qt\             the launcher's window (Qt 6), launcher\native\ its core and the game starter
#   gow3_pc\                 the game runtime with its DLLs and its scripts
#   u2mp\, u3mp\             the multiplayer importers (Python scripts, tables, the helper builds, ffmpeg, objdump)
#   python\                  the Python the importers run with (Pillow, numpy when installed)
#   tools\                   the update installer and the base game's file list
#   licenses\                the licenses of everything bundled
# Never any game file: each player points the launcher at their own copy.
#
# Run from Git Bash or an MSYS2 CLANG64 shell after gow3_pc/build.sh and tools/build_ffmpeg.sh. Settings:
#   PYTHON / PYTHON_HOME    the Windows Python (3.10 or newer, with Pillow) to bundle; `python` on PATH by default
#   MSYS2_ROOT              the MSYS2 folder (/c/msys64 by default)
#   NVIDIA_DLSS_LICENSE     the DLSS SDK's license file, when the DLSS files were fetched (tools/fetch_aa_files.sh)
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

# ---------------------------------------------------------------------------------------------------------- setup

# the Windows Python is found first: MSYS2's own python comes first on PATH after the next lines
py=${PYTHON:-python}
py_home=${PYTHON_HOME:-$("$py" -c 'import os, sys; print(os.path.dirname(sys.executable))')}
msys=${MSYS2_ROOT:-/c/msys64}
msys=${msys%/}   # MSYS2 at "/" (GitHub's runner) would otherwise make "//usr", a network path
[[ -d $msys/clang64/bin ]] && export PATH="$msys/clang64/bin:$PATH"
small_ffmpeg=downloads/ffmpeg-8.1.1-min/bin   # tools/build_ffmpeg.sh: the decoders the port uses, nothing else

need() {   # need <file> <how to make it>
    [[ -f $1 ]] || { echo "Missing $1: $2" >&2; exit 1; }
}
need gow3_pc/out/gow3-probe.exe 'build the runtime first (gow3_pc/build.sh)'
need "$small_ffmpeg/ffmpeg.exe" 'build the small FFmpeg first (tools/build_ffmpeg.sh)'
export PATH="$PWD/$small_ffmpeg:$PATH"   # its DLLs carry FFmpeg's usual names: these are found before MSYS2's

# Only the latest helper build may be committed (AGENTS.md): a new build replaces the old folder, it is never copied
committed_builds=$(git ls-files u3mp/patcher/build | cut -d/ -f4 | sort -u)
if (( $(grep -c . <<< "$committed_builds") > 1 )); then
    echo "More than one helper build committed under u3mp/patcher/build:" $committed_builds >&2
    echo "Keep only the latest: git rm -r the older folders (they stay in git history)." >&2
    exit 1
fi

out=dist/stage/Uncharted-windows
lic=$out/licenses
rm -rf -- dist/stage
mkdir -p "$out/gow3_pc/out" "$out/gow3_pc/scripts" "$out/tools" "$out/launcher/assets" "$out/launcher/native" \
    "$out/analysis/uncharted-user" "$lic"

# the DLLs a program loads from MSYS2 or the small FFmpeg, as ldd lists them
shipped_dlls() {
    ldd "$1" | awk '/clang64|ffmpeg-8\.1\.1-min\// {print $3}' | sort -u
}

# MSYS2 files that ship: licenses\ covers their packages (msys2_licenses at the end)
from_msys=()
note_msys_file() {   # note_msys_file <clang64 | ucrt64> <shipped file>; the small FFmpeg's are our own build
    [[ -f $small_ffmpeg/$(basename -- "$2") ]] && return 0
    from_msys+=("$(cygpath -m "$msys/$1/bin/$(basename -- "$2")")")
}

drop_caches() {
    find "$1" -name __pycache__ -prune -exec rm -rf {} +
}

# ---------------------------------------------------------------------------------------------------------- parts

runtime() {   # the game runtime, without debug information, and what it loads
    llvm-strip --strip-debug -o "$out/gow3_pc/out/gow3-probe.exe" gow3_pc/out/gow3-probe.exe
    local dll
    while read -r dll; do cp "$dll" "$out/gow3_pc/out/"; done < <(shipped_dlls gow3_pc/out/gow3-probe.exe)
    cp gow3_pc/scripts/*.py "$out/gow3_pc/scripts/"
    for dll in "$out"/gow3_pc/out/*.dll; do note_msys_file clang64 "$dll"; done
}

u2_import() {   # Uncharted 2's importer: scripts and its list of what it makes (it reads the player's own game)
    mkdir -p "$out/u2mp/docs"
    cp -r u2mp/patcher "$out/u2mp/patcher"
    cp u2mp/README.md "$out/u2mp/"
    cp u2mp/docs/import-list.tsv "$out/u2mp/docs/"
    drop_caches "$out/u2mp"
}

u3_import() {   # Uncharted 3's importer, its recipe and tools, and the newest multiplayer helper build
    mkdir -p "$out/u3mp/bin"
    cp -r u3mp/patcher "$out/u3mp/patcher"
    rm -rf "$out/u3mp/patcher/maintainer"   # the maintainers' own scripts
    drop_caches "$out/u3mp/patcher"
    # The package carries the committed build and nothing else. The private files can be ahead of this commit (a newer
    # helper pushed while a release builds): never let such a folder replace the committed one, and stop when the
    # helper for the committed build is missing or is not the one its build.json names.
    local builds=$out/u3mp/patcher/build newest=$committed_builds d want have
    for d in "$builds"/v*/; do [[ $(basename -- "$d") == "$newest" ]] || rm -rf -- "$d"; done
    want=$(grep -o '"profile-helper.bin": *"[0-9a-f]*"' "$builds/$newest/build.json" | grep -o '[0-9a-f]\{64\}')
    have=$(sha256sum "$builds/$newest/profile-helper.bin" 2>/dev/null | cut -c1-64)
    if [[ -z $want || $want != "$have" ]]; then
        echo "The helper of build $newest is missing or is not the one its build.json names (private files out of step?)" >&2
        exit 1
    fi
    echo "multiplayer helper build in the package: $newest"
    cp gow3_pc/LICENSE "$out/u3mp/patcher/LICENSE.txt"
    # the sound-bank encoder compiled (much faster than vagenc.py), kept only when it encodes exactly like the script
    local enc=$out/u3mp/patcher/tools/fixes/sound-banks/native
    if clang -O2 -ffp-contract=off -fno-fast-math -shared -o "$enc/vagenc.dll" "$enc/vagenc.c" &&
            "$py_home/python.exe" "$enc/check_native.py"; then
        rm -f "$enc"/*.lib "$enc"/*.exp
    else
        rm -f "$enc/vagenc.dll"
        echo "Note: no compiled sound-bank encoder; the import uses vagenc.py" >&2
    fi
    # u3fix.exe: the recipe steps written in C++ (the list is in tools/fixes/dctools/u3fix.cpp); the steps run it from
    # u3mp/bin, which the importer puts on PATH. Static, no DLL of ours. Without it those steps fail: stop here.
    local fx=u3mp/patcher/tools/fixes
    clang++ -std=c++20 -O2 -municode -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -I"$fx/dctools" -Ilauncher/native \
        "$fx/dctools/u3fix.cpp" "$fx/dctools/dc.cpp" "$fx/dctools/eula_box.cpp" "$fx/no-lab/remove_lab.cpp" \
        "$fx/no-rpcn-menu/remove_rpcn_page.cpp" "$fx/netgame-order/netgame_order.cpp" "$fx/wall-emblem-test/emblem_cap3.cpp" \
        "$fx/orange-wall/apply_orange.cpp" "$fx/excess-wall/apply_excess.cpp" "$fx/enemy-stall/apply_ss_option_defaults.cpp" \
        "$fx/coop-chars/apply_coop_desc_fix.cpp" "$fx/coop-tab/fix_vram_gap.cpp" "$fx/airport-igc3/airport_igc3.cpp" \
        "$fx/dctools/psarc.cpp" "$fx/text1-merge/merge_text1.cpp" "$fx/coop-audit/apply_option_defaults_2.cpp" \
        "$fx/registry/run_mkregistry.cpp" "$fx/pak23-registry/apply_registry.cpp" "$fx/hat-chloe/apply_hair_key.cpp" \
        "$fx/pak23-levels/levels.cpp" "$fx/actor33/build_actors.cpp" "$fx/dctools/proc.cpp" "$fx/dctools/ndpak.cpp" \
        "$fx/dctools/ps4writer.cpp" "$fx/dctools/ps4ingame.cpp" "$fx/dctools/vischeat.cpp" "$fx/dctools/windoff.cpp" \
        "$fx/dctools/texcodec.cpp" "$fx/dctools/ps3bg.cpp" "$fx/dctools/stageplan.cpp" "$fx/dctools/ps4stage.cpp" \
        "$fx/dctools/collision.cpp" "$fx/dctools/visinfo.cpp" "$fx/dctools/ps3fp.cpp" "$fx/dctools/anim.cpp" \
        "$fx/dctools/skel.cpp" "$fx/dctools/hkconvert.cpp" "$fx/dctools/fgcoll.cpp" "$fx/dctools/ps4actor.cpp" "$fx/dctools/ps4mats.cpp" \
        "$fx/dctools/dcconv.cpp" "$fx/dcconv/convert_dc1.cpp" "$fx/dcconv/apply_recipes.cpp" "$fx/sound-banks/convert_banks.cpp" \
        "$fx/part-emitters/build_part_modules.cpp" "$fx/actor33/actors_hk.cpp" "$fx/coop-vo/apply_vo.cpp" \
        "$fx/coop-voice2/apply_coop_vox.cpp" "$fx/dctools/net_edits.cpp" \
        "$fx/coop-audit/apply_shade_npc_paks.cpp" "$fx/inline/facility_collapse.cpp" \
        -o "$out/u3mp/bin/u3fix.exe"
    # ffmpeg (the EULA pages, sound banks with MP3 samples) and, when installed, GNU objdump (the characters' Havok
    # tables; mingw-w64-ucrt-x86_64-binutils: without it the import skips that step)
    local dll
    cp "$small_ffmpeg/ffmpeg.exe" "$out/u3mp/bin/"
    while read -r dll; do cp "$dll" "$out/u3mp/bin/"; done < <(shipped_dlls "$small_ffmpeg/ffmpeg.exe")
    for dll in "$out"/u3mp/bin/*.dll; do note_msys_file clang64 "$dll"; done
    local objdump=$msys/ucrt64/bin/objdump.exe
    if [[ -f $objdump ]]; then
        mkdir -p "$out/u3mp/bin/binutils"
        cp "$objdump" "$out/u3mp/bin/binutils/"
        from_msys+=("$(cygpath -m "$objdump")")
        ldd "$objdump" | awk -v tree="$(cygpath -u "$msys/ucrt64/bin")" 'index($3, tree) == 1 {print $3}' | sort -u |
            while read -r dll; do cp "$dll" "$out/u3mp/bin/binutils/"; done
        for dll in "$out"/u3mp/bin/binutils/*.dll; do [[ -f $dll ]] && note_msys_file ucrt64 "$dll"; done
    else
        echo "Note: no GNU objdump in $msys/ucrt64/bin: the multiplayer import will skip the Havok tables" >&2
    fi
}

launcher_core() {   # the launcher's native code: its core, the game starter, the update installer
    CLANG64="$msys/clang64" bash launcher/native/build.sh
    cp launcher/native/uncharted_native.dll launcher/native/game_starter.exe "$out/launcher/native/"
    cp launcher/native/apply_update.exe "$out/tools/"
    # files older launchers look for before they install an update; each one hands over to the native program
    cp launcher/uncharted_launcher.pyw launcher/updater.py "$out/launcher/"
    cp tools/apply_update.py tools/probe_uncharted.py tools/game_files_102.json.gz "$out/tools/"
    cp launcher/assets/uncharted.ico launcher/assets/icon.png "$out/launcher/assets/"
}

starter() {   # Uncharted Launcher.exe with the launcher's icon
    windres tools/windows_starter/starter.rc -O coff -o dist/stage/starter.res.o
    clang++ -O2 -std=c++17 -municode -mwindows -static -s tools/windows_starter/starter.cpp dist/stage/starter.res.o \
        -o "$out/Uncharted Launcher.exe"
}

# the Qt window with what it loads and nothing else
qt_window() {
    need "$msys/clang64/bin/windeployqt6.exe" 'install Qt 6 (pacman -S mingw-w64-clang-x86_64-qt6-declarative)'
    local build=dist/stage/qt-build win=$out/launcher-qt
    cmake -S launcher-qt -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ > /dev/null
    ninja -C "$build" UnchartedLauncher > "$build.log" 2>&1 || { tail -40 "$build.log" >&2; exit 1; }
    mkdir -p "$win"
    llvm-strip -o "$win/UnchartedLauncher.exe" "$build/UnchartedLauncher.exe"
    PATH="$msys/clang64/share/qt6/bin:$PATH" windeployqt6 --qtpaths "$msys/clang64/bin/qtpaths6.exe" --release \
        --qmldir launcher-qt/qml --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime \
        "$win/UnchartedLauncher.exe" > "$build-deploy.log" 2>&1 || { tail -40 "$build-deploy.log" >&2; exit 1; }
    # windeployqt misses QML plugin DLLs in CLANG64's layout: each deployed QML folder gets its source's DLLs
    local qml_from=$msys/clang64/share/qt6/qml dir plugin
    while read -r dir; do
        for plugin in "$qml_from/$dir"/*.dll; do
            [[ -f $plugin && ! -f $win/qml/$dir/$(basename -- "$plugin") ]] && cp "$plugin" "$win/qml/$dir/"
        done
    done < <(cd "$win/qml" && find . -type d)
    # the window is built for the Basic style, its QML imports (QtCore, QtQuick, Controls, Effects, Layouts), the
    # Windows platform plugin, the widget style of its file dialogs and the .ico reader; the rest goes
    local style module
    for style in FluentWinUI3 Fusion Imagine Material Universal Windows; do
        rm -rf "$win/qml/QtQuick/Controls/$style"
        rm -f "$win"/Qt6QuickControls2"$style"*.dll
    done
    for module in QtQml/XmlListModel QtQuick/Dialogs QtQuick/LocalStorage QtQuick/NativeStyle QtQuick/Particles \
            QtQuick/Shapes QtQuick/VectorImage QtQuick/tooling; do
        rm -rf "$win/qml/$module"
    done
    rm -rf "$win/qmltooling" "$win/generic" "$win/tls" "$win/networkinformation"
    find "$win/imageformats" -name '*.dll' ! -name qico.dll -delete
    printf '[Paths]\r\nPrefix = .\r\nPlugins = .\r\nQmlImports = qml\r\n' > "$win/qt.conf"   # never an installed Qt
    # the CLANG64 DLLs Qt and its plugins load (windeployqt only brings Qt's own)
    local dll
    find "$win" \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 -n 20 ldd 2>/dev/null |
        awk '/clang64/ {print $3}' | sort -u | while read -r dll; do
        [[ -f $win/$(basename -- "$dll") ]] || cp "$dll" "$win/"
    done
    # keep only the top-level DLLs reachable through the imports of the program and its plugins
    declare -A have=() reached=()
    for dll in "$win"/*.dll; do
        local base=${dll##*/}
        have[${base,,}]=$dll
    done
    local todo=("$win/UnchartedLauncher.exe") file name
    while IFS= read -r -d '' file; do todo+=("$file"); done < <(find "$win" -mindepth 2 -name '*.dll' -print0)
    while (( ${#todo[@]} )); do
        file=${todo[0]}
        todo=("${todo[@]:1}")
        while read -r name; do
            name=${name,,}
            if [[ -n ${have[$name]:-} && -z ${reached[$name]:-} ]]; then
                reached[$name]=1
                todo+=("${have[$name]}")
            fi
        done < <(llvm-objdump -p "$file" | awk '/DLL Name:/ {print $3}')
    done
    for name in "${!have[@]}"; do
        [[ -n ${reached[$name]:-} ]] || rm -f -- "${have[$name]}"
    done
    for dll in "$win"/*.dll; do from_msys+=("$(cygpath -m "$msys/clang64/bin/$(basename -- "$dll")")"); done
    echo "launcher window: $(du -sh "$win" | cut -f1)"
}

texts() {
    cp tools/VERSION "$out/VERSION"   # the update check compares this with the releases
    cp tools/README-Windows.txt "$out/README.txt"
    cp CHANGELOG.md "$out/"
    cp gow3_pc/LICENSE "$out/LICENSE.txt"
    cp tools/defaults/disabled-optimizations.txt "$out/analysis/uncharted-user/"   # what Uncharted can't take
}

# Python for the importers: the interpreter, its standard library without tests, tools and the unused modules (Tk,
# SQLite, ssl; OpenSSL's libcrypto stays for hashlib: much faster SHA-256 on the game files), Pillow without what the
# importers don't use, numpy when installed
python_for_imports() {
    local to=$out/python f
    mkdir -p "$to/Lib/site-packages"
    for f in "$py_home"/python.exe "$py_home"/pythonw.exe "$py_home"/python3*.dll "$py_home"/vcruntime140*.dll \
            "$py_home"/LICENSE.txt; do
        [[ -f $f ]] && cp -p "$f" "$to/"
    done
    tar -C "$py_home" --exclude=__pycache__ --exclude='_test*.pyd' --exclude='xxlimited*.pyd' --exclude='_ctypes_test*.pyd' \
        --exclude='_sqlite3*.pyd' --exclude=sqlite3.dll --exclude='_ssl*.pyd' --exclude='libssl-*.dll' \
        --exclude='_tkinter*.pyd' --exclude='tcl*.dll' --exclude='tk*.dll' -cf - DLLs |
        tar -C "$to" -xf -
    tar -C "$py_home" --exclude=site-packages --exclude=test --exclude=tests --exclude=idlelib --exclude=ensurepip \
        --exclude=turtledemo --exclude=turtle.py --exclude=tkinter --exclude=__pycache__ --exclude=lib2to3 --exclude=venv \
        --exclude=pydoc_data --exclude=sqlite3 --exclude='*.pyc' -cf - Lib | tar -C "$to" -xf -
    [[ -d $py_home/Lib/site-packages/PIL ]] || { echo "Pillow is missing in $py_home (pip install pillow)" >&2; exit 1; }
    tar -C "$py_home/Lib/site-packages" --exclude=__pycache__ --exclude='_avif*.pyd' --exclude='_webp*.pyd' \
        --exclude='_imagingcms*.pyd' --exclude='_imagingmath*.pyd' --exclude='_imagingmorph*.pyd' \
        --exclude='_imagingtk*.pyd' --exclude=ImageTk.py -cf - PIL | tar -C "$to/Lib/site-packages" -xf -
    if [[ -d $py_home/Lib/site-packages/numpy ]]; then
        for f in numpy numpy.libs; do
            [[ -d $py_home/Lib/site-packages/$f ]] &&
                tar -C "$py_home/Lib/site-packages" --exclude=__pycache__ --exclude=tests -cf - "$f" | tar -C "$to/Lib/site-packages" -xf -
        done
    else
        echo "Note: no numpy in this Python: the Uncharted 3 import will skip the steps that need it"
    fi
    echo "Python from $py_home"
}

aa_files() {   # DLAA, when tools/fetch_aa_files.sh fetched it (else the in-game menu leaves it out); FSR 4 models: never shipped
    if [[ -f gow3_pc/out/gow3_dlss.dll && -f gow3_pc/out/nvngx_dlss.dll ]]; then
        cp gow3_pc/out/gow3_dlss.dll gow3_pc/out/nvngx_dlss.dll "$out/gow3_pc/out/"
        local license=${NVIDIA_DLSS_LICENSE:-} f
        if [[ -z $license ]]; then
            for f in downloads/dlss-sdk-*/LICENSE.txt; do [[ -f $f ]] && { license=$f; break; }; done
        fi
        [[ -f $license ]] || { echo 'The DLSS files ship only with their license: set NVIDIA_DLSS_LICENSE.' >&2; exit 1; }
        mkdir -p "$lic/nvidia-dlss" "$lic/gow3-dlss-bridge"
        cp "$license" "$lic/nvidia-dlss/LICENSE.txt"
        cp gow3_pc/gpu/dlss_bridge/LICENSE.txt "$lic/gow3-dlss-bridge/"
    fi
}

licenses() {
    local d f
    for d in gow3_pc/gpu/third_party/* gow3_pc/third_party/*; do   # the runtime's bundled sources
        for f in "$d"/LICENSE* "$d"/COPYING* "$d"/COPYRIGHT*; do
            [[ -f $f ]] && { mkdir -p "$lic/${d##*/}"; cp "$f" "$lic/${d##*/}/"; }
        done
    done
    head -n 24 gow3_pc/gpu/third_party/gcn/include/gcn/si_ci_vi_merged_offset.h > "$lic/gcn-headers.txt"   # in its head
    if [[ -d $msys/clang64/share/licenses/opus ]]; then   # Opus, in the runtime for Uncharted 2's voice chat
        mkdir -p "$lic/opus"
        cp "$msys"/clang64/share/licenses/opus/* "$lic/opus/"
    fi
    mkdir -p "$lic/bearssl" "$lic/ffmpeg"
    cp tools/licenses/bearssl/LICENSE.txt tools/licenses/bearssl/SOURCE.txt "$lic/bearssl/"   # TLS of the online parts
    cp "$small_ffmpeg/../COPYING.LGPLv2.1" "$lic/ffmpeg/"
    printf '%s\n' "FFmpeg 8.1.1 (https://ffmpeg.org/releases/ffmpeg-8.1.1.tar.xz), LGPL 2.1 or later, unchanged; built by" \
        "tools/build_ffmpeg.sh in the port's source with only the decoders, formats and filters the port uses." \
        > "$lic/ffmpeg/SOURCE.txt"
    for f in "$py_home"/Lib/site-packages/pillow-*.dist-info/licenses/* "$py_home"/Lib/site-packages/pillow-*.dist-info/LICENSE*; do
        [[ -f $f ]] && { mkdir -p "$lic/pillow"; cp "$f" "$lic/pillow/"; }
    done
    for f in "$py_home"/Lib/site-packages/numpy-*.dist-info/licenses/* "$py_home"/Lib/site-packages/numpy-*.dist-info/LICENSE*; do
        [[ -f $f && -d $out/python/Lib/site-packages/numpy ]] && { mkdir -p "$lic/numpy"; cp "$f" "$lic/numpy/"; }
    done
    msys2_licenses
}

# every MSYS2 package that has a file in the package: its license files and, in THIRD-PARTY.txt, its version, license
# and source; pacman is asked three times in all (once per file takes minutes on GitHub's runner)
msys2_licenses() {
    local pacman packages f
    pacman=$(command -v pacman || echo "$msys/usr/bin/pacman")
    mapfile -t packages < <({ "$pacman" -Qqo "${from_msys[@]}" 2>/dev/null || true; } | sort -u)
    while read -r f; do
        local rel=${f#*/share/licenses/}
        mkdir -p "$lic/msys2/$(dirname -- "$rel")"
        cp "$msys$f" "$lic/msys2/$rel"
    done < <({ "$pacman" -Qlq "${packages[@]}" | grep '/share/licenses/.*[^/]$' || true; })
    {
        echo "Third-party packages whose files ship in this package (MSYS2 builds)."
        echo "Source code for each: https://repo.msys2.org/mingw/sources/ (package pages: https://packages.msys2.org/)"
        "$pacman" -Qi "${packages[@]}" | awk '$1 == "Name" { print "" } $1 ~ /^(Name|Version|Licenses|URL)$/ { print }'
    } > "$lic/THIRD-PARTY.txt"
}

# nothing from a game or a player may be in the package: executables and data, saves, caches, logs, settings files,
# and no folder of converted files (the recipe's own overlay-* folders hold descriptions only)
nothing_from_games() {
    drop_caches "$out"
    local found
    found=$(find "$out" \( -iname eboot.bin -o -iname '*.elf' -o -iname sce_sys -o -iname savedata -o -iname cache \
        -o -iname '*.log' -o -iname 'probe*.ini' -o -iname '*.pkg' -o -iname '*.psarc' -o -iname '*.pak' \
        -o -iname 'boot*.bin' -o -iname '*.dds' -o -iname '*.at9' \) -print)
    found+=$(find "$out" -type d -iname 'overlay-*' -not -path '*/u3mp/patcher/tools/*' -not -path '*/u3mp/patcher/recipe/*' -print)
    found+=$(find "$out/u3mp/patcher" -type d -iname 'overlay-*' -exec find {} -type f \
        \( -iname '*.pak' -o -iname '*.bin' -o -iname '*.psarc' -o -iname '*.dds' \) \; 2>/dev/null)
    if [[ -n $found ]]; then
        printf 'These must not be in the package:\n%s\n' "$found" >&2
        exit 1
    fi
}

# ---------------------------------------------------------------------------------------------------------- build

runtime
u2_import
u3_import
launcher_core
qt_window
starter
texts
python_for_imports
aa_files
licenses
nothing_from_games

rm -rf dist/Uncharted-windows dist/Uncharted-windows.zip
"$py_home/python.exe" -m zipfile -c dist/Uncharted-windows.zip "$out"
mv "$out" dist/Uncharted-windows
du -sh dist/Uncharted-windows dist/Uncharted-windows.zip
