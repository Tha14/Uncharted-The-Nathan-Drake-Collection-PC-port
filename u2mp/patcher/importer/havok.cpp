// importer/havok.cpp: Havok packfiles of Uncharted 2 actors, PS3 (Havok 6.1.0-r1, 32-bit BE) -> PS4 (hk_2011.3.0-r1,
// 64-bit LE), on the Uncharted 3 reflection-driven converter (dctools hkconvert with u3::g_u2), and the class tables it
// needs, built on the player's PC from the player's own files (nothing derived from the games ships with the import):
//
//   DB3  Havok 6.1 layouts: the hkClass constructor calls and static hkClassMember arrays of the decrypted PS3 EBOOT.elf.
//        The constructor is the function most often called with a 'hk...' class name in r4; r3 / r5 / r6 and the stack
//        slots 0x70 / 0x78 at the call give the class object, parent, size, member array and member count (the
//        register values are followed through the text segment: addi, addis, ori, lwz, mr, stw, std, ...).
//   DB4  Havok 2011.3 layouts: the same in the PS4 executable (big2-ps4_Shipping.elf, a fake-signed SELF: unwrapped,
//        R_X86_64_RELATIVE relocations applied) from a GNU objdump disassembly of its text segment (rsi = the name; rdi,
//        rdx, rcx and the stack slots 0x10 / 0x18), plus FILL: the 2011 classes whose member tables the executable builds
//        at run time, from their member declarations placed with the C layout rules and then checked on the player's
//        retail PS4 actor packfiles.
//   SIG4 PS4 class signatures, read from the retail PS4 actor packfiles.
// The tables are kept in work/u2hkclasses.json. Without them the actors with Havok collision or ragdolls are left out.
#include "actor.hpp"
#include "hkconvert.hpp"

#include <deque>

namespace u2imp {

namespace {

const char *const TY[] = {"VOID",  "BOOL",         "CHAR",  "INT8",        "UINT8",   "INT16",  "UINT16",          "INT32",           "UINT32",
                          "INT64", "UINT64",       "REAL",  "VECTOR4",     "QUATERNION", "MATRIX3", "ROTATION",     "QSTRANSFORM",     "MATRIX4",
                          "TRANSFORM", "ZERO",     "POINTER", "FUNCTIONPOINTER", "ARRAY", "INPLACEARRAY", "ENUM", "STRUCT",        "SIMPLEARRAY",
                          "HOMOGENEOUSARRAY", "VARIANT", "CSTRING", "ULONG", "FLAGS", "HALF", "STRINGPTR", "RELARRAY"};
const size_t NTY = sizeof TY / sizeof *TY;
const int TABLES_VERSION = 1;

struct TablesMissing : Unsupported {
    using Unsupported::Unsupported;
};

// ------------------------------------------------------------------------------------------------ executables
struct Elf {
    Bytes d;
    bool be = false;
    int ps = 8;
    struct Ph {
        uint32_t type, flags;
        uint64_t off, va, pa, fs, ms, al;
    };
    std::vector<Ph> ph;
    struct Seg {
        uint64_t va, of, fs, ms;
    };
    std::vector<Seg> seg;
    uint64_t entry = 0;
    uint64_t q(uint64_t o) const {
        if (o + 8 > d.size()) throw TablesMissing("ELF header truncated");
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) v |= uint64_t(d[o + uint64_t(be ? 7 - k : k)]) << (8 * k);
        return v;
    }
    uint32_t w(uint64_t o) const {
        if (o + 4 > d.size()) throw TablesMissing("ELF header truncated");
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) v |= uint32_t(d[o + uint64_t(be ? 3 - k : k)]) << (8 * k);
        return v;
    }
    uint16_t h(uint64_t o) const { return be ? uint16_t(d.at(o) << 8 | d.at(o + 1)) : uint16_t(d.at(o) | d.at(o + 1) << 8); }
    explicit Elf(Bytes data) : d(std::move(data)) {
        if (d.size() < 0x40 || std::memcmp(d.data(), "\x7f" "ELF", 4)) throw TablesMissing("not an ELF file");
        be = d[5] == 2;
        ps = be ? 4 : 8;
        const uint64_t phoff = q(0x20);
        const uint16_t es = h(0x36), n = h(0x38);
        for (uint16_t i = 0; i < n; i++) {
            const uint64_t o = phoff + uint64_t(i) * es;
            Ph p{w(o), w(o + 4), q(o + 8), q(o + 16), q(o + 24), q(o + 32), q(o + 40), q(o + 48)};
            ph.push_back(p);
            if ((p.type == 1 || p.type == 0x61000010 || p.type == 0x61000000) && p.fs) seg.push_back({p.va, p.off, p.fs, p.ms});
        }
        entry = q(0x18);
    }
    std::optional<uint64_t> off(uint64_t va) const {
        for (const Seg &s : seg)
            if (s.va <= va && va < s.va + s.fs) return s.of + va - s.va;
        return std::nullopt;
    }
    std::optional<uint32_t> u32(uint64_t va) const {
        const auto o = off(va);
        if (!o || *o + 4 > d.size()) return std::nullopt;
        return w(*o);
    }
    std::optional<std::string> cstr(uint64_t va) const {
        if (!va) return std::nullopt;
        const auto o = off(va);
        if (!o) return std::nullopt;
        const uint64_t lim = std::min<uint64_t>(*o + 200, d.size());
        uint64_t e = *o;
        while (e < lim && d[e]) e++;
        if (e >= lim || e <= *o) return std::nullopt;   // no NUL within 200 bytes, or empty
        std::string s(d.begin() + std::ptrdiff_t(*o), d.begin() + std::ptrdiff_t(e));
        for (unsigned char c : s)
            if (c < 0x20 || c > 0x7e) return std::nullopt;   // ASCII and printable
        return s;
    }
};

uint32_t rd32le_(const Bytes &d, uint64_t o) { return uint32_t(d.at(o)) | uint32_t(d.at(o + 1)) << 8 | uint32_t(d.at(o + 2)) << 16 | uint32_t(d.at(o + 3)) << 24; }
uint64_t rd64le_(const Bytes &d, uint64_t o) { return uint64_t(rd32le_(d, o)) | uint64_t(rd32le_(d, o + 4)) << 32; }

