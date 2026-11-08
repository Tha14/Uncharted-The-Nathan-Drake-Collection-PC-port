# part-emitters: the 3 overlay-r3 dc2 particle modules (recipe step convert-dc2-part-modules)

Rebuilds `overlay-r3/u3data/build/main/dc2/part-module/part-{cave,gompa,shambhala}-multi.bin` byte for byte from the
player's PS3 Uncharted 3 (1.19 update `dlc-patch18.psarc` + disc `build/main/bin.psarc`) and the retail PS4 Collection
`u3data/build/main/dc2/part-module/` files. Tools: the batch45/batch48 scripts of the 2026-10-27 cloud session
(recovered u3-particle-emitter-converter), made to work on bytes, with the pickles replaced by tsv tables.

    python build_part_modules.py [--apply] [--dest DIR]      (rendered by the recipe driver; dry run by default)

| step | what | check |
|---|---|---|
| batch45 | `graft44.py`: the 31 Sanctuary water effects (`batch45-graft.tsv`) copied with their object closures from the retail PS4 village / oasis / graveyard / fort / old-quarter / warzone modules into retail `part-gompa-multi.bin` | md5 c77a9e0c |
| samples | `epairs.s11_samples`: PS3 s11 / PS4 s8 typed-struct word lists of every emitter pair of the 33 MP modules (`mp-modules.tsv`); the emitter converter maps PS3 words onto PS4 positions and takes the unmapped PS4 words from the matching sample | (equal to the session's s11map.pkl) |
| batch48 | `build48.py` + `emitconv.py` (`rootconv`, `ptconv`, `bcconv`, `donormap`): 22 PS3 1.19 emitters converted to the PS4 emitter format and appended to retail cave / shambhala and to the batch45 gompa; 7 groups re-pointed to their 1.19 lists | md5 9c9d7dad / e9c4ea75 / 3e843674 |

Nothing is written unless all 3 md5s match. part-ruins-multi (also made by batch48) is not built: the reference
overlay never installed it.

## Tables (no game data)

| file | content |
|---|---|
| `root4rules.tsv` | 94 PS4 emitter-root word rules learned from the 1824 PS3/PS4 emitter pairs: 53 `const` (one fixed 4-byte value for that offset, mostly 0, 1.0, -1.0), 34 `sw` / 4 `id` / 3 `h16` (which PS3 root offset feeds it and how it is byte-swapped) |
| `mattab.tsv` | PS3 material string id -> PS4 material string id, with the number of emitter pairs that showed it (371 PS3 ids, 377 rows) |
| `emitter-names.tsv` | the 22 emitter names build48 writes (sid = string id of the name, checked); replaces the 121 k-entry siddict.pkl |
| `batch45-graft.tsv` | the 31 effect sids and the retail PS4 module each one is grafted from (batch45 `gompa_src.txt`) |
| `mp-modules.tsv` | the 33 MP part module pairs and the PS3 layer of each (patch18 or disc) |
| `inputs.tsv` | sha256 of the 66 source modules the reference was built from (a difference is reported, not fatal) |

The session's `s11map.pkl` held 1822 PS3/PS4 struct copies (game bytes); it is not shipped: `epairs.s11_samples`
collects the same samples from the player's files (verified equal, all 3 struct types, same order).
