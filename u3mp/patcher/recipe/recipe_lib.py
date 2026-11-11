"""recipe_lib.py - shared code of the overlay rebuild recipe (input discovery, PARAM.SFO, steps, state, rendering).

Layout (all relative to this file):
  ../tools/paktools/        u3mp/paktools converters (third party, see ../tools/LICENSES.md)
  ../tools/transfer-tools/  pre-session helper scripts (psarc_lib, ndpak, mkregistry, blkapply, ...)
  ../tools/fixes/<job>/     one folder per fix job; .py files carry {{PLACEHOLDER}} paths (see PLACEHOLDERS)
  steps/NNN-<id>.json       one step per fix / family; steps/ORDER.txt is the single registration list
  lists/*.tsv               copy lists (destination, source archive, entry, size, sha256)
  manifest.tsv              reference hashes of the four overlays (what a perfect rebuild must produce)

Output layout (<out> is the folder given with --out):
  <out>/work/overlay-r3|overlay-r2|overlay-port|overlay-mp/u3data/...   the rebuilt overlays
  <out>/work/backup-*       backups written by the fix scripts (same convention as the project)
  <out>/scratch/<job>/      rendered fix scripts (placeholders filled in)
  <out>/_recipe/state.json  which steps ran, with the hashes of the files they wrote (idempotency)
  <out>/_recipe/log.txt     full log
"""
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # PKG Converter/
TOOLS = os.path.join(ROOT, 'tools')
STEPS = os.path.join(HERE, 'steps')
LISTS = os.path.join(HERE, 'lists')
OVERLAYS = ('overlay-r3', 'overlay-r2', 'overlay-port', 'overlay-mp')
VERSION = open(os.path.join(HERE, 'VERSION'), encoding='utf-8').read().strip() if os.path.exists(os.path.join(HERE, 'VERSION')) else '0'

sys.path.insert(0, os.path.join(TOOLS, 'paktools'))
sys.path.insert(0, os.path.join(TOOLS, 'transfer-tools'))


# ----------------------------------------------------------------------------- small helpers
def sha256_file(p, bufsize=1 << 20):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(bufsize), b''):
            h.update(c)
    return h.hexdigest()


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def shadowed_files(manifest_rows=None):
    """(overlay, path) of the manifest level/actor paks a higher overlay hides: the game loads the overlays first-wins
    in OVERLAYS order, so these are never read.  The level and actor builders skip them (faster import) unless
    rebuild_overlays runs with --all-files; verify_output reports them as 'hidden'."""
    rows = manifest_rows if manifest_rows is not None else list(read_tsv(os.path.join(HERE, 'manifest.tsv')))
    have = {}
    for m in rows:
        have.setdefault(m['overlay'], set()).add(m['path'].lower())
    out = set()
    for m in rows:
        p = m['path']
        if not (p.startswith('u3data/build/main/pak23/ps4/') or p.startswith('u3data/build/main/actor33/ps4/')):
            continue
        hi = OVERLAYS[:OVERLAYS.index(m['overlay'])] if m['overlay'] in OVERLAYS else ()
        if any(p.lower() in have.get(o, ()) for o in hi):
            out.add((m['overlay'], p))
    return out


def read_tsv(path):
    with open(path, encoding='utf-8') as f:
        head = f.readline().rstrip('\n').split('\t')
        for line in f:
            if not line.strip():
                continue
            yield dict(zip(head, line.rstrip('\n').split('\t')))


def norm(p):
    return os.path.normpath(p).replace('\\', '/')


# ----------------------------------------------------------------------------- PARAM.SFO
def parse_sfo(path):
    """Return the PARAM.SFO key/value dict (strings and ints)."""
    d = open(path, 'rb').read()
    magic, ver, kto, dto, n = struct.unpack('<4sIIII', d[:20])
    if magic != b'\0PSF':
        raise ValueError('not a PARAM.SFO: %s' % path)
    out = {}
    for i in range(n):
        ko, fmt, ln, mx, do = struct.unpack('<HHIII', d[20 + 16 * i:36 + 16 * i])
        k = d[kto + ko:d.index(b'\0', kto + ko)].decode()
        v = d[dto + do:dto + do + ln]
        out[k] = struct.unpack('<I', v)[0] if fmt == 0x0404 else v.rstrip(b'\0').decode('utf-8', 'replace')
    return out