// a fake-signed PS4 SELF -> its ELF with the R_X86_64_RELATIVE relocations applied (load address 0)
Elf unwrap_ps4(const fs::path &path) {
    const Bytes d = read_bytes(path);
    Bytes out;
    if (d.size() >= 4 && !std::memcmp(d.data(), "\x7f" "ELF", 4)) out = d;
    else {
        if (d.size() < 0x20 || !(d[0] == 0x4f && d[1] == 0x15 && d[2] == 0x3d && d[3] == 0x1d))
            throw TablesMissing(S(path) + " is neither an ELF nor a PS4 SELF");
        const uint16_t nseg = uint16_t(d[0x18] | d[0x19] << 8);
        const uint64_t eo = 0x20 + 32ull * nseg;
        if (eo + 0x40 > d.size() || std::memcmp(d.data() + eo, "\x7f" "ELF", 4)) throw TablesMissing(S(path) + ": no ELF header in the SELF");
        const uint64_t phoff = rd64le_(d, eo + 0x20);
        const uint16_t phnum = uint16_t(d[eo + 0x38] | d[eo + 0x39] << 8);
        struct P {
            uint64_t off, fs;
        };
        std::vector<P> ph;
        uint64_t total = 0;
        for (uint16_t i = 0; i < phnum; i++) {
            const uint64_t o = eo + phoff + 56ull * i;
            ph.push_back({rd64le_(d, o + 8), rd64le_(d, o + 32)});
            total = std::max(total, ph.back().off + ph.back().fs);
        }
        out.assign(size_t(total), 0);
        std::copy(d.begin() + std::ptrdiff_t(eo), d.begin() + std::ptrdiff_t(eo + 0x40 + 56ull * phnum), out.begin());
        for (uint16_t i = 0; i < nseg; i++) {
            const uint64_t flags = rd64le_(d, 0x20 + 32ull * i), off = rd64le_(d, 0x28 + 32ull * i), csize = rd64le_(d, 0x30 + 32ull * i);
            if (!(flags & 0x800)) continue;
            if (flags & 0xa) throw TablesMissing(S(path) + " is encrypted or compressed (a decrypted / fake-signed executable is needed)");
            const P &p = ph.at(size_t(flags >> 20));
            for (uint64_t k = 0; k < csize && p.off + k < out.size() && off + k < d.size(); k++) out[size_t(p.off + k)] = d[size_t(off + k)];
        }
    }
    Elf e(out);
    const Elf::Ph *dyn = nullptr, *dld = nullptr;
    for (const auto &p : e.ph) {
        if (p.type == 2 && !dyn) dyn = &p;
        if (p.type == 0x61000000 && !dld) dld = &p;
    }
    if (dyn && dld) {
        std::map<uint64_t, uint64_t> tags;
        for (uint64_t o = dyn->off; o + 16 <= dyn->off + dyn->fs && o + 16 <= out.size(); o += 16) tags.emplace(rd64le_(out, o), rd64le_(out, o + 8));
        if (tags.count(0x6100002f)) {
            const uint64_t ro = dld->off + tags[0x6100002f], rl = tags.count(0x61000031) ? tags[0x61000031] : 0;
            for (uint64_t o = ro; o + 24 <= ro + rl && o + 24 <= out.size(); o += 24) {
                const uint64_t off = rd64le_(out, o), info = rd64le_(out, o + 8), add = rd64le_(out, o + 16);
                if ((info & 0xffffffff) != 8) continue;
                for (const auto &p : e.ph)
                    if ((p.type == 1 || p.type == 0x61000010) && p.va <= off && off < p.va + p.fs) {
                        const uint64_t at = p.off + off - p.va;
                        for (int k = 0; k < 8 && at + uint64_t(k) < out.size(); k++) out[size_t(at + uint64_t(k))] = uint8_t(add >> (8 * k));
                        break;
                    }
            }
        }
    }
    return Elf(out);
}

// ------------------------------------------------------------------------------------------------ class records
struct Member {
    std::string name, type, sub;
    int64_t carr = 0, flags = 0, off = 0;
    std::optional<std::string> cls;
};
struct Cls {
    std::optional<std::string> parent;
    std::optional<int64_t> size;
    std::vector<Member> members;
};
using DB = std::vector<std::pair<std::string, Cls>>;   // in insertion order (as the Python dict)
Cls *find(DB &db, const std::string &c) {
    for (auto &[n, x] : db)
        if (n == c) return &x;
    return nullptr;
}
struct Rec {
    std::optional<uint64_t> self, parent, size;
    std::string name;
    uint64_t mem = 0, nmem = 0;
};

std::vector<Member> members(const Elf &x, uint64_t mem, uint64_t n, const std::map<uint64_t, std::string> &names) {
    std::vector<Member> ms;
    const uint64_t sz = x.ps == 4 ? 0x18 : 0x28;
    const auto mo = mem ? x.off(mem) : std::nullopt;
    if (!mo || !n) return ms;
    for (uint64_t k = 0; k < n; k++) {
        const uint64_t r = *mo + k * sz;
        uint64_t mn, c;
        uint32_t t, st, fl, of;
        int32_t ca;
        if (r + sz > x.d.size()) throw std::invalid_argument("member");
        if (x.ps == 4) {
            mn = x.w(r), c = x.w(r + 4);
            t = x.d[r + 0xc], st = x.d[r + 0xd], ca = int16_t(x.h(r + 0xe)), fl = x.h(r + 0x10), of = x.h(r + 0x12);
        } else {
            mn = x.q(r), c = x.q(r + 8);
            t = x.d[r + 0x18], st = x.d[r + 0x19], ca = int16_t(x.h(r + 0x1a)), fl = x.h(r + 0x1c), of = x.h(r + 0x1e);
        }
        const auto nm = x.cstr(mn);
        if (!nm || t >= NTY || st >= NTY) throw std::invalid_argument("member");
        Member m{*nm, TY[t], TY[st], ca, fl, of, std::nullopt};
        if (auto it = names.find(c); it != names.end() && !it->second.empty()) m.cls = it->second;
        else m.cls = x.cstr(c);
        ms.push_back(m);
    }
    return ms;
}

