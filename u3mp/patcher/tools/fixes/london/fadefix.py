"""fadefix.py - give fade-flagged background prototypes their fade-distance record.

Cause: PS3 prototype header byte1 bit 0x04 = "fade out with distance", hdr+8 = u16 start, u16 end (metres).
The PS4 build keeps the bit as prototype flag[1] and reads {f32 start, f32 end, 0, 0} through prototype +0x48.
Instance builder 0xe9b3b5: flags[1] != 0 and +0x48 == NULL -> fade start 0, end 1e-8 (constant 0x144b1e0)
= the prototype is never drawn.  The converter dropped the record in the builds now in overlay-r3, so every
flagged prototype (London: 24 of 181 in ldn-train-multi, incl. ug-foundation-concrete x117 instances,
ug-rusted-i-beam-2 trusses, ug-wall-tile-mm, rails, crates) is missing.  Retail stores the record as a 16-byte
RAW_DATA item; this tool appends it (unlisted) to the page that holds the prototype records and points +0x48 at
it, keeping every other byte of the pak (pages are copied verbatim, fixups kept, one fixup added per record).

API: fix(ps4_path, ps3_bytes) -> (new_bytes or None, report dict)"""
import os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from llib import Pak, PT
sys.path.insert(0, PT)
from ps4writer import PakWriter, Blob
sys.path.insert(0, PT + '/stage')
import bggeom


def ps4_protos(q):
    """[(page, off, flags bytes, fade ptr resolved or None, name)] in prototype-list order"""
    gs = q.items_of('GEOMETRY_1')
    if not gs: return []
    g = gs[0]; pg, b = g.page, g.data_off
    n = q.pu32(pg, b + 0x18); pp = q.resolve(pg, b + 0x20)
    out = []
    for i in range(n):
        r = q.resolve(pp[0], pp[1] + 8 * i)
        fl = q.resolve(r[0], r[1] + 0x20)
        flags = q.d[q.pbase(fl[0]) + fl[1]:q.pbase(fl[0]) + fl[1] + 16] if fl else b'\0' * 16
        nm = q.resolve(r[0], r[1] + 0x10)
        out.append((r[0], r[1], flags, q.resolve(r[0], r[1] + 0x48), q.pstr(*nm) if nm else None))
    return out


def ps3_fades(p):
    """[(flag bit set, start, end)] per PS3 prototype"""
    out = []
    for c in bggeom.collection(p):
        hdr = c['hdr']; b = p.pbase(hdr[0]) + hdr[1]
        byte1 = p.d[b + 1]; fs, fe = struct.unpack_from('>HH', p.d, b + 8)
        out.append((bool(byte1 & 4), fs, fe))
    return out


def scan(q):
    pr = ps4_protos(q)
    return [i for i, x in enumerate(pr) if x[2][1] and x[3] is None], pr


def fix(ps4_path, ps3_bytes, inst_flags=False, ps3_pak=None):
    q = Pak(ps4_path)
    need, pr = scan(q)
    rep = dict(protos=len(pr), flagged=sum(1 for x in pr if x[2][1]), missing=len(need), fixed=0, mism=[])
    p = ps3_pak or Pak(ps3_bytes)
    f3 = ps3_fades(p)
    rep['ps3_protos'] = len(f3)
    # the converter writes PS3 prototypes in order, then possibly extra ones (water); match by index and flag
    for i in need:
        if i >= len(f3) or not f3[i][0]:
            rep['mism'].append(i)
    if rep['mism']:
        return None, rep
    for i, x in enumerate(pr):
        if i < len(f3) and bool(x[2][1]) != f3[i][0]:
            rep['mism'].append(('flag', i)); return None, rep
    if not need and not inst_flags:
        return None, rep
    # rebuild: every page verbatim, fade blobs appended to the prototypes' page(s)
    fx = {}
    for s, dpg, off in q.fixups:
        fx.setdefault(s, []).append((off, dpg))
    w = PakWriter()
    pages = []
    for pi, (po, ps, _) in enumerate(q.pages):
        pages.append(w.raw_page(q.d[po:po + ps], sorted(fx.get(pi, []))))
    for i in need:
        pg, off = pr[i][0], pr[i][1]
        fb = Blob('fade%d' % i)
        fb.raw(struct.pack('<4f', float(f3[i][1]), float(f3[i][2]), 0.0, 0.0))
        pages[pg].append(fb)
        pages[pg].redirect(off + 0x48, fb)
        rep['fixed'] += 1
    rep['fade'] = [(i, f3[i][1], f3[i][2]) for i in need]
    if inst_flags:
        rep['inst_flags'] = apply_inst_flags(q, p, pages)
    if not need and not rep.get('inst_flags'):
        return None, rep
    w.login_page = (q.hdr['loginPage'], q.hdr['loginOff'])
    w.vram_table_loc = (q.hdr['vramDescPage'], q.hdr['vramDescOff'])
    w.vram = q.d[q.vram_base:]
    out = w.build()
    verify(out, q, need, f3)
    return out, rep


