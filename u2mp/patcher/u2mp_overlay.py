"""Uncharted 2 multiplayer (u2mp): the /app0 overlay the launcher lays over Uncharted 2.

Made on the player's PC from their own PS4 game; nothing game-derived ships with the launcher.

  u2data/build/main/dc1/ps4/menu.bin   the main menu with a Multiplayer button

The Collection's menu script still holds the multiplayer pages (PAGE_MULTIPLAYER and the rest) and
the MENU_MULTIPLAYER text, but no button leads there. Two main menus need one:
  PAGE_NEWPROFILE  (no save yet)  keeps a hidden button for the multiplayer medals page
                                  (BUTTON_MULTIMEDALS, go-to-page with a visibility condition): it
                                  becomes Multiplayer (label MENU_MULTIPLAYER, target PAGE_MULTIPLAYER,
                                  always visible). PAGE_NEWPROFILE_BETA's copy is changed the same way.
  PAGE_PROFILE     (with saves)   Continue, Chapters (hidden), a spacer, New Game, Load Game, Options,
                                  Bonuses: the spacer (kind 1, no name) becomes the same Multiplayer
                                  button, in the left column under Continue, and the columns take the
                                  original layout: Continue, Multiplayer, Options, Bonuses on the left,
                                  New Game and Load Game on the right.

menu.bin is Naughty Dog DC data ("00CD", version 1): u32 data end at +0x08; 64-bit little-endian
pointers are offsets into the file; after the data, a u32 byte count and a bitmap with one bit per
8-byte slot of the file (least significant bit first), set where the slot holds a pointer (the
loader relocates those). Names are NUL-terminated strings with a 32-bit id, the CRC-32 of the name
(MSB first, polynomial 0x04C11DB7, no initial value, no final xor). Page record: +0x00 name,
+0x08 id, +0x38 pointer to {u64 count, u64 base} of 0x70-byte control records:
  +0x00 name           +0x08 name id         +0x10 kind (u32: 2 button, 1 spacer)  +0x14 flags
  +0x20 label          +0x28 label id
  +0x30 action (u32: 20 go to page)          +0x34 visibility condition (u32, 0 = always)
  +0x48 target page    +0x50 target page id  +0x58 column (u32, 0xffffffff for the spacer)
Every value is checked before anything is changed; the source file is only read.

Rebuilt only when the source changed (state.json). The launcher passes the overlay as
UNCHARTED_U2MP_OVERLAY (tools/probe_uncharted.py lays it over /app0 for Uncharted 2 only).

usage: python u2mp/patcher/u2mp_overlay.py <game folder> <overlay folder> [--update <folder>]
"""
import argparse
import json
import struct
import sys
from pathlib import Path

MENU = Path("u2data/build/main/dc1/ps4/menu.bin")
VERSION = 4  # bump when the overlay changes for the same sources (4: PAGE_PROFILE gets its own 8-control list)
RECORD = 0x70


def name_id(name):
    """A DC name's id: CRC-32, MSB first, polynomial 0x04C11DB7, init 0, no final xor."""
    crc = 0
    for byte in name.encode("ascii"):
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else crc << 1
            crc &= 0xFFFFFFFF
    return crc


def cstring(data, offset):
    """The NUL-terminated ASCII name at offset, or None."""
    if not 0 < offset < len(data):
        return None
    end = data.find(b"\0", offset, offset + 96)
    if end <= offset:
        return None
    try:
        return data[offset:end].decode("ascii")
    except UnicodeDecodeError:
        return None


def strings(data, name):
    """Offsets of every whole copy of `name` (a NUL-terminated string not part of a longer name)."""
    found, needle, at = [], name.encode("ascii") + b"\0", 0
    while (at := data.find(needle, at)) >= 0:
        before = data[at - 1] if at else 0
        if not (chr(before).isalnum() or before == ord("_")):
            found.append(at)
        at += 1
    return found


def records_named(data, name):
    """Offsets of 8-byte-aligned pointers to a copy of `name`: the records that start with that name."""
    targets = set(strings(data, name))
    return [at for at in range(0, len(data) - 0x58, 8)
            if struct.unpack_from("<Q", data, at)[0] in targets]


