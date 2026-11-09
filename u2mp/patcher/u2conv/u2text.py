"""Text banks (text1/<lang>.<category>): the PS4 banks, completed for every language (u2mp/docs/TEXTS.md).

Bank layout (big-endian on PS3 and PS4): u32 count; count x {u32 hash, u32 offset} sorted by hash; the string blob
(offsets relative to its start, NUL-terminated strings; offset 0 is "UNTRANSLATED STRING!!!", the game's miss text).
PS4 and PS3 use the same language numbers (2 english ... 18 polish); PS4 adds 19 sam-spanish, 20 brazilian, 21 greek,
22 czech, 23 turkish, whose networking banks are placeholders ("_", ~397 of 412) and common banks ~98 of them.

Rule: a real PS4 string is never changed. A text the PS4 bank lacks, or has only as the placeholder "_", comes from
(in this order) the GOTY PS3 bank of the same language, then the related PS4 language (sam-spanish <- spanish,
brazilian <- portuguese, each already completed from GOTY). The runtime (gow3_pc/src/u2mp/texts.cpp) then has its own
text for ids no bank has and for the menu texts greek / czech / turkish lack, and shows English for the rest (the
English banks are always written for that).
"""
import struct

ENGLISH = 2
# language number -> the language whose text it shows before English
RELATED = {19: 6, 20: 9}


def parse(d):
    n = struct.unpack_from('>I', d, 0)[0]
    blob = 4 + 8 * n
    recs = [struct.unpack_from('>II', d, 4 + 8 * i) for i in range(n)]
    return recs, d[blob:]


def string(blob, off):
    return blob[off:blob.index(b'\0', off)]


def strings(d):
    recs, blob = parse(d)
    return {h: string(blob, o) for h, o in recs}


def is_placeholder(s):
    """The PS4 stand-in for a text it never translated (shows as a blank: '_' is the game's hard space)."""
    return s == b'_'


def fill(bank, sources):
    """bank completed from `sources` (bank bytes, first wins): ids it lacks are added, its placeholders replaced.
    -> (bytes or None when nothing changed, [added ids], [replaced ids])"""
    recs, blob = parse(bank)
    have = {h: string(blob, o) for h, o in recs}
    src = [strings(s) for s in sources if s]
    out = bytearray(blob)
    index = dict(recs)
    added, replaced = [], []

    def pick(h):
        for s in src:
            v = s.get(h)
            if v is not None and not is_placeholder(v):
                return v
        return None

    for h, s in have.items():
        if is_placeholder(s):
            v = pick(h)
            if v is not None:
                index[h] = len(out)
                out += v + b'\0'
                replaced.append(h)
    for s in src:
        for h in s:
            if h not in index:
                v = pick(h)
                if v is not None:
                    index[h] = len(out)
                    out += v + b'\0'
                    added.append(h)
    if not added and not replaced:
        return None, [], []
    rs = sorted(index.items())
    data = struct.pack('>I', len(rs)) + b''.join(struct.pack('>II', h, o) for h, o in rs) + bytes(out)
    return data, added, replaced


def merge(ps4, ps3):
    """-> (bytes or None when nothing is missing, [added ids]): the ids ps3 has and ps4 lacks (or has as '_')"""
    d, added, replaced = fill(ps4, [ps3])
    return d, added + replaced


def complete(ps4, goty):
    """Every language's bank of one category, completed.
    ps4: {language: bank bytes}; goty: {language: [bank bytes, newest first]}.
    -> {language: (bytes or None when the PS4 bank needs nothing, {'goty': n, 'related': n, 'left': n})}
    English is always written: the runtime shows it for what a language still lacks ('left': the ids English has
    and the language has not, or only as '_'; texts.cpp has its own text for the menu ones)."""
    step1, counts = {}, {}
    for lang, bank in ps4.items():
        d, a, r = fill(bank, goty.get(lang, []))
        step1[lang] = d or bank
        counts[lang] = {'goty': len(a) + len(r), 'related': 0, 'left': 0}
    english = strings(step1[ENGLISH]) if ENGLISH in step1 else {}
    out = {}
    for lang, bank in step1.items():
        data = bank
        if lang in RELATED and RELATED[lang] in step1:
            d, a, r = fill(data, [step1[RELATED[lang]]])
            if d:
                data = d
                counts[lang]['related'] = len(a) + len(r)
        have = strings(data)
        counts[lang]['left'] = sum(1 for h, s in english.items()
                                   if not is_placeholder(s) and (h not in have or is_placeholder(have[h])))
        out[lang] = (data if data != ps4[lang] or lang == ENGLISH else None, counts[lang])
    return out