def apply_inst_flags(q, p, pages):
    """retail rule (ps4stage agent A, ldn-train-stage 13/13): PS3 instance word +8 bit 16+k set -> PS4 instance
    flag byte k (+0x18) = 0.  Instances are matched by their id (+4 == PS3 instance index order per prototype)."""
    gs = q.items_of('GEOMETRY_1')[0]; pg, b = gs.page, gs.data_off
    n = q.pu32(pg, b + 0x28); ip = q.resolve(pg, b + 0x30)
    x8s = [p.pu32(ia[0], ia[1] + 8) for c in bggeom.collection(p) for ia in c['insts']]
    # ps4stage writes the PS3 instances in collection order with id = x8 & 0xffff (+ possibly extra ones at the end)
    changed = 0
    for i in range(min(n, len(x8s))):
        r = q.resolve(ip[0], ip[1] + 8 * i)
        base = q.pbase(r[0]) + r[1]
        iid = struct.unpack_from('<I', q.d, base + 4)[0]
        if iid != x8s[i] & 0xffff:
            raise ValueError('instance order differs from PS3 at %d' % i)
        hi = (x8s[i] >> 16) & 0xf
        if not hi: continue
        cur = struct.unpack_from('<I', q.d, base + 0x18)[0]
        new = cur & ~sum(0xff << (8 * k) for k in range(4) if hi >> k & 1)
        if new != cur:
            pages[r[0]].set_u32(r[1] + 0x18, new); changed += 1
    return changed


def verify(out, q, need, f3):
    n = Pak(out)
    assert len(n.pages) == len(q.pages)
    assert len(n.fixups) == len(q.fixups) + len(need), (len(n.fixups), len(q.fixups), len(need))
    a = [(i.type, i.name, i.page) for i in q.items]; b = [(i.type, i.name, i.page) for i in n.items]
    assert a == b, 'item list changed'
    assert n.d[n.vram_base:] == q.d[q.vram_base:], 'vram changed'
    pr = ps4_protos(n)
    for i in need:
        r = pr[i][3]
        assert r is not None
        v = struct.unpack_from('<4f', n.d, n.pbase(r[0]) + r[1])
        assert v == (float(f3[i][1]), float(f3[i][2]), 0.0, 0.0), (i, v)
    # every other page byte identical (apart from the page-size word, the fade pointers, instance flag words)
    gs = n.items_of('GEOMETRY_1')[0]; ip = n.resolve(gs.page, gs.data_off + 0x30)
    ninst = n.pu32(gs.page, gs.data_off + 0x28)
    iflag = {}
    for i in range(ninst):
        r = n.resolve(ip[0], ip[1] + 8 * i); iflag.setdefault(r[0], set()).add(r[1] + 0x18)
    for pi in range(len(q.pages)):
        po, ps, _ = q.pages[pi]; npo, nps, _ = n.pages[pi]
        assert nps >= ps
        old = q.d[po:po + ps]; new = n.d[npo:npo + ps]
        allowed = {pr[i][1] + 0x48 for i in need if pr[i][0] == pi} | iflag.get(pi, set()) | {0xc}
        for k in range(0, ps - 3, 4):
            if old[k:k + 4] != new[k:k + 4] and k not in allowed and k - 4 not in allowed:
                raise AssertionError('unexpected change page %d off %#x' % (pi, k))
    return True
