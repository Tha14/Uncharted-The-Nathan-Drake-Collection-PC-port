// Recipe step coop-voice-all: the co-op adventure voice fix. In a co-op adventure level the vox controller only finds
// entries of the vox, vox-igc and vox-mp modules, and vox-mp is not resolvable there, so every scripted hero line
// failed ("no vox entry"); every co-op line lives in vox-mp.bin. The entries the co-op adventure scripts request
// (say / wait-say / say-pre-roll) that vox.bin lacks are copied from vox-mp.bin into the overlay's vox.bin, which the
// lookup searches first, and the 'vox' size word of modules.bin follows. Replaces apply_coop_vox.py.
//   u3fix coop-voice2 --project P --job J --ps4 D [--apply | --restore]
// Builds <job>/build/vox.bin and build/modules.bin and checks them (old entries and pointer slots moved, every copied
// entry parses back); --apply backs vox.bin and modules.bin up once to work/backup-coop-voice2 and installs them;
// --restore puts the backups back.
#include "proc.hpp"
#include "u3fix.hpp"

#include <regex>
#include <set>

namespace u3 {
namespace {

const uint32_t VOX = 0xe3825e8d, NETGAME = 0x3b9a067d;
const std::map<uint32_t, std::string> SAY = {{0x235cfa87, "say"}, {0x95f6b8dd, "wait-say"}, {0x159dcdd2, "say-pre-roll"}};

uint32_t u32(const Bytes &d, uint64_t o) {
    if (o + 4 > d.size()) throw Stop("struct.error: unpack_from requires a buffer of at least 4 bytes");
    return rd32le(d, size_t(o));
}
uint64_t u64(const Bytes &d, uint64_t o) {
    if (o + 8 > d.size()) throw Stop("struct.error: unpack_from requires a buffer of at least 8 bytes");
    return rd64le(d, size_t(o));
}
Bytes sub(const Bytes &d, uint64_t a, uint64_t b) {
    a = std::min<uint64_t>(a, d.size());
    b = std::min<uint64_t>(std::max(a, b), d.size());
    return Bytes(d.begin() + std::ptrdiff_t(a), d.begin() + std::ptrdiff_t(b));
}
std::string cstr(const Bytes &d, uint64_t o) {   // d[o:d.index(0, o)]
    uint64_t e = o;
    while (e < d.size() && d[size_t(e)]) e++;
    if (e >= d.size()) throw Stop("ValueError: subsection not found");
    return std::string(d.begin() + std::ptrdiff_t(o), d.begin() + std::ptrdiff_t(e));
}

struct VoxDc {
    uint64_t reloc;
    struct Sym {
        uint32_t s, t;
        uint64_t o;
    };
    std::vector<Sym> syms;
    std::set<uint64_t> ptrs;
};
VoxDc read_dc(const Bytes &d) {
    VoxDc r;
    r.reloc = u64(d, 8);
    const uint32_t n = u32(d, 0x14);
    for (uint32_t i = 0; i < n; i++) r.syms.push_back({u32(d, 0x18 + 16ull * i), u32(d, 0x1c + 16ull * i), u64(d, 0x20 + 16ull * i)});
    const uint32_t cnt = u32(d, r.reloc);
    for (uint64_t bi = 0; bi < cnt && r.reloc + 4 + bi < d.size(); bi++) {
        const uint8_t b = d[size_t(r.reloc + 4 + bi)];
        for (int k = 0; k < 8; k++)
            if (b >> k & 1) r.ptrs.insert((bi * 8 + uint64_t(k)) * 8);
    }
    return r;
}
// paktools dc.DC on a PS4 module: u32 / i32 / ptr / cstr, symbols by id
struct DcView {
    Bytes d;
    std::map<uint32_t, std::pair<uint32_t, uint64_t>> bysid;
    std::vector<VoxDc::Sym> syms;
    explicit DcView(const fs::path &p) : d(read_file(p)) {
        if (d.size() < 4 || !std::equal(d.begin(), d.begin() + 4, "00CD")) throw Stop("ValueError: not DC");
        const uint32_t n = u32(d, 0x14);
        for (uint32_t i = 0; i < n; i++) syms.push_back({u32(d, 0x18 + 16ull * i), u32(d, 0x1c + 16ull * i), u64(d, 0x20 + 16ull * i)});
        for (const auto &s : syms) bysid[s.s] = {s.t, s.o};
    }
    std::optional<std::string> str(uint64_t o) const {
        if (!o || o >= d.size()) return std::nullopt;
        uint64_t z = o;
        while (z < d.size() && d[size_t(z)]) z++;
        if (z >= d.size()) z = d.size() - 1;   // find() = -1: d[o:-1]
        return std::string(d.begin() + std::ptrdiff_t(o), d.begin() + std::ptrdiff_t(std::max(o, z)));
    }
};

struct LineRaw {
    std::string name;
    Bytes raw;   // 0x20
};
struct BlockRaw {
    Bytes raw;   // 0x18
    std::vector<LineRaw> lines;
};
struct EntryRaw {
    Bytes raw;   // 0x28
    std::vector<BlockRaw> blocks;
};
// a PS4 vox entry: the only pointers are entry +0x28 (blocks), block +0x10 (lines) and line +0 (name); any other
// pointer slot inside these objects stops the step, so every other word is carried over verbatim
EntryRaw parse_entry(const Bytes &a, const std::set<uint64_t> &ap, uint64_t o) {
    const uint32_t h = u32(a, o);
    bool bad = !ap.count(o + 0x28);
    for (uint64_t k = 0; k < 0x28; k += 8) bad = bad || ap.count(o + k);
    if (bad) throw Stop(u3::fmt("ValueError: entry %08x: pointer layout", h));
    if (o + 0x27 >= a.size()) throw Stop("IndexError: index out of range");
    const int nblocks = a[size_t(o + 0x27)];
    if (!nblocks) throw Stop(u3::fmt("ValueError: entry %08x: no blocks", h));
    EntryRaw e{sub(a, o, o + 0x28), {}};
    const uint64_t bp = u64(a, o + 0x28);
    for (int i = 0; i < nblocks; i++) {
        const uint64_t q = bp + uint64_t(i) * 0x18;
        if (!ap.count(q + 0x10) || ap.count(q) || ap.count(q + 8)) throw Stop(u3::fmt("ValueError: entry %08x: block %d pointer layout", h, i));
        const uint32_t count = u32(a, q + 0xc);
        const uint64_t lp = u64(a, q + 0x10);
        BlockRaw b{sub(a, q, q + 0x18), {}};
        for (uint32_t k = 0; k < count; k++) {
            const uint64_t l = lp + uint64_t(k) * 0x20;
            if (!ap.count(l) || ap.count(l + 8) || ap.count(l + 16) || ap.count(l + 24)) throw Stop(u3::fmt("ValueError: entry %08x: line %u pointer layout", h, k));
            b.lines.push_back({cstr(a, u64(a, l)), sub(a, l, l + 0x20)});
        }
        e.blocks.push_back(std::move(b));
    }
    return e;
}

// append the entries to the DC module c (apply_vo's writer): the table grows by 16 bytes per symbol, the data moves
Bytes build(const Bytes &c, const std::vector<EntryRaw> &entries, const std::vector<VoxDc::Sym> &add) {
    const VoxDc C = read_dc(c);
    const uint64_t shift = add.size() * 16, tbl_old_end = 0x18 + C.syms.size() * 16;
    Bytes body = sub(c, tbl_old_end, C.reloc);
    std::set<uint64_t> new_ptrs;
    auto put = [&](uint64_t at, uint64_t v, int n) {
        if (at + uint64_t(n) > body.size()) throw Stop("struct.error: pack_into out of range");
        for (int k = 0; k < n; k++) body[size_t(at) + size_t(k)] = uint8_t(v >> (8 * k));
    };
    for (uint64_t p : C.ptrs)
        if (p >= tbl_old_end) {
            put(p - tbl_old_end, u64(c, p) + shift, 8);
            new_ptrs.insert(p + shift);
        }
    const uint64_t base = tbl_old_end + shift;
    std::vector<VoxDc::Sym> out_syms;
    for (const auto &s : C.syms) out_syms.push_back({s.s, s.t, s.o + shift});
    auto here = [&] { return base + body.size(); };
    std::vector<std::string> order;
    std::map<std::string, uint64_t> strs;
    for (const EntryRaw &e : entries)
        for (const BlockRaw &b : e.blocks)
            for (const LineRaw &l : b.lines)
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
    for (size_t ei = 0; ei < entries.size(); ei++) {
        const EntryRaw &e = entries[ei];
        while (here() % 16) body.push_back(0);
        body.resize(body.size() + 12, 0);   // a 16-byte aligned object with its type word just before it
        for (int k = 0; k < 4; k++) body.push_back(uint8_t(VOX >> (8 * k)));
        const uint64_t off = here();
        body.insert(body.end(), e.raw.begin(), e.raw.end());
        const uint64_t blk_at = here() + 8;
        body.resize(body.size() + 8, 0);
        putq(off + 0x28, blk_at);
        for (const BlockRaw &b : e.blocks) body.insert(body.end(), b.raw.begin(), b.raw.end());
        for (size_t i = 0; i < e.blocks.size(); i++) {
            const uint64_t q = blk_at + i * 0x18, la = here();
            for (const LineRaw &l : e.blocks[i].lines) body.insert(body.end(), l.raw.begin(), l.raw.end());
            putq(q + 0x10, la);
            for (size_t k = 0; k < e.blocks[i].lines.size(); k++) putq(la + k * 0x20, strs.at(e.blocks[i].lines[k].name));
        }
        out_syms.push_back({add[ei].s, add[ei].t, off});
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
    if (C.ptrs.count(0x20))
        for (size_t i = 0; i < out_syms.size(); i++) new_ptrs.insert(0x18 + i * 16 + 8);
    res.insert(res.end(), body.begin(), body.end());
    const uint64_t nbytes = (reloc + 63) / 64;
    Bytes bm(size_t(nbytes), 0);
    for (uint64_t p : new_ptrs) bm.at(size_t(p / 8 / 8)) |= uint8_t(1 << (p / 8 % 8));
    for (int k = 0; k < 4; k++) res.push_back(uint8_t(nbytes >> (8 * k)));
    res.insert(res.end(), bm.begin(), bm.end());
    return res;
}

std::map<uint32_t, uint64_t> verify(const Bytes &res, const Bytes &old, const std::vector<EntryRaw> &entries, const std::vector<VoxDc::Sym> &add) {
    const VoxDc R = read_dc(res), O = read_dc(old);
    std::map<uint32_t, uint64_t> by;
    for (const auto &s : R.syms) by[s.s] = s.o;
    for (size_t i = 0; i + 1 < R.syms.size(); i++)
        if (R.syms[i].s > R.syms[i + 1].s) throw Stop("AssertionError: symbols not sorted");
    const uint64_t shift = add.size() * 16;
    for (const auto &s : O.syms) {
        if (O.ptrs.count(s.o)) {   // a symbol that starts with a pointer: it moved with the table growth
            if (u64(res, by.at(s.s)) != u64(old, s.o) + shift) throw Stop(u3::fmt("AssertionError: old pointer %08x", s.s));
        } else if (sub(old, s.o, s.o + 8) != sub(res, by.at(s.s), by.at(s.s) + 8))
            throw Stop(u3::fmt("AssertionError: old entry %08x changed", s.s));
    }
    for (uint64_t p : O.ptrs)   // every old pointer slot is still a pointer slot, moved, with its value moved
        if (p >= 0x18 + O.syms.size() * 16 && !(R.ptrs.count(p + shift) && u64(res, p + shift) == u64(old, p) + shift))
            throw Stop(u3::fmt("AssertionError: reloc %llx", (unsigned long long)p));
    if (R.syms.size() != O.syms.size() + add.size()) throw Stop("AssertionError: symbol count");
    auto mask = [](EntryRaw e) {   // the pointer slots inside the raw bytes masked: they moved
        for (BlockRaw &b : e.blocks) {
            std::fill(b.raw.begin() + 0x10, b.raw.end(), 0);
            for (LineRaw &l : b.lines) std::fill(l.raw.begin(), l.raw.begin() + 8, 0);
        }
        return e;
    };
    auto same = [](const EntryRaw &x, const EntryRaw &y) {
        if (x.raw != y.raw || x.blocks.size() != y.blocks.size()) return false;
        for (size_t i = 0; i < x.blocks.size(); i++) {
            if (x.blocks[i].raw != y.blocks[i].raw || x.blocks[i].lines.size() != y.blocks[i].lines.size()) return false;
            for (size_t k = 0; k < x.blocks[i].lines.size(); k++)
                if (x.blocks[i].lines[k].name != y.blocks[i].lines[k].name || x.blocks[i].lines[k].raw != y.blocks[i].lines[k].raw) return false;
        }
        return true;
    };
    for (size_t i = 0; i < entries.size(); i++) {
        const uint64_t o = by.at(add[i].s);
        if (!same(mask(parse_entry(res, R.ptrs, o)), mask(entries[i]))) throw Stop(u3::fmt("AssertionError: round trip %08x", add[i].s));
        if (u32(res, o - 4) != VOX) throw Stop("AssertionError: entry type");
    }
    return by;
}

}  // namespace

int coop_voice2(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--restore"}, {"--project", "--job", "--ps4"});
    const std::string P = a.need("--project") + "/";
    const std::string RET = a.need("--ps4") + "/u3data/build/main/dc1/ps4/", OVL = P + "work/overlay-r3/u3data/build/main/dc1/ps4/", BAK = P + "work/backup-coop-voice2/";
    auto served = [&](const std::string &n) { return fs::exists(upath(OVL + n)) ? OVL + n : RET + n; };
    const std::string dst = OVL + "vox.bin";
    if (a.has("--restore")) {
        for (const char *name : {"vox.bin", "modules.bin"}) {
            const std::string b = BAK + name;
            if (!fs::exists(upath(b))) {
                std::fprintf(stderr, "no backup at %s\n", b.c_str());
                return 1;
            }
            copy2(upath(b), upath(OVL + name));
            std::printf("restored %s from %s\n", (OVL + name).c_str(), b.c_str());
        }
        return 0;
    }
    const Bytes c = read_file(upath(dst));
    const Bytes vm = read_file(upath(served("vox-mp.bin")));
    const VoxDc A = read_dc(vm), C = read_dc(c);
    std::set<uint32_t> have;
    for (const auto &s : C.syms) have.insert(s.s);
    std::map<uint32_t, std::pair<uint32_t, uint64_t>> srcsyms;
    for (const auto &s : A.syms)
        if (s.t == VOX) srcsyms[s.s] = {s.t, s.o};
    std::set<uint32_t> other;
    for (const auto &s : read_dc(read_file(upath(served("vox-igc.bin")))).syms)
        if (s.t == VOX) other.insert(s.s);
    // the co-op adventure games of net.bin (type 11) with their hero rows, and the co-op adventure scripts
    {
        const DcView nb(upath(served("net.bin")));
        const auto [t, o] = nb.bysid.at(NETGAME);
        const uint32_t n = u32(nb.d, o);
        const uint64_t e = u64(nb.d, o + 8);
        const auto [tr, orow] = nb.bysid.at(0xbd3bb3b9);
        const uint64_t er = u64(nb.d, orow + 8);
        std::string games;
        for (uint32_t i = 0; i < n; i++) {
            const uint64_t g = e + uint64_t(i) * 0xd0;
            if (int32_t(u32(nb.d, g + 0xc)) != 11) continue;
            std::string rows;
            for (int k = 0; k < 3; k++) {
                const uint32_t d = u32(nb.d, u64(nb.d, u64(nb.d, g + 0x70) + 8ull * uint64_t(k)));
                const auto nm = nb.str(u64(nb.d, er + uint64_t(d) * 0x70));
                rows += (k ? ", " : "") + (nm ? "'" + *nm + "'" : std::string("None"));
            }
            games += (games.empty() ? "" : ", ") + u3::fmt("(%u, [", i) + rows + "])";
        }
        std::printf("co-op adventure games in net.bin: [%s]\n", games.c_str());
    }
    std::set<std::string> scripts;
    for (const std::string &d : {RET, OVL})
        if (fs::is_directory(upath(d)))
            for (const auto &e : fs::directory_iterator(upath(d))) {
                const std::string x = utf8(e.path().filename());
                if (x.rfind("ss-coop-adv", 0) == 0 && x.size() >= 4 && x.compare(x.size() - 4, 4, ".bin") == 0) scripts.insert(x);
            }
    std::set<uint32_t> allvox = have;
    for (const auto &[s, v] : srcsyms) allvox.insert(s);
    allvox.insert(other.begin(), other.end());
    auto script_lines = [&](const std::string &name) {
        const Bytes d = read_file(upath(served(name)));
        std::set<uint32_t> lines;
        for (int64_t i = 0; i < int64_t(d.size()) - 40; i += 4)
            if (SAY.count(u32(d, uint64_t(i))))
                for (int k : {8, 16, 24, 32, 40}) {
                    const uint32_t w = u32(d, uint64_t(i + k));
                    if (allvox.count(w)) lines.insert(w);
                }
        return lines;
    };
    std::set<uint32_t> need;
    for (const std::string &n : scripts)
        for (uint32_t s : script_lines(n)) need.insert(s);
    std::vector<VoxDc::Sym> add;
    for (uint32_t s : need)
        if (srcsyms.count(s) && !have.count(s)) add.push_back({s, srcsyms[s].first, srcsyms[s].second});
    std::vector<EntryRaw> entries;
    Bytes res;
    try {
        for (const auto &s : add) entries.push_back(parse_entry(vm, A.ptrs, s.o));
        res = build(c, entries, add);
        verify(res, c, entries, add);
    } catch (const Stop &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    const fs::path out = upath(a.need("--job")) / "build";
    fs::create_directories(out);
    write_file_replace(out / "vox.bin", res);
    std::printf("vox.bin: %zu entries -> %zu (+%zu from vox-mp.bin), %zu -> %zu bytes, written build/vox.bin\n", C.syms.size(), C.syms.size() + add.size(), add.size(),
                c.size(), res.size());
    // the per-map coverage of the result, with the speaker prefixes of the line names
    std::set<uint32_t> rsyms;
    for (const auto &s : read_dc(res).syms)
        if (s.t == VOX) rsyms.insert(s.s);
    std::map<uint32_t, std::string> names;
    for (size_t i = 0; i < entries.size(); i++)
        names[add[i].s] = !entries[i].blocks.empty() && !entries[i].blocks[0].lines.empty() ? entries[i].blocks[0].lines[0].name : "";
    static const std::regex SPK("-(drk|sul|chl|ctr|eln|laz|edy|fln|drz|prtb|prt)-");
    for (const std::string &n : scripts) {
        const std::set<uint32_t> ls = script_lines(n);
        std::string miss;
        int resolved = 0;
        std::vector<std::pair<std::string, int>> pre;
        for (uint32_t s : ls) {
            if (rsyms.count(s)) resolved++;
            else miss += (miss.empty() ? "'" : ", '") + u3::fmt("%#x", s) + "'";
            std::smatch m;
            const std::string nm = names.count(s) ? names[s] : "";
            if (std::regex_search(nm, m, SPK)) {
                auto it = std::find_if(pre.begin(), pre.end(), [&](const auto &x) { return x.first == m[1].str(); });
                if (it == pre.end()) pre.push_back({m[1].str(), 1});
                else it->second++;
            }
        }
        std::string ps;
        for (const auto &[k, v] : pre) ps += (ps.empty() ? "'" : ", '") + k + "': " + std::to_string(v);
        std::printf("  %-26s lines %3zu  resolve in vox.bin after fix: %3d  missing: %s  speakers: {%s}\n", n.c_str(), ls.size(), resolved, miss.empty() ? "none" : ("[" + miss + "]").c_str(), ps.c_str());
    }
    // modules.bin: the 'vox' entry's size word (+0x10) = the new vox.bin size
    Bytes mod;
    uint32_t oldsize = 0;
    {
        const DcView m(upath(OVL + "modules.bin"));
        mod = m.d;
        if (m.syms.empty()) throw Stop("IndexError: list index out of range");
        const uint64_t o = m.syms[0].o, arr = u64(m.d, o + 8);
        const uint32_t n = u32(m.d, o);
        bool found = false;
        for (uint32_t i = 0; i < n && !found; i++) {
            const uint64_t e = arr + uint64_t(i) * 0x28;
            if (m.str(u64(m.d, e)) == std::optional<std::string>("vox")) {
                oldsize = u32(m.d, e + 0x10);
                for (int k = 0; k < 4; k++) mod[size_t(e + 0x10) + size_t(k)] = uint8_t(res.size() >> (8 * k));
                found = true;
            }
        }
        if (!found) {
            std::fprintf(stderr, "modules.bin: no vox entry\n");
            return 1;
        }
    }
    write_file_replace(out / "modules.bin", mod);
    std::printf("modules.bin: vox size word %u -> %zu, written build/modules.bin\n", oldsize, res.size());
    if (!a.has("--apply")) {
        std::printf("dry run: nothing installed\n");
        return 0;
    }
    fs::create_directories(upath(BAK));
    for (const auto &[name, data] : std::vector<std::pair<std::string, const Bytes *>>{{"vox.bin", &res}, {"modules.bin", &mod}}) {
        if (!fs::exists(upath(BAK + name))) {
            copy2(upath(OVL + name), upath(BAK + name));
            std::printf("backup %s\n", (BAK + name).c_str());
        }
        write_file_replace(upath(OVL + name), *data);
        std::printf("installed %s\n", (OVL + name).c_str());
    }
    return 0;
}

}  // namespace u3
