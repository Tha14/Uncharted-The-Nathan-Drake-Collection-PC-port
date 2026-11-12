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

## ⚙️ 5. Prefer C++ implementations

**C++ is our first choice wherever practical.** Prioritize implementing new features and components in C++
when the project's architecture and dependencies allow it. If another language is needed, explain the
reason in your PR so the maintainer can review the tradeoff.

---

By contributing, you agree that your contribution is licensed under the **GNU GPL v2 or later**, like the
rest of the project.
