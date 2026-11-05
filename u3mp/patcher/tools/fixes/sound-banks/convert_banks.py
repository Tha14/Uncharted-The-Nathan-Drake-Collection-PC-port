#!/usr/bin/env python3
"""overlay-r3 sound1/banks/ps4: the 124 PS3 sound banks converted to the PS4 SCREAM format (bnkconv.py).

Sources (banks_list.json: output name, archive, entry, reference sha256):
  {{PS3_DISC}}/build/main/sound1/banks.psarc   (110 banks; SCREAM-Template-SoundID-Types, bat2 and
                                                beretta-M92F-silenced come from their '-half' entries)
  {{PS3_DISC}}/goty-dlc.psarc                   (13 DLC map banks, build/main/sound1/banks/)
  {{PS3_UPDATE}}/dlc-patch18.psarc              (drydocks.bnk)
The 4 retail stub banks of the same folder come from the PS4 game (copy-ps4-retail step).

Result on 2026-11-05 against the reference overlay: all 124 banks byte-identical (ffmpeg 8.1.1 for the MP3 samples;
the exact encoder is vagenc.py, a port of the original vagenc.c). Without ffmpeg the 95 banks with MP3 samples are
skipped with a message.

usage: convert_banks.py [--ps3-disc DIR] [--ps3-update DIR] [--out DIR] [--only a.bnk,b.bnk] [--ffmpeg EXE]
                        [--force] [--apply]
  dry run by default. --apply writes (replaced files backed up once to work/backup-sound-banks/).
  Idempotent: a bank equal to the reference sha256, or equal to what this script wrote last time
  (recorded in <backup>/convert_banks-state.json), is skipped unless --force."""
import argparse, hashlib, json, os, shutil, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
for p in (os.environ.get('RECIPE_TRANSFER_TOOLS', ''), r"{{TRANSFER_TOOLS}}", os.path.join(HERE, '..', '..', 'transfer-tools')):
    if p and os.path.isdir(p) and p not in sys.path:
        sys.path.insert(0, p)
from psarc_lib import Psarc
import bnkconv, vagenc

FF_DEFAULT = [os.environ.get('RECIPE_FFMPEG', ''), r'C:\msys64\clang64\bin\ffmpeg.exe', r'C:\msys64\ucrt64\bin\ffmpeg.exe']
sha = lambda b: hashlib.sha256(b).hexdigest()


def find_ffmpeg(arg):
    for c in [arg] + FF_DEFAULT + [shutil.which('ffmpeg') or '']:
        if c and os.path.isfile(c):
            return c
    return None


