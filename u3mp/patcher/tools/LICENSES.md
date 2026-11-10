# Third-party and project code shipped in PKG Converter/tools

| folder | origin | licence |
|---|---|---|
| `tools/paktools/` | `u3mp/paktools` of the GitHub repository `fidalgojoao/Uncharted2-Multiplayer-Dev` (branch `U3_Multiplayer`, snapshot of 2026-10-26 in `C:/Users/jonas/Downloads/Uncharted2-Multiplayer-Dev-main`; this project's own additions were pushed there in Oct 2026). PS3/PS4 pak, DC, geometry, texture and stage converters. | The repository ships `gow3_pc/LICENSE` = **GNU GPL v2** (the PC port's licence). `u3mp/` carries no separate licence file and no per-file headers; treat it as GPL v2 like the rest of the repository until the author states otherwise. The patcher must therefore ship these files with their source (it does: they are Python) and the GPL text. |
| `tools/transfer-tools/` | scripts written for this project in Oct 2026 (`C:/Users/jonas/Documents/Uncharted_Collection_MP/_claude_transfer/`): `psarc_lib.py`, `ndpak.py`, `ps4writer.py`, `ps4ingame.py`, `ps3geom.py`, `matsurvey.py`, `mkregistry.py`, `blkapply.py`, `blkmd5.py`, `pdump.py`, `anim-ex.py`, `shots.py`, `install_streams.py`, `fetch_*.py`, `findparts.py`, `swap.sh`. Several are older copies of files now in `paktools/`. | Project code (same terms as the project; GPL v2 when distributed with the paktools). |
| `tools/fixes/` | this project's fix jobs (`scratch/<job>` and `scratch/recovered/<job>`), 2026-10-31 .. 11-03, with machine paths replaced by placeholders. `airport-igc2/pylib/` contains a copy of **Pillow** (PIL) pure-Python files used by that job's image checks; the compiled `.pyd` modules were not copied. | Project code. Pillow: HPND licence (see `airport-igc2/pylib/PIL/` headers); install Pillow with pip instead of relying on this partial copy. |

Not shipped, on purpose: any game file or data derived from one (converted paks, extracted PS3/PS4
entries, built `out/` folders, block patches `*.patch`, `stage-assets` batches, backups), the
community server address, RPCN credentials, `u3-rpcn.cfg`.

Python packages the fix scripts import beyond the standard library: `numpy` (`excess-wall`,
`maps-batch/tools/build_paks.py`), `Pillow` (`airport-igc2`, `eula-batch`), `ffmpeg` executable
(`eula-batch/convert_eula.py`).
