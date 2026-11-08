"""u2mp_import.py - bring the Uncharted 2 multiplayer assets the PS4 Collection lacks in from the player's own PS3
copy, converted to the PS4 format, as an overlay folder.

  python u2mp_import.py --ps4 <PS4 game folder> --ps3 <PS3 base folder> [--ps3-dlc <PS3 DLC folder>] --out <folder>
                        [--groups A,B,C,D,E] [--speech-langs english,french,...] [--dlc-ruins] [--drop-navmesh]
                        [--only REGEX] [--validate] [--force]

  --ps4      the Collection's game folder (holds u2data/) or u2data itself
  --ps3      PS3 Uncharted 2 (e.g. NPUA80698): USRDIR, its parent or build/main (archives + all-patches.psarc)
  --ps3-dlc  folder with dlc-map1/2.psarc and dlc-patch*.psarc (RPCS3 dev_hdd0/game/BCUS98123/USRDIR/build/main or
             the USRDIR); without it the map packs (groups C, D) and Pinkerton/Rika are skipped unless the base patch
             carries them
  --out      overlay folder: <out>/u2data/build/main/... (only converted files and the registries), plus
             <out>/state.json, <out>/import.log, <out>/work (caches, removable)

Rule: what the PS4 game has (skin, map, picture, sound ...) stays the PS4 one; only what it lacks is imported. Every
row of u2mp/docs/import-list.tsv (group, key, PS3 source, size, SHA-1) is checked against the PS4 game first; PS3
inputs are verified by SHA-1. Groups: A skins, B the Fort's missing pieces, C map pack 1, D map pack 2, E base
extras (props, banks, music, DLC character voices), F optional (not referenced by PS4; only with --groups F).

Resumable: a converted file whose inputs and converter version are unchanged is not converted again (state.json).
Progress for the launcher: lines '@U2MP {json}'. Nothing is written outside --out. No game data ships with this
script; the import list holds names, sizes and hashes only.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'u2conv'))
sys.dont_write_bytecode = True

CONV_VERSION = '1'        # bump when a converter changes its output: those files are converted again
SKIN_VERSION = 'twin2'    # mp-hero skins: PS4-twin build (u2twin) first; skins converted before it are redone
ACTOR_VERSION = 'u2ag1'   # actor27 paks: U2 artgroup / lookup / LEVEL_PATS layout, props, anims (u2actorparts)
IMPORT_LIST = os.path.join(HERE, '..', 'docs', 'import-list.tsv')
# PS3 speech language -> PS4 speech archive (<name>_ps4.psarc). NPUA80698 is the North American release: its
# Spanish / Portuguese tracks are taken as the Latin American Spanish / Brazilian Portuguese the PS4 game has.
SPEECH_LANGS = {'english': 'english', 'french': 'french', 'spanish': 'sam-spanish', 'portuguese': 'brazilian'}
OUT_MAIN = os.path.join('u2data', 'build', 'main')

_log_file = None


def log(text):
    print(text, flush=True)
    if _log_file:
        _log_file.write(time.strftime('%H:%M:%S ') + text + '\n')
        _log_file.flush()


def event(**kw):
    print('@U2MP ' + json.dumps(kw), flush=True)


def sha1(b):
    return hashlib.sha1(b).hexdigest()


def read_list(path):
    rows = []
    with open(path, encoding='utf-8') as f:
        head = f.readline().rstrip('\n').split('\t')
        for line in f:
            v = line.rstrip('\n').split('\t')
            if len(v) >= len(head):
                rows.append(dict(zip(head, v)))
    return rows


class Importer:
    def __init__(self, a):
        self.a = a
        self.out = os.path.abspath(a.out)
        self.main = os.path.join(self.out, OUT_MAIN)
        self.work = os.path.join(self.out, 'work')
        self.state_file = os.path.join(self.out, 'state.json')
        try:
            self.state = json.load(open(self.state_file, encoding='utf-8'))
        except (OSError, ValueError):
            self.state = {}
        self.items = self.state.setdefault('items', {})
        self.ps3 = self.ps4 = None

    # ------------------------------------------------------------------ bookkeeping
    def save(self):
        tmp = self.state_file + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(self.state, f, indent=1, sort_keys=True)
        os.replace(tmp, self.state_file)

    def out_path(self, rel):
        p = os.path.join(self.main, *rel.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    def done_before(self, key, src_sha):
        it = self.items.get(key)
        if self.a.force or not it or it.get('conv') != CONV_VERSION or it.get('src_sha1') != src_sha:
            return False
        if key.lower().startswith('actor27/mp-hero') and it.get('skin') != SKIN_VERSION:
            return False
        if key.lower().startswith('actor27/') and it.get('actor') != ACTOR_VERSION:
            return False
        if it.get('status') != 'ok':
            return False
        o = os.path.join(self.out, it['out'])
        return os.path.isfile(o) and os.path.getsize(o) == it.get('size')

    def record(self, key, status, row, **kw):
        it = dict(status=status, group=row['group'], conv=CONV_VERSION, **kw)
        if key.lower().startswith('actor27/mp-hero'):
            it['skin'] = SKIN_VERSION
        if key.lower().startswith('actor27/'):
            it['actor'] = ACTOR_VERSION
        if 'out' in it:
            full = os.path.join(self.out, it['out'])
            it['size'] = os.path.getsize(full)
        self.items[key] = it

    # ------------------------------------------------------------------ inputs
    def source(self, row):
        """(entry, bytes) of the PS3 file for a row, verified by SHA-1; raises LookupError"""
        cands = self.ps3.candidates(row['key'])
        if not cands:
            raise LookupError('not in the given PS3 folders (needs %s)' % row['ps3_source'])
        want = row.get('sha1', '')
        mism = []
        for e in sorted(cands, key=lambda e: e.source != row['ps3_source']):
            data = e.read()
            if not want or self.a.no_verify or sha1(data) == want:
                return e, data
            mism.append(e.where())
        raise LookupError('SHA-1 differs from the import list in %s' % '; '.join(mism[:3]))

    def ps4_has(self, row):
        """'' when PS4 lacks the file, else why it stays the PS4 one"""
        key = row['key']
        k = key.lower()
        if k.startswith('sound1/speech/'):
            lang = k.split('/')[2]
            l4 = SPEECH_LANGS.get(lang)
            if l4 is None or l4 not in self.ps4.speech:
                return 'no PS4 speech language for %s' % lang
            if l4 not in self.langs:
                return 'language not selected'
            return 'PS4 %s speech has it' % l4 if k.split('/')[-1] in self.ps4.speech[l4] else ''
        if k.startswith('sound1/music/'):
            stem = os.path.basename(k)[:-5]
            refs = {r.lower() for r in self.music_refs}
            if self.ps4.has(k):
                return 'PS4 has it'
            if stem not in refs:
                return 'PS4 music settings do not use it (PS4 plays its own renumbered copy)'
            return ''
        if k in ('pak22/ruins-multi.pak', 'pak22/ruins-multi-ingame10.pak') and self.dlc_ruins:
            return ''
        if k.endswith('.txt') and k.startswith('pak22/'):
            return ''
        return 'PS4 has it' if self.ps4.has(k) else ''

    # ------------------------------------------------------------------ converters
    def conv_xvag(self, row, entry, data):
        import xvag
        k = row['key']
        if k.lower().startswith('sound1/speech/'):
            lang = SPEECH_LANGS[k.lower().split('/')[2]]
            rel = 'sound1/speech/%s/%s' % (lang, os.path.basename(entry.key))
        else:
            rel = '/'.join(k.split('/')[:-1]) + '/' + os.path.basename(entry.key)
        out = xvag.convert(data)
        p = self.out_path(rel)
        with open(p, 'wb') as f:
            f.write(out)
        return rel, []

    def havok_tables(self):
        """the Havok class tables (u2hkgen) are built from the player's executables on the first actor that needs
        them, into work/u2hkclasses.json; without them the Havok-dependent actors are skipped (unsupported)"""
        import u2havok
        import u2hkgen
        elf = self.a.ps3_elf or os.path.join(os.path.dirname(os.path.dirname(self.ps3.base_main)), 'EBOOT.elf')
        path = os.path.join(self.work, 'u2hkclasses.json')
        failed = []

        def build():
            if failed:
                raise u2hkgen.TablesMissing(failed[0])
            try:
                return u2hkgen.build(elf, self.a.ps4, self.ps4.actor_dir, path, self.a.objdump, log=log)
            except u2hkgen.TablesMissing as e:
                failed.append('Havok tables missing (%s): actor skipped' % e)
                log('Havok tables: ' + str(e) + '; the actors with Havok collision / ragdolls are skipped')
                raise u2hkgen.TablesMissing(failed[0])
        os.makedirs(self.work, exist_ok=True)
        u2havok.configure(path, build)

    def conv_bank(self, row, entry, data):
        import u2bank
        out, notes = u2bank.convert(data, ffmpeg=self.ffmpeg)
        rel = 'sound1/banks/ps4/' + os.path.basename(entry.key)
        with open(self.out_path(rel), 'wb') as f:
            f.write(out)
        return rel, notes

    def src_file(self, entry, data):
        d = os.path.join(self.work, 'src')
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, os.path.basename(entry.key))
        if not (os.path.isfile(p) and os.path.getsize(p) == len(data)):
            with open(p, 'wb') as f:
                f.write(data)
        return p

    def conv_actor(self, row, entry, data):
        import u2actor
        src = self.src_file(entry, data)
        pak = u2actor.U2Pak(src)
        why = u2actor.unsupported(pak)
        if why:
            raise NotImplementedError(why)
        if u2actor.kind(pak) != 'skin':             # props, animation paks, loose ragdolls (PS4 twins first)
            rel = 'actor27/ps4/' + os.path.basename(entry.key)
            out = self.out_path(rel)
            notes = u2actor.convert_prop(src, out + '.tmp', u2actor.kind(pak), self.ps4.actor_dir,
                                         os.path.basename(entry.key)[:-4].lower())
            errs = u2actor.check(out + '.tmp') if u2actor.kind(pak) == 'prop' else []
            if errs:
                os.remove(out + '.tmp')
                raise RuntimeError('ps4check: ' + '; '.join(errs[:3]))
            os.replace(out + '.tmp', out)
            return rel, notes
        twin = self.conv_twin(entry, src) if os.path.basename(entry.key).lower().startswith('mp-hero') else None
        if isinstance(twin, tuple):
            return twin
        tmpl, shared, total = u2actor.choose_template(src, self.ps4.actor_dir)
        rel = 'actor27/ps4/' + os.path.basename(entry.key)
        out = self.out_path(rel)
        u2actor.convert(src, tmpl, out + '.tmp')
        if twin is not None:            # skins: retail artgroup keys (u2twin.fix_artgroup)
            import u2twin
            u2twin.rekey_artgroup(out + '.tmp')
        errs = u2actor.check(out + '.tmp')
        if errs:
            os.remove(out + '.tmp')
            raise RuntimeError('ps4check: ' + '; '.join(errs[:3]))
        os.replace(out + '.tmp', out)
        return rel, (['PS3 (%s)' % twin] if twin is not None else []) +             ['template %s (%d of %d materials by name)' % (os.path.basename(tmpl), shared, total)]

    def conv_twin(self, entry, src):
        """a skin built from its PS4 campaign twin (u2twin): (rel, notes), or why not (str)"""
        import u2actor
        import u2twin
        name = os.path.basename(entry.key)[:-4].lower()
        twin = u2twin.TWINS.get(name)
        if not twin or not self.ps4.has('actor27/%s.pak' % twin):
            return 'no PS4 campaign twin'
        c = self.ps3.candidates('actor27/%s.pak' % twin)
        if not c:
            return 'no PS3 copy of the twin %s' % twin
        tsrc = self.src_file(c[0], c[0].read())
        rel = 'actor27/ps4/' + os.path.basename(entry.key)
        out = self.out_path(rel)
        try:
            n = u2twin.build(src, tsrc, self.ps4.files['actor27/%s.pak' % twin], out + '.tmp')
        except u2twin.NoTwin as e:
            return str(e)
        except Exception as e:          # the PS3 conversion still works: logged, not fatal
            for f in (out + '.tmp', out + '.tmp.conv'):
                if os.path.exists(f):
                    os.remove(f)
            return 'twin build error %s: %s' % (type(e).__name__, str(e)[:200])
        errs = u2actor.check(out + '.tmp')
        if errs:
            os.remove(out + '.tmp')
            return 'twin build failed ps4check: ' + '; '.join(errs[:2])
        os.replace(out + '.tmp', out)
        return rel, ['twin %s: %d/%d LOD0 submeshes from PS4 (%.0f%% of triangles), %d/%d materials from PS4' % (
            twin, n['swapped'], n['submeshes'], 100 * n['share'], n['twin_materials'], n['materials'])]

    def conv_level(self, row, entry, data):
        import u2level
        src = self.src_file(entry, data)
        rel = 'pak22/ps4/' + os.path.basename(entry.key)
        if self.res is None:
            self.res = u2level.build_res(self.ps4.pak_dir, os.path.join(self.work, 'res'), log=log)
        kind, notes = u2level.convert(src, self.out_path(rel), self.ps4.pak_dir, self.res,
                                      os.path.join(self.work, 'levels'), drop_navmesh=self.a.drop_navmesh, log=log)
        return rel, [kind] + notes

    # ------------------------------------------------------------------ main loop
    def run(self):
        a = self.a
        os.makedirs(self.out, exist_ok=True)
        global _log_file
        _log_file = open(os.path.join(self.out, 'import.log'), 'a', encoding='utf-8')
        log('=== u2mp import %s (converter %s)' % (time.strftime('%Y-%m-%d %H:%M:%S'), CONV_VERSION))
        self.state.update(status='running', started=time.strftime('%Y-%m-%d %H:%M:%S'), pid=os.getpid(),
                          inputs=dict(ps4=a.ps4, ps3=a.ps3, ps3_dlc=a.ps3_dlc or ''), conv=CONV_VERSION)
        self.state.pop('error', None)
        self.save()
        import sources
        try:
            self.ps4 = sources.PS4(a.ps4)
            self.ps3 = sources.PS3(a.ps3, a.ps3_dlc, log=log)
        except FileNotFoundError as e:
            return self.finish('failed', error=str(e))
        log('PS3 archives: %d (%s)' % (len(self.ps3.archives), ', '.join(os.path.basename(x) for x in self.ps3.archives)))
        self.music_refs = self.ps4.music_refs()
        self.langs = set(l.strip() for l in a.speech_langs.split(',')) if a.speech_langs else set(SPEECH_LANGS.values())
        self.res = None
        self.havok_tables()
        self.ffmpeg = None
        # The user's rule: what PS4 has stays the PS4 one. The PS4 base ruins-multi stays unless --dlc-ruins
        # (map pack 1's larger ruins-multi under the same name); --no-dlc-ruins is kept for older launchers.
        self.dlc_ruins = a.dlc_ruins and bool(a.ps3_dlc) and not a.no_dlc_ruins
        import u2bank
        self.ffmpeg = u2bank.find_ffmpeg(a.ffmpeg)
        log('ffmpeg: %s' % (self.ffmpeg or 'not found (banks with MP3 samples are skipped)'))
        try:
            import numpy  # noqa: F401
            have_numpy = True
        except ImportError:
            have_numpy = False
            log('numpy is not installed: skins and levels (they need it) are skipped')
        groups = set(g.strip().upper() for g in a.groups.split(','))
        rows = [r for r in read_list(a.list) if r['group'][:1] in groups]
        if a.only:
            rows = [r for r in rows if re.search(a.only, r['key'])]
        order = {'sound1': 0, 'actor27': 1, 'pak22': 2}
        rows.sort(key=lambda r: (order.get(r['key'].split('/')[0], 3), r['group'], r['key']))
        log('%d import-list rows in groups %s' % (len(rows), ','.join(sorted(groups))))
        level_txt = {}
        t0 = time.time()
        for n, row in enumerate(rows):
            key = row['key']
            kl = key.lower()
            if n % 50 == 0 or not kl.startswith('sound1/speech/'):
                event(phase='item', n=n + 1, total=len(rows), key=key)
            have = self.ps4_has(row)
            if have:
                st = 'ps4' if have.startswith('PS4 ') and 'do not use' not in have else 'skipped'
                self.items[key] = dict(status=st, group=row['group'], note=have, conv=CONV_VERSION)
                continue
            try:
                entry, data = self.source(row)
            except LookupError as e:
                self.items[key] = dict(status='missing', group=row['group'], note=str(e), conv=CONV_VERSION)
                log('MISSING %s: %s' % (key, e))
                continue
            src_sha = row.get('sha1') or sha1(data)
            if kl.endswith('.txt') and kl.startswith('pak22/'):
                level_txt[os.path.basename(key)[:-4]] = data.decode('latin-1')
                self.items[key] = dict(status='ok', group=row['group'], note='registry input', conv=CONV_VERSION,
                                       src_sha1=src_sha)
                continue
            if self.done_before(key, src_sha):
                continue
            if kl.endswith('.xvag'):
                fn = self.conv_xvag
            elif kl.endswith('.bnk'):
                fn = self.conv_bank
            elif kl.startswith('actor27/') and kl.endswith('.pak'):
                fn = self.conv_actor if have_numpy else None
            elif kl.startswith('pak22/') and kl.endswith('.pak'):
                fn = self.conv_level if have_numpy else None
            else:
                self.items[key] = dict(status='skipped', group=row['group'], conv=CONV_VERSION,
                                       note='optional (%s): not referenced by the PS4 game' % key.split('/')[0])
                continue
            if fn is None:
                self.items[key] = dict(status='skipped', group=row['group'], note='needs numpy', conv=CONV_VERSION)
                continue
            t = time.time()
            try:
                rel, notes = fn(row, entry, data)
                self.record(key, 'ok', row, out=os.path.join(OUT_MAIN, *rel.split('/')).replace('\\', '/'),
                            src=entry.source, src_sha1=src_sha, note='; '.join(notes)[:400])
                if not kl.startswith('sound1/speech/'):
                    log('ok      %s <- %s (%.1fs) %s' % (key, entry.source, time.time() - t, '; '.join(notes)[:200]))
            except NotImplementedError as e:
                self.items[key] = dict(status='unsupported', group=row['group'], note=str(e)[:400], conv=CONV_VERSION,
                                       src=entry.source)
                log('UNSUPPORTED %s: %s' % (key, e))
            except Exception as e:
                import u2level
                status = 'unsupported' if isinstance(e, u2level.Unsupported) else 'failed'
                self.items[key] = dict(status=status, group=row['group'], conv=CONV_VERSION, src=entry.source,
                                       note=('%s: %s' % (type(e).__name__, e))[:400])
                log('%s %s: %s: %s' % (status.upper(), key, type(e).__name__, str(e)[:300]))
                if status == 'failed':
                    log(traceback.format_exc()[-1500:])
            if n % 20 == 0:
                self.save()
        self.save()
        if have_numpy:
            self.net_images(level_txt)
            self.net_spawn_paks(level_txt)
        self.net_patch()
        self.registries(level_txt)
        self.texts()
        counts = {}
        for k, it in self.items.items():
            counts[it['status']] = counts.get(it['status'], 0) + 1
        self.state['counts'] = counts
        log('done in %.0f s: %s' % (time.time() - t0, ', '.join('%s %d' % kv for kv in sorted(counts.items()))))
        if a.validate:
            import validate
            self.state['validation'] = validate.run(self.ps3, self.ps4, os.path.join(self.work, 'validate'), log=log)
        bad = counts.get('failed', 0) + counts.get('missing', 0)
        return self.finish('partial' if bad or counts.get('unsupported') else 'imported')

    def ps3_data_pak22(self):
        """pak22 of the PS3 game-data folder (<game id>DATA2/USRDIR/build/main/pak22: what PS3 1.10 downloaded
        from the server), from --ps3-data or next to the DLC / disc folders under dev_hdd0/game"""
        cands = []
        if self.a.ps3_data:
            cands.append(self.a.ps3_data)
        # The game's own data folder first (GOTY NPUA80698 -> NPUA80698DATA2, which RPCS3 creates on first start:
        # net-patch9.bin, users, cinema5). Its net-patch9 names the same 13 packages ('1-<map>-pak-ingame'), but the
        # packages themselves came from the PS3 server, now offline, so a new GOTY install has none. Then any other
        # DATA2 folder: the packages are the server's map data, byte-identical in the BCUS98123DATA2 and
        # BCES00757DATA2 copies (checked 2026-11-08).
        title = None
        for part in reversed(os.path.abspath(self.a.ps3).replace('\\', '/').split('/')):
            if re.match(r'^[A-Z]{4}\d{5}$', part):
                title = part
                break
        for root in (self.a.ps3_dlc, self.a.ps3):
            if not root:
                continue
            p = os.path.abspath(root)
            for _ in range(6):                     # up to dev_hdd0/game
                if os.path.basename(p).lower() == 'game':
                    if title:
                        cands.append(os.path.join(p, title + 'DATA2'))
                    cands += sorted(os.path.join(p, d) for d in os.listdir(p) if d.upper().endswith('DATA2'))
                    break
                p = os.path.dirname(p)
        for c in cands:
            for sub in ('', 'USRDIR', os.path.join('USRDIR', 'build', 'main'), os.path.join('build', 'main')):
                d = os.path.join(c, sub, 'pak22') if sub or not c.lower().endswith('pak22') else c
                if os.path.isdir(d) and any(f.endswith('-pak-ingame10.pak') for f in os.listdir(d)):
                    return d
        return None

    def net_spawn_paks(self, level_txt):
        """pak22/ps4/<map>-pak-ingame10.pak: the multiplayer spawners (online-spawner-*, team / start tags), CTF
        flags and territories of each map. The game asks for the level '<map>-pak' (the net game's level record) and
        reads its spawner group (GetSpawnLocator 0x662fa0, start / respawn choice 0x644220 / 0x642d20). PS3 1.10
        downloaded these packages from the server into the game-data folder as 1-<map>-pak-ingame10.pak (net-patch9
        maps '<map>-pak' -> '1-<map>-pak-ingame'); neither disc has them and PS4 ships only stage-multi-pak-ingame10.
        Converted like any -ingame10 data pak (u2level -> u2ingame), registered as level '<map>-pak'."""
        d = self.ps3_data_pak22()
        found = sorted(f for f in os.listdir(d) if f.endswith('-pak-ingame10.pak')) if d else []
        if d:
            log('net spawn paks: from %s (server map data, the same for every PS3 version)' % d)
        if not d:
            log('net spawn paks: no PS3 game-data folder with <map>-pak-ingame10.pak (set --ps3-data): '
                'multiplayer maps have no player spawners')
        import u2level
        for f in found:
            m = re.match(r'^(?:\d+-)?(.+-pak)-ingame10\.pak$', f)
            if not m:
                continue
            level = m.group(1)
            key = 'pak22/%s-ingame10.pak' % level
            rel = 'pak22/ps4/%s-ingame10.pak' % level
            if self.ps4.has('pak22/%s-ingame10.pak' % level):          # stage-multi-pak: PS4 ships it
                level_txt[level] = 'level-id %s\npackage %s-ingame\n' % (level, level)
                self.items[key] = dict(status='ps4', group='A', note='PS4 has it', conv=CONV_VERSION)
                continue
            src = os.path.join(d, f)
            data = open(src, 'rb').read()
            src_sha = sha1(data)
            if not self.done_before(key, src_sha):
                t = time.time()
                try:
                    if self.res is None:
                        self.res = u2level.build_res(self.ps4.pak_dir, os.path.join(self.work, 'res'), log=log)
                    kind, notes = u2level.convert(src, self.out_path(rel), self.ps4.pak_dir, self.res,
                                                  os.path.join(self.work, 'levels'), drop_navmesh=self.a.drop_navmesh,
                                                  log=log)
                except Exception as e:
                    self.items[key] = dict(status='failed', group='A', conv=CONV_VERSION, src='ps3-data',
                                           note=('%s: %s' % (type(e).__name__, e))[:400])
                    log('FAILED %s: %s: %s' % (key, type(e).__name__, str(e)[:300]))
                    continue
                self.record(key, 'ok', dict(group='A'), out=os.path.join(OUT_MAIN, *rel.split('/')).replace('\\', '/'),
                            src='ps3-data', src_sha1=src_sha, note='; '.join([kind] + notes)[:400])
                log('ok      %s <- ps3-data %s (%.1fs) %s' % (key, f, time.time() - t, kind))
            if os.path.isfile(self.out_path(rel)):
                level_txt[level] = 'level-id %s\npackage %s-ingame\n' % (level, level)
        log('net spawn paks: %d maps' % sum(1 for k in level_txt if k.endswith('-pak')))
        self.save()

    def ps3_data_file(self, name):
        """a file of the PS3 game-data folder (<game id>DATA2/USRDIR/<name>), searched like ps3_data_pak22: --ps3-data,
        then the title's own DATA2 folder (GOTY NPUA80698 -> NPUA80698DATA2), then any other DATA2 folder"""
        cands = []
        if self.a.ps3_data:
            cands.append(self.a.ps3_data)
        title = None
        for part in reversed(os.path.abspath(self.a.ps3).replace('\\', '/').split('/')):
            if re.match(r'^[A-Z]{4}\d{5}$', part):
                title = part
                break
        for root in (self.a.ps3_dlc, self.a.ps3):
            if not root:
                continue
            p = os.path.abspath(root)
            for _ in range(6):
                if os.path.basename(p).lower() == 'game':
                    if title:
                        cands.append(os.path.join(p, title + 'DATA2'))
                    cands += sorted(os.path.join(p, d) for d in os.listdir(p) if d.upper().endswith('DATA2'))
                    break
                p = os.path.dirname(p)
        for c in cands:
            for sub in ('', 'USRDIR'):
                f = os.path.join(c, sub, name)
                if os.path.isfile(f):
                    return f
        return None

    def net_patch(self):
        """dc1/ps4/net-patch9.bin: the PS3 server live-data DC module (weapon sets such as 0x8cf8e1cc that the PS4
        net.bin names but does not define, playlists ...). PS3 1.10 downloaded it into the game-data folder and bound
        its symbols at NetInit (0x4e83c8); the u2mp runtime (netpatch.cpp) loads the converted file the same way.
        Converted with u2dc (schema learned from the PS3 dc1.psarc / PS4 dc1/ps4 module pairs)."""
        import u2dc
        key = 'dc1/net-patch9.bin'
        rel = 'dc1/ps4/net-patch9.bin'
        src = self.ps3_data_file('net-patch9.bin')
        if not src:
            self.items[key] = dict(status='missing', group='A', conv=CONV_VERSION,
                                   note='no PS3 game-data folder with net-patch9.bin (NPUA80698DATA2; set --ps3-data)')
            log('MISSING %s: no PS3 game-data folder with net-patch9.bin (start the PS3 GOTY once in RPCS3 or set '
                '--ps3-data): match start asserts on weapon set 0x8cf8e1cc' % key)
            return
        data = open(src, 'rb').read()
        src_sha = sha1(data)
        out = self.out_path(rel)
        if self.done_before(key, src_sha) and self.items[key].get('dc') == u2dc.VERSION:
            return
        t = time.time()
        try:
            schema, _ = u2dc.schema_for(self.ps3, os.path.join(self.ps4.main, 'dc1', 'ps4'),
                                        os.path.join(self.work, 'dc'), log=log, net_patch=data)
            conv, rep = u2dc.Converter(schema).convert(data)
            info = u2dc.check_module(conv)
        except Exception as e:
            self.items[key] = dict(status='failed', group='A', conv=CONV_VERSION, src='ps3-data',
                                   note=('%s: %s' % (type(e).__name__, e))[:400])
            log('FAILED %s: %s: %s' % (key, type(e).__name__, str(e)[:300]))
            return
        with open(out, 'wb') as f:
            f.write(conv)
        fallback = {k: v for k, v in rep.items() if v and k in ('key-unknown', 'layout-extend-failed', 'slot-not-ptr',
                                                                'ptr-target-missing', 'sym-target-missing')}
        note = '%d symbols, %d pointers; fallbacks %s' % (info['symbols'], info['pointers'], fallback or 'none')
        self.record(key, 'ok', dict(group='A'), out=os.path.join(OUT_MAIN, *rel.split('/')).replace('\\', '/'),
                    src='ps3-data', src_sha1=src_sha, note=note, dc=u2dc.VERSION)
        log('ok      %s <- %s (%.1fs) %s' % (key, src, time.time() - t, note))
        self.save()

    def net_images(self, level_txt):
        """pak22/ps4/net-u2mp.pak: the PS3 net.pak menu images the PS4 net.pak lacks (Leaderboards / Trophies
        buttons, DLC map photos, store skin portraits ...), u2netimg; registered as its own level / package"""
        import u2netimg
        key = 'pak22/' + u2netimg.OUT_NAME
        c = self.ps3.candidates('pak22/net.pak')
        ps4_net = os.path.join(self.ps4.pak_dir, 'net.pak')
        if not c or not os.path.isfile(ps4_net):
            self.items[key] = dict(status='missing', group='A', note='no PS3 or PS4 net.pak', conv=CONV_VERSION)
            return
        data = c[0].read()
        src_sha = sha1(data)
        out = self.out_path('pak22/ps4/' + u2netimg.OUT_NAME)
        if not self.done_before(key, src_sha) or self.items[key].get('netimg') != u2netimg.VERSION:
            src = os.path.join(self.work, 'ps3-net.pak')
            with open(src, 'wb') as f:
                f.write(data)
            t = time.time()
            try:
                names = u2netimg.convert(src, ps4_net, out, log=log)
            except Exception as e:
                self.items[key] = dict(status='failed', group='A', conv=CONV_VERSION, src=c[0].source,
                                       note=('%s: %s' % (type(e).__name__, e))[:400])
                log('FAILED %s: %s: %s' % (key, type(e).__name__, str(e)[:300]))
                return
            finally:
                os.remove(src)
            if not names:
                return
            self.record(key, 'ok', dict(group='A'), out=os.path.join(OUT_MAIN, 'pak22', 'ps4', u2netimg.OUT_NAME)
                        .replace('\\', '/'), src=c[0].source, src_sha1=src_sha, note='%d menu images' % len(names),
                        netimg=u2netimg.VERSION)
            log('ok      %s <- %s (%.1fs) %d menu images' % (key, c[0].source, time.time() - t, len(names)))
        if os.path.isfile(out):
            level_txt[u2netimg.OUT_NAME[:-4]] = 'level-id %s\npackage %s\n' % ((u2netimg.OUT_NAME[:-4],) * 2)

    def registries(self, level_txt):
        """pak22_ps4.txt and banks.txt of the overlay (from the PS4 files + what was converted)"""
        import registry

        def in_overlay(rel):
            return os.path.isfile(os.path.join(self.main, *rel.split('/')))

        def have_pak(name):
            return any(self.ps4.has('pak22/%s.pak' % n) or in_overlay('pak22/ps4/%s.pak' % n)
                       for n in registry.package_file_names(name))

        def have_actor(name):
            return self.ps4.has('actor27/%s.pak' % name) or in_overlay('actor27/ps4/%s.pak' % name)

        def have_bank(name):
            return self.ps4.has('sound1/banks/%s.bnk' % name) or in_overlay('sound1/banks/ps4/%s.bnk' % name)
        text = open(self.ps4.registry_path, encoding='latin-1', newline='').read()
        failed = {os.path.basename(k)[:-4] for k, it in self.items.items()
                  if k.startswith('pak22/') and k.endswith('.pak') and it['status'] in ('unsupported', 'failed', 'missing')}
        reg, added, skipped, dropped = registry.build_registry(text, level_txt, have_pak, have_actor, have_bank,
                                                               failed_paks=failed)
        p = self.out_path('pak22_ps4.txt')
        if self.a.only and not level_txt:      # --only without the level .txt rows: keep the registry as it is
            log('pak22_ps4.txt: unchanged (--only run without level rows)')
        else:
            if added:
                with open(p, 'w', encoding='latin-1', newline='') as f:
                    f.write(reg)
            elif os.path.exists(p):
                os.remove(p)
            self.state['registry'] = dict(added=added, skipped=skipped,
                                          dropped={k: ['%s %s' % x for x in v] for k, v in dropped.items()})
            log('pak22_ps4.txt: %d levels added %s; %d not added' % (len(added), added, len(skipped)))
        bank_dir = os.path.join(self.main, 'sound1', 'banks', 'ps4')
        new_banks = [f for f in os.listdir(bank_dir) if f.endswith('.bnk')] if os.path.isdir(bank_dir) else []
        if new_banks and os.path.isfile(self.ps4.banks_txt):
            text = open(self.ps4.banks_txt, encoding='latin-1', newline='').read()
            btxt, add = registry.build_banks_txt(text, new_banks)
            if add:
                with open(self.out_path('sound1/config/ps4/banks.txt'), 'w', encoding='latin-1', newline='') as f:
                    f.write(btxt)
            self.state['banks_txt'] = add
            log('banks.txt: %d banks added %s' % (len(add), add))
        self.save()

    def texts(self):
        """text1/ps4/<lang>.<category>: the PS4 bank plus the PS3 strings it lacks, u2text. networking: the DLC map
        titles and the ~25 multiplayer menu texts only PS3 title updates carried (stat labels, leaderboard footer,
        Time Played, daily players, Clan Tag ...); common: the two base-game ids PS4 dropped. The game
        reads its banks from core.psarc; the runtime answers TextDb misses from these files (remaining.cpp)."""
        import u2text
        from psarc_lib import Psarc
        core = os.path.join(self.ps4.main, 'core.psarc')
        if not os.path.isfile(core):
            return
        arc = Psarc(core)
        added = {}
        for i, n in enumerate(arc.names):
            m = re.match(r'text1/ps4/(\d+)\.(networking|common)$', n)
            if not m:
                continue
            c = self.ps3.candidates('text1/%s.%s' % m.groups())
            rel = 'text1/ps4/%s.%s' % m.groups()
            out = os.path.join(self.main, *rel.split('/'))
            if not c:
                continue
            data, ids = u2text.merge(arc.read(i + 1), c[0].read())
            if data is None:
                if os.path.exists(out):
                    os.remove(out)
                continue
            with open(self.out_path(rel), 'wb') as f:
                f.write(data)
            added[rel] = len(ids)
        self.state['text'] = added
        log('text banks: %d written (%s strings added)' % (len(added), sorted(set(added.values()))))
        self.save()

    def finish(self, status, **kw):
        self.state.update(kw, status=status, finished=time.strftime('%Y-%m-%d %H:%M:%S'))
        self.state.pop('pid', None)
        self.save()
        event(phase='done', status=status, error=self.state.get('error', ''))
        log('status: %s %s' % (status, self.state.get('error', '')))
        return 0 if status in ('imported', 'partial') else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ps4', required=True)
    ap.add_argument('--ps3', required=True)
    ap.add_argument('--ps3-dlc')
    ap.add_argument('--ps3-data', help='PS3 game-data folder (dev_hdd0/game/BCUS98123DATA2): the downloaded multiplayer spawn paks')
    ap.add_argument('--out', required=True)
    ap.add_argument('--groups', default='A,B,C,D,E')
    ap.add_argument('--speech-langs', default='', help='PS4 speech languages to fill (default: all four the PS4 game '
                                                       'has: english, french, sam-spanish, brazilian)')
    ap.add_argument('--list', default=IMPORT_LIST)
    ap.add_argument('--only', help='only keys matching this regex (tests)')
    ap.add_argument('--dlc-ruins', action='store_true', help='replace the PS4 (base game) ruins-multi with the '
                                                             'map pack 1 version (the default when --ps3-dlc is given)')
    ap.add_argument('--no-dlc-ruins', action='store_true', help='keep the PS4 (base game) ruins-multi even with the DLC')
    ap.add_argument('--drop-navmesh', action='store_true', help='convert level data paks without their NAV_MESH_1 '
                                                                '(a fallback; u2nav converts them)')
    ap.add_argument('--no-verify', action='store_true', help='accept PS3 files whose SHA-1 differs from the list')
    ap.add_argument('--ffmpeg', default='')
    ap.add_argument('--ps3-elf', default='', help='decrypted PS3 EBOOT.elf for the Havok tables of props with collision '
                                                  '(default: <PS3 USRDIR>/EBOOT.elf)')
    ap.add_argument('--objdump', default='', help='GNU objdump for the Havok tables (default: u3mp/bin, msys64, PATH)')
    ap.add_argument('--validate', action='store_true', help='also check the converters on PS3/PS4 reference pairs')
    ap.add_argument('--force', action='store_true', help='convert everything again')
    a = ap.parse_args()
    return Importer(a).run()


if __name__ == '__main__':
    sys.exit(main())
