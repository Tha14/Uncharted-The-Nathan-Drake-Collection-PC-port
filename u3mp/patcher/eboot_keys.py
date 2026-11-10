"""The values the multiplayer helper needs from the PS3 Uncharted 3 1.19 executable, read on the player's PC from their
own decrypted EBOOT.elf, so that no release carries them.

The helper (profile-helper.bin) ships with an empty key block: the 16-byte marker KEY_MAGIC followed by KEY_BLOCK_SIZE
zero bytes. mpimage.link() fills that block in the image it builds (see fill()), from the player's EBOOT.elf, or, for a
relink without one, from the player's current image (carry()).

Block layout (little-endian words, the helper reads them in place):
  +0x0000  Blowfish state for the profile seal: 18 P words then 4 x 256 S words (1042 x u32), scheduled here from the
           EBOOT's own start tables with the EBOOT's Blowfish key (PS3 f_9480b4 does the same at run time)
  +0x1048  HMAC-SHA1 key of the profile seal, 64 bytes, then a zero byte
  +0x1089  storage access key, 20 bytes, then a zero byte
  +0x109e  storage secret, 40 bytes, then a zero byte
The offsets are those of the tested 1.19 EBOOT (every region decrypts to the same file, PS3_ELF_SHA256 in u3mp_import)."""
import hashlib
import struct

KEY_MAGIC = b'U3MP-EBOOT-KEYS1'   # 16 bytes, right before the block in the helper
PS3_ELF_SHA256 = 'aac4faa63ab1387e8aaa0b8e9e80ee87ac4ae61e0b40292f7aa7401df7b3f50a'
BF_KEY = (0xdde3d0, 20)           # (file offset, length) in the decrypted 1.19 EBOOT.elf
HMAC_KEY = (0xdde388, 64)
STORE_ACCESS = (0xdddb10, 20)
STORE_SECRET = (0xdddb28, 40)
BF_P_START = (0x243f6a88, 0x85a308d3)   # Blowfish's standard start words, found in the EBOOT (P array and S boxes)
BF_S_START = (0xd1310ba6, 0x98dfb5ac)

OFF_BF, OFF_HMAC, OFF_ACCESS, OFF_SECRET = 0x0, 0x1048, 0x1089, 0x109e
KEY_BLOCK_SIZE = 0x10c8


class KeyError_(Exception):
    """The EBOOT is not the tested 1.19 one, or a value is not where it should be."""


def _blowfish_state(elf, key):
    p0 = elf.find(struct.pack('>2I', *BF_P_START))
    s0 = elf.find(struct.pack('>2I', *BF_S_START))
    if p0 < 0 or s0 < 0:
        raise KeyError_('the Blowfish tables are not in this EBOOT')
    P = list(struct.unpack_from('>18I', elf, p0))
    S = [list(struct.unpack_from('>256I', elf, s0 + 1024 * i)) for i in range(4)]
    for i in range(18):
        P[i] ^= int.from_bytes(bytes(key[(4 * i + k) % len(key)] for k in range(4)), 'big')
    mask = 0xffffffff

    def f(x):
        return ((((S[0][x >> 24] + S[1][(x >> 16) & 255]) & mask) ^ S[2][(x >> 8) & 255]) + S[3][x & 255]) & mask

    def enc(l, r):
        for i in range(16):
            l ^= P[i]
            r ^= f(l)
            l, r = r, l
        l, r = r, l
        return l ^ P[17], r ^ P[16]

    l = r = 0
    for i in range(0, 18, 2):
        l, r = enc(l, r)
        P[i], P[i + 1] = l, r
    for b in range(4):
        for i in range(0, 256, 2):
            l, r = enc(l, r)
            S[b][i], S[b][i + 1] = l, r
    return P + S[0] + S[1] + S[2] + S[3]