def find_sfo(folder):
    """PARAM.SFO / param.sfo in folder, its parent (USRDIR -> PS3_GAME) or sce_sys (PS4)."""
    for c in (os.path.join(folder, 'PARAM.SFO'), os.path.join(folder, 'param.sfo'),
              os.path.join(folder, 'sce_sys', 'param.sfo'),
              os.path.join(os.path.dirname(folder), 'PARAM.SFO'), os.path.join(os.path.dirname(folder), 'param.sfo')):
        if os.path.isfile(c):
            return c
    return None


# ----------------------------------------------------------------------------- input discovery
# Region-agnostic: a folder is accepted when it has the files the recipe reads, not by title id.
# The title id / title are only read to label the inputs and to pick the PKG title id later.
U3_WORDS = ('uncharted 3', "drake's deception", 'drakes deception', 'drake’s deception')
U2_WORDS = ('uncharted 2', 'among thieves')

PS3_U3_KINDS = {
    # kind: (description, marker files relative to the USRDIR (any one is enough), what the recipe takes from it)
    'disc': ('U3 PS3 disc / full game data (PS3_GAME/USRDIR)',
             ('build/main/pak23.psarc', 'build/main/actor33.psarc'),
             'pak23.psarc, actor33.psarc, bin.psarc, text1.psarc, sound1/banks.psarc, sound1/streams/speech/<lang>-mp.psarc, vtex1/, goty-*.psarc'),
    'update': ('U3 PS3 title update 1.19 install (USRDIR with dlc-patch13.psarc + dlc-patch18.psarc)',
               ('dlc-patch18.psarc', 'dlc-patch13.psarc'),
               'dlc-patch13.psarc, dlc-patch18.psarc (menus, net tables, patched level paks, vtex1 rank art)'),
    'data2': ('U3 PS3 game-data install written by the game/server (...DATA2/USRDIR: net18.bin, build/main/*-7.psarc)',
              ('net18.bin', 'build/main/actor-parts-7.psarc'),
              'net18.bin, actor-parts-7.psarc, pak23/*-7.psarc, http-cache (EULA text); its Lab psarcs are not used (no Lab maps on PC)'),
    'dlcmaps': ('U3 PS3 DLC map packs install (USRDIR with dlc-map1..4.psarc)',
                ('dlc-map1.psarc', 'dlc-map2.psarc', 'dlc-map3.psarc', 'dlc-map4.psarc'),
                'dlc-map1..4.psarc (Flashback maps, DLC sound banks/streams)'),
}


def _has_any(folder, rels):
    return any(os.path.exists(os.path.join(folder, r)) for r in rels)


def classify_ps3_u3(folder):
    kinds = [k for k, (_d, markers, _u) in PS3_U3_KINDS.items() if _has_any(folder, markers)]
    return kinds


def accepted_net18_sha256():
    """The net18.bin versions the DC recipes accept (tools/dcconv/net18-versions.json), else the one they were made
    from. The online server updates net18.bin in place; each new version is checked and added there."""
    p = os.path.join(TOOLS, 'dcconv', 'net18-versions.json')
    try:
        return set(json.load(open(p, encoding='utf-8'))['accepted'])
    except (OSError, ValueError, KeyError):
        h = expected_net18_sha256()
        return {h} if h else set()


def expected_net18_sha256():
    """sha256 of the game-data net18.bin the DC recipes were made from (dcconv/dc-recipes.json.gz sources)."""
    import gzip
    p = os.path.join(TOOLS, 'dcconv', 'dc-recipes.json.gz')
    try:
        recipes = json.load(gzip.open(p, 'rt', encoding='utf-8'))
    except (OSError, ValueError):
        return None
    for rec in recipes.values():
        h = (rec.get('sources') or {}).get('ps3/data2:dc1/ps4/net.bin')
        if h:
            return h
    return None


