// Recipe step facility-collapse: the Facility wall fix (2026-10-31). In overlay-r3 pak23/ps4/facility-multi.pak the
// GEOMETRY_1 Prototype360 is a coarse low-poly shell that drew a wall across the walk-through doorway:
//   1. every triangle of a '.Prototype360.' submesh whose extent exceeds 6 m is collapsed (index triple -> first index);
//   2. the remaining box in Prototype360.Submesh1 (x -32.2..-28.4, y 0.2..4.9, z -14.2..-10.25) is collapsed too.
// Replaces facility_collapse.py. Idempotent: collapsed triangles are skipped. Backup: work/backup-wall/facility-multi.pak
// (written once).
//   u3fix facility-collapse --project P [--apply]      (exit 3 when the converted level pak is missing)
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "u3fix.hpp"

#include <cstring>

namespace u3 {

int facility_collapse(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project"});
    const std::string W = a.need("--project") + "/work/", F = W + "overlay-r3/u3data/build/main/pak23/ps4/facility-multi.pak", BK = W + "backup-wall/";
    if (!fs::exists(upath(F))) {
        std::printf("facility-multi.pak not in overlay-r3: nothing to do (needs the converted level pak)\n");
        return 3;
    }
    Bytes d = read_file(upath(F));
    const Pak P(d, "facility-multi.pak");
    const auto gs = P.items_of("GEOMETRY_1");
    if (gs.empty()) throw Stop("IndexError: list index out of range");
    const PakItem &it = gs[0];
    const uint64_t b = P.pbase(it.page) + it.data_off();
    const uint32_t ns = rd32le(&d.at(b + 8));
    const Ref sr = bg::need(P, it.page, it.data_off() + 0x10);
    const uint64_t pb = P.pbase(sr.first);
    auto f32 = [&](uint64_t o) {
        if (o + 4 > d.size()) throw Stop("struct.error: unpack_from out of range");
        float f;
        std::memcpy(&f, &d[o], 4);
        return double(f);
    };
    auto u16 = [&](uint64_t o) {
        if (o + 2 > d.size()) throw Stop("struct.error: unpack_from out of range");
        return uint32_t(d[o] | d[o + 1] << 8);
    };
    int big = 0, box = 0;
    for (uint32_t i = 0; i < ns; i++) {
        const uint64_t o_ = sr.second + 0x88ull * i;
        const ORef nr = bg::res(P, sr.first, o_ + 8);
        const std::string nm = nr ? P.pstr(nr->first, nr->second) : "";
        if (nm.find(".Prototype360.") == std::string::npos) continue;
        const uint32_t nv = rd32le(&d.at(pb + o_ + 0x20)), ni = rd32le(&d.at(pb + o_ + 0x24));
        const Ref st = bg::need(P, sr.first, o_ + 0x30);
        const uint64_t sq = P.pbase(st.first) + st.second;
        const uint32_t stride = u16(sq + 2);
        const Ref da = bg::need(P, st.first, st.second + 16);
        const uint64_t vb = P.pbase(da.first) + da.second;
        const Ref ia = bg::need(P, sr.first, o_ + 0x40);
        const uint64_t ib = P.pbase(ia.first) + ia.second;
        std::vector<std::array<double, 3>> V(nv);
        for (uint32_t k = 0; k < nv; k++)
            for (int j = 0; j < 3; j++) V[k][size_t(j)] = f32(vb + uint64_t(k) * stride + 4ull * uint64_t(j));
        const bool sub1 = nm.size() >= 21 && nm.compare(nm.size() - 21, 21, "Prototype360.Submesh1") == 0;
        for (int64_t k = 0; k < int64_t(ni) - 2; k += 3) {
            const uint32_t x[3] = {u16(ib + uint64_t(k) * 2), u16(ib + uint64_t(k) * 2 + 2), u16(ib + uint64_t(k) * 2 + 4)};
            if (x[0] == x[1] && x[1] == x[2]) continue;
            const std::array<double, 3> *T[3] = {&V.at(x[0]), &V.at(x[1]), &V.at(x[2])};
            double ext = -1e300;
            for (int j = 0; j < 3; j++) {
                double mx = -1e300, mn = 1e300;
                for (const auto *t : T) mx = std::max(mx, (*t)[size_t(j)]), mn = std::min(mn, (*t)[size_t(j)]);
                ext = std::max(ext, mx - mn);
            }
            bool inbox = sub1;
            for (const auto *t : T)
                inbox = inbox && -32.2 <= (*t)[0] && (*t)[0] <= -28.4 && -14.2 <= (*t)[2] && (*t)[2] <= -10.25 && 0.2 <= (*t)[1] && (*t)[1] <= 4.9;
            if (ext > 6 || inbox) {
                for (int q = 1; q < 3; q++) d[size_t(ib + uint64_t(k) * 2 + 2 * uint64_t(q))] = uint8_t(x[0]), d[size_t(ib + uint64_t(k) * 2 + 2 * uint64_t(q) + 1)] = uint8_t(x[0] >> 8);
                (ext > 6 ? big : box)++;
            }
        }
    }
    std::printf("triangles to hide: %d large, %d box\n", big, box);
    if (!a.has("--apply")) {
        std::printf("dry run\n");
        return 0;
    }
    if (big || box) {
        fs::create_directories(upath(BK));
        if (!fs::exists(upath(BK + "facility-multi.pak"))) copy2(upath(F), upath(BK + "facility-multi.pak"));
        write_file_replace(upath(F), d);
        std::printf("written %s\n", F.c_str());
    } else
        std::printf("already applied\n");
    return 0;
}

}  // namespace u3
