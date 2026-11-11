"""mpimage.py - build the Uncharted 3 multiplayer image on the player's PC from the player's own game.

No game bytes are shipped. The chain, each step checked by sha256:

  1. base:        the port's own prepare.py + link_modules.py (gow3_pc/scripts) run on the player's retail
                  big3-ps4_Shipping.elf (CUSA02320 / CUSA02343 1.02, the same file).  Result: the PC runtime's prepared image (G3PROBE5).
  2. menu shell:  build/<version>/menu-shell.json applied to it: the PC port's code patches of 25 Oct 2026 and the
                  menu-flow stubs (code bytes we wrote; the bytes they replace are only checked by hash), plus the
                  menu-flow segment at 0x2050000 (our compiled helpers).  = outputs/builds/menu-shell-v1.
  3. MP image:    build_u3_profile_probe.build(menu shell, profile-helper.bin, u3_pool_init.bin), the same link
                  build_next.py runs, with the environment of the release (build.json "env").

usage (also imported by u3mp_import.py):
  python mpimage.py --game <PS4 game folder> --port <port root> --out <folder> [--version 7.55]
"""
import argparse
import base64
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILDS = HERE / 'build'
EXECUTABLE = 'big3-ps4_Shipping.elf'


class ImageError(Exception):
    pass


class KeysMissing(ImageError):
    """The new image would have no online profile keys (no usable EBOOT.elf, no previous image to take them from): the
    previous image is left in place."""


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def latest_version():
    versions = sorted((p.name[1:] for p in BUILDS.glob('v*') if (p / 'build.json').is_file()),
                      key=lambda v: [int(x) for x in v.split('.')])
    if not versions:
        raise ImageError('no multiplayer build in ' + str(BUILDS))
    return versions[-1]


def build_info(version):
    folder = BUILDS / ('v' + version)
    info = json.loads((folder / 'build.json').read_text(encoding='utf-8'))
    info['folder'] = folder
    return info


def parse(raw):
    """G3PROBE5 container: header, modules, bindings, segment table, import names, relocations, image."""
    if raw[:8] != b'G3PROBE5':
        raise ImageError('not a G3PROBE5 prepared image')
    size, entry, ns, nr, ni, flags = struct.unpack_from('<6Q', raw, 8)
    pos = 96
    nm = struct.unpack_from('<Q', raw, pos)[0]; pos += 8 + nm * 56
    nb = struct.unpack_from('<Q', raw, pos)[0]; pos += 8 + nb * 24
    segpos = pos
    namespos = segpos + ns * 24
    imagepos = namespos + ni * 128 + nr * 32
    if imagepos + size != len(raw):
        raise ImageError('invalid prepared image layout')
    return dict(size=size, ns=ns, segpos=segpos, namespos=namespos, imagepos=imagepos)


# ----------------------------------------------------------------------------- 1. base image
def find_retail_self(game, info, update=None):
    """The retail executable: the game folder's, or a backup the player kept (a modified one is reported)."""
    want = info['self_sha256']
    seen = []
    folders = ([update, update / 'backup'] if update else []) + [game, game / 'backup', game / 'uroot']
    for candidate in (folder / EXECUTABLE for folder in folders):
        if candidate.is_file():
            digest = sha256_file(candidate)
            if digest == want:
                return candidate
            seen.append('%s (sha256 %s..., %d bytes)' % (candidate, digest[:12], candidate.stat().st_size))
    raise ImageError(
        'The Uncharted 3 executable of this PS4 game folder is not the retail %s (sha256 %s...) the multiplayer build '
        'is made for: %s.  Supported: %s.  If you replaced it (for example with a patched PKG build), put the '
        'original back or keep it as backup/%s.' % (info['game'], want[:12], '; '.join(seen) or 'no ' + EXECUTABLE,
                                                   info['game'], EXECUTABLE))