def full_import_problems(inp):
    """What a full import needs beyond the disc and the update, in the player's words (empty = all there)."""
    out = []
    if 'dlcmaps' not in inp.ps3:
        out.append('the Uncharted 3 DLC map packs are not installed (no dlc-map1..4.psarc found in the folders given). '
                   'Install the DLC map packs in RPCS3 (they go to dev_hdd0/game/<the DLC title id>, e.g. BCES01176) '
                   'and add RPCS3\'s dev_hdd0/game folder.')
    d2 = inp.ps3.get('data2')
    if not d2:
        out.append('the Uncharted 3 game data folder (dev_hdd0/game/BCES01670DATA2) was not found: start Uncharted 3 '
                   'in RPCS3 with update 1.19 and go online once, so the game writes it, then add dev_hdd0/game.')
        return out
    missing = [r for r in ('net18.bin', 'build/main/actor-parts-7.psarc') if not os.path.exists(os.path.join(d2, r))]
    if missing:
        out.append('the game data folder %s is incomplete (missing %s): it is written by Uncharted 3 when it goes '
                   'online in RPCS3 (net tables and map updates); go online in Uncharted 3 multiplayer '
                   'once in RPCS3 with update 1.19, then import again.' % (d2, ', '.join(missing)))
        return out
    want = accepted_net18_sha256()
    have = sha256_file(os.path.join(d2, 'net18.bin'))
    if want and have not in want:
        out.append('the game data folder %s holds a net18.bin version this recipe does not know yet (sha256 %s): the '
                   'menus and modes cannot be converted from it. Report this hash so the version can be added; '
                   'until then Uncharted 3 starts without multiplayer.' % (d2, have))
    return out


def scan_dirs(roots, max_depth=4):
    """Yield every directory under roots up to max_depth (roots themselves included)."""
    for r in roots:
        r = os.path.abspath(r)
        if not os.path.isdir(r):
            continue
        base_depth = r.rstrip('\\/').count(os.sep)
        for dp, dn, fn in os.walk(r):
            yield dp
            if dp.count(os.sep) - base_depth >= max_depth:
                dn[:] = []
            else:
                dn[:] = [d for d in dn if d.lower() not in ('movie1', 'gallery1', 'cinema23', 'cinema5', 'users', 'http-cache')]


