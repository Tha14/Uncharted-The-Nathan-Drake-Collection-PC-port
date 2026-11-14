#!/usr/bin/env bash
# tools/fsr4_optimize.sh: builds fixed/faster variants of FSR 4 v07 passes into
# fsr4_shaders/opt/ (vk_fsr4.cpp prefers files there; GOW3_FSR4_OPT=0 uses the originals):
#   *_post.spv    stores through shared memory, bit-exact, ~3.5x faster (fsr4_rewrite post-lds);
#   *_pass11.spv  no out-of-bounds writes (a data race at the 1080 tier; fsr4_rewrite pass11-guard).
# Each pass is decompiled (spirv-cross), rewritten (tools/fsr4_rewrite.cpp, compiled here) and
# compiled again (glslang). Needs spirv-cross, glslangValidator (shell.nix) and a C++ compiler
# (CXX, else clang++). tools/fsr4_verify.sh checks the results.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
src=${GOW3_FSR4_DIR:-fsr4_shaders}
dest=$src/opt
mkdir -p "$dest"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
rewrite=tools/fsr4_rewrite.cpp
count=0
build() { # build <original.spv> <fsr4_rewrite command> <entry point>
    local spv=$1 command=$2 entry=$3 out
    out=$dest/$(basename "$spv")
    # Rebuilt when the original or the rewrite changed.
    if [[ -s $out && $out -nt $spv && $out -nt $rewrite ]]; then
        return
    fi
    # The rewrite tool is compiled for the first pass that needs it.
    if ((count == 0)); then
        "${CXX:-clang++}" -std=c++20 -O2 "$rewrite" -o "$tmp/fsr4_rewrite"
    fi
    spirv-cross "$spv" --vulkan-semantics --entry "$entry" --output "$tmp/in.comp"
    # Windows builds of spirv-cross write CRLF; the rewrites match LF lines.
    tr -d '\r' < "$tmp/in.comp" | "$tmp/fsr4_rewrite" "$command" > "$tmp/out.comp"
    glslangValidator -V --target-env vulkan1.3 -S comp -e "$entry" --source-entrypoint main \
        "$tmp/out.comp" -o "$tmp/out.spv" >/dev/null
    mv "$tmp/out.spv" "$out"
    count=$((count + 1))
}
for spv in "$src"/fsr4_model_v07_i8_*_post.spv; do
    build "$spv" post-lds main
done
for spv in "$src"/fsr4_model_v07_i8_*_pass11.spv; do
    build "$spv" pass11-guard fsr4_model_v07_i8_pass11
done
echo "FSR 4 optimized passes: $count built in $dest"
