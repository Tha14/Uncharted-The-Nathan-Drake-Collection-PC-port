# u2mp rewrite tests

The Uncharted 2 multiplayer runtime (`gow3_pc/src/u2mp`) first carried function bodies ported from the PS3 game.
Those files are reimplemented clean-room: a behaviour description (`u2mp/docs/spec/<group>.md`) is written from the
old code, new C++ is written from the description alone, and these tests run the old and the new code on the same
inputs. A test passes when both make the same guest calls with the same arguments in the same order, print the same
log lines and leave the fake game image the same.

```
bash gow3_pc/tests/u2mp_rewrite/run.sh <test> <source.cpp>...     # e.g. run.sh events events.cpp gameplay2.cpp
```

- `run.sh` takes the old sources and headers from git (`U2MP_REWRITE_BASE`, the last commit before the rewrites) and
  compiles them in namespace `u2mp_old`; the current ones are compiled in `u2mp`. `deps_<test>.cpp`, when present, is
  compiled for both (stand-ins for the other modules the sources call). `OLD_FLAGS` / `NEW_FLAGS` rename C symbols.
- `harness.h`: the fake image (`hx::mem`, image offsets are offsets into it), stubs that send a guest call to a host
  function (`hx::stub`), the record (`hx::rec`), image snapshots and the comparison (`hx::same`).
- `glue.cpp`: what `install.cpp` gives a module (`image`, `log`, `replace_function`, ...) for the fake image; every
  log line and replacement is recorded.

Needs MSYS2 CLANG64 (`clang++`). No game files: the scenarios build the guest state they need.