class Inputs:
    """Validated input folders.  Attributes: ps4, ps4_title_id, ps3 (dict kind -> USRDIR path), ps3_ids, u2, u2_title_id."""

    def __init__(self):
        self.ps4 = None
        self.ps4_sfo = {}
        self.ps3 = {}
        self.ps3_sfo = {}
        self.u2 = None
        self.u2_sfo = {}
        self.problems = []
        self.notes = []

    # ---- PS4
    def set_ps4(self, folder):
        folder = os.path.abspath(folder)
        need = ('eboot.bin', 'sce_sys/param.sfo', 'u3data/build/main/pak23_ps4.txt', 'u3data/build/main/core.psarc',
                'u3data/build/main/dc1/ps4', 'u3data/build/main/pak23/ps4', 'u3data/build/main/actor33/ps4',
                'u3data/build/main/sound1/streams', 'u2data/build/main/sound1/streams')
        missing = [n for n in need if not os.path.exists(os.path.join(folder, n))]
        if missing:
            self.problems.append('PS4 folder %s is missing: %s' % (folder, ', '.join(missing)))
            return
        self.ps4 = folder
        try:
            self.ps4_sfo = parse_sfo(os.path.join(folder, 'sce_sys', 'param.sfo'))
        except Exception as e:
            self.problems.append('PS4 param.sfo unreadable: %s' % e)
        tid = str(self.ps4_sfo.get('TITLE_ID', ''))
        if not re.match(r'^CUSA\d{5}$', tid):
            self.problems.append('PS4 param.sfo title id %r does not look like a PS4 title (CUSAnnnnn)' % tid)
        title = str(self.ps4_sfo.get('TITLE', '')).lower()
        if 'uncharted' not in title:
            self.notes.append('PS4 title is %r (expected an Uncharted collection); continuing' % self.ps4_sfo.get('TITLE'))

    @property
    def ps4_title_id(self):
        return str(self.ps4_sfo.get('TITLE_ID', 'CUSA?????'))

    @property
    def ps4_main(self):
        return os.path.join(self.ps4, 'u3data', 'build', 'main')

    # ---- PS3 U3 (several folders, one per kind; folders may also be scanned)
    def add_ps3_u3(self, folders, scan=True):
        cands = list(scan_dirs(folders)) if scan else [os.path.abspath(f) for f in folders]
        for d in cands:
            if os.path.basename(d).upper() != 'USRDIR' and not _has_any(d, ('build/main', 'dlc-patch18.psarc', 'net18.bin')):
                continue
            for kind in classify_ps3_u3(d):
                if kind in self.ps3:
                    continue
                sfo = find_sfo(d)
                info = {}
                if sfo:
                    try:
                        info = parse_sfo(sfo)
                    except Exception:
                        info = {}
                title = str(info.get('TITLE', '')).lower().replace('’', "'")
                tid = str(info.get('TITLE_ID', ''))
                if title and not any(w in title for w in U3_WORDS):
                    self.notes.append('%s: PARAM.SFO title %r is not Uncharted 3, folder skipped' % (d, info.get('TITLE')))
                    continue
                self.ps3[kind] = d
                self.ps3_sfo[kind] = {'TITLE_ID': tid, 'TITLE': info.get('TITLE', ''), 'APP_VER': info.get('APP_VER', ''), 'sfo': sfo}
        if 'dlcmaps' not in self.ps3 and 'disc' in self.ps3 and \
                os.path.exists(os.path.join(self.ps3['disc'], 'goty-dlc.psarc')):
            # Game of the Year disc: goty-dlc.psarc carries the DLC map packs' level paks (109 of the 110 the plan
            # uses are byte-identical; checked 2026-11-06), so the disc stands in for dlc-map1..4.psarc
            self.ps3['dlcmaps'] = self.ps3['disc']
            self.ps3_sfo['dlcmaps'] = dict(self.ps3_sfo.get('disc', {}))
            self.notes.append('DLC map packs: not installed separately; using the Game of the Year disc (goty-dlc.psarc)')
        for kind in ('disc', 'update'):
            if kind not in self.ps3:
                self.problems.append('no PS3 Uncharted 3 %s found (%s)' % (kind, PS3_U3_KINDS[kind][0]))
        for kind in ('data2', 'dlcmaps'):
            if kind not in self.ps3:
                self.notes.append('no PS3 Uncharted 3 %s found; the steps that need it will be skipped' % kind)

    # ---- PS3 U2
    def set_ps3_u2(self, folders, scan=True):
        cands = list(scan_dirs(folders)) if scan else [os.path.abspath(f) for f in folders]
        for d in cands:
            if _has_any(d, ('build/main/pak22.psarc', 'build/main/actor27.psarc', 'build/main/dc1.psarc')):
                sfo = find_sfo(d)
                info = {}
                if sfo:
                    try:
                        info = parse_sfo(sfo)
                    except Exception:
                        info = {}
                title = str(info.get('TITLE', '')).lower()
                if title and not any(w in title for w in U2_WORDS):
                    continue
                self.u2 = d
                self.u2_sfo = {'TITLE_ID': info.get('TITLE_ID', ''), 'TITLE': info.get('TITLE', ''), 'APP_VER': info.get('APP_VER', ''), 'sfo': sfo}
                return
        self.problems.append('no PS3 Uncharted 2 game data found (a PS3_GAME/USRDIR or RPCS3 USRDIR with build/main/pak22.psarc or actor27.psarc)')

    # ---- language of the speech streams: every <lang>-mp.psarc the disc has
    def speech_langs(self):
        d = self.ps3.get('disc')
        if not d:
            return []
        sd = os.path.join(d, 'build', 'main', 'sound1', 'streams', 'speech')
        return sorted(f[:-len('-mp.psarc')] for f in os.listdir(sd) if f.endswith('-mp.psarc')) if os.path.isdir(sd) else []

    def placeholders(self, out):
        """Values for the {{...}} placeholders of the fix scripts."""
        out = os.path.abspath(out)
        return {
            'PROJECT': norm(out),                                   # P (project root): <out>/work/..., <out>/scratch/...
            'WORK': norm(os.path.join(out, 'work')),
            'SCRATCH': norm(os.path.join(out, 'scratch')),
            'PS4': norm(self.ps4) if self.ps4 else '',
            'PS3_DISC': norm(self.ps3.get('disc', '')),
            'PS3_UPDATE': norm(self.ps3.get('update', '')),
            'PS3_DATA2': norm(self.ps3.get('data2', '')),
            'PS3_DLCMAPS': norm(self.ps3.get('dlcmaps', '')),
            'PS3_U2': norm(self.u2 or ''),
            'PAKTOOLS': norm(os.path.join(TOOLS, 'paktools')),
            'TRANSFER_TOOLS': norm(os.path.join(TOOLS, 'transfer-tools')),
            'RECOVERED': norm(os.path.join(out, 'scratch')),        # rendered copies of the tools/fixes helper modules
            'PYTHON': norm(sys.executable),
        }

    def describe(self):
        lines = ['Inputs:']
        if self.ps4:
            lines.append('  PS4  %s  [%s %s]' % (self.ps4, self.ps4_title_id, self.ps4_sfo.get('TITLE', '')))
        for k in PS3_U3_KINDS:
            if k in self.ps3:
                s = self.ps3_sfo[k]
                lines.append('  PS3 U3 %-8s %s  [%s %s %s]' % (k, self.ps3[k], s['TITLE_ID'], str(s['TITLE']).split('\n')[0], s['APP_VER']))
        if self.u2:
            s = self.u2_sfo
            lines.append('  PS3 U2 %-8s %s  [%s %s %s]' % ('', self.u2, s['TITLE_ID'], str(s['TITLE']).split('\n')[0], s['APP_VER']))
        for n in self.notes:
            lines.append('  note: ' + n)
        for p in self.problems:
            lines.append('  PROBLEM: ' + p)
        return '\n'.join(lines)


