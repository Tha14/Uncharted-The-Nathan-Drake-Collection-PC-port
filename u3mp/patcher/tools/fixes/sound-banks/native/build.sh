#!/bin/bash
# Maintainers: rebuild vagenc.dll (64-bit Windows) with msys2 ucrt64 gcc. -ffp-contract=off keeps the C double
# operations exactly as written (no fused multiply-add), as the original was built; output must stay byte-identical
# to vagenc.py (check: python check_native.py).
cd "$(dirname "$0")"
PATH=/c/msys64/ucrt64/bin:$PATH gcc -O2 -ffp-contract=off -fno-fast-math -shared -static-libgcc -o vagenc.dll vagenc.c