def block_from_eboot(elf):
    """The filled key block from the bytes of the decrypted 1.19 EBOOT.elf."""
    if hashlib.sha256(elf).hexdigest() != PS3_ELF_SHA256:
        raise KeyError_('not the tested PS3 1.19 EBOOT.elf')

    def take(at):
        off, n = at
        v = elf[off:off + n]
        if len(v) != n or b'\0' in v:
            raise KeyError_('a value is not where it should be in the EBOOT')
        return v

    out = bytearray(KEY_BLOCK_SIZE)
    struct.pack_into('<1042I', out, OFF_BF, *_blowfish_state(elf, take(BF_KEY)))
    for off, at in ((OFF_HMAC, HMAC_KEY), (OFF_ACCESS, STORE_ACCESS), (OFF_SECRET, STORE_SECRET)):
        v = take(at)
        out[off:off + len(v)] = v
    return bytes(out)


def find(image):
    """Offset of the key block in an image or helper (after KEY_MAGIC), or -1."""
    i = image.find(KEY_MAGIC)
    return -1 if i < 0 else i + len(KEY_MAGIC)


# Images from before v7.97 carry the same values, at fixed places: every player's image of one build is the same file
# (build.json image_sha256). Offsets only; measured with scratch/tools/measure_legacy_keys.py. v7.62 and older: no
# storage secret, so those need the EBOOT.
LEGACY = {   # release image sha256: (build, (Blowfish state, HMAC key, access key, secret) offsets)
    '052fed4ee4f28d3c074225314a4e910d20ab6fb285b04b50492c985cf7eb4457': ('7.67', (0x22b8bd8, 0x16dc9f2, 0x17013ba, 0x235c378)),
    '223914ded5f276f3e9571ca2dccbd7efa877ef766a76d1fb2e0a9003aeaeda63': ('7.68', (0x22b8bd8, 0x16dc9f2, 0x17013ba, 0x235c5f8)),
    '9a2250af821c119d9379d12213213eb692d9dcbd58a3e76b7ad7d248fd389472': ('7.69', (0x22b8bd8, 0x16dc9f2, 0x17013ba, 0x235c8a8)),
    '6c081ca6d540552dccf99f75c111914960e68973846a2a92d07794e1631c5dca': ('7.70', (0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235c7d8)),
    'e9b0053f57e32cd40718227c96a55e7e367a824f185b3fc739636955376b10fa': ('7.71', (0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235cb48)),
    'bbe25b75cc2f19288c4a8123ea1f070c0fa73b0c5bbd0925ee904fcea78c556f': ('7.72', (0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235cba8)),
    'f8a820317a758a5e6e84e62fbf33de94125ffa0ba99e68bb8dfafc38ec4d6924': ('7.73', (0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235cc28)),
    '824c00bdae8deeba25149fcc73cc5a8d72b44928f68c79f7c58041732ef75de1': ('7.74', (0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235cc38)),
    '82966c1af8785b34ce73c71db8353e009bfc1db4254f009995b67598938f9a9b': ('7.75', (0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235d248)),
    '277a8393b7c3f319a3420440d4a84a7486a5ee8a795da5764dc10a95bfb95e9c': ('7.76', (0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235d248)),
    'ab51fde046b0eccc6b08a922cde02c8d024e6881461e02588784a2fb17a3b8f5': ('7.77', (0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235d4a8)),
    'e0e3fdcc5abbfd3e4eda848bf88bccf7cdd2b1208af00aa6fc62e5c8a7c761e7': ('7.78', (0x22b8c18, 0x16dc9f2, 0x17013ba, 0x235eee8)),
    '7f516e80358449b698b1ff2f99f9bfb4b4d26aab375eeed06f4025825a63d710': ('7.79', (0x22b8c18, 0x16dc9f2, 0x17013ba, 0x235f778)),
    '2f370f6c08091ad7baa779d1f2397e04ba58bfdbcc2a74c20b42ff8755482b40': ('7.80', (0x22b8c38, 0x16dc9f2, 0x17013ba, 0x235fba8)),
    'ad151cc9ab8cf36cf993921cae11126507baf06e458bbbb6f87bbab2d621913a': ('7.81', (0x22b8c98, 0x16dc9f2, 0x17013ba, 0x2360988)),
    '6a7e730f8a3bfaca21fe5a946704b612238e21eec4311e40b318e5087c435411': ('7.82', (0x22b8d18, 0x16dc9f2, 0x17013ba, 0x2360c18)),
    '65bcd756f45be71c26e7cfd26ba6c27ab9ac811b0cf71a54ad9e8943ccad2eb2': ('7.83', (0x22b8d88, 0x16dc9f2, 0x17013ba, 0x2360cd8)),
    '3541a5c7d4f7d17fb675282ff496acbcbd9db846f5985eab8a4792c8d9932f1b': ('7.84', (0x22b8d88, 0x16dc9f2, 0x17013ba, 0x2361198)),
    'b1f73839dee6df1a35499ee813a83c0876f91d153e2a2d76211809233829c96d': ('7.85', (0x22b8d18, 0x16dc9f2, 0x17013ba, 0x23610d8)),
    '2c2f475befb5f3ada934698358127ba03f8ffbec5224cf69bde947192af02b9f': ('7.86', (0x22b8d18, 0x16dc9f2, 0x17013ba, 0x2361288)),
    'ec1f1b1ee2c1602ce342a1b0bba8f5448fa8efdcdf2295bd544ef4f82f0fc10c': ('7.87', (0x22b8d18, 0x16dc9f2, 0x17013ba, 0x23615f8)),
    '53f952d33a118cb38a8a9ecb11c03fc5cfcbb3d8c5567953195229381e77eb47': ('7.88', (0x22b8d78, 0x16dc9f2, 0x17013ba, 0x23619d8)),
    '6232dc503438c0448f8b63eb15a894346313beb079602562a074fbe8a9f7bd44': ('7.89', (0x22b8d78, 0x16dc9f2, 0x17013ba, 0x2362228)),
    '03377d79488440618ebd7968332e7cbab0aa07e27ae9c786a1dd2db7c2aa5def': ('7.90', (0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2362c28)),
    '4fc26ebbc8a7219bf3f24aa4e3cb3212131d26d2bca928e958f4207ab735e829': ('7.91', (0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2362e88)),
    'ce2961cd73ff0483907987eca50a0dd08af72f23aa335855cc4ad57b238b5e1c': ('7.92', (0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2362ee8)),
    '1a9d9e55dac78437ec1e54bca7ecbacf0bb55280997149d5cfdb46370dcfddc0': ('7.93', (0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2362ff8)),
    '3cace10b439372d5f4a4b7fe9ed8f59e5100371e04b01742b6b10bf9a76e8d4a': ('7.94', (0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2363008)),
    'fb1e1fc76554ded21d7fc18fd5d84f70ce503ceffa991f902c128fab78ecaa4b': ('7.95', (0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2363148)),
    '5abed2917cd43c8f201c4f6540c3144c20b39e9ea7504239e3f2cf74c462cd7d': ('7.96', (0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2362e88)),
}


