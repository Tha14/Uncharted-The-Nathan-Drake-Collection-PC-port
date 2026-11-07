"""Sound banks: PS3 SCREAM bank (SBlk v4/v5, big-endian) -> PS4 SCREAM bank (SBlk v13), with the Uncharted 3
converter (u3mp/patcher/tools/fixes/sound-banks: bnkconv.py, vagenc.py; MP3 samples need ffmpeg).

U2 banks are the same SBlk versions as U3's.  The retail U2 PS4 banks were rebuilt by Bluepoint from source data
(their .sbf files name the source .bank; samples are often longer/higher rate), so a converted bank is not
byte-identical to retail: validate.py compares structure (sound / grain / name counts and header fields) on the
banks both platforms have.  The PS4 game reads only the .bnk (no executable string names .sbf).
"""
import os
import shutil
import sys

import u2paktools

SB = os.path.join(os.path.dirname(u2paktools.PAKTOOLS), 'fixes', 'sound-banks')
TT = os.path.join(os.path.dirname(u2paktools.PAKTOOLS), 'transfer-tools')
for _p in (SB, TT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

FF_DEFAULT = [os.environ.get('RECIPE_FFMPEG', ''), r'C:\msys64\clang64\bin\ffmpeg.exe',
              r'C:\msys64\ucrt64\bin\ffmpeg.exe']


def find_ffmpeg(arg=''):
    here = os.path.dirname(os.path.abspath(__file__))
    bins = [os.path.join(here, '..', '..', '..', 'u3mp', 'bin', 'ffmpeg.exe')]
    for c in [arg] + bins + FF_DEFAULT + [shutil.which('ffmpeg') or '']:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    return None


def convert(ps3, ffmpeg=None):
    """-> (PS4 bank bytes, notes)"""
    import bnkconv
    import convert_banks
    enc = convert_banks.make_sample_encoder(ffmpeg)
    try:
        out = bnkconv.convert(ps3, mp3_to_adpcm=enc)
    except RuntimeError as e:
        if 'ffmpeg' in str(e):
            raise NotImplementedError('bank has MP3 samples and ffmpeg was not found')
        raise
    r = bnkconv.parse_ps3(ps3)
    return out, ['SBlk v%d -> v13, %d sounds, %d grains' % (r['ver'], r['nsnd'], r['ngr'])]
