// Recipe step coop-voice-fort: the Fort co-op voice fix, the 84 vox entries the PS4 vox-mp.bin lacks converted from
// PS3 1.19 (dlc-patch18.psarc dc1/vox-mp.bin) and appended, plus the speech streams they name that neither the
// overlay nor the PS4 english_ps4.psarc has, copied from the PS3 goty-speech-english.psarc. Replaces apply_vo.py.
//   u3fix coop-vo --project P --job J --ps4 D --ps3-disc D --ps3-update D [--apply]
// Entry (PS3 0x28 BE -> PS4 0x30 LE, an object with the type word before it): id, two u16 + two u8, flags, tag, a u16
// pair + two u8 (the last = the block count), the blocks {id, 0, u16 + two u8, line count, lines*} and the lines
// {name*, 0, 0, 0, u32, u16 + two u8} (PS4 0x20 bytes). Anything else in an entry stops the step. The PS4 symbol table
// grows (the data moves by 16 bytes per new symbol), the new symbols are sorted in, the relocation bitmap rebuilt.
// The build goes to <job>/build/vox-mp.bin; --apply installs it (the overlay copy saved as vox-mp.bin.bak-coopvo).
#include "proc.hpp"
#include "psarc.hpp"
#include "u3fix.hpp"

#include <set>