DB finish(const Elf &x, const std::vector<Rec> &recs) {
    std::map<uint64_t, std::string> names;
    for (const Rec &r : recs)
        if (r.self) names[*r.self] = r.name;
    DB out;
    for (const Rec &r : recs) {
        std::vector<Member> m;
        try {
            m = members(x, r.mem, r.nmem, names);
        } catch (const std::invalid_argument &) {
            m.clear();
        }
        Cls e;
        if (r.parent)
            if (auto it = names.find(*r.parent); it != names.end()) e.parent = it->second;
        if (r.size) e.size = int64_t(*r.size);
        e.members = m;
        Cls *have = find(out, r.name);
        if (!have) out.push_back({r.name, e});
        else if (!m.empty() && have->members.empty()) *have = e;
    }
    return out;
}

// PPC64: follows register values through the text segment up to every bl
std::vector<Rec> scan_ps3(const Elf &x) {
    const auto opd = x.off(x.entry);
    if (!opd) throw TablesMissing("PS3 EBOOT: no entry descriptor");
    const uint32_t toc = x.w(*opd + 4);
    if (x.seg.empty()) throw TablesMissing("PS3 EBOOT: no segments");
    const uint64_t v = x.seg[0].va, o = x.seg[0].of, end = o + x.seg[0].fs;
    std::map<int, uint32_t> r;
    std::map<int64_t, std::optional<uint32_t>> st;
    std::vector<std::pair<uint64_t, std::vector<Rec>>> sites;   // in first-call order
    auto get = [&](int k) -> std::optional<uint32_t> {
        auto it = r.find(k);
        return it == r.end() ? std::nullopt : std::optional<uint32_t>(it->second);
    };
    for (uint64_t j = o; j + 4 <= end && j + 4 <= x.d.size(); j += 4) {
        r[2] = toc;
        const uint32_t w = x.w(j);
        const uint32_t op = w >> 26, rt = (w >> 21) & 31, ra = (w >> 16) & 31, si = w & 0xffff;
        const int32_t s = (si & 0x8000) ? int32_t(si) - 0x10000 : int32_t(si);
        if (op == 14) {
            if (ra == 0) r[int(rt)] = uint32_t(s);
            else if (r.count(int(ra))) r[int(rt)] = r[int(ra)] + uint32_t(s);
            else r.erase(int(rt));
        } else if (op == 15) {
            if (ra == 0) r[int(rt)] = uint32_t(s) << 16;
            else if (r.count(int(ra))) r[int(rt)] = r[int(ra)] + (uint32_t(s) << 16);
            else r.erase(int(rt));
        } else if (op == 24) {
            if (r.count(int(rt))) r[int(ra)] = r[int(rt)] | si;
        } else if (op == 32) {
            if (ra == 1 && rt >= 13) continue;
            std::optional<uint32_t> val;
            if (r.count(int(ra))) val = x.u32(uint32_t(r[int(ra)] + uint32_t(s)));
            if (val) r[int(rt)] = *val;
            else r.erase(int(rt));
        } else if (op == 31 && ((w >> 1) & 1023) == 444 && rt == ((w >> 11) & 31)) {
            if (r.count(int(rt))) r[int(ra)] = r[int(rt)];
            else r.erase(int(ra));
        } else if ((op == 36 || op == 62) && ra == 1) {
            st[op == 62 ? (s & ~3) : s] = get(int(rt));
        } else if (op == 58 || op == 34 || op == 40 || op == 33 || op == 42) {
            if (ra == 1 && rt >= 13) continue;
            r.erase(int(rt));
        } else if (op == 30 || op == 21) {
            if (r.count(int(rt))) r[int(ra)] = r[int(rt)];
            else r.erase(int(ra));
        } else if (op == 31 && ((w >> 1) & 1023) == 986) {
            if (r.count(int(rt))) r[int(ra)] = r[int(rt)];
            else r.erase(int(ra));
        } else if (op == 18 && (w & 3) == 1) {
            int64_t li = w & 0x3fffffc;
            if (li & 0x2000000) li -= 0x4000000;
            const auto nm = x.cstr(get(4).value_or(0));
            if (nm && (starts(*nm, "hk") || starts(*nm, "Hk"))) {
                const uint64_t tgt = uint64_t(int64_t(v + j - o) + li);
                auto it = std::find_if(sites.begin(), sites.end(), [&](const auto &e) { return e.first == tgt; });
                if (it == sites.end()) sites.push_back({tgt, {}}), it = sites.end() - 1;
                auto sv = [&](int64_t k) -> uint64_t {
                    auto f = st.find(k);
                    return f != st.end() && f->second ? *f->second : 0;
                };
                Rec rec;
                if (auto a = get(3)) rec.self = *a;
                rec.name = *nm;
                if (auto a = get(5)) rec.parent = *a;
                if (auto a = get(6)) rec.size = *a;
                rec.mem = sv(0x70);
                rec.nmem = sv(0x78);
                it->second.push_back(rec);
            }
            for (int q = 3; q < 13; q++) r.erase(q);
            st.clear();
        }
    }
    if (sites.empty()) throw TablesMissing("no hkClass constructor calls found in the PS3 EBOOT");
    const auto *best = &sites[0];
    for (const auto &e : sites)
        if (e.second.size() > best->second.size()) best = &e;
    return best->second;
}

