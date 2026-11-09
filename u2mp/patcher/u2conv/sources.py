"""Where each file of the import comes from: the player's PS3 copies (base game + patch, optional DLC folder) and
the PS4 Collection (what it already has).

Keys are paths under build/main without platform folders: 'actor27/x.pak', 'pak22/x.pak', 'pak22/x.txt',
'sound1/banks/x.bnk', 'sound1/streams/x.xvag', 'sound1/music/x.xvag', 'sound1/speech/<lang>/x.xvag', 'text1/x'.
Keys compare lower-case (PS3 archives and the import list differ in case: MP_Win1 / mp_win1); the member's own
spelling is kept for the output name.

PS3 precedence (newest wins, as the PS3 game mounts them): dlc-patch7 > 5 > 3 > 2 > patch (all-patches.psarc)
> dlc-map2 > dlc-map1 > base archives.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)   # psarc_lib lives next to this file (it ships with the patcher; u2mp/tools does not)
from psarc_lib import Psarc  # noqa: E402

RANK = {'dlc-patch7': 9, 'dlc-patch5': 8, 'dlc-patch3': 7, 'dlc-patch2': 6, 'patch': 5, 'dlc-map2': 4,
        'dlc-map1': 3, 'base': 1}
# PS3 archive (relative to build/main) -> key prefix of its members when they carry no folder
BASE_ARCHIVES = {'actor27.psarc': 'actor27/', 'pak22.psarc': 'pak22/', 'dc1.psarc': 'dc1/', 'text1.psarc': 'text1/',
                 'sound1/banks.psarc': 'sound1/banks/', 'sound1/music.psarc': 'sound1/music/',
                 'sound1/streams.psarc': 'sound1/streams/'}


def norm_member(name, prefix=''):
    n = name.replace('\\', '/').lstrip('/')
    if n.startswith('build/main/'):
        n = n[len('build/main/'):]
    m = re.match(r'(dlc-map\d|dlc-patch\d)/(.*)$', n)
    if m:
        n = m.group(2)
    if '/' not in n and prefix:
        n = prefix + n
    return n


class Entry:
    __slots__ = ('key', 'name', 'source', 'arc', 'index', 'path', 'size')

    def __init__(self, key, name, source, arc=None, index=None, path=None, size=0):
        self.key, self.name, self.source, self.arc, self.index, self.path, self.size = key, name, source, arc, index, path, size

    def read(self):
        if self.path:
            with open(self.path, 'rb') as f:
                return f.read()
        return self.arc.read(self.index)

    def where(self):
        return self.path or '%s :: %s' % (self.arc.path, self.name)


class PS3:
    """index of the PS3 copies: base folder (USRDIR or its parent, or build/main) and an optional DLC folder"""

    def __init__(self, base, dlc=None, log=print):
        self.by_key = {}             # lower key -> [Entry] (all sources)
        self.archives = []
        self.log = log
        bm = self._build_main(base)
        if bm is None:
            raise FileNotFoundError('no PS3 Uncharted 2 build/main folder under %s' % base)
        self.base_main = bm
        usrdir = os.path.dirname(os.path.dirname(bm))
        for rel, prefix in BASE_ARCHIVES.items():
            self._add_arc(os.path.join(bm, rel), 'base', prefix)
        sp = os.path.join(bm, 'sound1', 'speech')
        if os.path.isdir(sp):
            for f in sorted(os.listdir(sp)):
                if f.endswith('.psarc'):
                    lang = f[:-6].split('-')[0]
                    self._add_arc(os.path.join(sp, f), 'base', 'sound1/speech/%s/' % lang)
        self._add_loose(os.path.join(bm, 'pak22.txt'), 'pak22.txt', 'base')
        self._add_arc(os.path.join(usrdir, 'all-patches.psarc'), 'patch', '')
        if dlc:
            dm = self._build_main(dlc) or dlc
            found = 0
            for tag in ('dlc-map1', 'dlc-map2', 'dlc-patch2', 'dlc-patch3', 'dlc-patch5', 'dlc-patch7'):
                arc = os.path.join(dm, tag + '.psarc')
                if os.path.isfile(arc):
                    self._add_arc(arc, tag, '')
                    found += 1
                elif os.path.isdir(os.path.join(dm, tag)):
                    self._add_tree(os.path.join(dm, tag), tag)
                    found += 1
            if not found:
                log('note: no dlc-map / dlc-patch archives under %s' % dlc)
            # speech-update / per-language speech archives of the disc copy carry the DLC character lines
            sp = os.path.join(dm, 'sound1', 'speech')
            if os.path.isdir(sp):
                for f in sorted(os.listdir(sp)):
                    if f.endswith('.psarc') and 'update' in f:
                        self._add_arc(os.path.join(sp, f), 'dlc-patch2', 'sound1/speech/%s/' % f.split('-')[0])

    @staticmethod
    def _build_main(root):
        for c in (root, os.path.join(root, 'build', 'main'), os.path.join(root, 'USRDIR', 'build', 'main')):
            if os.path.isdir(c) and os.path.basename(os.path.normpath(c)) == 'main' and \
                    os.path.basename(os.path.dirname(os.path.normpath(c))) == 'build':
                return c
        return None

    def _put(self, e):
        self.by_key.setdefault(e.key.lower(), []).append(e)

    def _add_arc(self, path, source, prefix):
        if not os.path.isfile(path):
            return
        try:
            p = Psarc(path)
        except Exception as ex:
            self.log('warning: cannot read %s: %s' % (path, ex))
            return
        p.path = path
        self.archives.append(path)
        for i, n in enumerate(p.names):
            key = norm_member(n, prefix)
            self._put(Entry(key, n, source, arc=p, index=i + 1, size=p.entries[i + 1][2]))

    def _add_tree(self, root, source):
        for dp, dn, fn in os.walk(root):
            for f in fn:
                full = os.path.join(dp, f)
                key = os.path.relpath(full, root).replace('\\', '/')
                self._put(Entry(key, key, source, path=full, size=os.path.getsize(full)))

    def _add_loose(self, path, key, source):
        if os.path.isfile(path):
            self._put(Entry(key, key, source, path=path, size=os.path.getsize(path)))

    def candidates(self, key):
        """entries for key, newest source first"""
        return sorted(self.by_key.get(key.lower(), []), key=lambda e: -RANK.get(e.source, 0))


class PS4:
    """what the PS4 Collection already has (names only)"""

    def __init__(self, game):
        u = os.path.join(game, 'u2data') if os.path.isdir(os.path.join(game, 'u2data')) else game
        self.main = os.path.join(u, 'build', 'main')
        if not os.path.isfile(os.path.join(self.main, 'pak22_ps4.txt')):
            raise FileNotFoundError('no Uncharted 2 PS4 data (u2data/build/main/pak22_ps4.txt) under %s' % game)
        self.pak_dir = os.path.join(self.main, 'pak22', 'ps4')
        self.actor_dir = os.path.join(self.main, 'actor27', 'ps4')
        self.bank_dir = os.path.join(self.main, 'sound1', 'banks', 'ps4')
        self.files = {}
        for sub, key in (('pak22/ps4', 'pak22/'), ('actor27/ps4', 'actor27/'), ('sound1/banks/ps4', 'sound1/banks/'),
                         ('sound1/streams', 'sound1/streams/'), ('sound1/music', 'sound1/music/')):
            d = os.path.join(self.main, sub)
            if os.path.isdir(d):
                for f in os.listdir(d):
                    self.files[(key + f).lower()] = os.path.join(d, f)
        self.speech = {}             # language -> set of lower names
        sp = os.path.join(self.main, 'sound1', 'speech')
        if os.path.isdir(sp):
            for f in os.listdir(sp):
                if f.endswith('_ps4.psarc'):
                    lang = f[:-len('_ps4.psarc')]
                    try:
                        names = Psarc(os.path.join(sp, f)).names
                    except Exception:
                        names = []
                    self.speech[lang] = {n.split('/')[-1].lower() for n in names}
        self.registry_path = os.path.join(self.main, 'pak22_ps4.txt')
        self.banks_txt = os.path.join(self.main, 'sound1', 'config', 'ps4', 'banks.txt')
        self.music_settings = os.path.join(self.main, 'dc1', 'ps4', 'music-settings.bin')

    def has(self, key):
        return key.lower() in self.files

    def music_refs(self):
        """track names (no extension) the PS4 music settings name"""
        if not os.path.isfile(self.music_settings):
            return set()
        d = open(self.music_settings, 'rb').read()
        return {m.group().decode() for m in re.finditer(rb'(?<![0-9A-Za-z_])(\d{3}_|MP_)[0-9A-Za-z_]{2,}', d)}
