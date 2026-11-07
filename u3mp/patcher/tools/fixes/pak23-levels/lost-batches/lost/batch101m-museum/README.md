# batch101m-museum (fixer W): batch101-detail for Museum, on top of n012-museum-catchup

This is the same fix as batch101-detail (see its README). museum-multi has 2 colour layer-3 detail slots that are bound to the black flat-mask:
- war-walls-courtyard-multi-emblem-dark:basic
- war-walls-courtyard-multi-emblem:basic

They now get the PS3 material's own `war-brick-worn-detail.tga` (Museum copy, hash B2AEF3DEC337F4D6, 128x128 DXT1 re-tiled 1:1 to BC1 sRGB). museum-multi-stage has no affected material.

- **md5 lock:** md5_before 7a7c20e84188f9e0e854ef37a4972e08 is museum-multi **after n012-museum-catchup**. I built it from the PC file (9fd5549c, staged read-only) with n012's own apply_patches.py. md5_after is 9d5fe9970ca3cd6b215b2aa21b17b392.
- **Gates (offline):** crashcheck clean, bindcheck 0, fidelity clean vs the PS3 museum-multi.

Install after n012:
```
python3 ../n012-museum-catchup/apply_patches.py "<work>/overlay-r3/u3data/build/main/pak23/ps4" <backup n012>
python3 append101.py "<...>/U3 Multiplayer/work" <backup 101m> --check
python3 append101.py "<...>/U3 Multiplayer/work" <backup 101m>
python3 append101.py "<...>/U3 Multiplayer/work" <backup 101m> --revert      # undo
```
