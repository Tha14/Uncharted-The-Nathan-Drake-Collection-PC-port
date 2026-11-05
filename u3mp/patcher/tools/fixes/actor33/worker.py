"""In-process ps4actor runner for one paktools version (used by search.py / build_actors.py).

usage: worker.py <paktools dir>   then one JSON job per stdin line:
  {"ps3":..., "tmpl":..., "out":..., "kw":{convert() keyword args}, "env":{PS4ACTOR_* vars}, "post":[...]}
answers one JSON line per job: {"sha":..., "size":..., "vram":..., "err":...}
post: "cubefix", "cb3fix" (the later assetbatch post passes from paktools/stage), "rename:<old>:<new>",
  "flatfix" (2026-10-27 batch30-flatcolor flatfix.py: generated flat-color.tga VRAM_DESC 0x47 -> sRGB 0x48, +0x20
  0x010f0000), "cubetype" (cubefix.fix_retail_types alone: desc type 1 -> 2 where +0x20 is 0x06xxxxxx).
"""
import hashlib, importlib, io, json, os, sys, contextlib, traceback
tools = sys.argv[1]
sys.path.insert(0, tools)
sys.path.insert(1, os.path.join(tools, 'stage'))
ENVK = ('PS4ACTOR_SB_EXACT', 'PS4ACTOR_SB_DROP', 'PS4ACTOR_FILL_STREAMS', 'PS4ACTOR_SPREAD_MATS', 'PS4ACTOR_CB3_MIN',
        'PS4ACTOR_SKEL3', 'PS4ACTOR_DROP_MAT', 'PS4ACTOR_DONORS', 'PS4ACTOR_NO_TMPL_TEX')
import ps4actor
cur_env = {}


def vdesc_fix(path, what):
    """recovered 10-27 post passes on the VRAM_DESC items (flatfix.py of batch30-flatcolor; cubefix.fix_retail_types)"""
    import struct
    from ndpak import Pak
    q = Pak(path); d = bytearray(q.d); n = 0
    for it in q.items_of('VRAM_DESC'):
        db = q.pbase(it.page) + it.data_off
        typ, fmt, u20 = struct.unpack_from('<III', d, db + 0x18)
        if what == 'flatfix':
            r = q.resolve(it.page, it.data_off + 0x38)
            if 'u3mp/flat-color.tga/' in (q.pstr(*r) if r else '') and fmt == 0x47 and u20 == 0x01000000:
                struct.pack_into('<II', d, db + 0x1c, 0x48, 0x010f0000); n += 1
        elif typ == 1 and (u20 >> 24) == 6:
            struct.pack_into('<I', d, db + 0x18, 2); n += 1
    if n:
        open(path, 'wb').write(bytes(d))
    return n
for line in sys.stdin:
    if not line.strip():
        continue
    j = json.loads(line)
    env = {k: v for k, v in j.get('env', {}).items()}
    if env != cur_env:
        for k in ENVK:
            os.environ.pop(k, None)
        os.environ.update(env)
        ps4actor = importlib.reload(ps4actor)
        cur_env = env
    res = {}
    try:
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            if j.get('rename'):
                import ps4rename
                old, new = j['rename']
                ps4rename.rename_pak(j['tmpl'], j['out'], old, new)
            elif j.get('anim') is not None:
                import anim
                anim.convert_pak(j['ps3'], j['out'], stm_dirs=j['anim'])
            else:
                ps4actor.convert(j['ps3'], j['tmpl'], j['out'], verbose=True, **j.get('kw', {}))
            for p in j.get('post', []):
                if p == 'cubefix':
                    import cubefix
                    cubefix.fix(j['out'], j['ps3'])
                elif p == 'cb3fix':
                    import cb3fix
                    cb3fix.fix(j['out'], j['out'])
                elif p in ('flatfix', 'cubetype'):
                    vdesc_fix(j['out'], p)
        d = open(j['out'], 'rb').read()
        last = (buf.getvalue().strip().splitlines() or [''])[-1]
        res = dict(sha=hashlib.sha256(d).hexdigest(), size=len(d), last=last[-160:])
    except BaseException as ex:
        res = dict(err=('%s: %s' % (type(ex).__name__, ex))[:300])
    sys.stdout.write(json.dumps(res) + '\n')
    sys.stdout.flush()
