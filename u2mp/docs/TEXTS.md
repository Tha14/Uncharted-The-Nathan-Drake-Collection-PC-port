# Uncharted 2 multiplayer texts in every language (texts.cpp, u2text.py)

## 1. How the game finds a text

`TextDb::Find(db, id)` (`0xcddc00`) walks the loaded banks (common -> networking -> subtitles, `+0x58` the
next bank, calling itself) and returns NULL when no bank has the id. Its callers then show the bank blob's
first string, `UNTRANSLATED STRING!!!` (`0xcddc60`). The game reads `text1/ps4/<n>.<category>` from
`core.psarc` for one language: `<n>` is the loaded bank's `+0x28` language index looked up in the table
`0x1450d18` (0x38 per language, `+0x14` the number). The PC language comes from the launcher
(GOW3_LANGUAGE, the PS4 system language; runtime_services.c).

| n | language | PS4 bank | GOTY PS3 bank |
|---|---|---|---|
| 2 | english | complete | yes |
| 3, 4, 5, 6 | french, italian, german, spanish | complete | yes |
| 7 | a second English (no name in the table) | complete | yes |
| 8, 9, 10 | danish, portuguese, dutch | complete | yes |
| 11, 12 | japanese, korean | complete | yes |
| 13, 14, 15 | finnish, norwegian, swedish | complete | yes |
| 16, 17, 18 | chinese (traditional), russian, polish | complete | yes |
| 19, 20 | sam-spanish, brazilian | `_` for 397 of 412 networking and 98 common (MP menu) texts | no |
| 21, 22, 23 | greek, czech, turkish | the same (czech: 388 networking, 95 common) | no |

`_` is the PS4's placeholder for a text it never translated (multiplayer was cut); it draws as a blank
(`_` is the game's hard space). So in those five languages the main menu's Multiplayer button and every
lobby / party / match text were empty.

## 2. Where each text comes from now

The hook (`text_find`, texts.cpp; installed by remaining.cpp) answers, in this order:

1. the game's own text, unless it is `_`;
2. an alias (net-patch9 ids whose text is a PS4 id's or one of our playlist texts);
3. the imported bank of the language (`/app0/u2data/build/main/text1/ps4/<n>.common` and `.networking`,
   written by the import's texts step, u2text.complete): the PS4 bank, completed with the GOTY PS3 bank of
   that language (every GOTY archive, newest first) for ids it lacks or has as `_`, and for sam-spanish and
   brazilian with the spanish and portuguese PS4 banks. A real PS4 string is never changed. GOTY data only;
4. our own text (`kOwn`: every language, `nullptr` = es-419 -> es, pt-BR -> pt, else English; `kMenu`:
   greek / czech / turkish for the 96 multiplayer menu texts their common bank has as `_`);
5. `unlocks_text` (a label from a GOTY skin's package name);
6. the imported English bank (logged once per id and language: "texts: <id> shown in English (language n)");
7. nothing: logged once per id and language ("texts: no text for <id> (language n)"); the game shows
   its fallback (or its `_`).

The two log lines are the coverage test: a run through the menus, a match and its results in each language
must show no "no text" line; "shown in English" lists what is still untranslated.

## 3. The ids, counted (static check, 2026-11-09)

Ids the multiplayer code and data use: the immediates of the executable and the dc files (`dc1/ps4/*.bin`,
the overlay's `net-patch9.bin`) that are text ids, plus every id of the PS4 English common and networking
banks. Text ids were found against the PS4, GOTY and (for finding ids only) BCUS banks; no BCUS text is used.

104 ids referenced by the code / net-patch9 are in no PS4 bank: 2 are in the GOTY base bank (47a936dc
"Rankings" for Leaderboards, ec7dd02d a treasure name), 4 are another PS4 id's text (aliases), 98 had no
text anywhere. All 98, the 5 playlist texts and the 11 PC texts are in `kOwn` (114 rows) in all 22 languages.

| language | PS4 own | imported (GOTY / related) | ours | still English |
|---|---|---|---|---|
| 2 english | 2677 | 2 | 114 | - |
| 3..18 (not 7) | 2671 (11, 12, 16: 2674-2675) | 2-5 | 114 | 2-3 |
| 19 sam-spanish, 20 brazilian | 2176 | 500 (spanish / portuguese) | 114 | 3 |
| 21 greek, 23 turkish | 2176 | 0 | 114 + 96 menu | 405 |
| 22 czech | 2189 | 0 | 114 + 93 menu | 395 |

"Still English" in 3..20: `Buy Game`, `BUY GAME` and the copyright line (only English has them; not
multiplayer). In greek, czech and turkish: the 397 (czech 388) networking texts (medals, boosters, game
modes, match messages) and the 2 blank common texts.

## 4. Left

- Greek, Czech and Turkish networking texts show English. They need our own translation (texts.cpp `kMenu`
  holds the menu ones; the same table form fits the networking ones).
- PS4's own untranslated strings are kept (rule: a real PS4 string is never changed), e.g. the Japanese and
  Chinese common banks have some English labels ("Competitive", "Competitive Stats").
- Ids no bank has and no static scan finds (texts the PS3 server sent at run time) show up only in the log.

## 5. Adding a text

Add a row to `kOwn` (texts.cpp) with all 21 columns (en fr it de es da pt nl ja ko fi no sv zh ru pl es-419
pt-BR el cs tr). Keep the game's tokens (`[A]`, `[TEXT]`, `[TEXT2]`) in every language; `\\` in the C++
string is the game's line break (one backslash in a bank). Our own wording only: never text from another
version's files.
