"""The Multiplayer tab's opt-in "keep only the PS3 files updates need" (only to save disk space).

<data>/ps3-kept holds just the PS3 files the import reads, in the layout of the player's PS3 folders:
  ps3-kept/<id>/<folder name>/...   one per PS3 folder (id = folder name + hash of its path), e.g. .../USRDIR/...
  ps3-kept/<id>/PARAM.SFO           a file next to that folder (PS3_GAME/PARAM.SFO beside USRDIR)
  ps3-kept/elf/EBOOT.elf            the decrypted EBOOT.elf when it is not in a PS3 folder
  ps3-kept/manifest.json            what is kept, how (hard link or verified copy), complete or not
Which files: the ones every import since this version recorded (<data>/ps3-reads.txt, written by ps3reads/) plus
ps3-needed.tsv (what a full import reads, traced 2026-11-09), so an import made before this version is covered too.
On the same drive the files are hard links (no extra space while the PS3 folders exist; deleting those folders then
frees everything else); on another drive they are copied and compared with the source.
Later imports add the kept folders after the player's own (u3mp_import.py): the player's folders win while they exist."""
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
KEPT = 'ps3-kept'
READS = 'ps3-reads.txt'
NEEDED = HERE / 'ps3-needed.tsv'
READS_DIR = HERE / 'ps3reads'


def root_id(root):
    r = os.path.abspath(root).rstrip('\\/')
    return '%s-%s' % (os.path.basename(r) or 'root', hashlib.sha256(os.path.normcase(r).encode('utf-8')).hexdigest()[:8])