// ---- x86-64 (Intel syntax objdump lines)
bool is_word(char c) { return isalnum(uint8_t(c)) || c == '_'; }
std::string reg64(const std::string &a) {
    static const std::map<std::string, std::string> LOW = {{"esi", "rsi"}, {"edi", "rdi"}, {"edx", "rdx"}, {"ecx", "rcx"}, {"r8d", "r8"},
                                                           {"r9d", "r9"},  {"eax", "rax"}, {"ebx", "rbx"}, {"ebp", "rbp"}};
    auto it = LOW.find(a);
    return it == LOW.end() ? a : it->second;
}
bool hexnum(const std::string &s, uint64_t &v) {   // 0x[0-9a-f]+
    if (s.size() < 3 || s[0] != '0' || s[1] != 'x') return false;
    for (size_t i = 2; i < s.size(); i++)
        if (!(isdigit(uint8_t(s[i])) || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    v = std::stoull(s.substr(2), nullptr, 16);
    return true;
}
bool wordstr(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!is_word(c)) return false;
    return true;
}
// finds 'mnem' followed by white space at any position of tx (a regex search), giving the operand text after it
bool after_mnem(const std::string &tx, const char *mnem, std::string &rest, size_t from = 0) {
    const size_t ml = std::strlen(mnem);
    for (size_t p = tx.find(mnem, from); p != std::string::npos; p = tx.find(mnem, p + 1)) {
        size_t q = p + ml;
        if (q >= tx.size() || !isspace(uint8_t(tx[q]))) continue;
        while (q < tx.size() && isspace(uint8_t(tx[q]))) q++;
        rest = tx.substr(q);
        return true;
    }
    return false;
}

struct X86State {
    std::map<std::string, uint64_t> r;
    std::map<uint64_t, uint64_t> st;
};
void x86_line(X86State &S_, const std::string &tx) {
    std::string rest;
    // lea REG,[rip...] # 0xADDR
    if (after_mnem(tx, "lea", rest)) {
        const size_t c = rest.find(",[rip");
        if (c != std::string::npos && wordstr(rest.substr(0, c))) {
            const size_t rb = rest.find(']', c);
            if (rb != std::string::npos) {
                size_t q = rb + 1;
                size_t ws = q;
                while (q < rest.size() && isspace(uint8_t(rest[q]))) q++;
                if (q > ws && rest.compare(q, 2, "# ") == 0) {
                    size_t e = q + 2;
                    size_t s0 = e;
                    if (rest.compare(e, 2, "0x") == 0) {
                        e += 2;
                        while (e < rest.size() && (isdigit(uint8_t(rest[e])) || (rest[e] >= 'a' && rest[e] <= 'f'))) e++;
                        if (e > s0 + 2) {
                            S_.r[reg64(rest.substr(0, c))] = std::stoull(rest.substr(s0 + 2, e - s0 - 2), nullptr, 16);
                            return;
                        }
                    }
                }
            }
        }
    }
    // the mov forms end the text
    std::string mv;
    if (after_mnem(tx, "mov", mv)) {
        // mov [WORD PTR ][rsp(+0xN)],X  -> X an immediate (0x..) or a register
        std::string m = mv;
        size_t sp = m.find(' ');
        if (m.compare(0, 1, "[") != 0 && sp != std::string::npos && wordstr(m.substr(0, sp)) && m.compare(sp, 5, " PTR ") == 0) m = m.substr(sp + 5);
        if (m.compare(0, 4, "[rsp") == 0) {
            size_t q = 4;
            uint64_t disp = 0;
            bool ok = true;
            if (q < m.size() && m[q] == '+') {
                size_t e = m.find(']', q);
                ok = e != std::string::npos && hexnum(m.substr(q + 1, e - q - 1), disp);
                q = e;
            }
            if (ok && q < m.size() && m[q] == ']' && q + 1 < m.size() && m[q + 1] == ',') {
                const std::string val = m.substr(q + 2);
                uint64_t imm;
                if (hexnum(val, imm)) {
                    S_.st[disp] = imm;
                    return;
                }
                if (wordstr(val)) {
                    if (auto it = S_.r.find(reg64(val)); it != S_.r.end()) S_.st[disp] = it->second;
                    return;
                }
            }
        }
        // mov REG,0x..   /   mov REG,REG
        const size_t c = mv.find(',');
        if (c != std::string::npos && wordstr(mv.substr(0, c))) {
            const std::string a = mv.substr(0, c), b = mv.substr(c + 1);
            uint64_t imm;
            if (hexnum(b, imm)) {
                S_.r[reg64(a)] = imm;
                return;
            }
            if (wordstr(b)) {
                if (auto it = S_.r.find(reg64(b)); it != S_.r.end()) S_.r[reg64(a)] = it->second;
                else S_.r.erase(reg64(a));
                return;
            }
        }
    }
    if (after_mnem(tx, "xor", rest)) {
        const size_t c = rest.find(',');
        if (c != std::string::npos && wordstr(rest.substr(0, c)) && wordstr(rest.substr(c + 1)) && rest.substr(0, c) == rest.substr(c + 1)) S_.r[reg64(rest.substr(0, c))] = 0;
    }
}

std::vector<Rec> scan_ps4(const Elf &x, const std::string &objdump, const fs::path &work) {
    const Elf::Ph *t = nullptr;
    for (const auto &p : x.ph)
        if (p.type == 1 && p.va == 0) {
            t = &p;
            break;
        }
    if (!t) throw TablesMissing("PS4 executable: no text segment at address 0");
    const fs::path tmp = work / "u2hk-text.bin";
    write_bytes(tmp, Bytes(x.d.begin() + std::ptrdiff_t(t->off), x.d.begin() + std::ptrdiff_t(std::min<uint64_t>(t->off + t->fs, x.d.size()))));
    std::vector<std::pair<uint64_t, std::vector<Rec>>> sites;
    std::deque<std::string> lines;
    auto keep = [](const std::string &l) {
        for (const char *k : {"rbx", "rbp", "r12", "r13", "r14", "r15", "[rsp"})
            if (l.find(k) != std::string::npos) return true;
        return false;
    };
    const int rc = run({objdump, "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "intel", S(tmp)}, work, {}, [&](const std::string &ln) {
        std::string target;
        bool call = false;
        if (std::string rest; after_mnem(ln, "call", rest)) {
            size_t e = 0;
            if (rest.compare(0, 2, "0x") == 0) {
                e = 2;
                while (e < rest.size() && (isdigit(uint8_t(rest[e])) || (rest[e] >= 'a' && rest[e] <= 'f'))) e++;
                if (e > 2) call = true, target = rest.substr(2, e - 2);
            }
        }
        if (!call) {
            lines.push_back(ln);
            if (lines.size() > 400) lines.pop_front();
            return;
        }
        X86State s;
        for (const std::string &l : lines) {
            const size_t tab = l.rfind('\t');
            std::string tx = tab == std::string::npos ? l : l.substr(tab + 1);
            tx = strip(tx);
            x86_line(s, tx);
        }
        const auto rsi = s.r.find("rsi");
        const auto nm = x.cstr(rsi == s.r.end() ? 0 : rsi->second);
        if (nm && (starts(*nm, "hk") || starts(*nm, "Hk"))) {
            const uint64_t tg = std::stoull(target, nullptr, 16);
            auto it = std::find_if(sites.begin(), sites.end(), [&](const auto &e) { return e.first == tg; });
            if (it == sites.end()) sites.push_back({tg, {}}), it = sites.end() - 1;
            Rec rec;
            auto g = [&](const char *k) -> std::optional<uint64_t> {
                auto f = s.r.find(k);
                return f == s.r.end() ? std::nullopt : std::optional<uint64_t>(f->second);
            };
            rec.self = g("rdi");
            rec.name = *nm;
            rec.parent = g("rdx");
            rec.size = g("rcx");
            rec.mem = s.st.count(0x10) ? s.st[0x10] : 0;
            rec.nmem = s.st.count(0x18) ? s.st[0x18] : 0;
            it->second.push_back(rec);
        }
        std::deque<std::string> kept;
        for (const std::string &l : lines)
            if (keep(l)) kept.push_back(l);
        while (kept.size() > 400) kept.pop_front();
        lines = kept;
    });
    std::error_code ec;
    fs::remove(tmp, ec);
    if (rc || sites.empty()) throw TablesMissing("objdump found no hkClass constructor calls in the PS4 executable");
    const auto *best = &sites[0];
    for (const auto &e : sites)
        if (e.second.size() > best->second.size()) best = &e;
    return best->second;
}

// ------------------------------------------------------------------------------------------------ run-time built classes
Member d_(const char *name, const char *t, const char *sub = "VOID", int64_t flags = 0, const char *cls = nullptr) {
    Member m{name, t, sub, 0, flags, 0, std::nullopt};
    if (cls) m.cls = cls;
    return m;
}
// Havok 2011.3 member declarations (order, type, subtype, flags, class) of the classes whose member tables are built at
// run time in the PS4 executable; no offsets: layout() places them with the C rules
const std::vector<std::tuple<std::string, std::optional<std::string>, std::vector<Member>>> &fill_decl() {
    static const std::vector<std::tuple<std::string, std::optional<std::string>, std::vector<Member>>> F = {
        {"hkpMoppCode", "hkReferencedObject", {d_("info", "STRUCT", "VOID", 0, "hkpMoppCodeCodeInfo"), d_("data", "ARRAY", "UINT8"), d_("buildType", "ENUM", "INT8")}},
        {"hkpExtendedMeshShapeTrianglesSubpart",
         "hkpExtendedMeshShapeSubpart",
         {d_("numTriangleShapes", "INT32"), d_("vertexBase", "POINTER", "VOID", 0x400), d_("numVertices", "INT32"), d_("indexBase", "POINTER", "VOID", 0x400),
          d_("vertexStriding", "UINT16"), d_("triangleOffset", "INT32"), d_("indexStriding", "UINT16"), d_("stridingType", "ENUM", "INT8"),
          d_("flipAlternateTriangles", "INT8"), d_("extrusion", "VECTOR4"), d_("transform", "QSTRANSFORM")}},
        {"hkpExtendedMeshShape",
         "hkpShapeCollection",
         {d_("embeddedTrianglesSubpart", "STRUCT", "VOID", 0, "hkpExtendedMeshShapeTrianglesSubpart"), d_("aabbHalfExtents", "VECTOR4"),
          d_("aabbCenter", "VECTOR4"), d_("materialClass", "POINTER", "VOID", 0x400), d_("numBitsForSubpartIndex", "INT32"),
          d_("trianglesSubparts", "ARRAY", "STRUCT", 0, "hkpExtendedMeshShapeTrianglesSubpart"),
          d_("shapesSubparts", "ARRAY", "STRUCT", 0, "hkpExtendedMeshShapeShapesSubpart"), d_("weldingInfo", "ARRAY", "UINT16"),
          d_("weldingType", "ENUM", "UINT8"), d_("defaultCollisionFilterInfo", "UINT32"), d_("cachedNumChildShapes", "INT32"), d_("triangleRadius", "REAL")}},
        {"hkaSkeletonMapperData",
         std::nullopt,
         {d_("skeletonA", "POINTER", "STRUCT", 0, "hkaSkeleton"), d_("skeletonB", "POINTER", "STRUCT", 0, "hkaSkeleton"),
          d_("simpleMappings", "ARRAY", "STRUCT", 0, "hkaSkeletonMapperDataSimpleMapping"),
          d_("chainMappings", "ARRAY", "STRUCT", 0, "hkaSkeletonMapperDataChainMapping"), d_("unmappedBones", "ARRAY", "INT16"),
          d_("extractedMotionMapping", "QSTRANSFORM"), d_("keepUnmappedLocal", "BOOL"), d_("mappingType", "ENUM", "INT32")}},
        {"hkpBallSocketConstraintAtom",
         "hkpConstraintAtom",
         {d_("solvingMethod", "UINT8"), d_("bodiesToNotify", "UINT8"), d_("velocityStabilizationFactor", "UINT8"), d_("maxImpulse", "REAL"),
          d_("inertiaStabilizationFactor", "REAL")}},
    };
    return F;
}
int64_t scalar(const std::string &t) {
    static const std::map<std::string, int64_t> SC = {{"BOOL", 1},  {"CHAR", 1},  {"INT8", 1},  {"UINT8", 1},   {"INT16", 2},   {"UINT16", 2},
                                                      {"HALF", 2},  {"INT32", 4}, {"UINT32", 4}, {"REAL", 4},   {"INT64", 8},   {"UINT64", 8},
                                                      {"ULONG", 8}, {"POINTER", 8}, {"CSTRING", 8}, {"STRINGPTR", 8}};
    auto it = SC.find(t);
    return it == SC.end() ? 0 : it->second;
}
int64_t class_align(DB &db, const std::string &cls);
std::pair<int64_t, int64_t> size_align(DB &db, const Member &m) {
    static const std::map<std::string, int64_t> VEC = {{"VECTOR4", 16}, {"QUATERNION", 16}, {"MATRIX3", 48}, {"ROTATION", 48},
                                                       {"QSTRANSFORM", 48}, {"MATRIX4", 64}, {"TRANSFORM", 64}};
    if (const int64_t s = scalar(m.type)) return {s, s};
    if (VEC.count(m.type)) return {VEC.at(m.type), 16};
    if (m.type == "ENUM" || m.type == "FLAGS") {
        if (!scalar(m.sub)) throw TablesMissing("KeyError: " + m.sub);
        return {scalar(m.sub), scalar(m.sub)};
    }
    if (m.type == "ARRAY") return {16, 8};
    if (m.type == "STRUCT") {
        Cls *c = find(db, m.cls.value_or(""));
        if (!c) throw TablesMissing("Havok 2011 class " + m.cls.value_or("") + " missing in the PS4 executable");
        return {c->size.value_or(0), class_align(db, *m.cls)};
    }
    throw TablesMissing("KeyError: " + m.type);
}
int64_t class_align(DB &db, const std::string &cls) {
    int64_t a = 1;
    std::optional<std::string> c = cls;
    while (c && !c->empty()) {
        Cls *x = find(db, *c);
        if (!x) throw TablesMissing("Havok 2011 class " + *c + " missing in the PS4 executable");
        for (const Member &m : x->members) a = std::max(a, size_align(db, m).second);
        a = std::max<int64_t>(a, (x->parent || *c == "hkReferencedObject" || *c == "hkBaseObject") ? 8 : 1);
        c = x->parent;
    }
    return a;
}
void layout(DB &db) {
    auto todo = fill_decl();
    while (!todo.empty()) {
        bool any = false;
        for (size_t i = 0; i < todo.size();) {
            const auto &[cls, parent, decl] = todo[i];
            bool wait = false;
            auto pending = [&](const std::string &d) {
                return std::any_of(todo.begin(), todo.end(), [&](const auto &t) { return std::get<0>(t) == d; });
            };
            if (parent && pending(*parent)) wait = true;
            for (const Member &m : decl)
                if (m.type == "STRUCT" && m.cls && pending(*m.cls)) wait = true;
            if (wait) {
                i++;
                continue;
            }
            int64_t off = 0, align = 1;
            if (parent) {
                Cls *p = find(db, *parent);
                if (!p) throw TablesMissing("Havok 2011 class " + *parent + " missing in the PS4 executable");
                off = p->size.value_or(0);
                align = class_align(db, *parent);
            }
            std::vector<Member> ms;
            for (Member m : decl) {
                const auto [s, a] = size_align(db, m);
                off = (off + a - 1) / a * a;
                m.off = off;
                ms.push_back(m);
                off += s;
                align = std::max(align, a);
            }
            Cls c;
            c.parent = parent;
            c.size = (off + align - 1) / align * align;
            c.members = ms;
            if (Cls *have = find(db, cls)) *have = c;
            else db.push_back({cls, c});
            todo.erase(todo.begin() + std::ptrdiff_t(i));
            any = true;
        }
        if (!any) throw TablesMissing("Havok 2011 run-time classes: a circular dependency");
    }
}

// ---- the retail PS4 packfiles
struct PF {
    Bytes data;
    std::set<uint32_t> loc, glob;
    std::vector<std::pair<uint32_t, std::string>> virt;
};
std::vector<PF> packfiles(const Bytes &d) {
    static const Bytes MAG = {0x57, 0xe0, 0xe0, 0x57, 0x10, 0xc0, 0xc0, 0x10};
    std::vector<PF> out;
    size_t i = 0;
    for (;;) {
        auto it = std::search(d.begin() + std::ptrdiff_t(i), d.end(), MAG.begin(), MAG.end());
        if (it == d.end()) return out;
        i = size_t(it - d.begin());
        if (i + 16 <= d.size() && std::equal(MAG.begin(), MAG.end(), d.begin() + std::ptrdiff_t(i + 8))) {
            i += 8;
            continue;
        }
        const uint8_t *b = d.data() + i;
        const size_t bl = d.size() - i;
        try {
            auto u32 = [&](size_t o) {
                if (o + 4 > bl) throw std::out_of_range("struct");
                return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24;
            };
            const uint32_t nsec = u32(0x14);
            std::map<std::string, std::array<uint32_t, 7>> secs;
            for (uint32_t s = 0; s < nsec; s++) {
                const size_t o = 0x40 + 0x30 * size_t(s);
                if (o + 48 > bl) throw std::out_of_range("struct");
                std::string nm(reinterpret_cast<const char *>(b + o), 19);
                nm = nm.substr(0, nm.find('\0'));
                std::array<uint32_t, 7> v;
                for (int k = 0; k < 7; k++) v[size_t(k)] = u32(o + 20 + 4 * size_t(k));
                secs[nm] = v;
            }
            const auto &ds = secs.at("__data__");
            const uint32_t st = ds[0], lf = ds[1], gf = ds[2], vf = ds[3], ex = ds[4];
            const auto &cs = secs.at("__classnames__");
            const uint32_t c0 = cs[0], cl = cs[1];
            std::map<uint32_t, std::string> cn;
            const size_t ce = std::min<size_t>(size_t(c0) + cl, bl);
            size_t k = 0;
            while (c0 + k + 5 < ce && !(b[c0 + k] == 0xff && b[c0 + k + 1] == 0xff && b[c0 + k + 2] == 0xff && b[c0 + k + 3] == 0xff)) {
                size_t j = k + 5;
                while (c0 + j < ce && b[c0 + j]) j++;
                std::string s(reinterpret_cast<const char *>(b + c0 + k + 5), j - k - 5);
                for (unsigned char ch : s)
                    if (ch > 0x7f) throw std::out_of_range("decode");
                cn[uint32_t(k + 5)] = s;
                if (c0 + j >= ce) break;
                k = j + 1;
            }
            PF pf;
            pf.data.assign(b + std::min<size_t>(st, bl), b + std::min<size_t>(size_t(st) + lf, bl));
            for (size_t o = size_t(st) + lf; o < size_t(st) + gf; o += 8) pf.loc.insert(u32(o));
            for (int64_t o = int64_t(st) + gf; o < int64_t(st) + int64_t(vf) - 11; o += 12) pf.glob.insert(u32(size_t(o)));
            for (int64_t o = int64_t(st) + vf; o < int64_t(st) + int64_t(ex) - 11; o += 12) {
                const uint32_t a = u32(size_t(o));
                if (a == 0xffffffff) continue;
                auto c = cn.find(u32(size_t(o) + 8));
                pf.virt.push_back({a, c == cn.end() ? std::string() : c->second});
            }
            std::sort(pf.virt.begin(), pf.virt.end());
            out.push_back(std::move(pf));
        } catch (const std::exception &) {
        }
        i += 8;
    }
}

// the FILL layouts against the retail packfiles: arrays where the arrays are (count, capacity flags, local fixup), object
// pointers where the global fixups are, objects not larger than their space
void check_layout(DB &db, const fs::path &actor_dir, size_t max_objects = 400) {
    std::set<std::string> want = {"hkpStorageExtendedMeshShape", "hkaSkeletonMapper"};
    for (const auto &f : fill_decl()) want.insert(std::get<0>(f));
    std::set<std::string> errs;
    std::vector<std::pair<std::string, int>> seen;
    size_t total = 0;
    std::function<void(const std::string &, size_t, const PF &)> walk = [&](const std::string &cls, size_t base, const PF &pf) {
        std::optional<std::string> c = cls;
        while (c && !c->empty()) {
            Cls *x = find(db, *c);
            if (!x) return;
            for (const Member &m : x->members) {
                const size_t o = base + size_t(m.off);
                if (m.type == "ARRAY" && !(m.flags & 0x400)) {
                    if (o + 16 > pf.data.size()) {
                        errs.insert(*c + "." + m.name);
                        continue;
                    }
                    const int32_t n = int32_t(rd32le_(pf.data, o + 8));
                    const uint32_t cap = rd32le_(pf.data, o + 12);
                    if (int64_t(cap & 0x3fffffff) != n || (n && !pf.loc.count(uint32_t(o)))) errs.insert(*c + "." + m.name);
                } else if (m.type == "POINTER" && m.sub == "STRUCT" && !(m.flags & 0x400)) {
                    bool zero = true;
                    for (size_t k = o; k < o + 8; k++) zero = zero && (k >= pf.data.size() || !pf.data[k]);
                    if (!pf.glob.count(uint32_t(o)) && !zero) errs.insert(*c + "." + m.name);
                } else if (m.type == "STRUCT")
                    walk(m.cls.value_or(""), o, pf);
            }
            c = x->parent;
        }
    };
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(actor_dir, ec)) names.push_back(S(e.path().filename()));
    std::sort(names.begin(), names.end());
    for (const std::string &f : names) {
        if (!ends(f, ".pak") || total >= max_objects) continue;
        const Bytes d = read_bytes(actor_dir / P(f));
        const std::string s1 = "hkpMoppCode", s2 = "hkaSkeletonMapper";
        if (std::search(d.begin(), d.end(), s1.begin(), s1.end()) == d.end() && std::search(d.begin(), d.end(), s2.begin(), s2.end()) == d.end()) continue;
        for (const PF &pf : packfiles(d))
            for (size_t k = 0; k < pf.virt.size(); k++) {
                const auto &[o, cls] = pf.virt[k];
                if (!want.count(cls) || !find(db, cls)) continue;
                const size_t end = k + 1 < pf.virt.size() ? pf.virt[k + 1].first : pf.data.size();
                if (find(db, cls)->size.value_or(0) > int64_t(end) - int64_t(o)) errs.insert(cls + " size");
                walk(cls, o, pf);
                auto it = std::find_if(seen.begin(), seen.end(), [&](const auto &e) { return e.first == cls; });
                if (it == seen.end()) seen.push_back({cls, 1});
                else it->second++;
                total++;
            }
    }
    if (!errs.empty() || seen.empty()) {
        std::string e;
        size_t n = 0;
        for (const std::string &x : errs) {
            if (n++ == 6) break;
            e += (e.empty() ? "" : ", ") + x;
        }
        throw TablesMissing("Havok 2011 layouts do not fit the retail packfiles: " + (e.empty() ? std::string("no retail objects found") : e));
    }
    std::string s;
    for (const auto &[c, n] : seen) s += (s.empty() ? "" : ", ") + fmt("'%s': %d", c.c_str(), n);
    log("Havok tables: layouts checked on {" + s + "} retail objects");
}