namespace u3 {
namespace {

const uint32_t VOX = 0xe3825e8d;

struct DcInfo {
    uint64_t reloc;
    struct Sym {
        uint32_t s, t;
        uint64_t o;
    };
    std::vector<Sym> syms;
    std::set<uint64_t> ptrs;
};
uint32_t rd32(const Bytes &d, uint64_t o, bool be) {
    if (o + 4 > d.size()) throw Stop("struct.error: unpack_from requires a buffer of at least 4 bytes");
    return be ? be32(d, size_t(o)) : rd32le(d, size_t(o));
}
uint64_t rd64(const Bytes &d, uint64_t o) {
    if (o + 8 > d.size()) throw Stop("struct.error: unpack_from requires a buffer of at least 8 bytes");
    return rd64le(d, size_t(o));
}
DcInfo read_dc(const Bytes &d, bool be) {
    DcInfo r;
    r.reloc = be ? rd32(d, 8, true) : rd64(d, 8);
    const uint32_t n = rd32(d, 0x14, be);
    uint64_t o = 0x18;
    for (uint32_t i = 0; i < n; i++) {
        if (be) r.syms.push_back({rd32(d, o, true), rd32(d, o + 4, true), rd32(d, o + 8, true)}), o += 12;
        else r.syms.push_back({rd32(d, o, false), rd32(d, o + 4, false), rd64(d, o + 8)}), o += 16;
    }
    const uint32_t cnt = rd32(d, r.reloc, be);
    const int ps = be ? 4 : 8;
    for (uint64_t bi = 0; bi < cnt && r.reloc + 4 + bi < d.size(); bi++) {
        const uint8_t b = d[size_t(r.reloc + 4 + bi)];
        for (int k = 0; k < 8; k++)
            if (b >> k & 1) r.ptrs.insert((bi * 8 + uint64_t(be ? 7 - k : k)) * uint64_t(ps));
    }
    return r;
}
std::string cstr(const Bytes &d, uint64_t o) {
    uint64_t e = o;
    while (e < d.size() && d[size_t(e)]) e++;
    if (e >= d.size()) throw Stop("ValueError: subsection not found");
    return std::string(d.begin() + std::ptrdiff_t(o), d.begin() + std::ptrdiff_t(e));
}
Bytes sub(const Bytes &d, uint64_t a, uint64_t b) {
    a = std::min<uint64_t>(a, d.size());
    b = std::min<uint64_t>(std::max(a, b), d.size());
    return Bytes(d.begin() + std::ptrdiff_t(a), d.begin() + std::ptrdiff_t(b));
}

struct Line {
    std::string name;
    uint32_t ls;
    Bytes lf;
};
struct Block {
    uint32_t sid;
    Bytes bf;
    std::vector<Line> lines;
};
struct Entry {
    uint32_t h, flags, tag;
    Bytes b4, meta;
    std::vector<Block> blocks;
};

// a PS3 big-endian vox entry at o; stops on anything this converter does not handle
Entry conv_entry(const Bytes &a, const std::set<uint64_t> &ap, uint64_t o) {
    auto u = [&](uint64_t p) { return rd32(a, p, true); };
    Entry e;
    e.h = u(o);
    e.b4 = sub(a, o + 4, o + 8);
    if (u(o + 8) || u(o + 0xc) || u(o + 0x10) || u(o + 0x18)) throw Stop(u3::fmt("entry %08x has fields this converter does not handle", e.h));
    e.flags = u(o + 0x14);
    e.tag = u(o + 0x1c);
    e.meta = sub(a, o + 0x20, o + 0x24);
    if (e.meta.size() < 4) throw Stop("IndexError: index out of range");
    const int nblocks = e.meta[3];
    if (!nblocks || !ap.count(o + 0x24)) throw Stop(u3::fmt("entry %08x: no blocks", e.h));
    const uint64_t bp = u(o + 0x24);
    for (int i = 0; i < nblocks; i++) {
        const uint64_t q = bp + uint64_t(i) * 0x18;
        Block b;
        b.sid = u(q);
        const uint32_t zero = u(q + 4);
        b.bf = sub(a, q + 8, q + 0xc);
        const uint32_t count = u(q + 0xc), lp = u(q + 0x10);
        if (zero || !ap.count(q + 0x10)) throw Stop(u3::fmt("entry %08x: block %d layout", e.h, i));
        for (uint32_t k = 0; k < count; k++) {
            const uint64_t l = lp + uint64_t(k) * 0x18;
            if (!ap.count(l) || u(l + 4) || u(l + 8) || u(l + 0xc)) throw Stop(u3::fmt("entry %08x: line %u layout", e.h, k));
            b.lines.push_back({cstr(a, u(l)), u(l + 0x10), sub(a, l + 0x14, l + 0x18)});
        }
        e.blocks.push_back(std::move(b));
    }
    return e;
}
Bytes sw16x(const Bytes &b) {   // swap the first u16, keep bytes 2..3
    if (b.size() < 2) throw Stop("IndexError: index out of range");
    Bytes o{b[1], b[0]};
    o.insert(o.end(), b.begin() + std::min<std::ptrdiff_t>(2, std::ptrdiff_t(b.size())), b.begin() + std::min<std::ptrdiff_t>(4, std::ptrdiff_t(b.size())));
    return o;
}

struct Built {
    Bytes res;
    std::vector<DcInfo::Sym> add;
    std::vector<Entry> entries;
};
Built build(const Bytes &c, const Bytes &a) {
    const DcInfo C = read_dc(c, false), A = read_dc(a, true);
    std::set<uint32_t> have;
    for (const auto &s : C.syms) have.insert(s.s);
    Built B;
    for (const auto &s : A.syms)
        if (!have.count(s.s)) B.add.push_back(s);
    std::string bad;
    for (const auto &s : B.add)
        if (s.t != VOX) bad += (bad.empty() ? "" : ", ") + u3::fmt("'%#x'", s.t);
    if (!bad.empty()) throw Stop("non-vox symbols to add: [" + bad + "]");
    for (const auto &s : B.add) B.entries.push_back(conv_entry(a, A.ptrs, s.o));
    const uint64_t shift = B.add.size() * 16;
    const uint64_t tbl_old_end = 0x18 + C.syms.size() * 16;
    Bytes body = sub(c, tbl_old_end, C.reloc);
    std::set<uint64_t> new_ptrs;
    auto put = [&](uint64_t at, uint64_t v, int n) {
        if (at + uint64_t(n) > body.size()) throw Stop("struct.error: pack_into out of range");
        for (int k = 0; k < n; k++) body[size_t(at) + size_t(k)] = uint8_t(v >> (8 * k));
    };
    for (uint64_t p : C.ptrs)
        if (p >= tbl_old_end) {
            put(p - tbl_old_end, rd64(c, p) + shift, 8);
            new_ptrs.insert(p + shift);
        }
    const uint64_t base = tbl_old_end + shift;
    std::vector<DcInfo::Sym> out_syms;
    for (const auto &s : C.syms) out_syms.push_back({s.s, s.t, s.o + shift});
    auto here = [&] { return base + body.size(); };
    auto pad = [&](uint64_t n) {
        while (here() % n) body.push_back(0);
    };
    std::vector<std::string> order;   // the line names, first use first
    std::map<std::string, uint64_t> strs;
    for (const Entry &e : B.entries)
        for (const Block &b : e.blocks)
            for (const Line &l : b.lines)
                if (!strs.count(l.name)) strs[l.name] = 0, order.push_back(l.name);
    for (const std::string &s : order) {
        strs[s] = here();
        body.insert(body.end(), s.begin(), s.end());
        body.push_back(0);
    }
    auto putq = [&](uint64_t pos, uint64_t v) {
        put(pos - base, v, 8);
        new_ptrs.insert(pos);
    };
    auto setb = [&](uint64_t pos, const Bytes &b) {
        if (pos - base + b.size() > body.size()) throw Stop("slice assignment out of range");
        std::copy(b.begin(), b.end(), body.begin() + std::ptrdiff_t(pos - base));
    };
    for (size_t ei = 0; ei < B.entries.size(); ei++) {
        const Entry &e = B.entries[ei];
        pad(16);
        body.resize(body.size() + 12, 0);   // a 16-byte aligned object with its type just before it
        for (int k = 0; k < 4; k++) body.push_back(uint8_t(VOX >> (8 * k)));
        const uint64_t off = here();
        body.resize(body.size() + 0x28, 0);
        put(off - base, e.h, 4);
        setb(off + 4, sw16x(e.b4));
        put(off - base + 0x18, e.flags, 4);
        put(off - base + 0x20, e.tag, 4);
        setb(off + 0x24, sw16x(e.meta));
        const uint64_t blk_at = here() + 8;
        body.resize(body.size() + 8, 0);
        putq(off + 0x28, blk_at);
        body.resize(body.size() + 0x18 * e.blocks.size(), 0);
        for (size_t i = 0; i < e.blocks.size(); i++) {
            const Block &b = e.blocks[i];
            const uint64_t q = blk_at + i * 0x18;
            put(q - base, b.sid, 4);
            setb(q + 8, sw16x(b.bf));
            put(q - base + 0xc, b.lines.size(), 4);
            const uint64_t la = here();
            body.resize(body.size() + 0x20 * b.lines.size(), 0);
            putq(q + 0x10, la);
            for (size_t k = 0; k < b.lines.size(); k++) {
                const uint64_t l = la + k * 0x20;
                putq(l, strs.at(b.lines[k].name));
                put(l + 0x18 - base, b.lines[k].ls, 4);
                setb(l + 0x1c, sw16x(b.lines[k].lf));
            }
        }
        out_syms.push_back({B.add[ei].s, B.add[ei].t, off});
    }
    std::stable_sort(out_syms.begin(), out_syms.end(), [](const auto &x, const auto &y) { return x.s < y.s; });
    while (here() % 16) body.push_back(0);
    const uint64_t reloc = here();
    Bytes res = sub(c, 0, 0x18);
    for (int k = 0; k < 8; k++) res[8 + size_t(k)] = uint8_t(reloc >> (8 * k));
    for (int k = 0; k < 4; k++) res[0x14 + size_t(k)] = uint8_t(out_syms.size() >> (8 * k));
    for (const auto &s : out_syms) {
        for (int k = 0; k < 4; k++) res.push_back(uint8_t(s.s >> (8 * k)));
        for (int k = 0; k < 4; k++) res.push_back(uint8_t(s.t >> (8 * k)));
        for (int k = 0; k < 8; k++) res.push_back(uint8_t(s.o >> (8 * k)));
    }
    // the symbol table slots are pointer slots in the retail bitmap too, when it marks them
    if (C.ptrs.count(0x20))
        for (size_t i = 0; i < out_syms.size(); i++) new_ptrs.insert(0x18 + i * 16 + 8);
    res.insert(res.end(), body.begin(), body.end());
    const uint64_t nbytes = (reloc + 63) / 64;
    Bytes bm(size_t(nbytes), 0);
    for (uint64_t p : new_ptrs) {
        const uint64_t w = p / 8;
        bm.at(size_t(w / 8)) |= uint8_t(1 << (w % 8));
    }
    for (int k = 0; k < 4; k++) res.push_back(uint8_t(nbytes >> (8 * k)));
    res.insert(res.end(), bm.begin(), bm.end());
    B.res = std::move(res);
    return B;
}

// the old entries unchanged, the new ones in place; the line names in order. An old entry whose first field is a
// pointer must hold the moved pointer (apply_vo.py compared the raw bytes, which fails on such an entry, e.g.
// 87b03989 of the retail vox-mp.bin, although its build equals the reference file)
std::vector<std::string> verify(const Bytes &res, const Bytes &retail, const std::vector<Entry> &entries, uint64_t shift) {
    const DcInfo R = read_dc(res, false), O = read_dc(retail, false);
    std::map<uint32_t, uint64_t> by;
    for (const auto &s : R.syms) by[s.s] = s.o;
    for (size_t i = 0; i + 1 < R.syms.size(); i++)
        if (R.syms[i].s > R.syms[i + 1].s) throw Stop("AssertionError: symbols not sorted");
    int bad = 0;
    for (const auto &s : O.syms) {
        const uint64_t n = by.at(s.s);
        const bool same = O.ptrs.count(s.o) ? R.ptrs.count(n) && rd64(res, n) == rd64(retail, s.o) + shift : sub(retail, s.o, s.o + 8) == sub(res, n, n + 8);
        if (!same) bad++;
    }
    if (bad) throw Stop(u3::fmt("AssertionError: old entries changed: %d", bad));
    std::vector<std::string> lines;
    for (const Entry &e : entries) {
        const uint64_t o = by.at(e.h);
        if (rd32(res, o - 4, false) != VOX) throw Stop("AssertionError: entry type");
        const uint64_t blk = rd64(res, o + 0x28);
        for (size_t i = 0; i < e.blocks.size(); i++) {
            const uint64_t q = blk + i * 0x18;
            if (rd32(res, q, false) != e.blocks[i].sid) throw Stop("AssertionError: block id");
            const uint64_t la = rd64(res, q + 0x10);
            for (size_t k = 0; k < e.blocks[i].lines.size(); k++) {
                const std::string nm = cstr(res, rd64(res, la + k * 0x20));
                if (nm != e.blocks[i].lines[k].name) throw Stop("AssertionError: line name");
                lines.push_back(nm);
            }
        }
    }
    return lines;
}

std::string base_noext(const std::string &n) {   // os.path.basename(n)[:-5]
    const size_t k = n.find_last_of("/\\");
    const std::string b = k == std::string::npos ? n : n.substr(k + 1);
    return b.size() >= 5 ? b.substr(0, b.size() - 5) : "";
}

}  // namespace

int coop_vo(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project", "--job", "--ps4", "--ps3-disc", "--ps3-update"});
    const bool apply = a.has("--apply");
    const std::string ret = a.need("--ps4") + "/u3data/build/main/dc1/ps4/";
    const std::string ovl = a.need("--project") + "/work/overlay-r3/u3data/build/main/";
    const Bytes c = read_file(upath(ret + "vox-mp.bin"));
    Psarc p(upath(a.get("--ps3-update", "") + "/dlc-patch18.psarc"));
    std::optional<Bytes> ps3;
    for (size_t i = 0; i < p.names.size(); i++) {
        const std::string &n = p.names[i];
        if (n.size() >= 14 && n.compare(n.size() - 14, 14, "dc1/vox-mp.bin") == 0) ps3 = p.read(i + 1);
    }
    if (!ps3) {
        std::fprintf(stderr, "PS3 vox-mp.bin not found\n");
        return 1;
    }
    Built b;
    std::vector<std::string> lines;
    try {
        b = build(c, *ps3);
        lines = verify(b.res, c, b.entries, b.add.size() * 16);
    } catch (const Stop &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    const std::string ov = ovl + "sound1/streams/speech/english/";
    std::vector<std::string> missing;
    for (const std::string &l : lines)
        if (!fs::exists(upath(ov + l + ".xvag"))) missing.push_back(l);
    std::set<std::string> in_ps4;
    {
        Psarc e4(upath(a.need("--ps4") + "/u3data/build/main/sound1/streams/speech/english_ps4.psarc"));
        for (const std::string &n : e4.names) in_ps4.insert(base_noext(n));
    }
    Psarc goty(upath(a.get("--ps3-disc", "") + "/goty-speech-english.psarc"));
    std::map<std::string, size_t> gidx;
    for (size_t i = 0; i < goty.names.size(); i++) gidx[base_noext(goty.names[i])] = i;
    std::vector<std::string> copy, unavailable;
    for (const std::string &l : missing)
        if (!in_ps4.count(l)) copy.push_back(l);
    for (const std::string &l : copy)
        if (!gidx.count(l)) unavailable.push_back(l);
    std::printf("added vox entries: %zu lines: %zu size %zu -> %zu\n", b.add.size(), lines.size(), c.size(), b.res.size());
    std::string un;
    for (const std::string &l : unavailable) un += (un.empty() ? "'" : ", '") + l + "'";
    std::printf("xvags to copy from goty-speech-english: %zu not obtainable: [%s]\n", copy.size(), un.c_str());
    if (!unavailable.empty()) {
        std::fprintf(stderr, "stopping: a line has no source file\n");
        return 1;
    }
    for (const std::string &l : copy) {
        const Bytes x = goty.read(gidx[l] + 1);
        if (x.size() < 4 || !std::equal(x.begin(), x.begin() + 4, "XVAG")) {
            std::fprintf(stderr, "not XVAG: %s\n", l.c_str());
            return 1;
        }
        if (apply) write_file_replace(upath(ov + l + ".xvag"), x);
    }
    const fs::path out = upath(a.need("--job")) / "build";
    fs::create_directories(out);
    const fs::path dst = out / "vox-mp.bin";
    write_file_replace(dst, b.res);
    std::printf("built %s\n", win_path(utf8(dst)).c_str());
    const fs::path target = upath(ovl + "dc1/ps4/vox-mp.bin");
    if (!apply) {
        std::printf("DRY RUN: would install to %s\n", utf8(target).c_str());
        return 0;
    }
    if (fs::exists(target)) copy2(target, upath(utf8(target) + ".bak-coopvo"));
    copy2(dst, target);
    std::printf("installed %s\n", utf8(target).c_str());
    return 0;
}

}  // namespace u3
