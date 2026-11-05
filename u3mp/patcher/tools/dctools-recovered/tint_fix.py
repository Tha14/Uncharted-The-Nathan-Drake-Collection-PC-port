"""MP colour fix (code, not DC data). Drop-in for build_u3_profile_probe.build():
    from tint_fix import apply_tint_fix; apply_tint_fix(image)
`image` is indexed by PS4 virtual address, as in that builder (image[0xc3b822] ...).

Cause (one systematic port bug). PS3 code and data pack colours as 0xAARRGGBB, and PS3 unpacks
them as R = c>>16, G = c>>8, B = c (e.g. PS3 medal text 0x64f250: rldicl 48/56/clrldi). Several
PS4 MP paths unpack the same u32 as R = c>>24, G = c>>16, B = c>>8 (lanes [c>>24, c>>16&ff,
c>>8&ff, c&ff]/255, lanes 0..2 used as RGB). The colour comes out shifted by one byte:
(A, R, G) = (1.0, R, G) for opaque colours.

  site      function                         colour source
  0x690533  NetCharacter init 0x690100       kit/hero part colours (desc +0x10..)
  0xbc9eed  preview refresh 0xbc9e50         same, lobby character
  0xbca852  0xbc9fc0                         same
  0xc24bce  award/medal popup 0xc22f70       medal text colour ([r12+0x20] or gold 0xffffb52d,
                                             i.e. (255,181,45) on PS3; PS4 shows (255,255,181))
Producers of the kit colours (GetDesc 0x997a40, Initialize 0x9954f0, GetRandomDesc 0x994d20,
picker 0x7590a0) all pack 0xff000000|R<<16|G<<8|B, exactly like PS3 GetDesc 0x40fa78.

Fix. Reorder the four lane inserts so the vector is [R, G, B, A]; same instruction lengths,
only ModRM bytes change, and the registers already hold those bytes."""

PATCHES = (
    # (address of vmovd, original bytes, new bytes)
    (0x69054f, 'c5f96ec1 c4e37922c201 c4e37922c602 c4e37922c003',
               'c5f96ec2 c4e37922c601 c4e37922c002 c4e37922c103'),
    (0xbc9f09, 'c5f96ec1 c4e37922c201 c4e37922c602 c4e37922c003',
               'c5f96ec2 c4e37922c601 c4e37922c002 c4e37922c103'),
    (0xbca86e, 'c5f96ec1 c4e37922c201 c4e37922c602 c4e37922c003',
               'c5f96ec2 c4e37922c601 c4e37922c002 c4e37922c103'),
    (0xc24beb, 'c5f96ec8 c4e37122c901 c4e37122ca02 c4e37122ce03',     # xmm1: eax,ecx,edx,esi
               'c5f96ec9 c4e37122ca01 c4e37122ce02 c4e37122c803'),    #    -> ecx,edx,esi,eax
)


def apply_tint_fix(image):
    for a, o, n in PATCHES:
        o, n = bytes.fromhex(o.replace(' ', '')), bytes.fromhex(n.replace(' ', ''))
        cur = bytes(image[a:a + len(o)])
        if cur == n:
            continue
        if cur != o:
            raise ValueError('tint site mismatch %#x' % a)
        image[a:a + len(n)] = n
    return image


if __name__ == '__main__':
    import sys
    d = bytearray(open(sys.argv[1], 'rb').read())   # flat image indexed by vaddr
    apply_tint_fix(d)
    open(sys.argv[2], 'wb').write(d)
