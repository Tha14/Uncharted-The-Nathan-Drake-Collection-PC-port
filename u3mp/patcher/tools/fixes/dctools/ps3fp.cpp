// dctools/ps3fp.cpp: see ps3fp.hpp.
#include "ps3fp.hpp"

#include "stageplan.hpp"
#include "texcodec.hpp"
#include "u3fix.hpp"

#include <cmath>
#include <cstring>
#include <regex>
#include <set>

#if defined(__clang__)
#pragma clang fp contract(off)   // float results must match numpy's
#endif

namespace u3::fp {
namespace {

const std::map<int, std::string> &ops() {
    static const std::map<int, std::string> m = {
        {0, "NOP"}, {1, "MOV"}, {2, "MUL"}, {3, "ADD"}, {4, "MAD"}, {5, "DP3"}, {6, "DP4"}, {7, "DST"}, {8, "MIN"}, {9, "MAX"},
        {0xA, "SLT"}, {0xB, "SGE"}, {0xC, "SLE"}, {0xD, "SGT"}, {0xE, "SNE"}, {0xF, "SEQ"}, {0x10, "FRC"}, {0x11, "FLR"},
        {0x12, "KIL"}, {0x13, "PK4"}, {0x14, "UP4"}, {0x15, "DDX"}, {0x16, "DDY"}, {0x17, "TEX"}, {0x18, "TXP"}, {0x19, "TXD"},
        {0x1A, "RCP"}, {0x1B, "RSQ"}, {0x1C, "EX2"}, {0x1D, "LG2"}, {0x1E, "LIT"}, {0x1F, "LRP"}, {0x20, "STR"}, {0x21, "SFL"},
        {0x22, "COS"}, {0x23, "SIN"}, {0x24, "PK2"}, {0x25, "UP2"}, {0x26, "POW"}, {0x27, "PKB"}, {0x28, "UPB"}, {0x29, "PK16"},
        {0x2A, "UP16"}, {0x2B, "BEM"}, {0x2C, "PKG"}, {0x2D, "UPG"}, {0x2E, "DP2A"}, {0x2F, "TXL"}, {0x31, "TXB"}, {0x33, "TEXBEM"},
        {0x34, "TXPBEM"}, {0x35, "BEMLUM"}, {0x36, "REFL"}, {0x37, "TIMESWTEX"}, {0x38, "DP2"}, {0x39, "NRM"}, {0x3A, "DIV"},
        {0x3B, "DIVSQ"}, {0x3C, "LIF"}, {0x3D, "FENCT"}, {0x3E, "FENCB"}, {0x40, "BRK"}, {0x41, "CAL"}, {0x42, "IFE"},
        {0x43, "LOOP"}, {0x44, "REP"}, {0x45, "RET"}};
    return m;
}
int nsrc(const std::string &n) {
    static const std::map<std::string, int> m = {
        {"MOV", 1}, {"MUL", 2}, {"ADD", 2}, {"MAD", 3}, {"DP3", 2}, {"DP4", 2}, {"MIN", 2}, {"MAX", 2}, {"SLT", 2}, {"SGE", 2},
        {"SLE", 2}, {"SGT", 2}, {"SNE", 2}, {"SEQ", 2}, {"FRC", 1}, {"FLR", 1}, {"KIL", 0}, {"TEX", 1}, {"TXP", 1}, {"TXL", 1},
        {"TXB", 1}, {"TXD", 3}, {"RCP", 1}, {"RSQ", 1}, {"EX2", 1}, {"LG2", 1}, {"LRP", 3}, {"COS", 1}, {"SIN", 1}, {"POW", 2},
        {"DP2", 2}, {"NRM", 1}, {"DIV", 2}, {"DIVSQ", 2}, {"DDX", 1}, {"DDY", 1}, {"PK2", 1}, {"UP2", 1}, {"PKB", 1}, {"UPB", 1},
        {"PK16", 1}, {"UP16", 1}, {"PK4", 1}, {"UP4", 1}, {"DP2A", 3}, {"REFL", 2}, {"LIT", 1}, {"DST", 2}};
    auto it = m.find(n);
    return it == m.end() ? 3 : it->second;
}
const char *ATTRN[] = {"WPOS", "COL0", "COL1", "FOGC", "TEX0", "TEX1", "TEX2", "TEX3", "TEX4", "TEX5", "TEX6", "TEX7", "TEX8", "TEX9", "SSA", "?15"};

uint32_t sw(uint32_t w) { return ((w & 0xffff) << 16) | (w >> 16); }

std::string pyg(double v) {   // Python '%g' % v
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    char b[64];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

}  // namespace

std::vector<std::string> dis(const Bytes &code, size_t maxn) {
    std::vector<std::string> out;
    size_t i = 0;
    auto be = [&](size_t o) {
        if (o + 4 > code.size()) throw Stop("struct.error: program code past the end");
        return uint32_t(code[o]) << 24 | uint32_t(code[o + 1]) << 16 | uint32_t(code[o + 2]) << 8 | code[o + 3];
    };
    while (i + 16 <= code.size() && out.size() < maxn) {
        const uint32_t d = sw(be(i)), s0 = sw(be(i + 4)), s1 = sw(be(i + 8)), s2 = sw(be(i + 12));
        i += 16;
        const int op = int(((d >> 24) & 0x3f) | (((s1 >> 31) & 1) << 6));
        auto oi = ops().find(op);
        const std::string name = oi != ops().end() ? oi->second : u3::fmt("op%x", op);
        const uint32_t srcs[3] = {s0, s1, s2};
        const int n = nsrc(name);
        std::optional<std::array<double, 4>> cst;
        bool anyc = false;
        for (int k = 0; k < std::min(n, 3); k++) anyc = anyc || (srcs[k] & 3) == 2;
        if (anyc) {
            std::array<double, 4> c;
            for (int k = 0; k < 4; k++) {
                const uint32_t w = sw(be(i + 4 * k));
                float f;
                std::memcpy(&f, &w, 4);
                c[k] = double(f);
            }
            cst = c;
            i += 16;
        }
        auto S = [&](uint32_t s, int k) {
            const uint32_t t = s & 3, idx = (s >> 2) & 0x3f, h = (s >> 8) & 1;
            std::string swz;
            for (int j = 0; j < 4; j++) swz += "xyzw"[(s >> (9 + 2 * j)) & 3];
            std::string r;
            if (t == 0) r = (h ? "H" : "R") + std::to_string(idx);
            else if (t == 1) r = std::string("f[") + ATTRN[(d >> 13) & 0xf] + "]";
            else if (t == 2) {
                if (!cst) throw Stop("TypeError: not enough arguments for format string");
                r = "{" + pyg((*cst)[0]) + "," + pyg((*cst)[1]) + "," + pyg((*cst)[2]) + "," + pyg((*cst)[3]) + "}";
            } else
                r = "?";
            if (swz != "xyzw") r += "." + swz;
            const uint32_t ab = k == 0 ? (s >> 29) & 1 : (s >> 18) & 1;
            if (ab) r = "|" + r + "|";
            if ((s >> 17) & 1) r = "-" + r;
            return r;
        };
        std::string mask;
        for (int j = 0; j < 4; j++)
            if ((d >> (9 + j)) & 1) mask += "xyzw"[j];
        std::string dst = std::string((d >> 7) & 1 ? "H" : "R") + std::to_string((d >> 1) & 0x3f) + (mask != "xyzw" ? "." + mask : "");
        if ((d >> 30) & 1) dst = "(nodst)";
        static const std::map<uint32_t, const char *> SC = {{1, "_x2"}, {2, "_x4"}, {3, "_x8"}, {5, "_d2"}, {6, "_d4"}, {7, "_d8"}};
        auto sci = SC.find((s1 >> 28) & 7);
        const std::string sc = sci == SC.end() ? "" : sci->second, sat = ((d >> 31) & 1) ? "_SAT" : "";
        std::vector<std::string> args;
        for (int k = 0; k < n; k++) {
            if (k > 2) throw Stop("IndexError: list index out of range");
            args.push_back(S(srcs[k], k));
        }
        if (name == "TEX" || name == "TXP" || name == "TXL" || name == "TXB" || name == "TXD") args.push_back("tex" + std::to_string((d >> 17) & 0xf));
        std::string joined;
        for (size_t k = 0; k < args.size(); k++) joined += (k ? ", " : "") + args[k];
        out.push_back(name + sc + sat + " " + dst + ", " + joined + ((d & 1) ? "  (end)" : ""));
        if (d & 1) break;
    }
    return out;
}

namespace {

struct Op {
    bool ok = false;
    std::string op, d;
    std::vector<std::string> a;
    std::string l;
};

std::vector<std::string> split_str(const std::string &s, const std::string &sep) {
    std::vector<std::string> v;
    size_t a = 0;
    while (true) {
        const size_t k = s.find(sep, a);
        if (k == std::string::npos) {
            v.push_back(s.substr(a));
            return v;
        }
        v.push_back(s.substr(a, k - a));
        a = k + sep.size();
    }
}
std::string replace_all(std::string s, const std::string &a, const std::string &b) {
    for (size_t k = 0; (k = s.find(a, k)) != std::string::npos; k += b.size()) s.replace(k, a.size(), b);
    return s;
}
std::string lstrip(const std::string &s, const char *cs) {
    size_t a = 0;
    while (a < s.size() && std::strchr(cs, s[a])) a++;
    return s.substr(a);
}
std::string rstrip(const std::string &s, const char *cs) {
    size_t e = s.size();
    while (e > 0 && std::strchr(cs, s[e - 1])) e--;
    return s.substr(0, e);
}
std::string strip_ws(const std::string &s) {
    size_t a = 0, e = s.size();
    while (a < e && py_space((unsigned char)s[a])) a++;
    while (e > a && py_space((unsigned char)s[e - 1])) e--;
    return s.substr(a, e - a);
}
bool starts(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }
bool ends(const std::string &s, const std::string &p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
bool in_HR(const std::string &x) {   // x[:1] in 'HR' (an empty string is in every string)
    return x.empty() || x[0] == 'H' || x[0] == 'R';
}
std::string dot1(const std::string &s) {   // s.split('.')[1]
    const auto v = split_str(s, ".");
    if (v.size() < 2) throw Stop("IndexError: list index out of range");
    return v[1];
}
bool isdigits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}
int xyzw(char c) {
    const char *p = std::strchr("xyzw", c);
    if (!c || !p) throw Stop("ValueError: substring not found");
    return int(p - "xyzw");
}

Op parse_op(const std::string &l) {   // re.match(r'(\w+) (\S+?),? (.*)$', l)
    static const std::regex R(R"(^(\w+) (\S+?),? (.*)$)");
    std::smatch m;
    Op o;
    o.l = l;
    if (!std::regex_search(l, m, R)) return o;
    o.ok = true;
    o.op = m[1].str();
    o.d = rstrip(m[2].str(), ",");
    for (const std::string &x : split_str(replace_all(m[3].str(), "  (end)", ""), ", ")) o.a.push_back(strip_ws(x));
    return o;
}
std::string base(const std::string &x) { return rstrip(split_str(lstrip(x, "-|"), ".")[0], "|"); }
std::optional<double> kval(const std::string &x) {
    static const std::regex KF(R"(^\{([^}]*)\}\.(\w+)$)");
    std::smatch m;
    const std::string y = lstrip(x, "-");
    if (!std::regex_search(y, m, KF)) return std::nullopt;
    std::vector<double> v;
    for (const std::string &t : split_str(m[1].str(), ",")) {
        const std::string s = strip_ws(t);
        char *e = nullptr;
        const double f = std::strtod(s.c_str(), &e);
        if (s.empty() || *e) throw Stop("ValueError: could not convert string to float");
        v.push_back(f);
    }
    const int k = xyzw(m[2].str()[0]);
    if (size_t(k) >= v.size()) throw Stop("IndexError: list index out of range");
    return v[size_t(k)] * (starts(x, "-") ? -1 : 1);
}
std::string comp(const std::string &x, const std::string &c) {
    const std::string x2 = rstrip(lstrip(x, "-|"), "|");
    if (x2.find('.') == std::string::npos) return c;
    const std::string s = dot1(x2);
    if (s.size() == 1) return s;
    const size_t k = size_t(xyzw(c.empty() ? '\0' : c[0]));
    if (c.size() != 1) throw Stop("ValueError: substring not found");
    if (s.empty()) throw Stop("IndexError: string index out of range");
    return std::string(1, s.size() > k ? s[k] : s.back());
}
std::string scal(const std::string &x0) {
    const std::string x = rstrip(lstrip(x0, "-|"), "|");
    if (x.find('.') == std::string::npos) return "x";
    const std::string s = dot1(x);
    if (s.empty()) throw Stop("IndexError: string index out of range");
    return s.substr(0, 1);
}
std::optional<int> tex_unit(const std::string &l) {   // re.search(r'tex(\d+)$', l)
    size_t e = l.size();
    while (e > 0 && l[e - 1] >= '0' && l[e - 1] <= '9') e--;
    if (e == l.size() || e < 3 || l.compare(e - 3, 3, "tex") != 0) return std::nullopt;
    return std::stoi(l.substr(e));
}

// the last write of reg.c before `before`; -1 when an aliasing R/H write intervenes
std::optional<int> writer(const std::vector<Op> &P, const std::string &reg, const std::string &c, int before) {
    for (int j = before - 1; j >= 0; j--) {
        const Op &o = P[size_t(j)];
        if (o.d.empty()) continue;
        const std::string b = base(o.d), m = o.d.find('.') != std::string::npos ? dot1(o.d) : "xyzw";
        if (b == reg && m.find(c) != std::string::npos) return j;
        if (b.substr(0, 1) == "R" && reg.substr(0, 1) == "H" && isdigits(b.substr(1)) && isdigits(reg.substr(1)) &&
            std::stoll(reg.substr(1)) / 2 == std::stoll(b.substr(1))) {
            const long long k = std::stoll(reg.substr(1));
            const std::vector<std::pair<std::string, std::string>> hit =
                k % 2 == 0 ? std::vector<std::pair<std::string, std::string>>{{"x", "xy"}, {"y", "zw"}} : std::vector<std::pair<std::string, std::string>>{{"z", "xy"}, {"w", "zw"}};
            for (const auto &[rc, hc] : hit)
                if (m.find(rc) != std::string::npos && hc.find(c) != std::string::npos) return -1;
        }
        if (b.substr(0, 1) == "H" && reg.substr(0, 1) == "R" && isdigits(b.substr(1)) && isdigits(reg.substr(1)) &&
            std::stoll(b.substr(1)) / 2 == std::stoll(reg.substr(1))) {
            const long long k = std::stoll(b.substr(1));
            static const std::map<std::string, std::string> HC = {{"x", "xy"}, {"y", "zw"}, {"z", "xy"}, {"w", "zw"}};
            auto hi = HC.find(c);
            if (hi == HC.end()) throw Stop("KeyError: " + c);
            bool any = false;
            for (char z : hi->second) any = any || m.find(z) != std::string::npos;
            if ((k % 2 == 0) == (c == "x" || c == "y" || c == "xy" || c.empty()) && any) return -1;
        }
    }
    return std::nullopt;
}

struct Chain {
    int unit;
    std::string chan, pol;
    double s;
};

std::optional<Chain> chain(const std::vector<std::string> &L) {
    std::vector<Op> P;
    for (const std::string &l : L) P.push_back(parse_op(l));
    static const std::regex VT(R"(^-?f\[TEX\d\])");
    int i = -1;
    std::string hr0;
    for (size_t k = 0; k < P.size(); k++) {
        const Op &o = P[k];
        if (o.op == "ADD_SAT" && !o.d.empty() && o.d.find('.') != std::string::npos && dot1(o.d).size() == 1) {
            int nvt = 0, nhr = 0;
            std::string h;
            for (const std::string &x : o.a) {
                if (std::regex_search(x, VT)) nvt++;
                if (in_HR(lstrip(x, "-|"))) {
                    if (!nhr) h = x;
                    nhr++;
                }
            }
            if (nvt == 1 && nhr == 1) {
                i = int(k);
                hr0 = h;
                break;
            }
        }
    }
    if (i < 0) return std::nullopt;
    const std::string dm = dot1(P[size_t(i)].d), x = hr0;
    if (starts(x, "-")) return std::nullopt;
    auto j = writer(P, base(x), comp(x, dm), i);
    if (!j || *j < 0) return std::nullopt;
    const Op &o2 = P[size_t(*j)];
    const std::string dm2 = dot1(o2.d);
    auto at = [](const std::vector<std::string> &a, size_t k) -> const std::string & {
        if (k >= a.size()) throw Stop("IndexError: list index out of range");
        return a[k];
    };
    auto two_s = [&](const std::string &reg, const std::string &c, int before) -> std::optional<double> {
        auto jd = writer(P, reg, c, before);
        if (!jd || *jd < 0) return std::nullopt;
        const Op &o3 = P[size_t(*jd)];
        if (o3.op == "MAD" && kval(at(o3.a, 1)) == 2.0 && std::fabs(kval(at(o3.a, 2)).value_or(0) - 0.01) < 1e-4) {
            auto js = writer(P, base(at(o3.a, 0)), comp(at(o3.a, 0), dot1(o3.d)), *jd);
            if (js && *js >= 0 && P[size_t(*js)].op == "MOV") return kval(at(P[size_t(*js)].a, 0));
        }
        return std::nullopt;
    };
    std::string num;
    std::optional<double> s;
    bool inv_mad;
    if (o2.op == "DIV") {
        num = at(o2.a, 0);
        const std::string den = at(o2.a, 1);
        s = two_s(base(den), scal(den), *j);
        inv_mad = false;
    } else if (o2.op == "MAD" && starts(at(o2.a, 0), "-") && at(o2.a, 1) == at(o2.a, 2) && in_HR(at(o2.a, 1))) {
        num = at(o2.a, 0).substr(1);
        auto jr = writer(P, base(at(o2.a, 1)), comp(at(o2.a, 1), dm2), *j);
        if (jr && *jr >= 0 && P[size_t(*jr)].op == "RCP") {
            const std::string xx = at(P[size_t(*jr)].a, 0);
            s = two_s(base(xx), scal(xx), *jr);
        }
        inv_mad = true;
    } else
        return std::nullopt;
    if (!s) return std::nullopt;
    std::string pol = inv_mad ? "inv" : "straight";
    if (starts(num, "-")) return std::nullopt;
    std::string c = comp(num, dm2);
    auto jn = writer(P, base(num), c, *j);
    if (!jn || *jn < 0) return std::nullopt;
    const Op *o4 = &P[size_t(*jn)];
    if (o4->op == "ADD" && starts(at(o4->a, 0), "-") && kval(at(o4->a, 1)) == 1.0) {
        pol = inv_mad ? "straight" : "inv";
        const std::string inner = at(o4->a, 0);
        c = comp(inner, c);
        jn = writer(P, base(inner), c, *jn);
        if (!jn || *jn < 0) return std::nullopt;
        o4 = &P[size_t(*jn)];
    }
    const auto u = tex_unit(o4->l);
    if (!(o4->op == "TEX" && u)) return std::nullopt;
    if (*u >= 12) return std::nullopt;
    return Chain{*u, c, pol, *s};
}

bool vc_only(const std::vector<std::string> &L) {
    static const std::regex VC(R"(^-f\[TEX\d\])");
    for (const std::string &l : L) {
        const Op o = parse_op(l);
        if (o.op == "ADD_SAT" && o.a.size() == 2 && std::regex_search(o.a[0], VC) && kval(o.a[1]) == 1.0) return true;
    }
    return false;
}

using Refs = std::vector<Ps3Programs::Ref_>;

std::optional<std::map<int, size_t>> unit_map(const Refs &nref, const std::vector<std::string> &L) {
    std::set<int> used;
    for (const std::string &l : L)
        if (auto u = tex_unit(l)) used.insert(*u);
    std::vector<int> U;
    for (int x : used)
        if (x < 12) U.push_back(x);
    std::vector<size_t> idx;
    for (size_t k = 0; k < nref.size() && idx.size() < 3; k++)
        if (nref[k].shrt == "missingtexture") idx.push_back(k);
    if (idx.size() < 3 || idx[1] != idx[0] + 1 || idx[2] != idx[0] + 2) return std::nullopt;
    if (idx[0] > 1) return std::nullopt;
    std::vector<size_t> S;
    for (size_t k = idx[2] + 1; k < nref.size(); k++) S.push_back(k);
    for (size_t k = 0; k < idx[0]; k++) S.push_back(k);
    if (S.size() != U.size()) return std::nullopt;
    std::vector<int> order;
    for (int x : U)
        if (x) order.push_back(x);
    if (std::find(U.begin(), U.end(), 0) != U.end()) order.push_back(0);
    std::map<int, size_t> um;
    for (size_t k = 0; k < order.size(); k++) um.emplace(order[k], S[k]);
    static const std::regex T(R"(^TEX\w* \w+\.(\w+),.*tex(\d+)$)"), COL("color|detail|reflection|env");
    for (const std::string &l : L) {
        std::smatch m;
        if (std::regex_search(l, m, T) && std::stoi(m[2].str()) < 12 && m[1].str() == "xyz") {
            auto ui = um.find(std::stoi(m[2].str()));
            if (ui == um.end()) throw Stop("KeyError: sampler unit");
            if (!std::regex_search(nref[ui->second].shrt, COL)) return std::nullopt;
        }
    }
    return um;
}

std::string second_last(const std::string &full) {   // full.split('/')[-2] if '/' in full else full
    if (full.find('/') == std::string::npos) return full;
    const auto v = split_str(full, "/");
    return v[v.size() - 2];
}

// the program text of a material's first pass node; throws when something on the way is missing
std::optional<std::pair<std::vector<std::string>, Ref>> program(const Pak &p, const Ref &m, const ORef &sr) {
    // ps3params._walk: the first node of the 8 technique passes
    const ORef pl = bg::res(p, m.first, m.second + 0x10);
    std::optional<Ref> node;
    if (pl)
        for (uint32_t k = 0; k < 8 && !node; k++) {
            const ORef ps = bg::res(p, pl->first, pl->second + 4 * k);
            if (!ps) continue;
            if (p.pu32(ps->first, ps->second) != 0) continue;
            if (const ORef nd = bg::res(p, ps->first, ps->second + 0x10)) node = *nd;
        }
    if (!node) return std::nullopt;
    const uint32_t pid = bg::be16(p, p.pbase(node->first) + node->second);
    const Ref s = bg::need(sr);
    const ORef t = bg::res(p, s.first, s.second + 0x10ull * pid + 12);
    if (!t) return std::nullopt;
    const uint64_t B = p.pbase(t->first) + t->second;
    const uint32_t size = be32(p.d, B), off = be32(p.d, B + 16);
    for (int k = 2; k < 8; k++) be32(p.d, B + 4ull * k);
    const uint64_t a = std::min<uint64_t>(B + off, p.d.size()), e = std::min<uint64_t>(B + off + size, p.d.size());
    return std::make_pair(dis(Bytes(p.d.begin() + std::ptrdiff_t(a), p.d.begin() + std::ptrdiff_t(std::max(a, e)))), *node);
}

void build_table(Ps3Programs &P) {
    P.table.emplace();
    const Pak &p = P.p;
    const auto mts = p.items_of("MATERIAL_TABLE_1");
    if (mts.empty()) return;
    const uint32_t pg = mts[0].page;
    const uint64_t b = mts[0].data_off();
    const uint32_t nmat = p.pu32(pg, b + 0xc);
    const ORef ma = bg::res(p, pg, b + 0x20), sr = bg::res(p, pg, b + 0x1c), th = bg::res(p, pg, b + 0x18);
    std::vector<bg::Material3> mats;
    std::vector<bg::Texture3> texs;
    bg::read_materials(p, mats, texs);
    for (uint32_t i = 0; i < nmat; i++) {
        const Ref m = bg::need(p, bg::need(ma).first, bg::need(ma).second + 4 * i);
        const Ref nmr = bg::need(p, m.first, m.second);
        const std::string nm = p.pstr(nmr.first, nmr.second);
        if (P.table->count(nm)) continue;
        try {
            auto pr = program(p, m, sr);
            if (!pr) continue;
            const Ref node = pr->second;
            const ORef tl = bg::res(p, node.first, node.second + 0x18);
            Refs nref;
            for (uint64_t y = 0; y < 0x200; y += 0x10) {
                const ORef r = tl ? bg::res(p, tl->first, tl->second + y) : std::nullopt;
                const Ref t = bg::need(th);
                if (!(r && r->first == t.first && t.second <= r->second && r->second < t.second + 4ull * texs.size())) break;
                const std::string full = texs[(r->second - t.second) / 4].name;
                Ps3Programs::Ref_ e;
                const uint64_t a = p.pbase(tl->first) + tl->second + y + 4;
                for (int k = 0; k < 3; k++) e.fl[k] = be32(p.d, a + 4 * k);
                e.shrt = second_last(full);
                e.full = full;
                nref.push_back(e);
            }
            (*P.table)[nm] = {pr->first, nref};
        } catch (const std::exception &) {
        }
    }
}

double srgb2lin(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

// numpy's pairwise summation of float64 (8 accumulators per block of <= 128, halves above)
double pairwise(const double *a, size_t n) {
    if (n < 8) {
        double r = 0.;
        for (size_t i = 0; i < n; i++) r += a[i];
        return r;
    }
    if (n <= 128) {
        double r[8];
        for (int k = 0; k < 8; k++) r[k] = a[k];
        size_t i;
        for (i = 8; i < n - (n % 8); i += 8)
            for (int k = 0; k < 8; k++) r[k] += a[i + size_t(k)];
        double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; i++) res += a[i];
        return res;
    }
    size_t n2 = n / 2;
    n2 -= n2 % 8;
    return pairwise(a, n2) + pairwise(a + n2, n - n2);
}

// (h, w) uint8 -> BC1 blocks (linear order), 4-colour mode, the value in G at 6 bits (R / B at 5)
Bytes bc1_grey(const std::vector<uint8_t> &img, uint32_t w, uint32_t h) {
    const uint32_t ph = (h + 3) / 4 * 4, pw = (w + 3) / 4 * 4;
    Bytes out;
    for (uint32_t by = 0; by < ph / 4; by++)
        for (uint32_t bx = 0; bx < pw / 4; bx++) {
            int bl[16];
            for (int i = 0; i < 16; i++) {
                const uint32_t x = std::min(bx * 4 + uint32_t(i & 3), w - 1), y = std::min(by * 4 + uint32_t(i >> 2), h - 1);   // edge padding
                bl[i] = img[size_t(y) * w + x];
            }
            int hi = bl[0], lo = bl[0];
            for (int v : bl) {
                hi = std::max(hi, v);
                lo = std::min(lo, v);
            }
            const int g0 = (hi * 63 + 127) / 255, g1 = (lo * 63 + 127) / 255, r0 = (hi * 31 + 127) / 255, r1 = (lo * 31 + 127) / 255;
            const int c0 = (r0 << 11) | (g0 << 5) | r0;
            int c1 = (r1 << 11) | (g1 << 5) | r1;
            const bool flat = g0 == g1;
            if (flat) c1 = c0;
            const int e0 = (g0 << 2) | (g0 >> 4), e1 = (g1 << 2) | (g1 >> 4);
            const int pal[4] = {e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3};
            uint32_t bits = 0;
            for (int i = 0; i < 16; i++) {
                int best = 0;
                for (int j = 1; j < 4; j++)
                    if (std::abs(bl[i] - pal[j]) < std::abs(bl[i] - pal[best])) best = j;
                if (!flat) bits |= uint32_t(best) << (2 * i);
            }
            out.push_back(uint8_t(c0));
            out.push_back(uint8_t(c0 >> 8));
            out.push_back(uint8_t(c1));
            out.push_back(uint8_t(c1 >> 8));
            for (int k = 0; k < 4; k++) out.push_back(uint8_t(bits >> (8 * k)));
        }
    return out;
}

}  // namespace

std::optional<std::string> material_polarity(Ps3Programs &P, const std::string &name) {
    if (!P.polarity) {
        P.polarity.emplace();
        const Pak &p = P.p;
        const auto mts = p.items_of("MATERIAL_TABLE_1");
        if (!mts.empty()) {
            const uint32_t pg = mts[0].page;
            const uint64_t b = mts[0].data_off();
            const uint32_t nmat = p.pu32(pg, b + 0xc);
            const ORef ma = bg::res(p, pg, b + 0x20), sr = bg::res(p, pg, b + 0x1c);
            for (uint32_t i = 0; i < nmat; i++) {
                const Ref m = bg::need(p, bg::need(ma).first, bg::need(ma).second + 4 * i);
                const Ref nmr = bg::need(p, m.first, m.second);
                const std::string nm = p.pstr(nmr.first, nmr.second);
                try {
                    auto pr = program(p, m, sr);
                    if (!pr) continue;
                    // 'inv' = the program computes (1 - map) * K: MAD x, -t.y, K, K with t a texture read into .y
                    std::set<std::string> tex;
                    std::string pol = "non";
                    for (const std::string &l : pr->first) {
                        const Op o = parse_op(l);
                        if (!o.ok) continue;
                        if (starts(o.op, "TEX") && !o.d.empty() && ends(o.d, ".y")) tex.insert(base(o.d));
                        if (starts(o.op, "MAD") && o.a.size() >= 3 && starts(o.a[0], "-") && o.a[1] == o.a[2] && tex.count(base(o.a[0])) &&
                            o.a[0].find('.') != std::string::npos && starts(dot1(lstrip(o.a[0], "-")), "y")) {
                            pol = "inv";
                            break;
                        }
                    }
                    (*P.polarity)[nm] = pol;
                } catch (const std::exception &) {
                }
            }
        }
    }
    auto it = P.polarity->find(name);
    return it == P.polarity->end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::optional<BlendSource> blend_source(Ps3Programs &P, const std::string &name) {
    if (!P.table) build_table(P);
    auto e = P.table->find(name);
    if (e == P.table->end()) return std::nullopt;
    const auto &[L, nref] = e->second;
    const auto ch = chain(L);
    if (!ch) {
        std::set<int> texu;
        for (const std::string &l : L)
            if (auto u = tex_unit(l); u && *u < 12) texu.insert(*u);
        if (vc_only(L) && texu.size() >= 2) {
            BlendSource b;
            b.vc_only = true;
            return b;
        }
        return std::nullopt;
    }
    const auto um = unit_map(nref, L);
    if (!um || !um->count(ch->unit)) return std::nullopt;
    const auto &r = nref[um->at(ch->unit)];
    BlendSource b;
    b.full = r.full;
    b.chan = ch->chan;
    b.pol = ch->pol;
    b.gamma = ((r.fl[2] >> 20) >> xyzw(ch->chan.empty() ? '\0' : ch->chan[0])) & 1;
    if (ch->chan.size() != 1) throw Stop("ValueError: substring not found");
    return b;
}

std::optional<std::tuple<Bytes, uint32_t, uint32_t, uint32_t>> derive_texels(const Pak &p, const PakTexture &t, const std::string &chan, bool gamma, bool invert) {
    const auto f = tc::ps3_format(t);
    if (!f) throw Stop("TypeError: cannot unpack non-iterable NoneType object");
    if (!f->compressed) return std::nullopt;
    const uint32_t w = t.width, h = t.height;
    Bytes raw = p.vram_bytes(t.vramOffset, t.vramSize);
    raw.resize(std::min<size_t>(raw.size(), tc::ps3_linear_size(w, h, 1, f->bpb)));
    tc::Img img = tc::decode(raw, w, h, f->kind);
    const uint32_t rm = t.gcmRemapCtl & 0xffff;
    if (rm != 0xaae4)
        for (size_t i = 0; i < size_t(w) * h; i++) {
            uint8_t *px = &img.px[i * 4];
            const uint8_t argb[4] = {px[3], px[0], px[1], px[2]};
            tc::sky::gcm_remap(argb, rm, px);
        }
    const int ci = xyzw(chan.empty() ? '\0' : chan[0]);
    std::vector<double> lv(size_t(w) * h);
    for (size_t i = 0; i < lv.size(); i++) {
        double v = double(img.px[i * 4 + size_t(ci)]) / 255.0;
        if (gamma) v = srgb2lin(v);
        if (invert) v = 1.0 - v;
        lv[i] = v;
    }
    Bytes all;
    uint32_t lw = w, lh = h, n = 0;
    while (true) {
        std::vector<uint8_t> q(lv.size());
        for (size_t i = 0; i < lv.size(); i++) q[i] = uint8_t(std::min(std::max(std::nearbyint(lv[i] * 255), 0.0), 255.0));
        const Bytes b = bc1_grey(q, lw, lh);
        all.insert(all.end(), b.begin(), b.end());
        n++;
        if (lh == 1 && lw == 1) break;
        uint32_t nw = lw, nh = lh;
        std::vector<double> d;
        if (lh > 1 && lw > 1) {
            if (lw % 2 || lh % 2) throw Stop("ValueError: operands could not be broadcast together");
            nw = lw / 2;
            nh = lh / 2;
            d.resize(size_t(nw) * nh);
            for (uint32_t y = 0; y < nh; y++)
                for (uint32_t x = 0; x < nw; x++) {
                    auto at = [&](uint32_t xx, uint32_t yy) { return lv[size_t(yy) * lw + xx]; };
                    d[size_t(y) * nw + x] = (((at(2 * x, 2 * y) + at(2 * x, 2 * y + 1)) + at(2 * x + 1, 2 * y)) + at(2 * x + 1, 2 * y + 1)) / 4;
                }
        } else if (lh > 1) {
            if (lh % 2) throw Stop("ValueError: operands could not be broadcast together");
            nh = lh / 2;
            d.resize(size_t(nw) * nh);
            for (uint32_t y = 0; y < nh; y++)
                for (uint32_t x = 0; x < nw; x++) d[size_t(y) * nw + x] = (lv[size_t(2 * y) * lw + x] + lv[size_t(2 * y + 1) * lw + x]) / 2;
        } else {
            if (lw % 2) throw Stop("ValueError: operands could not be broadcast together");
            nw = lw / 2;
            d.resize(size_t(nw) * nh);
            for (uint32_t y = 0; y < nh; y++)
                for (uint32_t x = 0; x < nw; x++) d[size_t(y) * nw + x] = (lv[size_t(y) * lw + 2 * x] + lv[size_t(y) * lw + 2 * x + 1]) / 2;
        }
        lv.swap(d);
        lw = nw;
        lh = nh;
    }
    return std::make_tuple(tc::ps3_to_ps4_texels(all, w, h, n, 8), w, h, n);
}

std::optional<double> mask_scale(Ps3Programs &P, const std::string &name) {
    if (auto it = P.mask.find(name); it != P.mask.end()) return it->second;
    std::optional<double> out;
    try {
        const Pak &p = P.p;
        std::vector<bg::Material3> mm;
        std::vector<bg::Texture3> texs;
        bg::read_materials(p, mm, texs);
        const bg::Material3 *m = nullptr;
        for (const auto &x : mm)
            if (x.name == name) { m = &x; break; }
        std::vector<const bg::Texture3 *> specs;
        static const std::regex SPEC("-spec");
        if (m)
            for (uint32_t i : m->textures) {
                std::string low = texs.at(i).name;
                for (char &c : low) c = char(std::tolower((unsigned char)c));
                if (std::regex_search(low, SPEC)) specs.push_back(&texs.at(i));
            }
        if (!specs.empty()) {
            const std::string full = specs[0]->name, shrt = second_last(full);
            std::string chan = "rgb";
            bool gam = false;
            if (!P.table) build_table(P);
            if (auto e = P.table->find(name); e != P.table->end()) {
                const auto &[L, nref] = e->second;
                const auto um = unit_map(nref, L);
                std::vector<size_t> ri;
                for (size_t k = 0; k < nref.size(); k++)
                    if (nref[k].shrt == shrt) ri.push_back(k);
                if (um && !um->empty() && !ri.empty()) {
                    std::optional<int> u;
                    for (const auto &[k, v] : *um)
                        if (v == ri[0]) u = k;   // {v: k}: the last unit of a reference wins
                    static const std::regex T(R"(^TEX\w* (\w+)\.(\w+),.*tex(\d+)$)");
                    std::vector<std::string> ch;
                    for (const std::string &l : L) {
                        std::smatch mt;
                        if (std::regex_search(l, mt, T) && u && std::stoi(mt[3].str()) == *u) ch.push_back(mt[2].str());
                    }
                    if (!ch.empty() && ch[0].size() == 1) {
                        chan = ch[0];
                        gam = ((nref[ri[0]].fl[2] >> 20) >> xyzw(chan[0])) & 1;
                    }
                }
            }
            const PakTexture *t = nullptr;
            const std::vector<PakTexture> ts = p.textures();
            for (const PakTexture &x : ts)
                if (x.path.find("/" + shrt + "/") != std::string::npos) { t = &x; break; }
            if (t) {
                const auto f = tc::ps3_format(*t);
                if (!f) throw Stop("TypeError: cannot unpack non-iterable NoneType object");
                if (f->compressed) {
                    const uint32_t w = t->width, h = t->height;
                    Bytes raw = p.vram_bytes(t->vramOffset, t->vramSize);
                    raw.resize(std::min<size_t>(raw.size(), tc::ps3_linear_size(w, h, 1, f->bpb)));
                    const tc::Img img = tc::decode(raw, w, h, f->kind);
                    std::vector<double> v(size_t(w) * h);
                    for (size_t i = 0; i < v.size(); i++) {
                        const uint8_t *px = &img.px[i * 4];
                        double x = chan != "rgb" ? double(px[xyzw(chan[0])]) : ((double(px[0]) + double(px[1])) + double(px[2])) / 3.0;
                        x = x / 255.0;
                        v[i] = gam ? srgb2lin(x) : x;
                    }
                    out = v.empty() ? std::nan("") : pairwise(v.data(), v.size()) / double(v.size());
                }
            }
        }
    } catch (const std::exception &) {
        out.reset();
    }
    P.mask[name] = out;
    return out;
}

}  // namespace u3::fp
