"""Convert the 'light' MP assets missing on PS4 (pickups, treasures, MP prop twins, skeleton skins).

usage: python lightbatch.py <PS3 dir> <PS4 template dir(s), ':'-separated> <out dir> [names...]

Each entry: (name, mode, template, options)
  rename  - PS4 template renamed with ps4rename.py (PS3 data equals the PS4 campaign actor)
  graft   - ps4actor.py --graft: PS3 geometry + template skeleton/anims/collision (props with own skeleton)
  kit     - ps4actor.py as for character parts (shared hero/chloe skeleton)
PS3 textures are resident on PS4; like mpbatch, textures are capped (256 -> 128 -> 64) until VRAM
fits LIGHT_VRAM_BUDGET (default 128 KB, the budget that made kit parts load).
"""
import os, re, sys, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
JOBS = [
    ('propane-tank-mp', 'rename', 'propane-tank', {}),
    ('mp-hero-skel-bones-glow', 'rename', 'mp-hero-skel-bones', {}),
    ('capture-idol-silver', 'graft', 'capture-idol-gold', {}),
    ('turret-gatling-tripod-mp', 'graft', 'turret-gatling-tripod', {}),
    ('gold-treasure-aa', 'graft', 'gold-treasure-ac', {}),
    ('gold-treasure-ab', 'graft', 'gold-treasure-ac', {}),
    ('gold-treasure-aj', 'graft', 'gold-treasure-ak', {}),
    ('ivory-treasure-aa', 'graft', 'ivory-treasure-ai', {}),
    ('ivory-treasure-ab', 'graft', 'ivory-treasure-ai', {}),
    ('ivory-treasure-ad', 'graft', 'ivory-treasure-ai', {}),
    ('silver-treasure-ab', 'graft', 'silver-treasure-av', {}),
    ('silver-treasure-ad', 'graft', 'silver-treasure-ax-1', {}),
    ('silver-treasure-aj', 'graft', 'silver-treasure-ax-1', {}),
    ('mp-chloe-skel', 'kit', 'mp-hero-skel-bones', {'skel': 'mp-chloe-syria-torso'}),
]


def find(tdirs, name):
    for d in tdirs:
        f = os.path.join(d, name + '.pak')
        if os.path.exists(f):
            return f
    return None


def main():
    src, tdirs, out = sys.argv[1], sys.argv[2].split(':'), sys.argv[3]
    only = set(sys.argv[4:])
    os.makedirs(out, exist_ok=True)
    budget = int(os.environ.get('LIGHT_VRAM_BUDGET', '131072'))
    for name, mode, tmpl, opt in JOBS:
        if only and name not in only:
            continue
        tp = find(tdirs, tmpl)
        dst = os.path.join(out, name + '.pak')
        if mode == 'rename':
            r = subprocess.run([sys.executable, os.path.join(HERE, 'ps4rename.py'), tp, dst, tmpl, name],
                               capture_output=True, text=True)
            last = (r.stdout.strip().splitlines() or [''])[-1]
            print(('OK ' if r.returncode == 0 else 'FAIL ') + name, '<- rename', tmpl, '|', last if r.returncode == 0 else r.stderr[-400:])
            continue
        cmd = [sys.executable, os.path.join(HERE, 'ps4actor.py'), os.path.join(src, name + '.pak'), tp, dst,
               '--match-streams', '--mat-by-file']
        if mode == 'graft':
            cmd += ['--graft', '--stream-flags', '3']
        else:
            cmd += ['--stream-flags', os.environ.get('MP_STREAM_FLAGS', '2')]
        if opt.get('skel'):
            cmd += ['--skel-template', find(tdirs, opt['skel'])]
        for mt in ('256', '128', '64'):
            r = subprocess.run(cmd + ['--max-tex', mt], capture_output=True, text=True)
            last = (r.stdout.strip().splitlines() or [''])[-1]
            m = re.search(r'vram 0x([0-9a-f]+)', last)
            if r.returncode or not m or int(m.group(1), 16) <= budget:
                break
        print(('OK ' if r.returncode == 0 else 'FAIL ') + name, '<-', mode, tmpl, 'tex<=' + mt, '|',
              last[len(dst) + 8:] if r.returncode == 0 else r.stderr.strip()[-600:])


if __name__ == '__main__':
    main()
