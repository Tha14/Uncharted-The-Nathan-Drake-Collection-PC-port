// Recipe step actors-hk-tables: the Havok reflection tables (hk2010_ps3.json, hk2011_ps4.json) that the actor
// converter's Havok collision conversion (dctools/hkconvert.cpp) needs for props with their own skeleton, rebuilt from
// the player's own executables into TMP/hk (never shipped: they are derived from the executables). Replaces
// actors_hk.py, mkhk.py and mkhk_write.py.
//   u3fix actors-hk --project P --job J --ps3-update D --ps4 D [--ps3-elf F] [--ps4-elf F] [--objdump EXE]
// (the executables default to $RECIPE_PS3_ELF / $RECIPE_PS4_ELF, then <update>/EBOOT.elf and <ps4>/eboot.elf)
// PS3: the DECRYPTED Uncharted 3 1.19 EBOOT (the 1.10 disc one gives other tables). hkClass objects are made by static
// constructors: the class name / parent / size and the member table come from the constructor call sites, found by
// following the register and stack values through the text segment (addi / addis / ori / lwz / mr / stw / std / ...
// up to each bl); the hkClassMember arrays are static data.
// PS4: the decrypted Collection eboot: the same from an Intel-syntax GNU objdump disassembly of the first PT_LOAD
// segment (the lea / mov / xor values before each call of the constructor at 0x122c40).
// Exit 3 when an executable is missing, 4 when the tables differ from the reference ones (other executables).
// The tables also go into the job's toolsets that still carry hkconvert.py. Idempotent.
#include "json.h"
#include "proc.hpp"
#include "u3fix.hpp"

#include <deque>
#include <exception>
#include <fstream>
#include <regex>
#include <thread>

