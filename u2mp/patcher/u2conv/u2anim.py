"""Uncharted 2 ANIM items, PS3 -> PS4, on the U3 clip converter (u3mp paktools/anim.py).

The clip (ICE 'CD\\x01\\x07' -> OrbisAnim 'AC\\x01\\x04') and the ANIM header are the U3 formats; U2 differs in
the float channels that PS3 keeps in an ANIM_RAWCHANNELS item and PS4 inside the ANIM item (found on the PS3/PS4
actor27 pairs, IMPORT.md):

  PS3 ANIM (0x1c)     {u32 6, u32 0, ptr clip, ptr chans, u32 0, u32 skeleton sid, ptr events}
  PS4 ANIM (0x30)     {u32 6, u32 0, ptr clip, ptr chans, u64 0, u32 skeleton sid, aa, ptr events}
  chans   PS3 {u32 n, f32 rate, ptr entry[n]}           PS4 {u32 n, f32 rate, ptr entry[n]} (aa pad when n = 0)
  entry   PS3 {u32 sid, ptr D}                          PS4 {u32 sid, aa, ptr D}
  D       two keys {f32 quat[4], scale[3], trans[3], 0} (0x58), ptr samples, u16 stride, u16 ?, u16 ?, u16 count
          PS3 0x64, PS4 0x68 (ptr 8 bytes; null when stride * count = 0)
  samples count x stride bytes of f32 (vec3)
PS4 item order: header, chans, then per channel {entry, D, samples}, events, clip; every piece 16-aligned with
0xaa fill.  Retail PS4 drops ANIM_RAWCHANNELS (the data moved into the ANIM item).
"""
import struct

import u2paktools

u2paktools.setup()
import anim as _anim  # noqa: E402
from ps4writer import Blob  # noqa: E402

PAD32 = 0xaaaaaaaa


def _sw4(b):
    n = len(b) - len(b) % 4
    return b''.join(b[i:i + 4][::-1] for i in range(0, n, 4)) + bytes(b[n:])


def read_chans(p, pg, co):
    """U2 PS3 raw channel table -> (rate bytes, [(sid, keys bytes, u16[4], samples bytes)])"""
    n = p.pu32(pg, co)
    rate = p.d[p.pbase(pg) + co + 4:p.pbase(pg) + co + 8]
    chans = []
    for i in range(n):
        e = p.resolve(pg, co + 8 + 4 * i)
        s = p.pu32(e[0], e[1])
        d = p.resolve(e[0], e[1] + 4)
        base = p.pbase(d[0]) + d[1]
        keys = p.d[base:base + 0x58]
        u16 = struct.unpack_from('>4H', p.d, base + 0x5c)
        n_bytes = u16[0] * u16[3]
        smp = b''
        if n_bytes:
            sr = p.resolve(d[0], d[1] + 0x58)
            sb = p.pbase(sr[0]) + sr[1]
            smp = p.d[sb:sb + n_bytes]
        chans.append((s, keys, u16, smp))
    return rate, chans


def read_ps3_anim(p, it):
    pg, b = it.page, it.data_off
    w = struct.unpack_from('>7I', p.d, p.pbase(pg) + b)
    if w[0] != 6 or w[4] != 0:
        raise ValueError('%s: unexpected ANIM header %r' % (it.name, w))
    a = dict(name=it.name, w0=w[0], w1=w[1], skel=w[5], clip=None, chans=None, events=None)
    r = p.resolve(pg, b + 8)
    if r:
        cb = p.pbase(r[0]) + r[1]
        a['clip'] = p.d[cb:cb + struct.unpack_from('>I', p.d, cb + 8)[0]]
    r = p.resolve(pg, b + 12)
    if r:
        a['chans'] = read_chans(p, *r)
    if p.resolve(pg, b + 24):
        a['events'] = _read_events(p, pg, b)       # the U3 event list layout
    return a


def _read_events(p, pg, b):
    epg, eo = p.resolve(pg, b + 24)
    tsid, cnt = p.pu32(epg, eo), p.pu32(epg, eo + 4)
    ents = []
    er = p.resolve(epg, eo + 8)
    for i in range(cnt if er else 0):
        e = er[1] + 16 * i
        s, f, c = struct.unpack_from('>IfI', p.d, p.pbase(epg) + e)
        kvr = p.resolve(epg, e + 12)
        kv = []
        if kvr:
            for j in range(c):
                kv.append(struct.unpack_from('>II', p.d, p.pbase(epg) + kvr[1] + 8 * j))
        ents.append((s, f, c, kv if kvr else None))
    return tsid, cnt, ents