def carry(old_image):
    """The filled key block of a previous image (a relink without the EBOOT), or None when it has none: a v7.97+ image's
    block, or the values of a released image from before v7.97 (LEGACY)."""
    i = find(old_image)
    if i >= 0:
        block = bytes(old_image[i:i + KEY_BLOCK_SIZE])
        return block if any(block) else None
    known = LEGACY.get(hashlib.sha256(bytes(old_image)).hexdigest())
    if not known:
        return None
    o_bf, o_hk, o_ak, o_sk = known[1]
    out = bytearray(KEY_BLOCK_SIZE)
    out[OFF_BF:OFF_BF + 4168] = old_image[o_bf:o_bf + 4168]
    for off, at, n in ((OFF_HMAC, o_hk, 64), (OFF_ACCESS, o_ak, 20), (OFF_SECRET, o_sk, 40)):
        v = bytes(old_image[at:at + n])
        if len(v) != n or b'\0' in v:
            return None
        out[off:off + n] = v
    return bytes(out)


def fill(image, block):
    """Put the key block into an image (bytearray) whose helper carries the empty block."""
    i = find(image)
    if i < 0:
        raise KeyError_('this multiplayer build has no key block')
    if image.find(KEY_MAGIC, i) != -1:
        raise KeyError_('two key blocks in one image')
    image[i:i + KEY_BLOCK_SIZE] = block
    return image
