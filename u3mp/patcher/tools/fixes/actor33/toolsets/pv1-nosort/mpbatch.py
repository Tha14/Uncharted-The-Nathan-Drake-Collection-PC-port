"""Pick a PS4 template for each missing MP actor and convert it with ps4actor.

usage: python mpbatch.py <PS3 actor dir> <PS4 template dir> <out dir> [names...]
PS3 textures are resident on PS4 (the PS4 kits stream theirs and keep ~13 KB resident). A kit part
whose VRAM is too large fails to load (level state 12) and NetCharacter::Init then asserts, so each
part is converted with its PS3 textures capped at MP_MAX_TEX (256) and retried at 128 / 64 until its
VRAM fits MP_VRAM_BUDGET (128 KB).
"""
import os, re, sys, subprocess
SKIP = re.compile(r'^mp-(wrk|ruins)-|skel|-coop|^mp-coop-')
def template(n):
    b = n[:-4]
    m = re.match(r'mp-kit-base-(.*)-skin-[bc](-00)?$', b)
    if m:
        part = m.group(1)
        for c in ('mp-kit-base-%s-skin-a-00' % part, 'mp-kit-base-%s-skin-a' % part):
            if c in PS4: return c
        if part.startswith('torso'): return 'mp-kit-base-neck-skin-a'
    if b.startswith('mp-kit-base-torso'): return 'mp-kit-base-neck-skin-a'
    if b.startswith('mp-kit-'):
        if '-torso-' in b or 'killzone-full-a' == b[7:] or b.startswith('mp-kit-misc') or b.startswith('mp-kit-shirt'):
            return 'mp-kit-merc-torso-shirt-w' if '-merc-' in b else 'mp-kit-hunter-torso-shirt-h'
        if '-legs-' in b: return 'mp-kit-hunter-legs-pants-j'
        if '-feet-' in b: return 'mp-kit-hunter-feet-boots-a'
        if '-arms-' in b: return 'mp-kit-base-arms-hands-skin-a-00'
        return 'mp-kit-hunter-head-hat-d'
    if b.startswith('mp-chloe-'): return 'mp-chloe-elena-hair'
    if b.startswith('mp-hero-'):
        if b.endswith('-torso') or b == 'mp-hero-sol-med-b-1a': return 'mp-hero-london-torso'
        if b.endswith('-legs'): return 'mp-hero-london-legs'
        if b.endswith('-hair'): return 'mp-hero-sullivan-hair'
        return 'mp-hero-ramses-bandana'
    return None
PS4 = set()
if __name__ == '__main__':
    src, tdir, out = sys.argv[1:4]
    names = sys.argv[4:] or sorted(os.listdir(src))
    PS4.update(f[:-4] for f in os.listdir(tdir))
    lst = os.path.join(os.path.dirname(__file__), 'ps4_mp_list.txt')   # optional: names of every PS4 mp actor
    if os.path.exists(lst):
        PS4.update(l.strip() for l in open(lst) if l.strip())
    for n in names:
        if SKIP.search(n): print('skip', n); continue
        t = template(n)
        tp = os.path.join(tdir, t + '.pak') if t else None
        if not tp or not os.path.exists(tp): print('no template', n, t); continue
        budget = int(os.environ.get('MP_VRAM_BUDGET', '131072'))
        for mt in (os.environ.get('MP_MAX_TEX', '256'), '128', '64'):
            r = subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), 'ps4actor.py'), os.path.join(src, n), tp, os.path.join(out, n), '--max-tex', mt, '--stream-flags', os.environ.get('MP_STREAM_FLAGS', '2')], capture_output=True, text=True)
            last = (r.stdout.strip().splitlines() or [''])[-1]
            m = re.search(r'vram 0x([0-9a-f]+)', last)
            if r.returncode or not m or int(m.group(1), 16) <= budget:
                break
        print(('OK ' if r.returncode == 0 else 'FAIL ') + n, '<-', t, 'tex<=' + mt, '|', last[:160] if r.returncode == 0 else r.stderr.strip().splitlines()[-1][:200])
