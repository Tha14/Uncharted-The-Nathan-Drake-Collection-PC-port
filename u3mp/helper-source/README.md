# Uncharted 3 multiplayer helper: source

The source of the helper that the patcher loads into Uncharted 3 (the built helper is in `u3mp/patcher/build/v*`).
This folder has no version in its name on purpose: it is never renamed, so a pull request here never collides with a
new helper version.

| File | What it is |
|---|---|
| `u3_lan_profiles.c` | the C part, and at its end a generated block (see below) |
| `u3x_fixes.cpp` | the C++ part: where ported code goes |
| `agc_port.inc`, `kr_port.inc` | C parts included by `u3_lan_profiles.c` |
| `plat.h`, `plat_*.inc`, `u3_bearssl.inc` | the platform layer and TLS |
| `tests/` | the old-versus-new test: `rewrite_test.cpp` and its `rewrite_test_b*.inc` parts |

**The generated block.** Everything in `u3_lan_profiles.c` from the line
`/* ===== u3x (C++): generated from u3x_fixes.cpp ... do not edit by hand ===== */` to the end of the file is
assembly the maintainer's build makes from `u3x_fixes.cpp`. Never edit it, and never commit a regenerated copy of it:
every build rewrites it.

How to contribute here without conflicts: [CONTRIBUTING.md, section 7](../../CONTRIBUTING.md#-7-helper-source-working-side-by-side).

## Running the test

```bash
cd u3mp/helper-source
grep -v '^#define U3X_FN \|^#define U3X_RO \|^#define U3X_BSS ' u3x_fixes.cpp > tests/u3x_fixes_for_test.inc
cd tests && clang++ -std=c++20 -O1 rewrite_test.cpp -o rt && ./rt && clang++ -std=c++20 -O2 rewrite_test.cpp -o rt && ./rt
```

`u3x_fixes_for_test.inc` and `rt` are generated: do not commit them.
