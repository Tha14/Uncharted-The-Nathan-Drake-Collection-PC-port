# Shipping updates to players (launcher, emulator, multiplayer)

Players never re-import from scratch. A release replaces the program folders, and the Multiplayer tab then brings
the existing import up to date by itself, in the background and at below-normal priority, redoing only what
changed.

## What the player's launcher does

1. **UPDATE** (launcher/updater.py) installs the newest release of
   `fidalgojoao/Uncharted-The-Nathan-Drake-Collection-PC-port` that has an `Uncharted-windows.zip` asset with a
   GitHub SHA-256 digest. Saves, settings and the shader cache (`analysis\`) are kept, and so is the multiplayer
   data folder (it lives outside the program folder).
2. On start, and again when Play is pressed with multiplayer on, `u3mp_pending()` compares the import's
   `state.json` with the installed files:
   * **image**: `build/v*/build.json` names a new helper build. The import reruns with `--image-only` (it
     relinks the multiplayer image from the retail eboot and takes seconds).
   * **recipe**: `recipe/CONTENT_ID` (the fingerprint of recipe/ + tools/) differs from the `recipe_id` the last
     import recorded. The import (`u3import.exe`) runs again on the same data folder. The recipe reruns only the steps
     whose own fingerprint (importer/recipe.cpp `step_fingerprint`: step file, fix job folder, list) changed, plus the
     steps whose recorded output files those reruns changed. Every other step is "already applied".
   * While an update runs, Play asks whether to wait or to start Uncharted 3 without multiplayer: the import
     rewrites overlay files, so the game must not read them at the same time.
3. The status line says "Updating the multiplayer files (launcher update ...)". The log is `import.log` in the
   data folder.

## What the maintainer does

* **Converter or recipe change**: make the change in `u3mp/patcher/recipe|tools` (C++ for any code: a u3fix
  command, see `recipe/ADDING-A-FIX.md`) and test it with `u3import.exe --recipe ... --only <id>`. Bump
  `recipe/VERSION` when steps change, because the status line shows it. `recipe/CONTENT_ID` is written by the
  release build (the private files' apply.cpp: `u3import --recipe-fingerprint` gives the same value).
* **New multiplayer helper build**: `scratch/u3x-tools/ship_helper.sh` (maintainer tooling, not in this
  repository) moves `u3mp/patcher/build/vX.YZ` along and makes the build's `link.json`
  (`maintainer-tools/link-delta`): the linked image as copies from the menu shell plus the helper build's own bytes,
  shipped through the private files. The launcher picks the highest version, and the players' image is relinked.
* **Emulator / launcher change**: this works as in the upstream port. Merge `upstream/main`
  (bc160201983) when BilalReloaded publishes a new version, so players get the port's fixes through this
  repository's releases.
* **Release**: write `tools/release-notes/vX.Y.Z.md`, set `tools/VERSION` to X.Y.Z (higher than every earlier
  release: 0.9.0 and up, above upstream 0.8.3), commit, `git tag -a vX.Y.Z -m "Release X.Y.Z"`, and push `main`
  and the tag. `.github/workflows/build-windows.yml` builds the package (with `tools/package_windows.sh`, which
  also compiles `sound-banks/native/vagenc.dll` and checks it byte-identical) and publishes the release.
  Launchers of earlier versions then offer it under UPDATE.

## Notes

* Imports made with recipe 1.7.0 have no step fingerprints. The first 1.8.0 run adopts the current ones for
  steps whose files are unchanged, because 1.8.0 changed no step's output.
* Faster import (1.8.0): the level and actor paks a higher overlay hides are not built (740 files the game never
  loads), worker processes default to all logical cores but 2, and the sound banks use the C encoder.
