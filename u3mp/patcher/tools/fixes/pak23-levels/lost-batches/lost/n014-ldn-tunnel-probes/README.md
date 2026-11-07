# n014-ldn-tunnel-probes (fixer L): London "No Intro" player blown out white in the tunnel / North Atwood

## Finding
- The NoIntro tunnel BG paks (ldn-train-multi-tunnel / -tunnel-2) carry their light tables exactly as the PS3 base game does. I checked our converted paks against PS3 pak23.psarc: the sun, the probe positions, the probe SH and the vec4 region all match byte for byte after the endian swap, and the point and spot light records differ only in the raw type word, which is intentional. The 1.19 goty-patch / goty-dlc archives do not replace these paks. So this is not a conversion error.
- Character ambient comes from the light probes, and the tunnel pak has only 6 of them, in two vertical columns:
  - (203.5, 10.5..14.5, 135.5): scales 0.067 / 0.056 / 0.046 (normal; typical probes are 0.01..0.1).
  - (924, 10.5..14.5, -794): scales **2.02 / 1.42** / 0.014. Its DC is (67, 45, 20), so the lower two are 15..30x brighter than every other probe in the level. This is a bad bake: inside the same column the value jumps 140x.
- The tunnel lamps the player stands among (lights at x 866..937, z -1750..-1800) have no probe nearby. Their nearest probe is the bright (924, -794) column, about 1 km away. The level's single probe region (centre (203, 135), radius 1176) does not contain them. My reading is that PS3 skips the probes there because of that region test, while the PS4 picks the nearest probe anyway. I have not verified this in code.
- tunnel-2's probes (3 at (-681, y, 973), about 0.039) and the -lit pak (0.0097) are normal.

## Fix (data, md5-locked)
ldn-train-multi-tunnel.pak, PC overlay-r3 md5 e98b5cac... -> 617b033a...: probe SH #3 and #4 get probe #5's 32 bytes (the same column at 14.5 m, scale 0.014). That is 64 bytes and nothing else.
`python3 apply_patches.py "<work>/overlay-r3/u3data/build/main/pak23/ps4" <backup dir>` (`--dry`, `--revert`). A dry run against the PC file passes.

## Test
London "No Intro" TDM: look at the player at the North Atwood spawn and along the tunnel, then compare with London Underground (with intro).
- Expect a normal character, like the carriage shots: no white shirt or skin.
- If the character is now too dark, apply `--revert`. Then the next lead is the probe-selection code rather than the data.
