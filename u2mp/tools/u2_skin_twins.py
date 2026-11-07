"""u2_skin_twins.py - for every multiplayer skin the PS4 game lacks (mp-hero-*), find whether the PS4 campaign has
the same character model under another actor name, so the import can build the skin from the remastered PS4 model
instead of converting the PS3 one.

Method (PS3 side, both files from the player's PS3 copy): the skin's textures and mesh names against every PS3
campaign actor that also exists on PS4; a twin shares most texture names (the MP skin pak and the SP actor pak use the
same art). Prints, per skin: the best PS4 actor, shared/total texture names, and the verdict. Read-only.

usage: python u2mp/tools/u2_skin_twins.py <PS3 USRDIR> <PS4 game folder> [--dlc <PS3 DLC build/main>]
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'patcher', 'u2conv'))
import sources  # noqa: E402

NAME = re.compile(rb'[a-z0-9][a-z0-9_\-/.]{5,80}\.(?:tga|dds|png)', re.I)


def names(data):
    return {m.group().lower() for m in NAME.finditer(data)}


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    dlc = sys.argv[sys.argv.index('--dlc') + 1] if '--dlc' in sys.argv else None
    ps3 = sources.PS3(a[0], dlc, log=lambda *_: None)
    ps4_actors = os.path.join(a[1], 'u2data', 'build', 'main', 'actor27', 'ps4')
    ps4 = {f[:-4] for f in os.listdir(ps4_actors) if f.endswith('.pak')}

    def ps3_data(key):
        c = ps3.candidates(key)
        return c[0].read() if c else None

    skins = sorted(k[len('actor27/'):-4] for k in ps3.by_key
                   if k.startswith('actor27/mp-hero') and k.endswith('.pak') and k[len('actor27/'):-4] not in ps4)
    sp = sorted(n for n in ps4 if not n.startswith('mp-') and ps3.candidates('actor27/%s.pak' % n))
    sp_names = {}
    for n in sp:
        d = ps3_data('actor27/%s.pak' % n)
        if d:
            sp_names[n] = names(d)
    for skin in skins:
        d = ps3_data('actor27/%s.pak' % skin)
        if not d:
            continue
        s = names(d)
        best = max(sp_names, key=lambda n: len(s & sp_names[n]), default=None)
        shared = len(s & sp_names[best]) if best else 0
        verdict = 'twin' if s and shared >= 0.8 * len(s) else ('partial' if shared else 'none')
        print('%-34s %-30s %3d/%-3d %s' % (skin, best or '-', shared, len(s), verdict))


if __name__ == '__main__':
    main()