std::vector<std::pair<std::string, uint32_t>> ps4_signatures(const fs::path &actor_dir) {
    std::vector<std::pair<std::string, uint32_t>> sig;
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(actor_dir, ec)) names.push_back(S(e.path().filename()));
    std::sort(names.begin(), names.end());
    for (const std::string &f : names) {
        if (!ends(f, ".pak")) continue;
        const Bytes d = read_bytes(actor_dir / P(f));
        size_t last_end = 0;
        for (size_t i = 4; i + 3 < d.size(); i++) {   // {4 bytes} 0x09 'hk' [A-Za-z0-9_]+ NUL, scanned left to right
            if (d[i] != 0x09 || d[i + 1] != 'h' || d[i + 2] != 'k' || i - 4 < last_end) continue;
            size_t j = i + 1;
            while (j < d.size() && (isalnum(d[j]) || d[j] == '_')) j++;
            if (j >= d.size() || d[j] != 0 || j < i + 4) continue;   // at least one character after 'hk'
            const std::string nm(d.begin() + std::ptrdiff_t(i + 1), d.begin() + std::ptrdiff_t(j));
            if (std::none_of(sig.begin(), sig.end(), [&](const auto &e) { return e.first == nm; })) sig.push_back({nm, rd32le_(d, i - 4)});
            last_end = j + 1;
            i = j;
        }
    }
    return sig;
}