def build_ps4_anim(a):
    """Blob with the PS4 ANIM item body"""
    B = Blob('anim:' + a['name'])
    clip4 = _anim.convert_clip(a['clip']) if a['clip'] is not None else None
    hdr = B.pos
    B.u32(a['w0']).u32(a['w1']).u64(0).u64(0).u64(0).u32(a['skel']).u32(PAD32).u64(0)
    fix = []
    if a['chans'] is not None:
        rate, chans = a['chans']
        B.pad(16, 0xaa)
        ct = B.pos
        fix.append((hdr + 0x10, ct))
        B.u32(len(chans)).raw(_sw4(rate))
        tab = B.pos
        B.raw(b'\0' * (8 * len(chans)))
        for i, (s, keys, u16, smp) in enumerate(chans):
            B.pad(16, 0xaa)
            fix.append((tab + 8 * i, B.pos))
            ent = B.pos
            B.u32(s).u32(PAD32).u64(0)
            B.pad(16, 0xaa)
            fix.append((ent + 8, B.pos))
            d = B.pos
            B.raw(_sw4(keys)).u64(0).raw(struct.pack('<4H', *u16))
            if smp:
                B.pad(16, 0xaa)
                fix.append((d + 0x58, B.pos))
                B.raw(_sw4(smp))
    if a['events'] is not None:
        tsid, cnt, ents = a['events']
        B.pad(16, 0xaa)
        et = B.pos
        fix.append((hdr + 0x28, et))
        B.u32(tsid).u32(cnt).u64(0)
        if ents:
            B.pad(16, 0xaa)
            ep = B.pos
            fix.append((et + 8, ep))
            for s, f, c, kv in ents:
                B.u32(s).raw(struct.pack('<f', f)).u32(c).u32(PAD32).u64(0)
            for i, (s, f, c, kv) in enumerate(ents):
                if not kv:
                    continue
                B.pad(16, 0xaa)
                fix.append((ep + 24 * i + 16, B.pos))
                for k, v in kv:
                    B.u32(k).u32(v)
    if clip4 is not None:
        B.pad(16, 0xaa)
        fix.append((hdr + 8, B.pos))
        B.raw(clip4)
    for pos, tgt in fix:
        B.relocs.append((pos, B, tgt))
    return B


def anim_blob(p, it):
    return build_ps4_anim(read_ps3_anim(p, it))


# ------------------------------------------------------------------ animation-only paks (anim-*, taps-*)
_pakmod = None


def _anim_pak_module():
    """anim.py loaded from its own source with the U2 ANIM reader / builder in place of the U3 ones"""
    global _pakmod
    if _pakmod is None:
        import os
        import types
        path = os.path.join(u2paktools.PAKTOOLS, 'anim.py')
        import u2actorparts
        src = open(path, encoding='utf-8').read()
        # U2 artgroup / LEVEL_PATS_1 / PACKAGE_ITEM_LOOKUP layouts (u2actorparts)
        a, b = src.find('    # artgroup\n'), src.find("    artgroup = pg.item('ARTGROUP_DATA', ab, name=art)\n")
        if a < 0 or b < a or src.count('    # artgroup\n') != 1:
            raise RuntimeError('anim.py changed: the artgroup block was not found once')
        src = src[:a] + (
            "    art, _t3, _nb, _lists = _u2parts.read_artgroup(p)\n"
            "    targets = [newitem.get((t.page, t.off)) if t is not None else None for t in _t3]\n"
            "    artgroup = pg.item('ARTGROUP_DATA', _u2parts.build_artgroup(art, targets, _u2parts.keys_for(_t3), _nb,"
            " _lists), name=art)\n") + src[b + len("    artgroup = pg.item('ARTGROUP_DATA', ab, name=art)\n"):]
        for old, new in (("    lp.u64(0)\n", "    lp.raw(_u2parts.level_pats(p))\n"),
                         ("    pl = Blob('pkglookup')\n"
                          "    pl.ptr(pl, 0x20).u32(0).u32(1).ptr(pl, 0x20 + ((len(art) + 1 + 15) // 16) * 16)"
                          ".u32(PAD32).u32(PAD32)\n"
                          "    pl.cstr(art)\n"
                          "    pl.pad(16, 0xaa)\n"
                          "    pl.u32(sid(art)).u32(PAD32).ptr(artgroup.data, -0x20)\n",
                          "    pl = _u2parts.lookup_blob(art, artgroup)\n")):
            if src.count(old) != 1:
                raise RuntimeError('anim.py changed: an edit point was not found once')
            src = src.replace(old, new)
        mod = types.ModuleType('u2_anim')
        mod.__file__ = path
        mod._u2parts = u2actorparts
        exec(compile(src, path, 'exec'), mod.__dict__)
        mod.read_ps3_anim = read_ps3_anim
        mod.build_ps4_anim = build_ps4_anim
        _pakmod = mod
    return _pakmod


def convert_pak(src, dst, verbose=False):
    """a U2 PS3 pak that holds only animations (artgroup + ANIM / ANIM_RAWCHANNELS) -> PS4"""
    return _anim_pak_module().convert_pak(src, dst, verbose=verbose)