def prepare_base(game, port, work, info, log=print, update=None):
    """Run the port's own prepare scripts on the retail executable; return the prepared image bytes."""
    scripts = port / 'gow3_pc' / 'scripts'
    if not (scripts / 'prepare.py').is_file():
        raise ImageError('the port\'s gow3_pc/scripts/prepare.py was not found in ' + str(port))
    source = find_retail_self(game, info, update)
    view = work / 'game-view'
    if view.exists():
        shutil.rmtree(view)
    (view / 'sce_sys').mkdir(parents=True)
    shutil.copyfile(source, view / EXECUTABLE)
    modules = next((f / 'sce_module' for f in (source.parent, update, game) if f and (f / 'sce_module').is_dir()), None)
    if modules is None:
        raise ImageError('no sce_module folder in the PS4 game folder ' + str(game))
    shutil.copytree(modules, view / 'sce_module')
    if (game / 'sce_sys' / 'param.sfo').is_file():
        shutil.copyfile(game / 'sce_sys' / 'param.sfo', view / 'sce_sys' / 'param.sfo')
    out = work / 'prepared'
    for script in ('prepare.py', 'link_modules.py'):
        r = subprocess.run([sys.executable, str(scripts / script), str(view), '--out', str(out), '--executable', EXECUTABLE],
                           capture_output=True, text=True, errors='replace',
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        for line in (r.stdout + r.stderr).strip().splitlines()[-3:]:
            log('  ' + line)
        if r.returncode:
            raise ImageError('%s failed (exit %d)' % (script, r.returncode))
    raw = (out / 'boot-linked.bin').read_bytes()
    shutil.rmtree(view, ignore_errors=True)
    digest = sha256(raw)
    if digest != info['base_sha256']:
        raise ImageError('The port prepared a different Uncharted 3 image (sha256 %s..., expected %s...). The '
                         'multiplayer build %s was made with the image of port 0.8.x; this port version lays the game '
                         'out differently, so the multiplayer patch needs an update for it.'
                         % (digest[:12], info['base_sha256'][:12], info['version']))
    return raw


# ----------------------------------------------------------------------------- 2. menu shell
def apply_menu_shell(raw, delta):
    p = parse(raw)
    if sha256(raw) != delta['base_sha256']:
        raise ImageError('menu shell: unexpected base image')
    image = bytearray(raw[p['imagepos']:])
    for patch in delta['patches']:
        at = int(patch['at'], 16)
        new = bytes.fromhex(patch['new'])
        if sha256(bytes(image[at:at + len(new)])) != patch['old_sha256']:
            raise ImageError('menu shell: unexpected bytes at image offset %#x' % at)
        image[at:at + len(new)] = new
    seg = delta['segment']
    va, length, prot = int(seg['va'], 16), seg['size'], seg['prot']
    if va != (p['size'] + 0x3fff) & ~0x3fff:
        raise ImageError('menu shell: segment address mismatch')
    data = zlib.decompress(base64.b64decode(seg['zlib_b64']))
    if len(data) != length or sha256(data) != seg['sha256']:
        raise ImageError('menu shell: segment data damaged')
    image.extend(b'\0' * (va - len(image)))
    image.extend(data)
    header = bytearray(raw[:p['namespos']]) + struct.pack('<3Q', va, length, prot) + raw[p['namespos']:p['imagepos']]
    struct.pack_into('<Q', header, 8, len(image))
    struct.pack_into('<Q', header, 24, p['ns'] + 1)
    result = bytes(header + image)
    parse(result)
    if sha256(result) != delta['output_sha256']:
        raise ImageError('menu shell: result differs from the reference (%s...)' % sha256(result)[:12])
    return result


# ----------------------------------------------------------------------------- 3. link
def link(shell, info, env_extra=None):
    folder = info['folder']
    helper = (folder / 'profile-helper.bin').read_bytes()
    pool = (folder / 'u3_pool_init.bin').read_bytes()
    for name, data in (('profile-helper.bin', helper), ('u3_pool_init.bin', pool)):
        if sha256(data) != info['files'][name]:
            raise ImageError(name + ' is damaged (sha256 mismatch)')
    saved = {k: os.environ.get(k) for k in list(info['env']) + list(env_extra or {})}
    sys.path[:0] = [str(folder)]
    sys.dont_write_bytecode = True
    try:
        os.environ.update(info['env'])
        os.environ.update(env_extra or {})
        import build_u3_profile_probe as builder
        out = builder.build(shell, helper, pool)
    finally:
        sys.path.remove(str(folder))
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        for m in ('build_u3_profile_probe', 'build_u3_bootstrap_probe', 'build_u3_pool_blob'):
            sys.modules.pop(m, None)
    return out[0] if isinstance(out, tuple) else out


def build_image(game, port, out, version=None, log=print, elf_out=None, update=None, ps3_elf=None):
    """Steps 1-3; writes <out>/boot-linked.bin and returns its sha256 (checked against the release).
    elf_out: also write the recipe's PS4 ELF (ps4_elf) there.
    ps3_elf: the player's decrypted PS3 1.19 EBOOT.elf; the helper's key block (eboot_keys) is filled from it, or carried
    over from the image already in <out> when there is none. 'sha256' stays the digest of the release's image (before the
    keys); 'file_sha256' is the digest of the file written (what the game starter checks)."""
    version = version or latest_version()
    info = build_info(version)
    delta = json.loads((info['folder'] / 'menu-shell.json').read_text(encoding='utf-8'))
    out.mkdir(parents=True, exist_ok=True)
    work = out / '_work'
    work.mkdir(exist_ok=True)
    log('Preparing the retail Uncharted 3 image with the port\'s own scripts ...')
    raw = prepare_base(game, port, work, info, log, update)
    log('  base image ok (sha256 %s...)' % info['base_sha256'][:12])
    if elf_out:
        Path(elf_out).parent.mkdir(parents=True, exist_ok=True)
        Path(elf_out).write_bytes(ps4_elf(raw))
    shell = apply_menu_shell(raw, delta)
    log('  menu shell ok (sha256 %s...)' % delta['output_sha256'][:12])
    log('Linking multiplayer helper v%s ...' % version)
    image = link(shell, info)
    digest = sha256(image)
    if digest != info['image_sha256']:
        raise ImageError('the linked image differs from the v%s release (sha256 %s..., expected %s...)'
                         % (version, digest[:12], info['image_sha256'][:12]))
    image, keys = with_keys(bytearray(image), ps3_elf, out / 'boot-linked.bin', log)
    if keys == 'none' and (out / 'boot-linked.bin').is_file():
        shutil.rmtree(work, ignore_errors=True)
        raise KeysMissing('no decrypted PS3 1.19 EBOOT.elf of your own copy of the game for the online profile keys; '
                          'the current multiplayer image stays')
    tmp = out / 'boot-linked.bin.tmp'
    tmp.write_bytes(image)
    os.replace(tmp, out / 'boot-linked.bin')
    shutil.rmtree(work, ignore_errors=True)
    log('  multiplayer image v%s ok (sha256 %s)' % (version, digest))
    return dict(version=version, sha256=digest, path=str(out / 'boot-linked.bin'), base_sha256=info['base_sha256'],
                file_sha256=sha256(bytes(image)), keys=keys)


def with_keys(image, ps3_elf, previous, log=print):
    """The image with its helper key block filled (eboot_keys): from the player's EBOOT.elf, else from the previous image
    file (a relink after the EBOOT was moved), else left empty. Returns (image, 'eboot' | 'carried' | 'none' |
    'not needed')."""
    if str(HERE) not in sys.path:
        sys.path.insert(0, str(HERE))
    import eboot_keys
    if eboot_keys.find(image) < 0:
        return image, 'not needed'      # a build from before v7.97: its helper carries no key block
    if ps3_elf:
        try:
            block = eboot_keys.block_from_eboot(Path(ps3_elf).read_bytes())
            log('  online profile keys: read from your PS3 EBOOT.elf')
            return eboot_keys.fill(image, block), 'eboot'
        except (OSError, eboot_keys.KeyError_) as e:
            log('  online profile keys: not from %s (%s)' % (ps3_elf, e))
    try:
        block = eboot_keys.carry(Path(previous).read_bytes())
    except OSError:
        block = None
    if block:
        log('  online profile keys: kept from your previous multiplayer image')
        return eboot_keys.fill(image, block), 'carried'
    log('  online profile keys: missing (choose your decrypted 1.19 EBOOT.elf on the Multiplayer tab)')
    return image, 'none'


# ----------------------------------------------------------------------------- PS4 ELF for the recipe
def ps4_elf(raw):
    """A plain ELF of the prepared image for the recipe's tools (RECIPE_PS4_ELF: Havok reflection tables): every
    image segment as a PT_LOAD at its address, base-relative pointers relocated to load address 0 (the layout of
    the u3_ps4.elf the 2026-11-01 tables were made from).  Made on the player's PC, never shipped."""
    p = parse(raw)
    size, entry, ns, nr, ni, flags = struct.unpack_from('<6Q', raw, 8)
    image = bytearray(raw[p['imagepos']:])
    relpos = p['namespos'] + ni * 128
    for i in range(nr):
        target, kind, value, addend = struct.unpack_from('<QQqq', raw, relpos + i * 32)
        if kind == 0 and target + 8 <= len(image):
            struct.pack_into('<Q', image, target, value & 0xffffffffffffffff)
    segs = [struct.unpack_from('<3Q', raw, p['segpos'] + i * 24) for i in range(ns)]
    phoff, align = 0x40, 0x4000
    offset = (phoff + 56 * len(segs) + align - 1) & ~(align - 1)
    headers, body = bytearray(), bytearray()
    for va, length, prot in segs:
        flags_ = (1 if prot & 4 else 0) | (2 if prot & 2 else 0) | (4 if prot & 1 else 0)
        headers += struct.pack('<IIQQQQQQ', 1, flags_, offset + len(body), va, va, length, length, align)
        body += image[va:va + length]
        body += b'\x00' * (-len(body) % align)
    ehdr = bytearray(b'\x7fELF\x02\x01\x01' + b'\x00' * 9)
    ehdr += struct.pack('<HHIQQQIHHHHHH', 2, 62, 1, entry, phoff, 0, 0, 64, 56, len(segs), 64, 0, 0)
    out = bytearray(ehdr + headers)
    out += b'\x00' * (offset - len(out))
    return bytes(out + body)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', type=Path, required=True)
    ap.add_argument('--port', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--version')
    a = ap.parse_args()
    try:
        r = build_image(a.game, a.port, a.out, a.version)
    except ImageError as e:
        print('ERROR: ' + str(e))
        return 2
    print(json.dumps(r))
    return 0


if __name__ == '__main__':
    sys.exit(main())