std::string find_objdump(const std::string &arg) {
    const fs::path bin = launcher_root() / "u3mp" / "bin";
    std::vector<std::string> c = {arg, S(bin / "objdump.exe"), S(bin / "binutils" / "objdump.exe"), R"(C:\msys64\ucrt64\bin\objdump.exe)",
                                  R"(C:\msys64\mingw64\bin\objdump.exe)", R"(C:\msys64\usr\bin\objdump.exe)"};
    std::error_code ec;
    for (const std::string &x : c)
        if (!x.empty() && fs::is_regular_file(P(x), ec)) return S(fs::absolute(P(x)));
    return find_program({}, "objdump");
}
std::optional<fs::path> ps4_executable(const fs::path &game) {
    std::error_code ec;
    for (const char *c : {"big2-ps4_Shipping.elf", "eboot.bin"})
        for (const fs::path &base : {game, game.parent_path()})
            if (fs::is_regular_file(base / c, ec)) return base / c;
    return std::nullopt;
}

std::string db_json(const DB &db) {
    std::string o = "{";
    bool first = true;
    for (const auto &[n, c] : db) {
        o += first ? "" : ",";
        first = false;
        json::quote(o, n);
        o += ":{\"parent\":";
        if (c.parent) json::quote(o, *c.parent);
        else o += "null";
        o += ",\"size\":" + (c.size ? std::to_string(*c.size) : std::string("null")) + ",\"members\":[";
        for (size_t i = 0; i < c.members.size(); i++) {
            const Member &m = c.members[i];
            o += i ? ",{" : "{";
            o += "\"name\":";
            json::quote(o, m.name);
            o += ",\"type\":\"" + m.type + "\",\"sub\":\"" + m.sub + "\",\"carr\":" + std::to_string(m.carr) + ",\"flags\":" + std::to_string(m.flags) +
                 ",\"off\":" + std::to_string(m.off) + ",\"cls\":";
            if (m.cls) json::quote(o, *m.cls);
            else o += "null";
            o += "}";
        }
        o += "]}";
    }
    return o + "}";
}

