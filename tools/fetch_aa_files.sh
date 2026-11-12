#!/usr/bin/env bash
# tools/fetch_aa_files.sh [dlss] - downloads what DLAA needs (in-game menu -> Image -> Anti-aliasing); it is not kept
# in this repository.
#
#   dlss  NVIDIA's DLSS SDK 310.9.16 (github.com/NVIDIA/DLSS; its LICENSE.txt applies): the headers,
#         nvsdk_ngx_s.lib and nvngx_dlss.dll go to downloads/dlss-sdk-310.9.16. Visual Studio's C++
#         tools (2022 or 2026, any edition or the Build Tools) then build gow3_pc/gpu/dlss_bridge,
#         and gow3_dlss.dll and nvngx_dlss.dll are put in gow3_pc/out next to the runtime (~63 MB).
# The FSR 4 models are no longer fetched or shipped (2026-11-12): they were built by a third party from AMD source
# that AMD published by mistake and withdrew, so their license is disputed. The runtime still uses them when a player
# puts them in gow3_pc/fsr4_shaders; otherwise the menu leaves FSR 4 out.
# A file that is already there is not downloaded again.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
parts=${*:-dlss}

download() {   # download <url> <file>, by way of <file>.part so a broken download never counts
    local url=$1 file=$2
    [[ -s $file ]] && return 0
    mkdir -p -- "$(dirname -- "$file")"
    curl -fsSL --retry 3 -o "$file.part" "$url"
    mv -- "$file.part" "$file"
}

get_dlss() {
    local version=310.9.16
    local sdk=downloads/dlss-sdk-$version
    local from=https://raw.githubusercontent.com/NVIDIA/DLSS/v$version
    local name
    for name in nvsdk_ngx nvsdk_ngx_defs nvsdk_ngx_defs_dlssd nvsdk_ngx_defs_dlssg nvsdk_ngx_defs_vk \
        nvsdk_ngx_helpers nvsdk_ngx_helpers_cuda nvsdk_ngx_helpers_d3d nvsdk_ngx_helpers_dlssd \
        nvsdk_ngx_helpers_dlssd_cuda nvsdk_ngx_helpers_dlssd_d3d nvsdk_ngx_helpers_dlssd_vk \
        nvsdk_ngx_helpers_dlssg nvsdk_ngx_helpers_dlssg_d3d nvsdk_ngx_helpers_dlssg_vk nvsdk_ngx_helpers_vk \
        nvsdk_ngx_loader nvsdk_ngx_params nvsdk_ngx_params_dlssd nvsdk_ngx_params_dlssg \
        nvsdk_ngx_standalone_common nvsdk_ngx_standalone_cuda nvsdk_ngx_vk; do
        download "$from/include/$name.h" "$sdk/include/$name.h"
    done
    for name in LICENSE.txt lib/Windows_x86_64/x64/nvsdk_ngx_s.lib lib/Windows_x86_64/rel/nvngx_dlss.dll; do
        download "$from/$name" "$sdk/$name"
    done

    # The bridge is built with MSVC. It gets the Vulkan headers by themselves, copied from MSYS2's
    # CLANG64 tree (MSVC must not see that tree's C library headers).
    local msys=${MSYS2_ROOT:-/c/msys64}
    msys=${msys%/}   # GitHub's runner has MSYS2 at "/", and "//clang64" would be a network path
    mkdir -p downloads/vulkan-include
    cp -r "$msys/clang64/include/vulkan" "$msys/clang64/include/vk_video" downloads/vulkan-include/

    # CMake: Visual Studio's own when vswhere finds an installation with the C++ tools, else one on
    # PATH. The generator goes by the Visual Studio version (GitHub's windows-latest has had both).
    local cmake generator="Visual Studio 17 2022"
    cmake=$(command -v cmake.exe || true)
    local vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [[ -x $vswhere ]]; then
        local query=(-latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64)
        local install major
        install=$("$vswhere" "${query[@]}" -property installationPath | tr -d '\r')
        major=$("$vswhere" "${query[@]}" -property installationVersion | tr -d '\r')
        major=${major%%.*}
        if [[ $major == 18 ]]; then
            generator="Visual Studio 18 2026"
        fi
        if [[ -n $install ]]; then
            local bundled
            bundled="$(cygpath -u "$install")/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
            [[ -x $bundled ]] && cmake=$bundled
        fi
    fi
    if [[ -z $cmake || ! -x $cmake ]]; then
        echo "No MSVC (Visual Studio Build Tools) found: DLSS bridge not built" >&2
        exit 1
    fi
    local here
    here=$(cygpath -w "$PWD")
    "$cmake" -S "$here\\gow3_pc\\gpu\\dlss_bridge" -B "$here\\gow3_pc\\out\\dlss-bridge" -G "$generator" -A x64 \
        -DDLSS_SDK_ROOT="$here\\downloads\\dlss-sdk-$version" -DVULKAN_INCLUDE="$here\\downloads\\vulkan-include" \
        > /dev/null
    "$cmake" --build "$here\\gow3_pc\\out\\dlss-bridge" --config Release > /dev/null
    cp gow3_pc/out/dlss-bridge/Release/gow3_dlss.dll "$sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll" gow3_pc/out/
    echo "DLSS: gow3_pc/out/gow3_dlss.dll and nvngx_dlss.dll ($version)"
}

[[ $parts == *dlss* ]] && get_dlss
exit 0