def patch_menu(data):
    """Turn the main menu's hidden medals button into Multiplayer. Returns the patched records."""
    pages = strings(data, "PAGE_MULTIPLAYER")
    if not pages:
        raise ValueError("menu.bin has no PAGE_MULTIPLAYER (not Uncharted 2?)")
    label_id, page_id = name_id("MENU_MULTIPLAYER"), name_id("PAGE_MULTIPLAYER")
    done = []
    for record in records_named(data, "BUTTON_MULTIMEDALS"):
        own_id, = struct.unpack_from("<I", data, record + 0x08)
        target, = struct.unpack_from("<Q", data, record + 0x48)
        target_id, = struct.unpack_from("<I", data, record + 0x50)
        if own_id != name_id("BUTTON_MULTIMEDALS") or cstring(data, target) != "PAGE_MULTIMEDALS" \
                or target_id != name_id("PAGE_MULTIMEDALS"):
            continue
        struct.pack_into("<Q", data, record + 0x28, label_id)
        struct.pack_into("<Q", data, record + 0x48, pages[0])
        struct.pack_into("<Q", data, record + 0x50, page_id)
        struct.pack_into("<I", data, record + 0x34, 0)
        done.append(record)
    if not done:
        raise ValueError("menu.bin has no hidden medals button (not the Uncharted 2 1.02 menu?)")
    done.append(patch_profile_page(data, done[0]))
    return done


def pointer_bitmap(data):
    """(offset of the bitmap, its byte count): one bit per 8-byte slot, set for pointers."""
    if data[:4] != b"00CD":
        raise ValueError("menu.bin is not DC data")
    end, = struct.unpack_from("<I", data, 8)
    size, = struct.unpack_from("<I", data, end)
    if end + 4 + size != len(data) or size * 8 < end // 8:
        raise ValueError("menu.bin's pointer bitmap is not where expected")
    return end + 4, size


