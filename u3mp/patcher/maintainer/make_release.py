"""make_release.py - (maintainers) put a multiplayer helper build into u3mp/patcher/build/v<version>/.

  python make_release.py --version 7.55 --build <outputs/builds/lan-lobby-v7.55> --menu-shell <outputs/builds/menu-shell-v1>
                         --pool <work/u3_pool_init.bin> --base <retail prepared boot-linked.bin> --self <retail big3 SELF>
                         --env U3MP_N033=2 --env U3MP_LAN_LOOP=0 --env U3MP_ONLINE_MM=1

Copies only our own files (profile-helper.bin compiled from u3_lan_profiles.c, u3_pool_init.bin compiled from
u3_pool_init.c, the link scripts) and writes:
  menu-shell.json  the difference between the frozen menu shell and the retail prepared image: the code patches at
                   their image offsets (new bytes = port / helper code; the replaced bytes are recorded by sha256 only)
                   and the menu-flow segment the menu shell appends (our compiled code)
  build.json       the hashes the player's PC must reproduce (retail SELF, prepared base, menu shell, linked image)
Refuses to write when the new bytes of a patch run are longer than --max-run (default 80): a long run would be a
sign of copied game bytes rather than a code patch.
"""
import argparse
import base64
import hashlib
import json
import shutil
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import mpimage  # noqa: E402


def runs(a, b):
    """Differing runs between two equal-length byte strings (gaps of up to 2 equal bytes are merged)."""
    out = []
    i, n = 0, len(a)
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and (a[j] != b[j] or (j + 1 < n and a[j + 1] != b[j + 1]) or (j + 2 < n and a[j + 2] != b[j + 2])):
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--menu-shell', type=Path, required=True)
    ap.add_argument('--pool', type=Path, required=True)
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--self', dest='self_', type=Path, required=True)
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--game', default='Uncharted: The Nathan Drake Collection CUSA02320 (EU) 1.02')
    ap.add_argument('--max-run', type=int, default=80)
    a = ap.parse_args()
    dest = mpimage.BUILDS / ('v' + a.version)
    dest.mkdir(parents=True, exist_ok=True)

    base = a.base.read_bytes()
    shell = (a.menu_shell / 'boot-linked.bin').read_bytes()
    pb, ps = mpimage.parse(base), mpimage.parse(shell)
    ib, im = base[pb['imagepos']:], shell[ps['imagepos']:]
    seg_va = (pb['size'] + 0x3fff) & ~0x3fff
    if ps['ns'] != pb['ns'] + 1 or ps['size'] != seg_va + 0x4000:
        sys.exit('the menu shell is not the base plus one 0x4000 segment')
    patches = []
    for i, j in runs(ib, im[:len(ib)]):
        if j - i > a.max_run:
            sys.exit('patch run at %#x is %d bytes long: refusing (copied game bytes?)' % (i, j - i))
        patches.append(dict(at=hex(i), new=im[i:j].hex(), old_sha256=hashlib.sha256(ib[i:j]).hexdigest()))
    if any(im[len(ib):seg_va]):
        sys.exit('bytes between the base image and the segment')
    segment = im[seg_va:seg_va + 0x4000]
    delta = dict(note='Menu shell = retail prepared image + these code patches + the menu-flow segment (our code).',
                 base_sha256=hashlib.sha256(base).hexdigest(), output_sha256=hashlib.sha256(shell).hexdigest(),
                 patches=patches,
                 segment=dict(va=hex(seg_va), size=0x4000, prot=5, sha256=hashlib.sha256(segment).hexdigest(),
                              zlib_b64=base64.b64encode(zlib.compress(segment, 9)).decode()))
    rebuilt = mpimage.apply_menu_shell(base, delta)
    assert rebuilt == shell
    (dest / 'menu-shell.json').write_text(json.dumps(delta, indent=1) + '\n', encoding='utf-8')

    files = {}
    for name, src in (('profile-helper.bin', a.build / 'profile-helper.bin'), ('u3_pool_init.bin', a.pool),
                      ('build_u3_profile_probe.py', a.build / 'build_u3_profile_probe.py'),
                      ('build_u3_pool_blob.py', a.build / 'build_u3_pool_blob.py'),
                      ('build_u3_bootstrap_probe.py', a.build.parent.parent / 'build_u3_bootstrap_probe.py')):
        shutil.copyfile(src, dest / name)
        files[name] = hashlib.sha256((dest / name).read_bytes()).hexdigest()
    env = dict(kv.split('=', 1) for kv in a.env)
    info = dict(version=a.version, game=a.game, executable=mpimage.EXECUTABLE,
                self_sha256=hashlib.sha256(a.self_.read_bytes()).hexdigest(),
                base_sha256=delta['base_sha256'], menu_shell_sha256=delta['output_sha256'],
                image_sha256=hashlib.sha256((a.build / 'boot-linked.bin').read_bytes()).hexdigest(),
                env=env, files=files)
    (dest / 'build.json').write_text(json.dumps(info, indent=1) + '\n', encoding='utf-8')
    print('patches %d (%d bytes), segment %s; release v%s written to %s'
          % (len(patches), sum(len(p['new']) // 2 for p in patches), hex(seg_va), a.version, dest))


if __name__ == '__main__':
    main()
