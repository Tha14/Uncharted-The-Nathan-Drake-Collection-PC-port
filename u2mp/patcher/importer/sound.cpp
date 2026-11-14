// importer/sound.cpp: the sounds of the Uncharted 2 import.
//
// XVAG streams (music, speech, effects): PS3 XVAG (big-endian header, PS-ADPCM codec 6) -> the PS4 XVAG the game reads.
// No re-encoding; only the container changes (the four PS-ADPCM music tracks retail PS4 ships, 146/148/149/150_
// JungleExplore_*, are the reference pairs):
//   header  'XVAG', u32 header size, version, 0, 0, flags, 20 zero bytes (PS3 01 00 00 60, PS4 02 00 00 62; PS4 words
//           little endian)
//   chunks  tag, u32 size, data. fmat: PS3 7 words (channels, codec, samples, samples, 1, rate, data size); PS4 the same
//           7 words with word 4 = the interleave (16-byte frames per channel and block) and then 1, channels. cpan: the
//           words byte-swapped except word 2 (a float PS3 already stores little endian). 0000 ends the header.
//   data    PS3 interleaves the channels frame by frame; PS4 stores blocks of <interleave> frames per channel, the
//           last block zero-padded (one channel: padded to a whole block).
// Sound banks (SCREAM SBlk v4 / v5 -> v13): the Uncharted 3 converter (u3mp/patcher/tools/fixes/sound-banks), MP3
// samples through ffmpeg.
#include "u2import.hpp"
#include "u3fix.hpp"   // u3::sound_bank_convert, u3::find_ffmpeg

namespace u2imp {
namespace {

// frames per channel and block: XVAG_INTERLEAVE (u2import.hpp; retail 148/149_JungleExplore)

uint32_t be(const Bytes &d, size_t o) {
    if (o + 4 > d.size()) throw std::invalid_argument("XVAG: truncated");
    return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}
void le(Bytes &o, uint32_t v) {
    for (int k = 0; k < 4; k++) o.push_back(uint8_t(v >> (8 * k)));
}

}  // namespace

Bytes xvag_convert(const Bytes &d, uint32_t interleave) {
    if (d.size() < 4 || std::string(d.begin(), d.begin() + 4) != "XVAG") throw std::invalid_argument("ValueError: not an XVAG file");
    const uint32_t hs = be(d, 4);
    if (hs > 0x1000 || d[4] || d[5]) throw std::invalid_argument("ValueError: not a big-endian (PS3) XVAG");
    struct Chunk {
        std::string tag;
        Bytes data;
    };
    std::vector<Chunk> chunks;
    for (size_t o = 0x20; o + 8 <= hs;) {
        const uint32_t size = be(d, o + 4);
        const size_t a = std::min(d.size(), o + 8), b = std::min(d.size(), o + 8 + size);
        chunks.push_back({std::string(d.begin() + long(o), d.begin() + long(o + 4)), Bytes(d.begin() + long(a), d.begin() + long(b))});
        o += 8 + size;
    }
    bool ok = !chunks.empty() && chunks.front().tag == "fmat" && chunks.back().tag == "0000";
    for (const Chunk &c : chunks) ok = ok && (c.tag == "fmat" || c.tag == "cpan" || c.tag == "0000");
    if (!ok) throw std::invalid_argument("ValueError: unexpected XVAG chunks");
    const Bytes &f = chunks[0].data;
    if (f.size() < 28) throw std::invalid_argument("XVAG: short fmat");
    const uint32_t ch = be(f, 0), codec = be(f, 4), ns = be(f, 8), ns2 = be(f, 12), rate = be(f, 20), dsize = be(f, 24);
    if (codec != 6) throw std::invalid_argument(fmt("ValueError: XVAG codec %u (only PS-ADPCM, codec 6, is converted)", codec));
    if (!ch || size_t(hs) + dsize > d.size() || dsize % (16 * ch))
        throw std::invalid_argument(fmt("ValueError: XVAG data size %u does not hold whole frames for %u channels", dsize, ch));
    const uint8_t *data = d.data() + hs;
    const uint32_t nfr = dsize / 16 / ch;   // frames per channel
    const uint32_t il = ch > 1 ? interleave : std::max(1u, std::min(interleave, nfr));
    Bytes body_data;
    if (ch > 1) {
        const uint32_t nblk = (nfr + il - 1) / il;
        for (uint32_t b = 0; b < nblk; b++)
            for (uint32_t c = 0; c < ch; c++)
                for (uint32_t k = 0; k < il; k++) {
                    const uint32_t fr = b * il + k;   // the channel's frame
                    if (fr < nfr) body_data.insert(body_data.end(), data + 16 * (size_t(fr) * ch + c), data + 16 * (size_t(fr) * ch + c) + 16);
                    else body_data.insert(body_data.end(), 16, 0);
                }
    } else {
        body_data.assign(data, data + dsize);
        const size_t blk = 16 * size_t(il);
        body_data.resize(body_data.size() + (blk - body_data.size() % blk) % blk, 0);
    }
    Bytes body;
    for (const Chunk &c : chunks) {
        Bytes c2;
        if (c.tag == "fmat") {
            for (uint32_t v : {ch, codec, ns, ns2, il, rate, dsize, 1u, ch}) le(c2, v);
        } else if (c.tag == "cpan") {
            const size_t n = c.data.size() / 4;
            for (size_t k = 0; k < n; k++) le(c2, be(c.data, 4 * k));
            if (n > 2)
                for (int k = 0; k < 4; k++) c2[8 + k] = c.data[8 + k];   // the float is in PS4 byte order already
            c2.insert(c2.end(), c.data.begin() + long(4 * n), c.data.end());
        } else
            c2 = c.data;
        body.insert(body.end(), c.tag.begin(), c.tag.end());
        le(body, uint32_t(c2.size()));
        body.insert(body.end(), c2.begin(), c2.end());
    }
    Bytes out = {'X', 'V', 'A', 'G'};
    le(out, uint32_t(0x20 + body.size()));
    for (uint8_t b : {2, 0, 0, 0x62}) out.push_back(b);
    out.insert(out.end(), 20, 0);
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), body_data.begin(), body_data.end());
    return out;
}

// --ffmpeg, the package's u3mp/bin/ffmpeg.exe, then RECIPE_FFMPEG, msys2 and PATH (as the U3 recipe looks)
std::string find_ffmpeg(const std::string &arg) {
    if (!arg.empty() && fs::is_regular_file(P(arg))) return S(fs::absolute(P(arg)));
    const fs::path bin = launcher_root() / "u3mp" / "bin" / "ffmpeg.exe";
    std::error_code ec;
    if (fs::is_regular_file(bin, ec)) return S(fs::absolute(bin));
    return u3::find_ffmpeg("");
}

Bytes bank_convert(const Bytes &ps3, const std::string &ffmpeg, std::string &note) {
    try {
        return u3::sound_bank_convert(ps3, ffmpeg, launcher_root() / "u3mp" / "patcher" / "tools" / "fixes" / "sound-banks" / "adsr_env.json", note);
    } catch (const std::invalid_argument &e) {
        throw Unsupported(e.what());
    }
}

}  // namespace u2imp