# ----------------------------------------------------------------------------- steps
def load_steps():
    """Steps in the order of steps/ORDER.txt (one id per line, # comments)."""
    order = []
    for line in open(os.path.join(STEPS, 'ORDER.txt'), encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if line:
            order.append(line)
    files = {}
    for f in os.listdir(STEPS):
        if f.endswith('.json'):
            s = json.load(open(os.path.join(STEPS, f), encoding='utf-8'))
            s['_file'] = f
            files[s['id']] = s
    missing = [i for i in order if i not in files]
    if missing:
        raise SystemExit('steps/ORDER.txt names steps without a json file: %s' % missing)
    unlisted = [i for i in files if i not in order]
    if unlisted:
        print('warning: step files not in ORDER.txt (ignored): %s' % unlisted)
    return [files[i] for i in order]


class State:
    def __init__(self, out):
        self.path = os.path.join(out, '_recipe', 'state.json')
        self.d = json.load(open(self.path, encoding='utf-8')) if os.path.exists(self.path) else {'recipe_version': VERSION, 'steps': {}}

    def save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + '.tmp'
        json.dump(self.d, open(tmp, 'w', encoding='utf-8'), indent=1)
        os.replace(tmp, self.path)

    def done(self, step_id):
        return self.d['steps'].get(step_id)

    def mark(self, step_id, outputs, status='ok', note='', fp=''):
        self.d['steps'][step_id] = {'status': status, 'when': time.strftime('%Y-%m-%d %H:%M:%S'), 'recipe_version': VERSION,
                                    'outputs': outputs, 'note': note, 'fp': fp}
        self.save()


_TREE_FP = {}


def _tree_fp(folder):
    """sha256 of a tool folder's files (relative path + content; text files with \\n line ends, so a CRLF checkout
    gives the same value)."""
    if folder in _TREE_FP:
        return _TREE_FP[folder]
    h = hashlib.sha256()
    if os.path.isdir(folder):
        for dp, dn, fn in os.walk(folder):
            dn[:] = sorted(d for d in dn if d != '__pycache__')
            for f in sorted(fn):
                if f.endswith(('.pyc', '.orig')):
                    continue
                p = os.path.join(dp, f)
                b = open(p, 'rb').read()
                if f.endswith(('.py', '.sh', '.txt', '.json', '.tsv', '.md', '.cmd', '.c')):
                    b = b.replace(b'\r\n', b'\n')
                h.update(os.path.relpath(p, folder).replace('\\', '/').encode() + b'\0' + hashlib.sha256(b).digest())
    _TREE_FP[folder] = h.hexdigest()
    return _TREE_FP[folder]


def step_fingerprint(s):
    """What a step runs: its step file and, for script steps, its fix job folder(s) and the shared helper modules of
    tools/fixes; for copy steps, its list.  A recorded step whose fingerprint differs runs again (an update)."""
    h = hashlib.sha256(json.dumps({k: v for k, v in s.items() if not k.startswith('_')}, sort_keys=True).encode())
    if s.get('kind') == 'script':
        for j in [s['job']] + list(s.get('with_jobs', [])):
            h.update(_tree_fp(os.path.join(TOOLS, 'fixes', j)).encode())
        fx = os.path.join(TOOLS, 'fixes')
        for f in sorted(os.listdir(fx)) if os.path.isdir(fx) else []:
            if f.endswith('.py') and os.path.isfile(os.path.join(fx, f)):
                h.update(hashlib.sha256(open(os.path.join(fx, f), 'rb').read().replace(b'\r\n', b'\n')).digest())
    elif s.get('list'):
        p = os.path.join(LISTS, s['list'])
        if os.path.exists(p):
            h.update(hashlib.sha256(open(p, 'rb').read().replace(b'\r\n', b'\n')).digest())
    return h.hexdigest()


def outputs_unchanged(out, rec):
    """True when every file a step recorded still has the recorded hash (so the step is already applied)."""
    for rel, h in rec.get('outputs', {}).items():
        p = os.path.join(out, rel)
        if h is None:
            if os.path.exists(p):
                return False
            continue
        if not os.path.exists(p) or sha256_file(p) != h:
            return False
    return True


# ----------------------------------------------------------------------------- rendering fix jobs
PLACEHOLDER_RE = re.compile(r'\{\{([A-Z0-9_]+)\}\}')


def fill(text, values, where, log):
    """Replace {{PLACEHOLDERS}}.  Inside a single-quoted Python literal an apostrophe in the value is escaped as \\'
    (a path like "Drake's Deception" would otherwise end the literal); double-quoted literals need nothing."""
    unknown = sorted(set(PLACEHOLDER_RE.findall(text)) - set(values))
    if unknown:
        log('  warning: %s has unknown placeholders %s' % (where, unknown))

    def repl(m):
        k = m.group(1)
        if k not in values:
            return m.group(0)
        v = values[k]
        if "'" in v:
            line_start = text.rfind('\n', 0, m.start()) + 1
            before = text[line_start:m.start()]
            q = max(before.rfind("'"), before.rfind('"'))
            if q >= 0 and before[q] == "'":
                if q > 0 and before[q - 1] in 'rR':
                    log('  warning: %s: raw single-quoted literal cannot hold %r' % (where, v))
                v = v.replace("'", "\\'")
        return v
    return PLACEHOLDER_RE.sub(repl, text)


def render_job(job, out, values, log):
    """Copy tools/fixes/<job> to <out>/scratch/<job>, filling the {{PLACEHOLDERS}} of every text file.
    The top-level helper modules of tools/fixes (mp1.py, dc.py, ...) are rendered into <out>/scratch/ as well."""
    src = os.path.join(TOOLS, 'fixes', job)
    dst = os.path.join(out, 'scratch', job)
    if not os.path.isdir(src):
        raise FileNotFoundError('fix job folder missing: %s' % src)
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(os.path.join(out, 'scratch'), exist_ok=True)
    for f in os.listdir(os.path.join(TOOLS, 'fixes')):
        s = os.path.join(TOOLS, 'fixes', f)
        if os.path.isfile(s) and f.endswith('.py'):
            t = open(s, encoding='utf-8', errors='surrogateescape').read()
            open(os.path.join(out, 'scratch', f), 'w', encoding='utf-8', errors='surrogateescape', newline='\n').write(fill(t, values, f, log))
    for dp, dn, fn in os.walk(src):
        rel = os.path.relpath(dp, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in fn:
            s = os.path.join(dp, f)
            d = os.path.join(dst, rel, f)
            if f.endswith(('.py', '.sh', '.txt', '.json', '.tsv', '.md', '.cmd')):
                t = open(s, encoding='utf-8', errors='surrogateescape').read()
                open(d, 'w', encoding='utf-8', errors='surrogateescape', newline='\n').write(fill(t, values, job + '/' + f, log))
            else:
                shutil.copy2(s, d)
    return dst
