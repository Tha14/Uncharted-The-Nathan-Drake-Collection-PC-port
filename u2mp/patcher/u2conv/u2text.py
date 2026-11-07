"""Text banks (text1/<lang>.<category>): the PS4 banks with the PS3 strings they lack (the DLC map titles
'The Facility', 'The Highrise', 'The Museum' ... are missing from the PS4 networking bank; the dc map table names
them by these ids; the multiplayer menus use about 25 more ids that only the PS3 dlc-patch7 networking bank has:
stat labels, leaderboard footer, 'Time Played', '[A] daily players', 'Clan Tag' ...; see u2mp/docs/MENUS.md).

Bank layout (big-endian on PS3 and PS4): u32 count; count x {u32 hash, u32 offset} sorted by hash; the string blob
(offsets relative to its start, NUL-terminated strings).  PS4 and PS3 use the same language numbers (2..23).
Rule: PS4 strings are never changed; only ids the PS4 bank does not have are added, their PS3 string appended to the
blob.
"""
import struct


def parse(d):
    n = struct.unpack_from('>I', d, 0)[0]
    blob = 4 + 8 * n
    recs = [struct.unpack_from('>II', d, 4 + 8 * i) for i in range(n)]
    return recs, d[blob:]


def string(blob, off):
    return blob[off:blob.index(b'\0', off)]


def merge(ps4, ps3):
    """-> (bytes or None when nothing is missing, [added ids])"""
    r4, b4 = parse(ps4)
    r3, b3 = parse(ps3)
    have = {h for h, _ in r4}
    add = [(h, string(b3, o)) for h, o in r3 if h not in have]
    if not add:
        return None, []
    blob = bytearray(b4)
    recs = list(r4)
    for h, s in add:
        recs.append((h, len(blob)))
        blob += s + b'\0'
    recs.sort()
    out = struct.pack('>I', len(recs)) + b''.join(struct.pack('>II', h, o) for h, o in recs) + bytes(blob)
    return out, [h for h, _ in add]