namespace u3 {
namespace {

const char *const TY[] = {"VOID",  "BOOL",         "CHAR",  "INT8",        "UINT8",   "INT16",  "UINT16",          "INT32",           "UINT32",
                          "INT64", "UINT64",       "REAL",  "VECTOR4",     "QUATERNION", "MATRIX3", "ROTATION",     "QSTRANSFORM",     "MATRIX4",
                          "TRANSFORM", "ZERO",     "POINTER", "FUNCTIONPOINTER", "ARRAY", "INPLACEARRAY", "ENUM", "STRUCT",        "SIMPLEARRAY",
                          "HOMOGENEOUSARRAY", "VARIANT", "CSTRING", "ULONG", "FLAGS", "HALF", "STRINGPTR", "RELARRAY"};
const size_t NTY = sizeof TY / sizeof *TY;

struct Elf {
    Bytes d;
    bool be;
    int ps;
    struct Seg {
        uint64_t va, of, fs, ms;
    };
    std::vector<Seg> seg;
    uint64_t entry;
    uint64_t rd(size_t o, int n) const {
        if (o + size_t(n) > d.size()) throw Stop("struct.error: unpack_from out of range");
        uint64_t v = 0;
        for (int k = 0; k < n; k++) v = be ? (v << 8) | d[o + size_t(k)] : v | uint64_t(d[o + size_t(k)]) << (8 * k);
        return v;
    }
    explicit Elf(const fs::path &p) : d(read_file(p)) {
        if (d.size() < 0x40 || d[0] != 0x7f || d[1] != 'E' || d[2] != 'L' || d[3] != 'F') throw Stop("AssertionError: not an ELF");
        be = d[5] == 2;
        ps = be ? 4 : 8;
        const uint64_t phoff = rd(0x20, 8);
        const uint64_t es = rd(0x36, 2), n = rd(0x38, 2);
        for (uint64_t i = 0; i < n; i++) {
            const size_t o = size_t(phoff + i * es);
            const uint64_t t = rd(o, 4), of = rd(o + 8, 8), va = rd(o + 16, 8), fs = rd(o + 32, 8), ms = rd(o + 40, 8);
            if ((t == 1 || t == 0x61000010 || t == 0x61000000) && fs) seg.push_back({va, of, fs, ms});
        }
        entry = rd(0x18, 8);
    }
    std::optional<uint64_t> off(uint64_t va) const {
        for (const Seg &s : seg)
            if (s.va <= va && va < s.va + s.fs) return s.of + va - s.va;
        return std::nullopt;
    }
    std::optional<uint64_t> u32(uint64_t va) const {
        const auto o = off(va);
        if (!o) return std::nullopt;
        return rd(size_t(*o), 4);
    }
    std::optional<std::string> cstr(uint64_t va) const {
        if (!va) return std::nullopt;
        const auto o = off(va);
        if (!o) return std::nullopt;
        size_t e = size_t(*o);
        const size_t lim = std::min<size_t>(size_t(*o) + 200, d.size());
        while (e < lim && d[e]) e++;
        if (e >= lim || e <= *o) return std::nullopt;   // no NUL within 200 bytes, or empty
        std::string s(d.begin() + std::ptrdiff_t(*o), d.begin() + std::ptrdiff_t(e));
        for (unsigned char c : s)
            if (c < 0x20 || c > 0x7e) return std::nullopt;   // ascii, isprintable
        return s;
    }
};

using OInt = std::optional<uint64_t>;
struct Rec {
    OInt self, parent, size;
    std::string name;
    uint64_t mem = 0, nmem = 0;
    std::map<int64_t, OInt> slots;
};

json::Value members(const Elf &x, uint64_t mem, uint64_t n, const std::map<OInt, std::string> &names, bool &bad) {
    json::Value ms;
    ms.kind = json::Value::Array;
    const size_t sz = x.ps == 4 ? 0x18 : 0x28;
    const auto mo = mem ? x.off(mem) : std::nullopt;
    if (!mo || !n) return ms;
    for (uint64_t k = 0; k < n; k++) {
        const size_t r = size_t(*mo + k * sz);
        uint64_t mn, c;
        size_t q;
        if (x.ps == 4) mn = x.rd(r, 4), c = x.rd(r + 4, 4), q = r + 0xc;
        else mn = x.rd(r, 8), c = x.rd(r + 8, 8), q = r + 0x18;
        const uint64_t t = x.rd(q, 1), st = x.rd(q + 1, 1), fl = x.rd(q + 4, 2), of = x.rd(q + 6, 2);
        const int16_t ca = int16_t(uint16_t(x.rd(q + 2, 2)));
        const auto nm = x.cstr(mn);
        if (!nm || t >= NTY || st >= NTY) {
            bad = true;
            ms.items.clear();
            return ms;
        }
        json::Value m = json::Value::object();
        m.set("name", json::Value::text(*nm));
        m.set("type", json::Value::text(TY[t]));
        m.set("sub", json::Value::text(TY[st]));
        m.set("carr", json::Value::integer(ca));
        m.set("flags", json::Value::integer(int64_t(fl)));
        m.set("off", json::Value::integer(int64_t(of)));
        auto cn = names.find(OInt(c));
        std::optional<std::string> cls = cn != names.end() && !cn->second.empty() ? std::optional<std::string>(cn->second) : x.cstr(c);
        m.set("cls", cls ? json::Value::text(*cls) : json::Value());
        ms.items.push_back(m);
    }
    return ms;
}

json::Value finish(const Elf &x, const std::vector<Rec> &recs) {
    std::map<OInt, std::string> names;   // 'this' -> name (a later one wins)
    for (const Rec &r : recs) names[r.self] = r.name;
    json::Value out = json::Value::object();
    for (const Rec &r : recs) {
        bool bad = false;
        json::Value m = members(x, r.mem, r.nmem, names, bad);
        if (bad) std::printf("bad %s member\n", r.name.c_str());
        json::Value e = json::Value::object();
        auto pn = names.find(r.parent);
        e.set("parent", pn != names.end() ? json::Value::text(pn->second) : json::Value());
        e.set("size", r.size ? json::Value::integer(int64_t(*r.size)) : json::Value());
        const bool has_m = !m.items.empty();
        e.set("members", m);
        json::Value *cur = out.get(r.name);
        if (!cur) out.set(r.name, e);
        else if (has_m && cur->get("members")->items.empty()) *cur = e;
    }
    return out;
}

std::vector<Rec> scan_ps3(const Elf &x) {
    const auto opd = x.off(x.entry);
    if (!opd) throw Stop("TypeError: the entry point is not mapped");
    const uint64_t toc = x.rd(size_t(*opd + 4), 4);
    const Elf::Seg &s0 = x.seg.at(0);
    const uint64_t end = s0.of + s0.fs;
    const uint64_t M = 0xffffffffu;
    std::map<int, uint64_t> r{{2, toc}};
    std::map<int64_t, OInt> st;
    std::vector<std::pair<uint64_t, std::vector<Rec>>> sites;
    auto get = [&](int k) -> OInt { auto it = r.find(k); return it == r.end() ? OInt() : OInt(it->second); };
    for (uint64_t j = s0.of; j < end; j += 4) {
        r[2] = toc;
        const uint32_t w = uint32_t(x.rd(size_t(j), 4));
        const int op = int(w >> 26), rt = int((w >> 21) & 31), ra = int((w >> 16) & 31);
        const int64_t si = w & 0xffff, s = (si & 0x8000) ? si - 0x10000 : si;
        const int xo = int((w >> 1) & 1023);
        if (op == 14) {
            if (ra == 0) r[rt] = uint64_t(s) & M;
            else if (r.count(ra)) r[rt] = (r[ra] + uint64_t(s)) & M;
            else r.erase(rt);
        } else if (op == 15) {
            if (ra == 0) r[rt] = uint64_t(s << 16) & M;
            else if (r.count(ra)) r[rt] = (r[ra] + uint64_t(s << 16)) & M;
            else r.erase(rt);
        } else if (op == 24) {
            if (r.count(rt)) r[ra] = r[rt] | uint64_t(si);
        } else if (op == 32) {
            if (ra == 1 && rt >= 13) continue;
            const OInt val = r.count(ra) ? x.u32((r[ra] + uint64_t(s)) & M) : OInt();
            if (val) r[rt] = *val;
            else r.erase(rt);
        } else if (op == 31 && xo == 444 && rt == int((w >> 11) & 31)) {
            if (r.count(rt)) r[ra] = r[rt];
            else r.erase(ra);
        } else if ((op == 36 || op == 62) && ra == 1) {
            st[op == 62 ? (s & ~int64_t(3)) : s] = get(rt);
        } else if (op == 58 || op == 34 || op == 40 || op == 33 || op == 42) {
            if (ra == 1 && rt >= 13) continue;
            r.erase(rt);
        } else if (op == 30 || op == 21) {
            if (r.count(rt)) r[ra] = r[rt] & M;
            else r.erase(ra);
        } else if (op == 31 && xo == 986) {
            if (r.count(rt)) r[ra] = r[rt];
            else r.erase(ra);
        } else if (op == 18 && (w & 3) == 1) {
            int64_t li = w & 0x3fffffc;
            if (li & 0x2000000) li -= 0x4000000;
            const OInt r4 = get(4);
            const auto nm = x.cstr(r4 ? *r4 : 0);
            const uint64_t pc = s0.va + j - s0.of, tgt = uint64_t(int64_t(pc) + li);
            if ((nm && (nm->rfind("hk", 0) == 0 || nm->rfind("Hk", 0) == 0)) || tgt == 0xbf6fe8) {
                Rec rec;
                rec.self = get(3);
                rec.name = nm ? *nm : u3::fmt("?%llx", (unsigned long long)pc);
                rec.parent = get(5);
                rec.size = get(6);
                rec.slots = st;
                auto it = std::find_if(sites.begin(), sites.end(), [&](const auto &v) { return v.first == tgt; });
                if (it == sites.end()) sites.push_back({tgt, {rec}});
                else it->second.push_back(rec);
            }
            for (int q = 3; q < 13; q++) r.erase(q);
            st.clear();
        }
    }
    if (sites.empty()) throw Stop("ValueError: max() arg is an empty sequence");
    const auto *best = &sites[0];
    for (const auto &v : sites)
        if (v.second.size() > best->second.size()) best = &v;
    std::printf("ps3 ctor %#llx %zu toc %#llx\n", (unsigned long long)best->first, best->second.size(), (unsigned long long)toc);
    std::vector<Rec> recs = best->second;
    for (Rec &q : recs) {   // the member table and count from the constructor's stack arguments (or 0)
        auto a = q.slots.find(0x70), b = q.slots.find(0x78);
        q.mem = a != q.slots.end() && a->second ? *a->second : 0;
        q.nmem = b != q.slots.end() && b->second ? *b->second : 0;
    }
    return recs;
}

std::vector<Rec> scan_ps4(const Elf &x, const std::vector<std::string> &cmd) {
    std::vector<Rec> recs;
    std::deque<std::string> lines;
    static const std::regex re_lea(R"(lea\s+(\w+),\[rip[^\]]*\]\s+# (0x[0-9a-f]+))"), re_mi(R"(mov\s+(?:\w+ PTR )?\[rsp(?:\+(0x[0-9a-f]+))?\],(0x[0-9a-f]+)$)"),
        re_ms(R"(mov\s+(?:\w+ PTR )?\[rsp(?:\+(0x[0-9a-f]+))?\],(\w+)$)"), re_mr(R"(mov\s+(\w+),(\w+)$)"), re_mimm(R"(mov\s+(\w+),(0x[0-9a-f]+)$)"),
        re_xor(R"(xor\s+(\w+),(\w+)$)"), re_keep(R"((?:rbx|rbp|r12|r13|r14|r15|\[rsp))");
    static const std::map<std::string, std::string> low = {{"esi", "rsi"}, {"edi", "rdi"}, {"edx", "rdx"}, {"ecx", "rcx"}, {"r8d", "r8"},
                                                           {"r9d", "r9"},  {"eax", "rax"}, {"ebx", "rbx"}, {"ebp", "rbp"}};
    auto nrm = [&](const std::string &a) { auto it = low.find(a); return it == low.end() ? a : it->second; };
    auto hexv = [](const std::string &h) { return uint64_t(std::stoull(h, nullptr, 16)); };
    auto take = [&](const std::string &ln) {
        if (ln.find("call   0x122c40") != std::string::npos) {
            std::map<std::string, uint64_t> r;
            std::map<int64_t, uint64_t> st;
            for (const std::string &l : lines) {
                std::string t = l.substr(l.rfind('\t') == std::string::npos ? 0 : l.rfind('\t') + 1);
                const size_t a = t.find_first_not_of(" \t\r\n\f\v"), b = t.find_last_not_of(" \t\r\n\f\v");
                t = a == std::string::npos ? "" : t.substr(a, b - a + 1);
                std::smatch m;
                if (std::regex_search(t, m, re_lea)) {
                    r[nrm(m[1])] = hexv(m[2]);
                    continue;
                }
                if (std::regex_search(t, m, re_mi)) {
                    st[int64_t(m[1].matched ? hexv(m[1]) : 0)] = hexv(m[2]);
                    continue;
                }
                if (std::regex_search(t, m, re_ms)) {
                    auto it = r.find(nrm(m[2]));
                    if (it != r.end()) st[int64_t(m[1].matched ? hexv(m[1]) : 0)] = it->second;
                    continue;
                }
                if (std::regex_search(t, m, re_mimm)) {
                    r[nrm(m[1])] = hexv(m[2]);
                    continue;
                }
                if (std::regex_search(t, m, re_mr)) {
                    auto it = r.find(nrm(m[2]));
                    if (it != r.end()) r[nrm(m[1])] = it->second;
                    else r.erase(nrm(m[1]));
                    continue;
                }
                if (std::regex_search(t, m, re_xor) && m[1] == m[2]) r[nrm(m[1])] = 0;
            }
            auto g = [&](const char *k) { auto it = r.find(k); return it == r.end() ? OInt() : OInt(it->second); };
            const OInt rsi = g("rsi");
            if (const auto nm = x.cstr(rsi ? *rsi : 0)) {
                Rec rec;
                rec.self = g("rdi");
                rec.name = *nm;
                rec.parent = g("rdx");
                rec.size = g("rcx");
                rec.mem = st.count(0x10) ? st[0x10] : 0;
                rec.nmem = st.count(0x18) ? st[0x18] : 0;
                recs.push_back(rec);
            }
            std::deque<std::string> keep;
            for (const std::string &l : lines)
                if (std::regex_search(l, re_keep)) keep.push_back(l);
            while (keep.size() > 400) keep.pop_front();
            lines.swap(keep);
        } else {
            lines.push_back(ln);
            if (lines.size() > 400) lines.pop_front();
        }
    };
    if (run_to_lines(cmd, take)) throw Stop("objdump failed (GNU binutils objdump is needed for the PS4 tables)");
    return recs;
}

}  // namespace

int actors_hk(const std::vector<std::string> &argv) {
    const Args a(argv, {}, {"--project", "--job", "--ps3-elf", "--ps4-elf", "--ps3-update", "--ps4", "--objdump"});
    static const std::vector<std::pair<std::string, std::string>> REF = {{"hk2010_ps3.json", "de095f792cf29b43b57488a05e984d93766699ad2d30fe19cd3864f184a1c145"},
                                                                         {"hk2011_ps4.json", "73ef4b8c2cb179983dc6eda851f86082e77ca957ed2fa4508252335df9c36d04"}};
    const fs::path here = upath(a.need("--job")), tmp = upath(a.need("--project")) / "scratch" / "actor33-work" / "hk";
    auto ok = [&](const fs::path &d) {
        for (const auto &[k, v] : REF)
            if (!fs::exists(d / k) || sha_file(d / k) != v) return false;
        return true;
    };
    std::vector<fs::path> dirs;   // toolsets that still carry the Python Havok converter
    if (fs::is_directory(here / "toolsets")) {
        std::vector<std::string> ts;
        for (const auto &e : fs::directory_iterator(here / "toolsets")) ts.push_back(utf8(e.path().filename()));
        std::sort(ts.begin(), ts.end());
        for (const std::string &t : ts)
            if (fs::exists(here / "toolsets" / upath(t) / "hkconvert.py")) dirs.push_back(here / "toolsets" / upath(t) / "data");
    }
    bool all = ok(tmp);
    for (const fs::path &d : dirs) all = all && ok(d);
    if (all) {
        std::printf("hk tables already in place (TMP/hk, %zu toolsets)\n", dirs.size());
        return 0;
    }
    if (!ok(tmp)) {
        const fs::path *from = nullptr;
        for (const fs::path &d : dirs)
            if (ok(d)) { from = &d; break; }
        fs::create_directories(tmp);
        if (from)
            for (const auto &[k, v] : REF) fs::copy_file(*from / k, tmp / k, fs::copy_options::overwrite_existing);
        else {
            // defaults: $RECIPE_PS3_ELF / $RECIPE_PS4_ELF, then <PS3 update>/EBOOT.elf and <PS4>/eboot.elf
            auto pick = [&](const char *arg, const char *env, const std::string &def) {
                if (a.val.count(arg)) return a.get(arg, "");
                const char *e = std::getenv(env);
                return std::string(e && *e ? e : def.c_str());
            };
            const std::string ps3e = pick("--ps3-elf", "RECIPE_PS3_ELF", a.get("--ps3-update", "") + "/EBOOT.elf"), ps4e = pick("--ps4-elf", "RECIPE_PS4_ELF", a.get("--ps4", "") + "/eboot.elf");
            for (const auto &[f, what] : std::vector<std::pair<std::string, std::string>>{{ps3e, "decrypted PS3 1.19 EBOOT.elf (--ps3-elf / RECIPE_PS3_ELF)"},
                                                                                         {ps4e, "decrypted PS4 eboot ELF (--ps4-elf / RECIPE_PS4_ELF)"}}) {
                bool elf = false;
                if (!f.empty() && fs::is_regular_file(upath(f))) {
                    std::ifstream in(upath(f), std::ios::binary);
                    char m[4] = {};
                    in.read(m, 4);
                    elf = m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
                }
                if (!elf) {
                    std::printf("missing input: %s (%s); own-skeleton actor paks cannot be converted\n", what.c_str(), f.c_str());
                    return 3;
                }
            }
            const Elf A(upath(ps3e)), Bx(upath(ps4e));
            // the PS4 text segment (the first PT_LOAD, at address 0) disassembled by GNU objdump
            const uint64_t ph = Bx.rd(0x20, 8);
            if (Bx.rd(size_t(ph), 4) != 1 || Bx.rd(size_t(ph) + 16, 8) != 0) throw Stop("AssertionError: first PT_LOAD is not the text segment at 0");
            const uint64_t of = Bx.rd(size_t(ph) + 8, 8), fsz = Bx.rd(size_t(ph) + 32, 8);
            const fs::path text = tmp / "text.bin";
            write_file_replace(text, Bytes(Bx.d.begin() + std::ptrdiff_t(of), Bx.d.begin() + std::ptrdiff_t(std::min<uint64_t>(of + fsz, Bx.d.size()))));
            std::string objdump = a.get("--objdump", "");
            if (objdump.empty()) objdump = "objdump";
            const std::vector<std::string> cmd = {objdump, "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "intel", "--no-show-raw-insn", utf8(text)};
            std::vector<Rec> ra, rb;
            std::exception_ptr pe, oe;
            std::thread ts([&] {
                try {
                    ra = scan_ps3(A);
                } catch (...) {
                    pe = std::current_exception();
                }
            });
            try {
                rb = scan_ps4(Bx, cmd);
            } catch (...) {
                oe = std::current_exception();
            }
            ts.join();
            if (oe) std::rethrow_exception(oe);
            if (pe) std::rethrow_exception(pe);
            std::printf("ps3 recs %zu\n", ra.size());
            std::printf("ps4 recs %zu\n", rb.size());
            for (const auto &[name, val] : std::vector<std::pair<std::string, json::Value>>{{"hk2010_ps3.json", finish(A, ra)}, {"hk2011_ps4.json", finish(Bx, rb)}}) {
                std::string o;
                dump_compact(o, val);
                write_file_replace(tmp / name, Bytes(o.begin(), o.end()));
            }
            std::error_code ec;
            fs::remove(text, ec);
            std::vector<std::string> bad;
            for (const auto &[k, v] : REF)
                if (sha_file(tmp / k) != v) bad.push_back(k);
            if (!bad.empty()) {
                std::printf("the rebuilt tables differ from the 2026-11-01 ones (%s%s): other executables? not installed\n", bad[0].c_str(), bad.size() > 1 ? ", ..." : "");
                return 4;
            }
        }
    }
    for (const fs::path &d : dirs) {
        fs::create_directories(d);
        for (const auto &[k, v] : REF) fs::copy_file(tmp / k, d / k, fs::copy_options::overwrite_existing);
    }
    std::printf("hk tables in place (TMP/hk, %zu toolsets)\n", dirs.size());
    return 0;
}

}  // namespace u3
