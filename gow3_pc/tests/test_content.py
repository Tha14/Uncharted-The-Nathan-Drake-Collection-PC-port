from paths import ROOT
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import content_profile

EXE=ROOT/'out/content-test'

def write_sfo(path, values):
    """A param.sfo with these values (int: integer, str: UTF-8 text)."""
    keys, data, index = b'', b'', b''
    for k, v in values.items():
        raw = struct.pack('<I', v) if isinstance(v, int) else v.encode() + b'\0'
        size = len(raw) if isinstance(v, int) else (len(raw) + 3) // 4 * 4
        index += struct.pack('<HHIII', len(keys), 0x0404 if isinstance(v, int) else 0x0204, len(raw), size, len(data))
        keys += k.encode() + b'\0'
        data += raw.ljust(size, b'\0')
    keys = keys.ljust((len(keys) + 3) // 4 * 4, b'\0')
    start = 20 + len(index)
    path.write_bytes(struct.pack('<4sIIII', b'\0PSF', 0x101, start, start + len(keys), len(values)) + index + keys + data)

class ProfileTests(unittest.TestCase):
    def test_sfo_parameters_and_explicit_trial_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir()
            write_sfo(p/'sce_sys/param.sfo',{'USER_DEFINED_PARAM_1':13,'USER_DEFINED_PARAM_4':0xffffffff})
            content_profile.prepare(p,p,'trial')
            self.assertEqual(struct.unpack('<8s5I',(p/'content.bin').read_bytes()),(b'G3CONT01',1,13,0,0,0xffffffff))

    def test_invalid_sfo_parameter_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir()
            write_sfo(p/'sce_sys/param.sfo',{'USER_DEFINED_PARAM_1':'13'})
            with self.assertRaisesRegex(ValueError,'invalid user-defined'):
                content_profile.prepare(p,p)

@unittest.skipUnless(EXE.exists(),'run bash build.sh --test first')
class ContentTests(unittest.TestCase):
    def run_case(self,*args):
        return subprocess.run([str(EXE.resolve()),*args],capture_output=True,text=True,timeout=5)

    def test_lifecycle_and_output_boundaries(self):
        r=self.run_case();self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_missing_profile_does_not_report_loaded(self):
        r=self.run_case('--missing');self.assertEqual(r.returncode,21)
        self.assertIn('needs --content-profile',r.stderr)

    def test_other_modules_are_counted_independently(self):
        r=self.run_case('--unknown');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