def set_pointer_bit(data, bitmap, offset):
    slot = offset // 8
    data[bitmap + slot // 8] |= 1 << (slot % 8)


def is_pointer(data, bitmap, offset):
    slot = offset // 8
    return bool(data[bitmap + slot // 8] >> (slot % 8) & 1)


def page_controls(data, page_name):
    """(count, base) of the one page record named page_name."""
    found = []
    for record in records_named(data, page_name):
        if struct.unpack_from("<I", data, record + 0x08)[0] != name_id(page_name):
            continue
        array, = struct.unpack_from("<Q", data, record + 0x38)
        if 0 < array < len(data) - 16:
            found.append(struct.unpack_from("<QQ", data, array))
    if len(found) != 1:
        raise ValueError(f"expected one {page_name} page, found {len(found)}")
    return found[0]


def patch_profile_page(data, button):
    """PAGE_PROFILE (the main menu once there are saves) gets a new control list, appended after the
    data: Continue, Chapters, Multiplayer (a copy of the patched button `button` from PAGE_NEWPROFILE,
    with this page's button flags), the spacer (column 0xffffffff: a row break, the columns start again
    below it), then Options and Bonuses in the left column and New Game and Load Game in the right one,
    as in the original game (the Collection had New Game / Load Game left, Options / Bonuses right).
    The page's list pointer moves to the new list; the old list stays, unused. The pointer bitmap is
    rebuilt for the longer file. Returns the offset of the new list."""
    bitmap, _ = pointer_bitmap(data)
    end, = struct.unpack_from("<I", data, 8)
    pages = [r for r in records_named(data, "PAGE_PROFILE")
             if struct.unpack_from("<I", data, r + 0x08)[0] == name_id("PAGE_PROFILE")]
    if len(pages) != 1:
        raise ValueError(f"expected one PAGE_PROFILE page, found {len(pages)}")
    page = pages[0]
    if not is_pointer(data, bitmap, page + 0x38):
        raise ValueError("PAGE_PROFILE's control list pointer is not a pointer")
    count, base = page_controls(data, "PAGE_PROFILE")
    if count != 7:
        raise ValueError(f"PAGE_PROFILE has {count} controls, expected 7")
    names = [cstring(data, struct.unpack_from("<Q", data, base + i * RECORD)[0]) for i in range(count)]
    if names != ["BUTTON_CONTINUE", "BUTTON_PROFILE_CHAPTERS", None, "BUTTON_NEWGAME", "BUTTON_LOADGAME",
                 "BUTTON_OPTIONS", "BUTTON_BONUSES"]:
        raise ValueError(f"unexpected PAGE_PROFILE controls {names}")
    spacer = base + 2 * RECORD
    kind, flags = struct.unpack_from("<II", data, spacer + 0x10)
    column, = struct.unpack_from("<I", data, spacer + 0x58)
    if (kind, flags, column) != (1, 0, 0xFFFFFFFF) or \
            any(is_pointer(data, bitmap, spacer + f) for f in range(0, RECORD, 8)):
        raise ValueError("PAGE_PROFILE's spacer is not as expected")
    for index, column in ((3, 0), (4, 0), (5, 1), (6, 1)):
        if struct.unpack_from("<I", data, base + index * RECORD + 0x58)[0] != column:
            raise ValueError("PAGE_PROFILE's columns are not as expected")

    def old(index, new_column=None):
        """(record bytes, pointer fields) of the old list's record `index`."""
        at = base + index * RECORD
        rec = bytearray(data[at:at + RECORD])
        if new_column is not None:
            struct.pack_into("<I", rec, 0x58, new_column)
        return rec, [f for f in range(0, RECORD, 8) if is_pointer(data, bitmap, at + f)]

    multiplayer = bytearray(data[button:button + RECORD])
    struct.pack_into("<I", multiplayer, 0x14, struct.unpack_from("<I", data, base + 3 * RECORD + 0x14)[0])
    struct.pack_into("<I", multiplayer, 0x58, 0)
    records = [old(0), old(1), (multiplayer, [0x00, 0x20, 0x48]), old(2),
               old(5, 0), old(6, 0), old(3, 1), old(4, 1)]
    # The new list: {u64 count, u64 base} then the records, 16-byte aligned after the old data.
    start = (end + 15) & ~15
    header = start
    first = header + 16
    blob = bytearray(start - end) + struct.pack("<QQ", len(records), first)
    pointers = [header + 8]
    for i, (rec, fields) in enumerate(records):
        blob += rec
        pointers += [first + i * RECORD + f for f in fields]
    new_end = end + len(blob)
    slots = (new_end + 7) // 8
    bits = bytearray((slots + 7) // 8)
    old_bits = data[bitmap:]
    bits[:len(old_bits)] = old_bits
    for offset in pointers:
        bits[offset // 64] |= 1 << (offset // 8 % 8)
    rebuilt = bytearray(data[:end]) + blob
    struct.pack_into("<I", rebuilt, 8, new_end)
    struct.pack_into("<Q", rebuilt, page + 0x38, header)
    rebuilt += struct.pack("<I", len(bits)) + bits
    data[:] = rebuilt
    return header


def find(roots, rel):
    for root in roots:
        for base in (root, root / "uroot"):
            if (base / rel).is_file():
                return base / rel
    return None


def stamp(path):
    st = path.stat()
    return [str(path), st.st_size, int(st.st_mtime)]


def build(game, out, update=None, log=print):
    """Make (or keep) the overlay in `out`; returns `out`, or raises with the reason."""
    menu = find([r for r in (update, game) if r], MENU)
    if menu is None:
        raise FileNotFoundError(f"{MENU} not found in the game")
    want = {"version": VERSION, "menu": stamp(menu)}
    state = out / "state.json"
    try:
        if json.loads(state.read_text(encoding="utf-8")) == want and (out / MENU).is_file():
            return out
    except (OSError, ValueError):
        pass
    data = bytearray(menu.read_bytes())
    records = patch_menu(data)
    target = out / MENU
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_suffix(".tmp")
    tmp.write_bytes(data)
    tmp.replace(target)
    state.write_text(json.dumps(want), encoding="utf-8")
    log(f"u2mp: Multiplayer button in the main menu ({len(records)} records) -> {target}")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--update", type=Path)
    a = ap.parse_args()
    build(a.game, a.out, a.update)
    return 0


if __name__ == "__main__":
    sys.exit(main())
