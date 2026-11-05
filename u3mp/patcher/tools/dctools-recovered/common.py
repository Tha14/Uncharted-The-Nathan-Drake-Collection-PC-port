import sys, struct
sys.path.insert(0, '/home/claude/u3/paktools')
from dc import DC, sid, dump
P4R = "/mnt/user-data/uploads/Uncharted - The Nathan Drake Collection [CUSA02320]/u3data/build/main/dc1/ps4/"
P4 = "/mnt/user-data/outputs/dc/u3data/build/main/dc1/ps4/net.bin"
P3 = "/home/claude/u3/ps3pkg/tree/dlc-patch18/build/main/bin/dc1/net.bin"
P3D = "/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/ps3dc/net.bin/build/main/bin/dc1/net.bin"
P3_13 = "/home/claude/u3/ps3pkg/tree/dlc-patch13/build/main/bin/dc1/net.bin"
P3DIR = "/home/claude/u3/ps3pkg/tree/dlc-patch18/build/main/bin/dc1/"
PAL = 0x290349e3

def objtype(c, o):
    return c.u32(o - 4)   # type id precedes object on both (PS4: [0][type])

def part_entries(c):
    """find part entries: pointer to an 'mp-' artgroup string at entry+ps, palette sid at +0x2c/+0x20"""
    pal = {s for s, t, o in c.syms if t == PAL}
    es, po = (0x38, 0x2c) if c.ps == 8 else (0x2c, 0x20)
    r = {}
    for q in sorted(c.ptrs):
        e = q - c.ps
        if e < 0: continue
        s = c.cstr(c.ptr(q))
        if not s or not s.startswith('mp-'): continue
        if c.u32(e + po) not in pal and c.u32(e + po) != 0: continue
        r.setdefault(e, s)
    return r

def fields(c, e):
    if c.ps == 8:
        return [c.u32(e + k) for k in (0x10, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34)]
    return [c.u32(e + k) for k in (0x8, 0xc, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28)]
