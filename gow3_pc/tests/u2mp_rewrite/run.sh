#!/bin/bash
# run.sh <test> <source.cpp>...: builds and runs test_<test>.cpp. The sources are compiled twice: the old version from
# git (U2MP_REWRITE_BASE, the last commit before the rewrites; its headers too) in namespace u2mp_old, and the current
# one in namespace u2mp. deps_<test>.cpp, when present, is compiled for both (what the sources need from other modules).
# Extra flags: OLD_FLAGS / NEW_FLAGS (e.g. -Dsome_c_symbol=some_c_symbol_old). Needs clang++ (MSYS2 CLANG64).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)          # gow3_pc
repo=$(cd "$root/.." && pwd)
base=${U2MP_REWRITE_BASE:-62fc26b}
t=$1
shift
# flags_<test>.sh, when present, may add to OLD_FLAGS / NEW_FLAGS / LINK_FLAGS (e.g. force-include fake_clock.h)
if [ -f "$here/flags_$t.sh" ]; then . "$here/flags_$t.sh"; fi
out=$root/out/u2mp_rewrite/$t
rm -rf "$out"
mkdir -p "$out/old"
for f in $(git -C "$repo" ls-tree --name-only "$base" gow3_pc/src/u2mp/ | grep -E '\.(h|hpp)$'); do
    git -C "$repo" show "$base:$f" > "$out/old/$(basename "$f")"
done
cxx="clang++ -std=c++20 -O1 -g -w"
objs=()
for s in "$@"; do
    git -C "$repo" show "$base:gow3_pc/src/u2mp/$s" > "$out/old/$s"
    $cxx -c -I"$out/old" -Du2mp=u2mp_old $OLD_FLAGS "$out/old/$s" -o "$out/old_${s%.cpp}.o"
    $cxx -c -I"$root/src/u2mp" $NEW_FLAGS "$root/src/u2mp/$s" -o "$out/new_${s%.cpp}.o"
    objs+=("$out/old_${s%.cpp}.o" "$out/new_${s%.cpp}.o")
done
$cxx -c -I"$out/old" -I"$here" -Du2mp=u2mp_old -DHX_GLUE=hx::glue_old "$here/glue.cpp" -o "$out/glue_old.o"
$cxx -c -I"$root/src/u2mp" -I"$here" "$here/glue.cpp" -o "$out/glue_new.o"
if [ -f "$here/deps_$t.cpp" ]; then
    $cxx -c -I"$out/old" -I"$here" -Du2mp=u2mp_old $OLD_FLAGS "$here/deps_$t.cpp" -o "$out/deps_old.o"
    $cxx -c -I"$root/src/u2mp" -I"$here" $NEW_FLAGS "$here/deps_$t.cpp" -o "$out/deps_new.o"
    objs+=("$out/deps_old.o" "$out/deps_new.o")
fi
$cxx -c -I"$here" -I"$root/src/u2mp" "$here/harness.cpp" -o "$out/harness.o"
$cxx -c -I"$here" -I"$root/src/u2mp" "$here/test_$t.cpp" -o "$out/test.o"
$cxx "$out/test.o" "$out/harness.o" "$out/glue_old.o" "$out/glue_new.o" "${objs[@]}" $LINK_FLAGS -o "$out/test_$t.exe"
"$out/test_$t.exe"
