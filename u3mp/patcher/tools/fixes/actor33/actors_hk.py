#!/usr/bin/env python3
"""Rebuild the Havok reflection tables (data/hk2010_ps3.json, data/hk2011_ps4.json) that hkconvert.py / fgcoll.py need
to convert the PS3 foreground collision of props with their own skeleton (ps4actor --own-skel, the Lab boxes), from the
user's own executables, and put them into every toolset of this job. Derived from the executables, so never shipped.

  actors_hk.py [--ps3-elf EBOOT.elf] [--ps4-elf eboot.elf] [--objdump objdump]

  PS3: the DECRYPTED Uncharted 3 1.19 EBOOT (RPCS3 'Decrypt PS3 binaries' on dev_hdd0/game/BCES01670/USRDIR/EBOOT.BIN);
       the 1.10 disc EBOOT gives 356 instead of 359 classes and different files: the 2026-11-01 tables came from 1.19.
  PS4: the decrypted Nathan Drake Collection eboot (ELF, 33 961 008 bytes on this PC: u3_ps4.elf).
  Defaults: $RECIPE_PS3_ELF / $RECIPE_PS4_ELF, then {{PS3_UPDATE}}/EBOOT.elf and {{PS4}}/eboot.elf.
The PS4 constructor scan reads an Intel-syntax disassembly of the first PT_LOAD segment (GNU objdump, as on 2026-11-01).
Reference: hk2010_ps3.json sha256 de095f79..., hk2011_ps4.json 73ef4b8c... (359/306 and 490/341 classes/with members).
Idempotent: nothing is done when both tables already have the reference hashes."""
import argparse, hashlib, os, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REF = {'hk2010_ps3.json': 'de095f792cf29b43b57488a05e984d93766699ad2d30fe19cd3864f184a1c145',
       'hk2011_ps4.json': '73ef4b8c2cb179983dc6eda851f86082e77ca957ed2fa4508252335df9c36d04'}
TMP = "{{SCRATCH}}/actor33-work/hk"


def sha(f):
    return hashlib.sha256(open(f, 'rb').read()).hexdigest() if os.path.exists(f) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ps3-elf', default=os.environ.get('RECIPE_PS3_ELF') or "{{PS3_UPDATE}}/EBOOT.elf")
    ap.add_argument('--ps4-elf', default=os.environ.get('RECIPE_PS4_ELF') or "{{PS4}}/eboot.elf")
    ap.add_argument('--objdump', default=shutil.which('objdump') or 'objdump')
    a = ap.parse_args()
    dirs = [os.path.join(HERE, 'toolsets', t, 'data') for t in sorted(os.listdir(os.path.join(HERE, 'toolsets')))
            if os.path.exists(os.path.join(HERE, 'toolsets', t, 'hkconvert.py'))]
    if all(sha(os.path.join(d, k)) == v for d in dirs for k, v in REF.items()):
        print('hk tables already in place (%d toolsets)' % len(dirs)); return 0
    have = all(sha(os.path.join(TMP, k)) == v for k, v in REF.items())
    if not have:
        for f, what in ((a.ps3_elf, 'decrypted PS3 1.19 EBOOT.elf (--ps3-elf / RECIPE_PS3_ELF)'),
                        (a.ps4_elf, 'decrypted PS4 eboot ELF (--ps4-elf / RECIPE_PS4_ELF)')):
            if not os.path.exists(f) or open(f, 'rb').read(4) != b'\x7fELF':
                print('missing input: %s (%s); own-skeleton actor paks cannot be converted' % (what, f)); return 3
        os.makedirs(TMP, exist_ok=True)
        d = open(a.ps4_elf, 'rb').read()
        ph = struct.unpack_from('<Q', d, 0x20)[0]
        t, fl, of, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, ph)
        assert t == 1 and va == 0, 'first PT_LOAD is not the text segment at 0'
        open(os.path.join(TMP, 'text.bin'), 'wb').write(d[of:of + fs])
        with open(os.path.join(TMP, 'ps4.dis'), 'w') as fo:
            subprocess.run([a.objdump, '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', os.path.join(TMP, 'text.bin')],
                           stdout=fo, check=True)
        subprocess.run([sys.executable, os.path.join(HERE, 'mkhk.py'), a.ps3_elf, a.ps4_elf, os.path.join(TMP, 'ps4.dis'), TMP], check=True)
        subprocess.run([sys.executable, os.path.join(HERE, 'mkhk_write.py'), a.ps3_elf, a.ps4_elf, os.path.join(TMP, '_recs.pkl'), TMP], check=True)
        os.remove(os.path.join(TMP, 'ps4.dis')); os.remove(os.path.join(TMP, 'text.bin'))
        # (the tables stay in TMP: build_actors.py copies them into its toolsets on every run)
        bad = [k for k, v in REF.items() if sha(os.path.join(TMP, k)) != v]
        if bad:
            print('the rebuilt tables differ from the 2026-11-01 ones (%s): other executables? not installed' % bad); return 4
    for dd in dirs:
        os.makedirs(dd, exist_ok=True)
        for k in REF:
            shutil.copyfile(os.path.join(TMP, k), os.path.join(dd, k))
    print('hk tables installed into %d toolsets' % len(dirs))
    return 0


if __name__ == '__main__':
    sys.exit(main())