def manifest(data):
    try:
        return json.loads((Path(data) / KEPT / 'manifest.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def kept_roots(data):
    """The kept folders to add after the player's own (complete kept copies only)."""
    m = manifest(data)
    if not m.get('complete'):
        return []
    out = [str(Path(data) / KEPT / rid / name) for rid, name in sorted(m.get('roots', {}).items())]
    if (Path(data) / KEPT / 'elf').is_dir():
        out.append(str(Path(data) / KEPT / 'elf'))
    return [p for p in out if os.path.isdir(p)]


def record(roots, data, env):
    """Start recording the PS3 files this import opens: in this process and (env) in the processes it starts."""
    roots = [os.path.abspath(r) for r in roots if r and os.path.isdir(r)]
    if not roots:
        return
    log_path = str(Path(data) / READS)
    env['U3MP_PS3_ROOTS'] = os.pathsep.join(roots)
    env['U3MP_PS3_READS'] = log_path
    env['PYTHONPATH'] = os.pathsep.join([str(READS_DIR)] + ([env['PYTHONPATH']] if env.get('PYTHONPATH') else []))
    if str(READS_DIR) not in sys.path:
        sys.path.insert(0, str(READS_DIR))
    import u3mp_ps3reads
    u3mp_ps3reads.install(roots, log_path)


def _needed(roots):
    """ps3-needed.tsv mapped onto the PS3 folders found in roots: absolute paths (missing ones left out)."""
    try:
        import recipe_lib as R
    except ImportError:
        sys.path.insert(0, str(HERE / 'recipe'))
        import recipe_lib as R
    inp = R.Inputs()
    inp.add_ps3_u3(roots)
    out = set()
    if not NEEDED.exists():
        return out, inp
    for line in NEEDED.read_text(encoding='utf-8').splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        kind, rel = line.split('\t', 1)
        d = inp.ps3.get(kind)
        if d:
            out.add(os.path.normpath(os.path.join(d, rel)))
    for d in inp.ps3.values():          # every folder's PARAM.SFO (names the inputs in the log)
        sfo = R.find_sfo(d)
        if sfo:
            out.add(os.path.abspath(sfo))
    # no Game of the Year disc (its goty-dlc.psarc stands in for the DLC map packs): keep the packs installed on their own
    d = inp.ps3.get('dlcmaps')
    if d and d != inp.ps3.get('disc') and not os.path.exists(os.path.join(inp.ps3.get('disc', ''), 'goty-dlc.psarc')):
        for n in range(1, 5):
            out.add(os.path.join(d, 'dlc-map%d.psarc' % n))
    return out, inp


def _place(p, roots, elf):
    """Where p goes under ps3-kept (relative), and the (id, folder name) of its PS3 folder; None when not kept."""
    q = os.path.normcase(p)
    for r in roots:
        rn = os.path.normcase(r)
        if q.startswith(rn + os.sep):
            rid, name = root_id(r), os.path.basename(r)
            return '%s/%s/%s' % (rid, name, os.path.relpath(p, r).replace('\\', '/')), (rid, name)
    for r in roots:
        if os.path.normcase(os.path.dirname(p)) == os.path.normcase(os.path.dirname(r)):
            rid, name = root_id(r), os.path.basename(r)
            return '%s/%s' % (rid, os.path.basename(p)), (rid, name)
    if elf and os.path.normcase(os.path.abspath(elf)) == q:
        return 'elf/' + os.path.basename(p), None
    return None, None


def _sha(path, chunk=8 << 20):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(chunk), b''):
            h.update(b)
    return h.hexdigest()


def _copy_verified(src, dst):
    tmp = dst + '.part'
    h = hashlib.sha256()
    with open(src, 'rb') as fi, open(tmp, 'wb') as fo:
        for b in iter(lambda: fi.read(8 << 20), b''):
            h.update(b)
            fo.write(b)
    want = h.hexdigest()
    if _sha(tmp) != want:
        os.remove(tmp)
        raise OSError('the copy of %s differs from the original' % src)
    os.replace(tmp, dst)
    shutil.copystat(src, dst)
    return want


def keep(data, roots, elf, log=print, event=None):
    """Make or refresh <data>/ps3-kept. Returns the state.json 'ps3_kept' record."""
    data = Path(data)
    kept = data / KEPT
    roots = [os.path.abspath(r).rstrip('\\/') for r in roots if r and os.path.isdir(r)
             and not os.path.normcase(os.path.abspath(r)).startswith(os.path.normcase(str(kept)) + os.sep)]
    m = manifest(data)
    m.setdefault('roots', {})
    m.setdefault('files', {})
    want = set()
    if (data / READS).exists():
        want |= {l.strip() for l in (data / READS).read_text(encoding='utf-8', errors='replace').splitlines() if l.strip()}
    needed, inp = _needed(roots)
    want |= needed
    if elf and os.path.isfile(elf):
        want.add(os.path.abspath(elf))
    plan = []
    for p in sorted(want):
        if not os.path.isfile(p):
            continue
        rel, root = _place(p, roots, elf)
        if rel:
            plan.append((p, rel, root))
    if not plan and not m.get('complete'):
        return dict(complete=False, error='no PS3 folder found to keep files from')
    # space for the files that cannot be hard links (another drive)
    kept.mkdir(parents=True, exist_ok=True)
    same_drive = {}
    need = 0
    for p, rel, _r in plan:
        dst = kept / rel
        drv = os.path.splitdrive(p)[0].lower()
        if drv not in same_drive:
            same_drive[drv] = drv == os.path.splitdrive(str(kept))[0].lower()
        if not same_drive[drv] and not dst.exists():
            need += os.path.getsize(p)
    free = shutil.disk_usage(str(kept)).free
    if need > free - 1e9:
        return dict(m.get('state', {}), complete=bool(m.get('complete')),
                    error='%.1f GB to copy, %.1f GB free on %s' % (need / 1e9, free / 1e9, kept.anchor))
    if event:
        event(phase='ps3keep', status='running', note='Keeping the PS3 files the import needs (%d files)...' % len(plan))
    log('PS3 files: keeping %d file(s) in %s' % (len(plan), kept))
    linked = copied = same = 0
    for p, rel, root in plan:
        dst = kept / rel
        st = os.stat(p)
        rec = m['files'].get(rel, {})
        if dst.exists():
            try:
                if os.path.samefile(p, dst):
                    same += 1
                    m['files'][rel] = dict(size=st.st_size, how='link')
                    if root:
                        m['roots'][root[0]] = root[1]
                    continue
            except OSError:
                pass
            if dst.stat().st_size == st.st_size and rec.get('mtime') == st.st_mtime_ns and rec.get('sha256'):
                same += 1
                if root:
                    m['roots'][root[0]] = root[1]
                continue
            dst.unlink()
        dst.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.link(p, dst)
            m['files'][rel] = dict(size=st.st_size, how='link')
            linked += 1
        except OSError:
            m['files'][rel] = dict(size=st.st_size, how='copy', mtime=st.st_mtime_ns, sha256=_copy_verified(p, str(dst)))
            copied += 1
        if root:
            m['roots'][root[0]] = root[1]
    # files kept earlier whose PS3 original is gone stay (they are then the only copy)
    missing = [rel for rel in m['files'] if not (kept / rel).is_file()]
    for rel in missing:
        m['files'].pop(rel)
    # the kept folders alone must be found as the same PS3 inputs
    try:
        import recipe_lib as R
        alone = R.Inputs()
        alone.add_ps3_u3([str(kept / rid / name) for rid, name in m['roots'].items()])
        lost = sorted(set(inp.ps3) - set(alone.ps3))
    except Exception as e:      # noqa: BLE001 - reported
        lost = ['check failed: %s' % e]
    size = sum(f['size'] for f in m['files'].values())
    extra = sum(f['size'] for f in m['files'].values() if f.get('how') == 'copy')
    m['complete'] = not lost
    m['state'] = dict(complete=m['complete'], files=len(m['files']), size=size, copied_size=extra,
                      folder=str(kept), error=('the kept files are not found as: %s' % ', '.join(lost)) if lost else '')
    (kept / 'manifest.json').write_text(json.dumps(m, indent=1), encoding='utf-8')
    log('PS3 files: %d kept (%.1f GB; %d new hard links, %d verified copies, %d unchanged)%s'
        % (len(m['files']), size / 1e9, linked, copied, same, '' if not lost else '; NOT complete: ' + m['state']['error']))
    if event:
        event(phase='ps3keep', status='ok' if not lost else 'failed',
              note=('PS3 files kept: %.1f GB. Your PS3 Uncharted 3 folders are no longer needed.' % (size / 1e9))
              if not lost else 'PS3 files: ' + m['state']['error'])
    return m['state']


def drop(data, log=print):
    kept = Path(data) / KEPT
    if kept.exists():
        shutil.rmtree(kept)
        log('PS3 files: the kept copy was removed (%s)' % kept)
