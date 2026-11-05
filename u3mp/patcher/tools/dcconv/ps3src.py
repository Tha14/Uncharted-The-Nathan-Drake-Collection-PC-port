"""Index of the PS3 Uncharted 3 1.19 DC modules (dc1 / dc2) by their PS4 relative path.

Precedence (the 1.19 game mounts later archives over earlier ones): DATA2 net18.bin (server copy of net.bin) >
dlc-patch18 > dlc-patch13 > goty-patch > goty-dlc > disc bin.psarc. Every candidate is kept, so a caller can pick
another layer (e.g. 'disc') when the newest one is not what was converted.

  idx = Index(disc_usrdir, update_usrdir, data2_usrdir)
  idx.layers('dc1/ps4/net.bin') -> [(layer, archive path, entry name), ...] newest first
  idx.read('dc1/ps4/net.bin')   -> bytes of the newest layer (or idx.read(rel, layer='disc'))
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'transfer-tools'))
from psarc_lib import Psarc  # noqa: E402

ORDER = ['data2', 'patch18', 'patch13', 'goty-patch', 'goty-dlc', 'disc']


def ps4_rel(entry):
    """'build/main/bin/dc1/ss-multiplayer/x.bin' -> 'dc1/ps4/ss-multiplayer/x.bin' (None if not a DC entry)"""
    e = entry.replace('\\', '/').lstrip('/')
    for pre in ('build/main/bin/', 'bin/', 'build/main/'):
        if e.startswith(pre): e = e[len(pre):]; break
    for d in ('dc1/', 'dc2/'):
        if e.startswith(d):
            return d + 'ps4/' + e[len(d):] if d == 'dc1/' else e
    return None


class Index:
    def __init__(self, disc=None, update=None, data2=None):
        self.arch = {}; self.map = {}
        cands = []
        if data2 and os.path.exists(os.path.join(data2, 'net18.bin')):
            self.map.setdefault('dc1/ps4/net.bin', []).append(('data2', os.path.join(data2, 'net18.bin'), None))
        if update:
            cands += [('patch18', os.path.join(update, 'dlc-patch18.psarc')), ('patch13', os.path.join(update, 'dlc-patch13.psarc'))]
        if disc:
            cands += [('goty-patch', os.path.join(disc, 'goty-patch.psarc')), ('goty-dlc', os.path.join(disc, 'goty-dlc.psarc')),
                      ('disc', os.path.join(disc, 'build', 'main', 'bin.psarc'))]
        for layer, p in cands:
            if not os.path.exists(p): continue
            a = Psarc(p); self.arch[p] = a
            for i, n in enumerate(a.names):
                r = ps4_rel(n)
                if r and (r.endswith('.bin') or r.endswith('.dci')):
                    self.map.setdefault(r, []).append((layer, p, i + 1))
        for r in self.map:
            self.map[r].sort(key=lambda x: ORDER.index(x[0]))

    def layers(self, rel):
        return self.map.get(rel, [])

    def read(self, rel, layer=None):
        for ly, p, i in self.layers(rel):
            if layer and ly != layer: continue
            return open(p, 'rb').read() if i is None else self.arch[p].read(i)
        return None

    def names(self):
        return sorted(self.map)
