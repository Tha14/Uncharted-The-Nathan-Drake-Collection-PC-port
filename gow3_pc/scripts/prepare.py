#!/usr/bin/env python3
"""Inspect an already plaintext PS4 SELF; prepare a native entry-point probe.

No decryption, patches to source files, third-party modules or Python dependencies.
Format references are linked in README.md. All offsets are checked before use.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import struct
import sys


def span(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f"out of bounds: {offset:#x}+{size:#x} / {len(data):#x}")
    return data[offset:offset + size]


def unpack(fmt, data, offset):
    return struct.unpack(fmt, span(data, offset, struct.calcsize(fmt)))


def parse_self(data):
    if span(data, 0, 4) != b'O\x15=\x1d':
        raise ValueError("expected PS4 SELF")
    count, = unpack('<H', data, 24)
    base = 32 + count * 32
    header = unpack('<16sHHIQQQIHHHHHH', data, base)
    if header[0][:7] != b'\x7fELF\x02\x01\x01' or header[2] != 62:
        raise ValueError("expected little-endian x86-64 ELF")
    if header[9] != 56 or not 0 < header[10] < 256:
        raise ValueError("unsupported program headers")
    ph = [dict(zip(('type', 'flags', 'offset', 'vaddr', 'paddr', 'filesz', 'memsz', 'align'),
                   unpack('<IIQQQQQQ', data, base + header[5] + i * 56)))
          for i in range(header[10])]
    end = max(p['offset'] + p['filesz'] for p in ph)
    if end > 512 * 1024 * 1024:
        raise ValueError("probe image exceeds 512 MiB limit")
    elf = bytearray(end)
    header_end = header[5] + header[10] * 56
    elf[:header_end] = span(data, base, header_end)
    covered = [(0, header_end)]
    segments = []
    for i in range(count):
        flags, off, size, mem = unpack('<QQQQ', data, 32 + i * 32)
        segments.append(dict(flags=hex(flags), offset=off, size=size))
        if not flags & 0x800:
            continue
        if flags & 10:
            raise ValueError("encrypted/compressed SELF segment is unsupported")
        index = (flags >> 20) & 4095
        if index >= len(ph):
            raise ValueError("invalid segment index")
        p = ph[index]
        if size != p['filesz'] or size != mem:
            raise ValueError("unsupported blocked segment layout")
        elf[p['offset']:p['offset'] + size] = span(data, off, size)
        covered.append((p['offset'], p['offset'] + size))
    missing = []
    for i, p in enumerate(ph):
        if p['filesz'] and not any(a <= p['offset'] and p['offset'] + p['filesz'] <= z
                                  for a, z in covered):
            missing.append(i)
            if p['type'] in (1, 2, 0x61000000, 0x61000010):
                raise ValueError(f"required segment {i} is unavailable")
    return elf, header, ph, segments, missing


def sfo(data):
    magic, version, keys, values, count = unpack('<4sIIII', data, 0)
    if magic != b'\0PSF':
        raise ValueError('bad SFO signature')
    result = {}
    for i in range(count):
        key, fmt, size, capacity, off = unpack('<HHIII', data, 20 + i * 16)
        k = data[keys + key:data.index(0, keys + key)].decode()
        v = span(data, values + off, size)
        result[k] = struct.unpack('<I', v)[0] if fmt == 0x404 else v.rstrip(b'\0').decode('utf8', 'replace')
    return result


def write_report(path, report):
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def nid(name):
    salt = bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')
    digest = hashlib.sha1(name.encode('ascii') + salt).digest()[:8][::-1]
    return base64.b64encode(digest).decode().rstrip('=').replace('/', '-')


def inspect_libc(path):
    """Prove that this dump's _init_env is exactly RET; never assume it."""
    source = path.read_bytes()
    elf, header, ph, _, missing = parse_self(source)
    dynamic = next(p for p in ph if p['type'] == 2)
    tags = dict(unpack('<QQ', elf, pos) for pos in
                range(dynamic['offset'], dynamic['offset'] + dynamic['filesz'], 16))
    lib = next(p for p in ph if p['type'] == 0x61000000)
    blob = span(elf, lib['offset'], lib['filesz'])
    strings = span(blob, tags[0x61000035], tags[0x61000037])
    syms = span(blob, tags[0x61000039], tags[0x6100003f])
    evidence = {'sha256': hashlib.sha256(source).hexdigest(), 'init_env_is_ret': False}
    for pos in range(0, len(syms), 24):
        name, info, other, section, value, size = unpack('<IBBHQQ', syms, pos)
        span(strings, name, 1)
        symbol = strings[name:strings.index(0, name)].decode('ascii')
        key = symbol.split('#')[0]
        if key not in (nid('_init_env'), nid('_ZNSt8ios_base4InitC1Ev')) or not section or info & 15 != 2:
            continue
        p = next(p for p in ph if p['type'] == 1 and p['flags'] & 1
                 and p['vaddr'] <= value < p['vaddr'] + p['filesz'])
        code = span(elf, p['offset'] + value - p['vaddr'], size)
        if key == nid('_init_env'):
            evidence.update(symbol=symbol, address=hex(value), size=size, bytes=code.hex(),
                            init_env_is_ret=code == b'\xc3')
        else:
            evidence['ios_base_init'] = dict(symbol=symbol, address=hex(value), size=size,
                                             code_sha256=hashlib.sha256(code).hexdigest(),
                                             name='_ZNSt8ios_base4InitC1Ev')
    return evidence


def prepare(game, out, executable='eboot.bin'):
    """boot.bin (the memory image the runtime loads: segments, relocations, imports), eboot.elf, entry.bin and the
    analysis.json report in `out`, from the game folder's executable (the launcher's native code,
    launcher/native/prepare.cpp). Prints a summary; ValueError when the executable can't be used."""
    import ctypes
    dll = ctypes.CDLL(str(Path(__file__).resolve().parent.parent.parent / 'launcher' / 'native' / 'uncharted_native.dll'))
    dll.un_prepare.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    dll.un_prepare.restype = ctypes.c_int
    size = 1 << 16
    while True:
        text = ctypes.create_string_buffer(size)
        status = dll.un_prepare(str(game), str(out), executable.encode(), text, size)
        if status >= 0:
            break
        size = -status
    message = text.value.decode('utf-8', 'replace')
    if status != 0:
        raise ValueError(message)
    print(message, end='')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--executable', default='eboot.bin')
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    args = parser.parse_args()
    # The title is printed; a console or pipe in a legacy code page cannot encode every language.
    sys.stdout.reconfigure(errors='replace')
    try:
        prepare(args.game, args.out, args.executable)
    except (ValueError, OSError, StopIteration, KeyError, IndexError) as error:
        parser.exit(1, f'prepare failed: {error}\n')
