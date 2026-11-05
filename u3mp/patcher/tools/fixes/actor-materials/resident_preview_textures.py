"""Build bounded resident preview mips from matching installed PS4 texture dictionaries.

Original actor pages, geometry, texture identities and streaming flags stay intact.
Only descriptor ranges/dimensions and the appended VRAM payload change. Output must
be a separate overlay; this does not modify or launch the running game.
Use --install PROJECT to merge verified repairs into overlay-r3, backing up replacements.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile

TOOLS = Path(__file__).resolve().parents[2] / 'paktools'
sys.path.insert(0, str(TOOLS))
from ndpak import Pak
import texcodec as tc

ACTORS = (
    # Pants J has a resident/dictionary unk20 mismatch; do not reinterpret that flag.
    'mp-kit-base-legs-feet-skin-a',
    'mp-kit-base-head-skin-a',
    'mp-kit-base-arms-low-arm-skin-a-00',
    'mp-kit-base-neck-skin-a',
    'mp-kit-base-arms-hands-skin-a-00',
)
DICTIONARIES = ('common-tstrm.pak', 'chunk-41-tstrm.pak')
ACTOR_DIR = Path('u3data/build/main/actor33/ps4')
DICT_DIR = Path('u3data/build/main/texturedict1/ps4')
VRAM_BUDGET = 0x80000


def digest(data):
    return hashlib.sha256(data).hexdigest()


def texture_key(texture):
    return texture['hash'], texture['path'].replace('\\', '/').lower()


def dictionary_metadata(path):
    # Parse the small metadata prefix, not hundreds of MB of unrelated textures.
    size = path.stat().st_size
    with path.open('rb') as stream:
        header = stream.read(0x2c)
        if len(header) != 0x2c:
            raise ValueError('Truncated texture dictionary: ' + str(path))
        vram_size = struct.unpack_from('<I', header, 0x20)[0]
        base = size - vram_size
        if not 0x2c <= base <= min(size, 64 * 1024 * 1024):
            raise ValueError('Invalid dictionary metadata extent: ' + str(path))
        stream.seek(0)
        prefix = stream.read(base)
    pak = Pak(prefix)
    pak.vram_base = base
    if pak.platform != 'ps4' or pak.validate():
        raise ValueError('Invalid texture dictionary: ' + str(path))
    return pak


def dictionary_payload(path, pak, texture):
    offset, size = texture['vramOffset'], texture['vramSize']
    if not 0 <= offset < offset + size <= pak.hdr['vramSize']:
        raise ValueError('Dictionary texture is outside its VRAM extent.')
    with path.open('rb') as stream:
        stream.seek(pak.vram_base + offset)
        data = stream.read(size)
    if len(data) != size:
        raise ValueError('Truncated dictionary texture payload.')
    return data


def linear_mips(texture, data):
    fmt = tc.DXGI.get(texture['format'])
    if not fmt or not fmt[0].startswith('BC') or texture['type'] != 1:
        raise ValueError('Only verified 2D block-compressed textures are supported.')
    bpb = fmt[1]
    if not 1 <= texture['mips'] <= max(texture['width'], texture['height']).bit_length():
        raise ValueError('Invalid mip count.')
    required = tc.ps4_tiled_size(texture['width'], texture['height'], texture['mips'], bpb)
    if len(data) != required:
        raise ValueError('Texture payload does not match its tiled mip layout.')
    result, offset = [], 0
    for width, height in tc.mip_dims(texture['width'], texture['height'], texture['mips']):
        bw, bh = tc.blocks(width, height, True)
        size = ((bw + 7) // 8 * 8) * ((bh + 7) // 8 * 8) * bpb
        result.append(tc.detile_mip(data[offset:offset + size], bw, bh, bpb))
        offset += size
    return result


def promoted_payload(low, low_data, high, high_data, cap=256):
    if cap not in (128, 256):
        raise ValueError('Preview mip cap must stay within the tested 128/256 budgets.')
    for field in ('hash', 'type', 'format', 'unk20', 'unk30', 'unk34'):
        if low[field] != high[field]:
            raise ValueError('Mismatched source texture field: ' + field)
    if texture_key(low) != texture_key(high):
        raise ValueError('Texture dictionary identity differs from the actor texture.')
    dimensions = tc.mip_dims(high['width'], high['height'], high['mips'])
    try:
        resident = dimensions.index((low['width'], low['height']))
    except ValueError:
        raise ValueError('Resident dimensions are not a mip of the dictionary texture.') from None
    low_levels, high_levels = linear_mips(low, low_data), linear_mips(high, high_data)
    if high_levels[resident:] != low_levels:
        raise ValueError('Resident mip chain differs from the dictionary source.')
    selected = next(i for i, size in enumerate(dimensions) if max(size) <= cap)
    if selected >= resident:
        return None
    width, height = dimensions[selected]
    count = len(high_levels) - selected
    payload = tc.ps3_to_ps4_texels(b''.join(high_levels[selected:]), width, height, count,
                                  tc.DXGI[low['format']][1])
    desc = dict(low, width=width, height=height, mips=count)
    if linear_mips(desc, payload) != high_levels[selected:]:
        raise ValueError('Promoted mip tiling round-trip failed.')
    return desc, payload


def promote_actor(raw, dictionary_index, cap=256):
    source = Pak(raw)
    if source.platform != 'ps4' or source.validate():
        raise ValueError('Invalid source actor package.')
    data, changes = bytearray(raw), []
    for texture in source.textures():
        if texture['type'] != 1 or max(texture['width'], texture['height']) >= cap:
            continue
        candidates = dictionary_index.get(texture_key(texture), ())
        candidates = [c for c in candidates if max(c[2]['width'], c[2]['height']) > max(texture['width'], texture['height'])]
        if not candidates:
            continue
        candidates.sort(key=lambda c: (c[2]['width'] * c[2]['height'], c[2]['mips']), reverse=True)
        path, dictionary, high = candidates[0]
        high_data = dictionary_payload(path, dictionary, high)
        for other_path, other_dictionary, other in candidates[1:]:
            if (other['width'], other['height'], other['mips']) == (high['width'], high['height'], high['mips']):
                if dictionary_payload(other_path, other_dictionary, other) != high_data:
                    raise ValueError('Conflicting complete texture dictionary entries.')
        promoted = promoted_payload(texture, source.vram_bytes(texture['vramOffset'], texture['vramSize']),
                                    high, high_data, cap)
        if promoted is None:
            continue
        desc, payload = promoted
        offset = len(data) - source.vram_base
        padding = (-offset) % 0x100
        data.extend(bytes(padding))
        offset += padding
        data.extend(payload)
        body = source.pbase(texture['page']) + texture['off'] + 0x20
        struct.pack_into('<QI', data, body + 8, offset, len(payload))
        struct.pack_into('<III', data, body + 0x24, desc['mips'], desc['width'], desc['height'])
        changes.append(dict(path=texture['path'], descriptor=body, before=[texture['width'], texture['height'], texture['mips']],
                            after=[desc['width'], desc['height'], desc['mips']], dictionary=str(path),
                            source_payload_sha256=digest(high_data), payload_sha256=digest(payload),
                            vram_offset=offset, vram_size=len(payload)))
    if not changes:
        raise ValueError('No matching higher-resolution preview textures found.')
    size = len(data) - source.vram_base
    if size > VRAM_BUDGET:
        raise ValueError(f'Resident actor budget exceeded: {size:#x} > {VRAM_BUDGET:#x}')
    struct.pack_into('<I', data, 0x20, size)
    result = Pak(bytes(data))
    if result.validate() or result.vram_base != source.vram_base:
        raise ValueError('Invalid promoted actor layout.')
    # Prove that existing geometry, bindings, flags, identities and VRAM are untouched.
    comparison = bytearray(data[:len(raw)])
    comparison[0x20:0x24] = raw[0x20:0x24]
    for change in changes:
        body = change['descriptor']
        comparison[body + 8:body + 0x14] = raw[body + 8:body + 0x14]
        comparison[body + 0x24:body + 0x30] = raw[body + 0x24:body + 0x30]
    if comparison != raw:
        raise ValueError('Unexpected mutation outside texture descriptors.')
    return bytes(data), changes


def install_preview_overlay(project):
    """Fit the runtime's four-root limit without dropping any existing overlay data."""
    project = Path(project).resolve()
    source = project / 'work/overlay-preview-fixes'
    target = project / 'work/overlay-r3' / ACTOR_DIR
    backups = project / 'work/preview-fixes-backup'
    if not target.is_dir() or not source.is_dir():
        raise ValueError('Both the generated preview overlay and overlay-r3 must exist.')
    if not all(path.resolve().is_relative_to(project) for path in (source, target, backups)):
        raise ValueError('Overlay and backup paths must stay inside this project.')
    report = json.loads((source / 'resident-preview-report.json').read_text())
    if len(report) != len(ACTORS) or {item['actor'] for item in report} != set(ACTORS):
        raise ValueError('Unexpected preview asset manifest.')
    jacket = json.loads((source / 'hunter-jacket-report.json').read_text())
    expected = [(item['actor'], item['output_sha256'], item['source_sha256']) for item in report]
    expected.append(('mp-kit-hunter-torso-jacket-a', jacket['output_sha256'], jacket['input_sha256']['source']))
    planned = []
    for actor, new_hash, old_hash in expected:
        name = actor + '.pak'
        data = (source / ACTOR_DIR / name).read_bytes()
        if digest(data) != new_hash:
            raise ValueError('Generated preview asset hash mismatch: ' + name)
        destination, backup = target / name, backups / name
        if destination.is_symlink() or backup.is_symlink():
            raise ValueError('Refusing symlinked asset or backup: ' + name)
        previous = destination.read_bytes() if destination.exists() else None
        if previous is not None and digest(previous) not in (old_hash, new_hash):
            raise ValueError('Existing overlay asset has unexpected changes: ' + name)
        if backup.exists() and digest(backup.read_bytes()) != old_hash:
            raise ValueError('Existing backup has unexpected contents: ' + name)
        planned.append((destination, backup, previous, data))
    installed = 0
    for destination, backup, previous, data in planned:
        if previous == data:
            continue
        if previous is not None and not backup.exists():
            backups.mkdir(parents=True, exist_ok=True)
            with backup.open('xb') as stream:
                stream.write(previous)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=target, prefix='.preview-', suffix='.tmp', delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(data)
            # Do not replace concurrent edits, and never expose a partially copied package.
            current = destination.read_bytes() if destination.exists() else None
            if current != previous:
                raise ValueError('Overlay asset changed during installation: ' + destination.name)
            os.replace(temporary, destination)
            installed += 1
        finally:
            if temporary is not None and temporary.exists():
                temporary.unlink()
    return dict(installed=installed, verified=len(planned), backup_directory=str(backups))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--install', type=Path, metavar='PROJECT', help='Merge verified repairs into overlay-r3 with backups.')
    parser.add_argument('--cap', type=int, choices=(128, 256), default=256)
    args = parser.parse_args()
    if args.install is not None:
        if args.game is not None or args.output is not None:
            parser.error('--install cannot be combined with generation paths')
        print(json.dumps(install_preview_overlay(args.install)))
        return
    if args.game is None or args.output is None:
        parser.error('generation requires --game and --output')
    game, output = args.game.resolve(), args.output.resolve()
    if output == game or game in output.parents:
        raise ValueError('Output must be a separate overlay, outside the installed game.')
    index = {}
    for name in DICTIONARIES:
        path = game / DICT_DIR / name
        pak = dictionary_metadata(path)
        for texture in pak.textures():
            index.setdefault(texture_key(texture), []).append((path, pak, texture))
    built, report = [], []
    for actor in ACTORS:
        original = game / ACTOR_DIR / (actor + '.pak')
        raw = original.read_bytes()
        fixed, changes = promote_actor(raw, index, args.cap)
        target = output / ACTOR_DIR / original.name
        if target.exists():
            raise FileExistsError('Refusing to overwrite an existing overlay asset: ' + str(target))
        built.append((target, fixed))
        report.append(dict(actor=actor, source=str(original), source_sha256=digest(raw),
                           output_sha256=digest(fixed), vram_size=Pak(fixed).hdr['vramSize'], textures=changes))
    for target, fixed in built:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(fixed)
    report_path = output / 'resident-preview-report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='ascii')
    print(json.dumps(dict(actors=len(report), textures=sum(len(item['textures']) for item in report),
                         report=str(report_path))))


if __name__ == '__main__':
    main()
