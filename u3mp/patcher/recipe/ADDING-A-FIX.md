# How to add a fix to the patcher

The recipe is a list of **steps**. Each step is one json file in `recipe/steps/`, and the only
registration point is `recipe/steps/ORDER.txt` (one step id per line, run top to bottom). The
scripts a step runs live in `PKG Converter/tools/fixes/<job>/`. Nothing else has to change.

## 1. Put the fix's scripts in a job folder

`PKG Converter/tools/fixes/<job>/` holds the scripts of one fix job (the same folder name as the
`scratch/<job>` folder you developed it in). Rules:

* **No game data.** No `out/` paks, no PS3/PS4 extracts, no `.pak/.bin/.dds/.pkl` built from the
  game. Only scripts, small json/tsv plans and name lists. `recipe/gather_tools.py` does this copy
  for the historical jobs (it excludes derived files by pattern); for a new job either add it to
  `JOBS` in `gather_tools.py` and run it, or copy the folder by hand.
* **No machine paths.** Replace every absolute path by a placeholder; the driver fills them in
  when it renders the job into `<out>/scratch/<job>/` before running it:

  | placeholder | value |
  |---|---|
  | `{{PROJECT}}` | the output root; `{{PROJECT}}/work/overlay-r3/...` is the overlay being built (the project's `work/` convention) |
  | `{{WORK}}` | `{{PROJECT}}/work` |
  | `{{SCRATCH}}` / `{{RECOVERED}}` | `{{PROJECT}}/scratch` (every job is rendered there; the helper modules of `tools/fixes/*.py` too) |
  | `{{PS4}}` | the PS4 game folder (`eboot.bin`, `u3data/`, `u2data/`) |
  | `{{PS3_DISC}}` | PS3 U3 disc data `USRDIR` (`build/main/pak23.psarc`, `goty-*.psarc`, ...) |
  | `{{PS3_UPDATE}}` | PS3 U3 1.19 update `USRDIR` (`dlc-patch13.psarc`, `dlc-patch18.psarc`) |
  | `{{PS3_DATA2}}` | PS3 U3 `...DATA2/USRDIR` (`net18.bin`, `build/main/*-7.psarc`, `http-cache`) |
  | `{{PS3_DLCMAPS}}` | PS3 U3 DLC map packs `USRDIR` (`dlc-map1..4.psarc`) |
  | `{{PS3_U2}}` | PS3 U2 `USRDIR` |
  | `{{PAKTOOLS}}` | `PKG Converter/tools/paktools` |
  | `{{TRANSFER_TOOLS}}` | `PKG Converter/tools/transfer-tools` (`psarc_lib`, `ndpak`, `ps4writer`, `mkregistry`, ...) |
  | `{{PYTHON}}` | the interpreter running the driver |
  | `{{SPEECH_LANGS}}` | comma list of the speech languages chosen with `--speech-langs` |

  The same values are exported to the script's environment as `RECIPE_<NAME>` and `PAKTOOLS`,
  `PKT`, and `PYTHONPATH` already contains paktools, transfer-tools, the job folder and
  `{{SCRATCH}}`. Put a placeholder only inside a **double-quoted** string if the value may contain
  an apostrophe (the driver escapes `'` inside single-quoted literals, but not inside raw ones).
* **Dry run by default, `--apply` to write, idempotent.** The script must detect that it has
  already been applied (compare a hash, a magic value, a size, or keep a `.bak`) and then do
  nothing and exit 0. The driver also records the sha256 of every file the step wrote in
  `<out>/_recipe/state.json`; on the next run a step whose recorded outputs are unchanged is
  skipped as "already applied", so an updated patcher only reruns new or changed steps. Back up
  what you overwrite to `{{PROJECT}}/work/backup-<job>/` (never overwrite an existing backup).
* Read the PS3/PS4 data only through the placeholders above; do not look for other folders.

## 2. Write the step file

`recipe/steps/<NNN>-<id>.json` (NNN = three digits, leave gaps; `id` = short kebab-case name):

```json
{
 "id": "my-fix",
 "title": "One line: what it changes and where (job/script.py)",
 "kind": "script",
 "job": "my-job",
 "family": "overlay-r3/pak23",
 "needs": ["ps4", "ps3_disc"],
 "commands": [["{{PYTHON}}", "{{JOB}}/apply_my_fix.py", "--apply"]],
 "writes": ["work/overlay-r3/u3data/build/main/pak23/ps4/air-multi*.pak"],
 "status": "reproducible",
 "applied": "2026-11-10",
 "notes": "what it depends on, what the user will see",
 "recipe_version_added": "1.1.0"
}
```

Fields:

* `kind`: `script` (runs `commands` in the rendered job folder), `copy-list` (the driver copies
  the files of `recipe/lists/<list>.tsv` from the inputs, sha256-checked; no script), or
  `unavailable` (documents a step that cannot run; give `reason`).
* `needs`: inputs the step must have: `ps4`, `ps3_disc`, `ps3_update`, `ps3_data2`,
  `ps3_dlcmaps`, `ps3_u2`, `python:<module>` (e.g. `python:numpy`), `tool:<path under tools/>`.
  Missing input = the step is skipped with a message, not an error.
  External programs are not a `needs` token: a script that uses ffmpeg looks it up itself (`RECIPE_FFMPEG`, PATH,
  `C:/msys64/clang64/bin/ffmpeg.exe`) and degrades or skips with a message (see `eula-batch/make_eula.py`).
* `commands`: argv lists. `{{JOB}}` = the rendered job folder, `{{OUT}}` = the output root, every
  placeholder of the table works; an argument `@<file>` is replaced by that file's content (for
  long `--only` lists). `cwd` (optional) overrides the working directory (default `{{JOB}}`).
  `with_jobs` (optional): other job folders the scripts import from (e.g. `["dctools"]` for the DC reader / editor);
  the driver renders them into `<out>/scratch/<job>` too and puts them on PYTHONPATH.
  `ok_codes` (default `[0]`), `expect` (a string that must appear in the output), `log_lines`.
* `writes`: globs (relative to the output root) of the files the step may change; the driver hashes
  them before and after to record what changed. `must_write: false` lets a step succeed when it
  changes nothing (already applied).
  A step whose `writes` files all equal `manifest.tsv` is skipped as "already at the reference state". A new
  step that changes files the manifest predates must set `reference_check: false` (or update the manifest),
  or it never runs on a build that matches the reference (recipe 1.11.0's map-lighting was skipped that way).
* `status`: `reproducible`, `needs-converted-base` (works only once the converted paks exist),
  `needs-build` (ships the build script but not its output), `blocked-lost-tool`,
  `not-reproducible-locally`. Informational, printed by `--list`.

## 3. Register it and bump the version

1. Append the id to `recipe/steps/ORDER.txt` at the position it must run (new fixes normally go
   last; a fix to `net.bin` must come after everything else that rewrites `net.bin`, and
   `classic-netgames` must be rerun after any `net.bin` rebuild).
2. Bump `recipe/VERSION` (semver: new step = minor, changed step = patch).
3. Test: `python rebuild_overlays.py ... --out <existing output> --only my-fix`, then run it a
   second time: it must print `already applied`. Then `python verify_output.py <out>` if the
   reference `manifest.tsv` was updated with the new file hashes (`make_manifest.py <work> <recipe>`).

## 4. Updating a user's build

Users rerun `rebuild_overlays.py` with the same `--out`. Steps already recorded in
`<out>/_recipe/state.json` whose outputs still match are skipped; new steps run; a step whose
script changed is rerun with `--force` or `--only <id>`. `--list` shows the current step table.
