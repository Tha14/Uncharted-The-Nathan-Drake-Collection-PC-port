"""What the PS4 game needs to see imported content: pak22_ps4.txt (level registry) and sound1/config/ps4/banks.txt.

pak22_ps4.txt is a flat list of 'key value' lines; 'level-name X ' (trailing space, as retail) starts a level,
followed by level-id, package, sound-bank, cache, actor and package-ts lines.  The PS4 file is kept byte for byte;
levels it does not define are appended in the same form, made from the PS3 per-level .txt files (dlc-map*, patch):
  * 'package' / 'actor' / 'sound-bank' lines are kept only when the PS4 game or the overlay has that file (as the
    PS4 registry itself dropped the PS3-only weapons of ruins-multi); dropped names are reported;
  * the 8 'package-ts texturedict1/...' lines every retail multiplayer level carries are added (texture streaming
    of the retail textures the converted levels reuse).
Levels with a package the import could not convert get no entry, nor levels none of whose packages exists; any
other missing package line is dropped and
reported (the DLC '-npc' levels name an '<level>-npc' package that no PS3 archive has either).
"""
import re


def parse(text):
    levels, order, cur = {}, [], None
    for line in text.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        k, v = parts[0], parts[1].strip()
        if k == 'level-name':
            cur = levels.setdefault(v, [])
            order.append(v)
            continue
        if cur is not None:
            cur.append((k, v))
    return levels, order


def mp_package_ts(levels):
    """the package-ts lines most retail '*multi*' levels carry"""
    from collections import Counter
    c = Counter(tuple(v for k, v in lines if k == 'package-ts') for n, lines in levels.items() if 'multi' in n)
    return list(c.most_common(1)[0][0]) if c else []


def level_block(name, ps3_txt, have_pak, have_actor, have_bank, ts_lines):
    """-> (text, dropped [(key, value)]) for a level from its PS3 .txt"""
    out = ['level-name %s ' % name]
    dropped = []
    seen_id = False
    for line in ps3_txt.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        k, v = parts[0], parts[1].strip()
        if k == 'level-name':
            continue
        if k == 'level-id':
            seen_id = True
        ok = {'package': have_pak, 'actor': have_actor, 'sound-bank': have_bank}.get(k, lambda _v: True)(v)
        if ok:
            out.append('%s %s' % (k, v))
        else:
            dropped.append((k, v))
    if not seen_id:
        out.insert(1, 'level-id %s' % name)
    out += ['package-ts %s' % t for t in ts_lines]
    return '\n'.join(out) + '\n', dropped


def build_registry(ps4_text, new_levels, have_pak, have_actor, have_bank, failed_paks=()):
    """new_levels: {name: PS3 txt}.  -> (text, added [names], skipped {name: why}, dropped {name: [...]})"""
    levels, order = parse(ps4_text)
    ts = mp_package_ts(levels)
    ps4_text, dropped = prune_actors(ps4_text, have_actor)
    text = ps4_text if ps4_text.endswith('\n') else ps4_text + '\n'
    added, skipped = [], {}
    for name in sorted(new_levels):
        if name in levels:
            # kept as PS4 has it; say whether the PS3 entry would have read differently
            mine = [(k, v) for k, v in levels[name] if k != 'package-ts']
            ps3 = [(k, v) for k, v in parse('level-name %s\n%s' % (name, new_levels[name]))[0][name]]
            extra = [kv for kv in ps3 if kv not in mine]
            gone = [kv for kv in mine if kv not in ps3]
            skipped[name] = 'the PS4 registry already defines it (%s)' % (
                'same lines as the PS3 entry' if not extra and not gone else
                'PS3 differs: %d lines only PS3 has %s, %d only PS4 has %s' % (
                    len(extra), ['%s %s' % kv for kv in extra[:3]], len(gone), ['%s %s' % kv for kv in gone[:3]]))
            continue
        pk = [v for k, v in parse('level-name %s\n%s' % (name, new_levels[name]))[0][name] if k == 'package']
        pk = pk or [name]          # no package line (the-fort-multi-wide-2): the level's own pak
        bad = [v for v in pk if any(n in failed_paks for n in package_file_names(v))]
        if bad:
            skipped[name] = 'its package %s was not converted' % ', '.join(bad)
            continue
        if not any(have_pak(v) for v in pk):
            skipped[name] = 'none of its packages (%s) was converted or is on PS4' % ', '.join(pk)
            continue
        block, d = level_block(name, new_levels[name], have_pak, have_actor, have_bank, ts)
        text += block
        added.append(name)
        if d:
            dropped[name] = d
    return text, added, skipped, dropped


MP_LEVEL = re.compile(r'multi|coop')


def prune_actors(ps4_text, have_actor):
    """The retail PS4 entries of the multiplayer / co-op levels with the 'actor' lines removed whose actor pak exists
    neither on PS4 nor in the overlay. Retail ruins-multi lists the 4 map pack 1 props (mp-ruins-heli-blade,
    plankbridge-break-lod0, plankbridge-colblocker, railing-destructible) although it ships the base-game Ruins,
    whose GOTY entry and data name none of them; without the map pack nothing provides them. Every other line stays
    byte for byte.  -> (text, {'<level> (PS4 entry)': ['actor x', ...]})"""
    out, pruned, cur = [], {}, None
    for line in ps4_text.splitlines(keepends=True):
        parts = line.strip().split(None, 1)
        if len(parts) == 2 and parts[0] == 'level-name':
            cur = parts[1].strip()
        elif len(parts) == 2 and parts[0] == 'actor' and cur and MP_LEVEL.search(cur) and \
                not have_actor(parts[1].strip()):
            pruned.setdefault('%s (PS4 entry)' % cur, []).append(('actor', parts[1].strip()))
            continue
        out.append(line)
    return ''.join(out), pruned


def build_banks_txt(ps4_text, new_banks):
    names = [l.strip() for l in ps4_text.splitlines() if l.strip()]
    have = {n.lower() for n in names}
    add = [b for b in sorted(new_banks) if b.lower() not in have]
    nl = '\r\n' if '\r\n' in ps4_text else '\n'
    text = ps4_text if not ps4_text or ps4_text.endswith(('\n', '\r\n')) else ps4_text + nl
    return text + ''.join(b + nl for b in add), add


def package_file_names(pkg):
    """file names a registry 'package' line may refer to (retail: 'x-ingame' -> x-ingame10.pak)"""
    names = [pkg]
    if re.search(r'-ingame$', pkg):
        names.append(pkg + '10')
    return names