std::string build_tables(const Context &c, const fs::path &out) {
    std::error_code ec;
    if (c.ps3_elf.empty() || !fs::is_regular_file(P(c.ps3_elf), ec)) throw TablesMissing("no decrypted PS3 Uncharted 2 EBOOT.elf (--ps3-elf)");
    const Bytes d3 = read_bytes(P(c.ps3_elf));
    if (d3.size() < 4 || std::memcmp(d3.data(), "\x7f" "ELF", 4)) throw TablesMissing(c.ps3_elf + " is not a decrypted ELF");
    const auto ps4 = ps4_executable(P(c.ps4_game));
    if (!ps4) throw TablesMissing("no big2-ps4_Shipping.elf / eboot.bin in the PS4 game folder");
    const std::string od = find_objdump(c.objdump);
    if (od.empty()) throw TablesMissing("GNU objdump not found (u3mp/bin, msys64 or PATH)");
    log("Havok tables: PS3 " + c.ps3_elf + ", PS4 " + S(*ps4) + ", objdump " + od);
    const Elf x3(d3);
    const DB db3 = finish(x3, scan_ps3(x3));
    const Elf x4 = unwrap_ps4(*ps4);
    DB db4 = finish(x4, scan_ps4(x4, od, c.work));
    layout(db4);
    check_layout(db4, c.ps4->actor_dir);
    const auto sig = ps4_signatures(c.ps4->actor_dir);
    std::string js = "{\"version\":" + std::to_string(TABLES_VERSION) + ",\"db3\":" + db_json(db3) + ",\"db4\":" + db_json(db4) + ",\"sig4\":{";
    for (size_t i = 0; i < sig.size(); i++) {
        js += i ? "," : "";
        json::quote(js, sig[i].first);
        js += ":" + std::to_string(sig[i].second);
    }
    js += "}}";
    write_text(S(out) + ".tmp", js);
    fs::rename(S(out) + ".tmp", out, ec);
    log(fmt("Havok tables: %zu PS3 / %zu PS4 classes, %zu PS4 signatures -> %s", db3.size(), db4.size(), sig.size(), S(out).c_str()));
    return js;
}

