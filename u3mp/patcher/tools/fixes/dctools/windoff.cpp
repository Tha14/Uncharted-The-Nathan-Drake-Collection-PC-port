// dctools/windoff.cpp: zero, in place, the g_pivotWind* strength constants (g_pivotWindStrength,
// g_pivotWindSubobjBendStrength, g_pivotWindSubobjVarStrength, g_pivotWindPlayerBendIntensity) of the GEOMETRY_1
// materials of a retail PS4 level pak whose name contains one of the given substrings (PS3 1.19 has no g_pivotWind* on
// these materials); speed, radius and position stay. The latest converter's lib/windoff.py in C++.
// GEOMETRY_1: u64 material count at data +0x48, pointer to the 0x48-byte material records at +0x50; a record: name
// pointer +0, parameter pointer +0x20, u32 parameter count +0x34; a parameter (0x18 bytes): name pointer +0, value
// pointer at +8 or +0x10 (the first one that is a pointer) to a float.
#include "windoff.hpp"

#include "ndpak.hpp"
#include "u3fix.hpp"

namespace u3 {

Bytes windoff_convert(const fs::path &src, std::vector<std::string> subs) {
    if (subs.empty()) subs = {"oas-reeds", "grass-shrub"};
    static const std::set<std::string> ZERO = {"g_pivotWindStrength", "g_pivotWindSubobjBendStrength", "g_pivotWindSubobjVarStrength",
                                               "g_pivotWindPlayerBendIntensity"};
    const Pak q(src);
    Bytes d = q.d;
    auto S = [&](uint32_t a, uint64_t o) -> std::string {
        auto r = q.resolve(a, uint32_t(o));
        return r ? q.pstr(r->first, r->second) : "";
    };
    const auto geo = q.items_of("GEOMETRY_1");
    if (geo.empty()) throw Stop("IndexError: no GEOMETRY_1 in " + utf8(src));
    const uint32_t pg = geo[0].page;
    const uint64_t b = geo[0].data_off();
    const uint64_t n = q.u64(q.pbase(pg) + b + 0x48);
    const auto m = q.resolve(pg, uint32_t(b + 0x50));
    if (!m) throw Stop("TypeError: GEOMETRY_1 without materials");
    for (uint64_t i = 0; i < n; i++) {
        const uint64_t o = m->second + 0x48 * i;
        const std::string name = S(m->first, o);
        bool hit = false;
        for (const std::string &s : subs) hit = hit || name.find(s) != std::string::npos;
        if (!hit) continue;
        const uint32_t npar = rd32le(d, q.pbase(m->first) + o + 0x34);
        const auto pr = q.resolve(m->first, uint32_t(o + 0x20));
        for (uint32_t k = 0; pr && k < npar; k++) {
            const uint64_t eo = pr->second + 0x18ull * k;
            if (!ZERO.count(S(pr->first, eo))) continue;
            for (uint64_t slot : {8ull, 0x10ull}) {
                if (!q.is_ptr(pr->first, uint32_t(eo + slot))) continue;
                const auto vp = q.resolve(pr->first, uint32_t(eo + slot));
                const uint64_t at = q.pbase(vp->first) + vp->second;
                if (at + 4 > d.size()) throw Stop("struct.error: wind constant past the end");
                wr32le(&d[at], 0);   // 0.0f
                break;
            }
        }
    }
    return d;
}

}  // namespace u3
