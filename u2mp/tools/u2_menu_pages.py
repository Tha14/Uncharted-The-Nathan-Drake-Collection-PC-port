"""List the pages of Uncharted 2's menu.bin that hold given controls, with every control on them.

Page record (pointed to by a pointer to its name): +0x00 name, +0x08 id, +0x38 pointer to
{u64 count, u64 base} of control records 0x70 bytes each. Control: +0x00 name, +0x08 id,
+0x10 kind (u32), +0x14 flags (u32), +0x20 label, +0x28 label id, +0x30 action (u32),
+0x34 condition (u32), +0x48 target page, +0x58 column (u32), +0x5c width (u32).
Read-only; prints names and numbers only.

usage: python u2mp/tools/u2_menu_pages.py <menu.bin> [control name ...]
"""
import struct
import sys


def cstr(d, o):
    if not 0 < o < len(d):
        return None
    e = d.find(b"\0", o, o + 96)
    try:
        return d[o:e].decode("ascii") if e > o else None
    except UnicodeDecodeError:
        return None


def main():
    d = open(sys.argv[1], "rb").read()
    want = set(sys.argv[2:]) or {"BUTTON_MULTIMEDALS", "BUTTON_CONTINUE", "BUTTON_NEWGAME"}
    pages = []
    for at in range(0, len(d) - 0x48, 8):
        name = cstr(d, struct.unpack_from("<Q", d, at)[0])
        if not name or not name.startswith("PAGE_"):
            continue
        arr, = struct.unpack_from("<Q", d, at + 0x38)
        if not 0 < arr < len(d) - 16:
            continue
        count, base = struct.unpack_from("<QQ", d, arr)
        if not 0 < count < 200 or not 0 < base < len(d) or base + count * 0x70 > len(d):
            continue
        ctrls = []
        for r in range(base, base + count * 0x70, 0x70):
            cname = cstr(d, struct.unpack_from("<Q", d, r)[0])
            kind_check, = struct.unpack_from("<I", d, r + 0x10)
            if cname is None and kind_check != 1:   # only spacers (kind 1) have no name
                break
            kind, flags = struct.unpack_from("<II", d, r + 0x10)
            label = cstr(d, struct.unpack_from("<Q", d, r + 0x20)[0])
            action, cond = struct.unpack_from("<II", d, r + 0x30)
            target = cstr(d, struct.unpack_from("<Q", d, r + 0x48)[0])
            col, width = struct.unpack_from("<II", d, r + 0x58)
            ctrls.append((r, cname or "(spacer)", kind, flags, label, action, cond, target, col, width))
        else:
            pages.append((at, name, arr, ctrls))
    for at, name, arr, ctrls in pages:
        if not want & {c[1] for c in ctrls}:
            continue
        print(f"{name} record 0x{at:x} controls {len(ctrls)} (array 0x{arr:x})")
        for r, cname, kind, flags, label, action, cond, target, col, width in ctrls:
            print(f"   0x{r:x} {cname:<34} kind {kind} flags 0x{flags:x} action {action} cond {cond} "
                  f"col {col} w {width} label {label} -> {target}")


if __name__ == "__main__":
    main()