const Context *g_ctx = nullptr;
bool g_loaded = false;
std::optional<std::string> g_failed;

}  // namespace

void havok_configure(const Context &c) { g_ctx = &c; }

Bytes havok_convert(const Bytes &pf3) {
    if (!g_loaded) {
        if (g_failed) throw TablesMissing(*g_failed);
        if (!g_ctx) throw TablesMissing("no Havok tables configured");
        std::error_code ec;
        fs::create_directories(g_ctx->work, ec);
        const fs::path path = g_ctx->work / "u2hkclasses.json";
        std::string text;
        if (fs::is_regular_file(path, ec)) {
            json::Value v;
            if (json::parse(read_text(path), v) && v.get("version") && v.get("version")->i == TABLES_VERSION && v.get("db3") && v.get("db4")) text = read_text(path);
        }
        if (text.empty()) {
            try {
                text = build_tables(*g_ctx, path);
            } catch (const TablesMissing &e) {
                g_failed = std::string("Havok tables missing (") + e.what() + "): actor skipped";
                log(std::string("Havok tables: ") + e.what() + "; the actors with Havok collision / ragdolls are skipped");
                throw TablesMissing(*g_failed);
            }
        }
        u3::hk::load_tables_u2(text);
        g_loaded = true;
    }
    return u3::hk::convert_packfile(pf3);
}

}  // namespace u2imp
