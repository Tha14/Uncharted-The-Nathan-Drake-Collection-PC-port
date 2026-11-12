# 🤝 Contributing

Thank you for helping improve the project. Read the
[README's Contributing section](README.md#contributing) for the contribution scope and workflow.

The rules below are ordered by priority: **legal and distribution requirements come first**, followed by
the contribution process and development priorities. Every contribution must follow all applicable rules.

## ⚖️ 1. Legal and distribution requirements — highest priority

This is an unofficial, non-commercial fan project, not affiliated with Sony Interactive Entertainment,
Naughty Dog or Bluepoint Games. These requirements protect the project and take priority over convenience,
features and implementation preferences. Contributions that break them will be rejected. If prohibited
material is merged by mistake, it must be removed, including from repository history where necessary.

### 🚫 No game files or game-derived data

Do not contribute or distribute game executables, paks, textures, audio, models, text, fonts or images from
the games. This also includes converted or extracted files, patched game images, and tables or blobs copied
out of game files or executables. If a fix needs game data, it must be generated on the player's PC from
their own legally obtained copy, using project code or a recipe step.

### 🔒 No decryption, dumping or game-download assistance

Do not add game-decryption or copy-protection bypass functionality. Do not provide instructions for
decrypting or dumping games, links to game downloads, or directions for obtaining unauthorized copies.

### 🛑 No copied or identically reproduced proprietary game code

Write independent implementations that aim to reproduce the required behavior. Do not copy and paste
Naughty Dog's, Bluepoint's or Sony's proprietary game code, reproduce whole functions from it, or include
large arrays of executable bytes. Renaming variables, reformatting code or mechanically translating
decompiled code does not make a copied implementation independent.

Short instruction patterns used only to locate and verify patch sites in the player's own copy remain
permitted; they must not embed complete game functions or substantial executable code.

### 🔑 No keys or secrets

Do not include encryption keys, signing keys, passwords, tokens or credentials belonging to the games,
Sony, a server or anyone else.

### 📜 Only code you have permission to share

Contribute your own code, or third-party code whose license permits its inclusion here and is compatible
with the project's GNU GPL v2 or later license. Preserve the required credits, copyright notices and
license notices.

### 💸 Keep the project non-commercial

Do not add donation links, paid features, advertisements or anything sold in connection with this project.

## 🛡️ 2. Community server privacy

Do not publish the community server's IP address or host names in code, documentation, logs, screenshots
or other contributed material.

## 🔀 3. Submit contributions through pull requests

**All contributions must be submitted as a pull request (PR) from a fork.** Follow the scope and coordination
steps in the [README](README.md#contributing), keep each PR focused on one change, and rebase your branch
on the latest `main` before submitting.

Describe the problem, how to reproduce it, what you changed and how you tested it. Contributions must be
reviewed and tested by the maintainer before they are merged.

## 🔄 4. Develop against current supported versions

Keep the launcher and game installation up to date with the versions currently supported by the project.
Base new work on the latest project code and test against that supported launcher and game combination
to reduce compatibility problems and regressions.

State the launcher/build version and game update used for testing in your PR. When changing supported
versions, check that the change does not regress existing supported behavior.

## ⚙️ 5. C++ above everything else

**Everything in this project is written in C++.** Another language is accepted only where C++ is really impossible or
clearly not practical. Pull requests that add Python, add C, or grow existing Python or C are not accepted.

- **New code:** C++ (C++20), for features, fixes, tools, tests and build or import steps.
- **Existing C** (for example the helper's `u3_lan_profiles.c`) and **existing Python** are being replaced: if your
  change touches them, port the part you need to C++ and remove the old code.
- **The only exceptions:** third-party libraries kept as they are (BearSSL), a few lines of assembly inside C++ where
  the game's calling convention needs it, QML for the launcher window's layout (logic in C++), and short shell /
  PowerShell / CMake glue for CI, packaging and builds.
- If you think something really needs another language, ask the maintainer first.

If you are using a coding agent, it reads the same rules from [AGENTS.md](AGENTS.md).

## 🎨 6. Assets: PS4 first, PS3 only when PS4 has none

**Port an asset from the PS3 version only when the PS4 version does not have it.** If PS4 already has the asset
anywhere, multiplayer or campaign, the import uses the PS4 one: it usually has better textures, and not bringing
over a second copy keeps the import smaller. For example, baseball-shirt Drake (red shirt Drake) is already in PS4
as a campaign skin, so it is not ported from PS3. Before adding a PS3 asset to an import step, check PS4's data for
the same asset.

## 🧩 7. Helper source: working side by side

The Uncharted 3 helper's source is in [`u3mp/helper-source/`](u3mp/helper-source/README.md). Several people port
its code at the same time, so these rules keep pull requests from colliding:


**In short, how a contributor works on the helper:**

1. Click **Sync fork** on your fork of this repo (always before you start, and again after each new helper version).
2. Edit only the files in `u3mp/helper-source/`.
3. Open a **Pull Request**.
4. The maintainer merges it, then builds and ships the next helper version.

**How nobody gets in each other's way:**

- **Never change a version number or build anything.** Only the maintainer does that, once per batch of rows.
- **Write only in your own marked spots** (between the "contributor" marker lines, details below). The maintainer
  writes only at the very end of `u3x_fixes.cpp`, so two people never change the same lines.
- **Port only the rows assigned to you** in the current split file, so two people never work on the same function.

The details:

**Only the maintainer makes helper versions.**
- Never change a helper version: no new or renamed `u3mp/patcher/build/v*` folder, no `build.json`, no version
  number anywhere. The maintainer merges the open helper PRs, then makes **one** new helper version per batch.
- Never commit a built helper or anything generated: not `profile-helper.bin`, not `tests/u3x_fixes_for_test.inc`,
  and not the generated block at the end of `u3_lan_profiles.c` (from the line `u3x (C++): generated from
  u3x_fixes.cpp` to the end of the file).

**Write only in your own places.**
- New C++ goes into `u3x_fixes.cpp` between `===== contributor section =====` and
  `===== end of contributor section =====`, just above the end line. The maintainer adds code only at the end of
  the file, so the two never touch the same lines.
- New `u3x_` prototypes go into `u3_lan_profiles.c` between `contributor prototypes` and
  `end of contributor prototypes`; new `U3H` exports and `_Static_assert` checks go between `contributor exports`
  and `end of contributor exports`.
- The C function you port keeps its name and place and becomes a one-line wrapper that calls your `u3x_` function,
  so every other caller keeps working. That is the only place you edit outside your sections.
- Never change the three lines right after each end marker.
- Take only rows assigned to you (the current split lists who does what) so two people never port the same function.

**Pull requests.**
1. Sync your fork and start a branch from the latest `main` for each batch of rows.
2. Port, then run the old-versus-new test (how: the [helper source README](u3mp/helper-source/README.md)). Add your
   test as a new `tests/rewrite_test_bNN.inc` (take the next free number) with its `#include` and `batchNN();` call
   in `rewrite_test.cpp`, and stub every new `u3h_` import in `tests/rewrite_test_b27.inc`.
3. Open the PR with the rows you ported and the test result. The maintainer reviews, merges, builds, checks it in
   the game and ships the next helper version with it.
4. After a new helper version ships, update your branch from `main` (`git pull --rebase` on your branch) before you
   continue. Your sections and wrappers carry over; only the generated block changes, and you never touch it.

---

By contributing, you agree that your contribution is licensed under the **GNU GPL v2 or later**, like the
rest of the project.