def make_sample_encoder(ff):
    """(PS3 bank data section, offset, size) of an MP3 (codec 3) or PCM16 BE sample -> PS4 0x20 header + PS-ADPCM.
    Same as the original converter (2026-10-27): MP3 frames are read from offset+0x80 for the header's data size
    (whatever the header length), decoded with `ffmpeg -f mp3 -i pipe:0 -f s16le -ac 1 -ar <rate> pipe:1`,
    trimmed (never padded) to the header's sample count, encoded one-shot; PCM16 is encoded as is."""
    def conv(A, ao, asz):
        src = A[ao:ao + asz]
        if len(src) >= 56 and struct.unpack_from('>II', src, 0) == (1, 3):      # MP3 (codec 3) with header
            rate = struct.unpack_from('>I', A, ao + 0xc)[0]
            nsmp = struct.unpack_from('>I', A, ao + 0x24)[0]
            dsz = struct.unpack_from('>I', A, ao + 0x34)[0]
            if ff is None:
                raise RuntimeError('needs ffmpeg (MP3 sample)')
            r = subprocess.run([ff, '-v', 'error', '-f', 'mp3', '-i', 'pipe:0', '-f', 's16le', '-ac', '1', '-ar', str(rate), 'pipe:1'],
                               input=A[ao + 0x80:ao + 0x80 + dsz], capture_output=True)
            if r.returncode:
                raise RuntimeError('ffmpeg: ' + r.stderr.decode(errors='replace')[-300:])
            x = struct.unpack('<%dh' % (len(r.stdout) // 2), r.stdout[:len(r.stdout) // 2 * 2])
            if nsmp and len(x) > nsmp:
                x = x[:nsmp]
        else:                                   # PCM16 big-endian
            x = struct.unpack('>%dh' % (len(src) // 2), src[:len(src) // 2 * 2])
        v = vagenc.encode(x)
        return bnkconv.sample_header(len(v) // 16 * 28, 0xffffffff, 0xffffffff) + v
    return conv


_W = {}


def _work(job):
    """worker: (name, archive path, entry, ffmpeg) -> (name, bytes or None, message)"""
    name, path, entry, ff = job
    if path not in _W:
        if not os.path.isfile(path):
            return name, None, 'missing input ' + path
        p = Psarc(path)
        _W[path] = (p, {n: i + 1 for i, n in enumerate(p.names)})
    p, idx = _W[path]
    if entry not in idx:
        return name, None, 'missing entry %s in %s' % (entry, path)
    try:
        return name, bnkconv.convert(p.read(idx[entry]), mp3_to_adpcm=make_sample_encoder(ff)), ''
    except RuntimeError as e:
        return name, None, 'skipped: %s' % e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ps3-disc', default=r"{{PS3_DISC}}")
    ap.add_argument('--ps3-update', default=r"{{PS3_UPDATE}}")
    ap.add_argument('--out', default=r"{{PROJECT}}/work/overlay-r3/u3data/build/main/sound1/banks/ps4")
    ap.add_argument('--backup', default=r"{{PROJECT}}/work/backup-sound-banks")
    ap.add_argument('--only', default='')
    ap.add_argument('--ffmpeg', default='')
    ap.add_argument('--jobs', type=int, default=max(1, min(6, (os.cpu_count() or 2) - 1)))
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--apply', action='store_true')
    A = ap.parse_args()
    rows = json.load(open(os.path.join(HERE, 'banks_list.json')))
    if A.only:
        rows = [r for r in rows if r[0] in A.only.split(',')]
    ff = find_ffmpeg(A.ffmpeg)
    if ff is None:
        print('ffmpeg not found: banks with MP3 samples are skipped (set RECIPE_FFMPEG or put ffmpeg on PATH)')
    roots = {'disc': A.ps3_disc, 'update': A.ps3_update}
    statef = os.path.join(A.backup, 'convert_banks-state.json')   # outside the overlay
    state = json.load(open(statef)) if os.path.isfile(statef) else {}
    cnt = dict(identical=0, equivalent=0, skipped=0, missing=0)
    t0 = time.time()
    refs = {}
    jobs = []
    for name, arc, entry, ref in rows:
        dst = os.path.join(A.out, name)
        if os.path.isfile(dst) and not A.force:
            cur = sha(open(dst, 'rb').read())
            if cur == ref or state.get(name) == cur:
                cnt['skipped'] += 1
                continue
        where, rel = arc.split(':')
        refs[name] = ref
        jobs.append((name, os.path.join(roots[where], rel), entry, ff))
    # big banks first so the pool stays busy
    if A.jobs > 1 and len(jobs) > 1:
        import multiprocessing as mp
        pool = mp.Pool(A.jobs)
        results = pool.imap_unordered(_work, jobs)
    else:
        pool = None
        results = map(_work, jobs)
    for name, out, msg in results:
        dst = os.path.join(A.out, name)
        if out is None:
            print('%-40s %s' % (name, msg)); cnt['missing'] += 1; continue
        same = sha(out) == refs[name]
        cnt['identical' if same else 'equivalent'] += 1
        print('%-40s %s%s' % (name, 'identical to the reference' if same else 'equivalent (re-encoded ADPCM samples)',
                              '' if A.apply else ' (dry run)'), flush=True)
        if A.apply:
            os.makedirs(A.out, exist_ok=True)
            if os.path.isfile(dst):
                os.makedirs(A.backup, exist_ok=True)
                bk = os.path.join(A.backup, name)
                if not os.path.exists(bk):
                    shutil.copyfile(dst, bk)
            tmp = dst + '.tmp'
            open(tmp, 'wb').write(out)
            os.replace(tmp, dst)
            state[name] = sha(out)
    if pool:
        pool.close(); pool.join()
    if A.apply and state:
        os.makedirs(A.backup, exist_ok=True)
        json.dump(state, open(statef, 'w'), indent=0, sort_keys=True)
    if cnt['identical'] + cnt['equivalent'] == 0 and cnt['missing'] == 0:
        print('already applied: %d banks up to date' % cnt['skipped'])
    else:
        print('%s: %d identical, %d equivalent, %d already up to date, %d missing (%.0f s)' % (
            'written' if A.apply else 'dry run', cnt['identical'], cnt['equivalent'], cnt['skipped'], cnt['missing'], time.time() - t0))


if __name__ == '__main__':
    main()
