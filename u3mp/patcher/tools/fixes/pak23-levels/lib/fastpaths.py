"""Exact faster versions of the two hottest functions of the level converter (profile 2026-11-06, r3 stage paks:
collision._enc_box 37 %, ps3geom._bits_be 27 % of the time).  install() replaces them in the already imported
paktools modules, but only when the module's function is the known original (sha256 of its source), so a converter
version with other code keeps its own.  Both replacements give the same results bit for bit (check_fastpaths.py
compares them on random and recorded inputs); U3_LEVELS_FAST=0 turns them off.
"""
import hashlib
import inspect
import os
import sys

import numpy as np

F32 = np.float32
_KK = np.array([k * k for k in range(16)], dtype=F32)        # F32(k * k), k = 0..15
_INV = F32(1.0 / 226.0)
_K_DESC = np.arange(15, -1, -1)


def enc_box(cmin, cmax, pmin, pmax):
    """collision._enc_box: conservative Codec3Axis byte triple + the decoded box.
    The original tries k = 15 .. 0 one float32 at a time; here the 16 candidates of an axis are computed at once
    with the same float32 operations in the same order (span * k^2, * 1/226, then pmin + / pmax -), and the first
    k from 15 down that passes is taken; the decoded bound is that candidate itself."""
    xyz = []
    dmin = np.empty(3, F32)
    dmax = np.empty(3, F32)
    for a in range(3):
        span = F32(pmax[a] - pmin[a])
        step = (span * _KK) * _INV
        vlo = (pmin[a] + step).astype(F32)
        vhi = (pmax[a] - step).astype(F32)
        tlo = cmin[a] - 1e-6 * max(1.0, abs(cmin[a]))
        thi = cmax[a] + 1e-6 * max(1.0, abs(cmax[a]))
        ok = (vlo <= tlo)[::-1]
        lo = int(_K_DESC[ok.argmax()]) if ok.any() else 0
        ok = (vhi >= thi)[::-1]
        hi = int(_K_DESC[ok.argmax()]) if ok.any() else 0
        xyz.append((lo << 4) | hi)
        dmin[a] = vlo[lo]
        dmax[a] = vhi[hi]
    return bytes(xyz), dmin, dmax


def bits_be(buf, widths, count):
    """ps3geom._bits_be: `count` records of big-endian bit fields of the given widths (int64 array count x fields).
    Same values and the same error as the original when the buffer is too short (a negative shift count)."""
    widths = list(widths)
    W = sum(widths)
    tot = len(buf) * 8
    out = np.zeros((count, len(widths)), dtype=np.int64)
    if count == 0 or not widths:
        return out
    if count * W > tot:
        raise ValueError('negative shift count')
    if max(widths) > 62:
        raise OverflowError('field wider than 62 bits')       # never in the PS3 vertex formats; see fallback
    bits = np.unpackbits(np.frombuffer(bytes(buf), dtype=np.uint8))[:count * W].reshape(count, W).astype(np.int64)
    o = 0
    for c, w in enumerate(widths):
        if w:
            weights = np.left_shift(np.int64(1), np.arange(w - 1, -1, -1, dtype=np.int64))
            out[:, c] = bits[:, o:o + w] @ weights
        o += w
    return out


# sha256 of the original functions' source (identical in tools/paktools and paktools-003b433 / -7d45c1f)
KNOWN = {
    ('collision', '_enc_box'): 'b377aa684b2f38534d7b92d1ee70c678a48d815188d83bfd7c55796807709b78',
    ('ps3geom', '_bits_be'): 'c6fa4ca6d83efe6543c73f2db8f16071a9897c6a85b50b47a0c4816fa70877a6',
}


def _src_sha(f):
    return hashlib.sha256(inspect.getsource(f).replace('\r\n', '\n').encode()).hexdigest()


def install(log=None):
    """Replace the originals in the loaded modules (import collision / ps3geom first or let this import them)."""
    if os.environ.get('U3_LEVELS_FAST', '1') == '0':
        return []
    done = []
    for (mod, name), fast in ((('collision', '_enc_box'), enc_box), (('ps3geom', '_bits_be'), bits_be)):
        try:
            m = sys.modules.get(mod) or __import__(mod)
        except ImportError:
            continue
        f = getattr(m, name, None)
        if f is None or getattr(f, '_u3_fast', False):
            continue
        try:
            same = _src_sha(f) == KNOWN[(mod, name)]
        except (OSError, TypeError):
            same = False
        if not same:
            continue                       # another converter version's code: keep it
        orig = f
        if name == '_bits_be':
            def wrapped(buf, widths, count, _f=fast, _o=orig):
                try:
                    return _f(buf, widths, count)
                except OverflowError:
                    return _o(buf, widths, count)
        else:
            wrapped = fast
        wrapped._u3_fast = True
        wrapped._u3_orig = orig
        setattr(m, name, wrapped)
        done.append(mod + '.' + name)
    if log and done:
        log('fast paths: ' + ', '.join(done))
    return done
