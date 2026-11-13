// Recipe step coop-desc-fix: byte order of the Co-op Adventure hero descs in overlay-r3 net.bin.
//   u3fix coop-desc --project P [--apply]
//
// The netgame table (DC symbol 0x3b9a067d: count, then a pointer to 0xd0-byte entries) has, for each co-op adventure
// netgame (int at +0xc = 11, pointer at +0x70), three hero desc pointers. The PS4 conversion left each desc's four
// bytes at +0 reversed (00 00 00 TT), so the game copies a hunter look; the fix makes them TT 00 00 00. A desc whose
// bytes do not have that form is reported as 'skip' and left alone (so a second run fixes nothing). Dry run without
// --apply; --apply backs net.bin up once to net.bin.bak-coopdesc next to it. Replaces apply_coop_desc_fix.py.
#include "u3fix.hpp"

#include <algorithm>
#include <cstdio>

namespace u3 {

int coop_desc(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project"});
    const std::string f = a.need("--project") + "/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin";
    const Dc c(upath(f));
    Bytes d = c.d;
    auto it = c.bysid.find(0x3b9a067d);
    if (it == c.bysid.end()) throw Stop("KeyError: 0x3b9a067d (netgame table) not in net.bin");
    const uint64_t o = it->second.second, e = c.ptr(o + 8);
    const uint32_t n = c.u32(o);
    std::set<uint64_t> seen;
    struct Fixed {
        uint32_t i;
        int k;
        uint64_t q;
        uint8_t type;
    };
    std::vector<Fixed> fix;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t a0 = e + uint64_t(i) * 0xd0;
        if (int32_t(c.u32(a0 + 0xc)) != 11 || !c.isptr(a0 + 0x70)) continue;
        const uint64_t t = c.ptr(a0 + 0x70);
        for (int k = 0; k < 3; k++) {
            const uint64_t q = c.ptr(t + uint64_t(k) * 8);
            if (seen.count(q)) continue;
            seen.insert(q);
            const uint64_t have = q < d.size() ? std::min<uint64_t>(4, d.size() - q) : 0;
            if (have < 4) {
                if (have == 0) throw Stop(fmt("IndexError: desc %s of netgame %u is past the end of net.bin", pyhex(q).c_str(), i));
                std::string hx;
                for (uint64_t j = 0; j < have; j++) hx += fmt("%02x", d[q + j]);
                if (have < 4 && d[q] == 0 && (have < 2 || d[q + 1] == 0) && (have < 3 || d[q + 2] == 0))
                    throw Stop(fmt("IndexError: desc %s of netgame %u is cut off by the end of net.bin", pyhex(q).c_str(), i));
                std::printf("skip %u %d %s %s\n", i, k, pyhex(q).c_str(), hx.c_str());
                continue;
            }
            const uint8_t *b = &d[q];
            if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] > 0 && b[3] < 0x40) {
                fix.push_back({i, k, q, b[3]});
                d[q] = b[3];
                d[q + 3] = 0;
            } else {
                std::printf("skip %u %d %s %02x%02x%02x%02x\n", i, k, pyhex(q).c_str(), b[0], b[1], b[2], b[3]);
            }
        }
    }
    for (const Fixed &x : fix) std::printf("netgame %u slot %d desc %s type %u\n", x.i, x.k, pyhex(x.q).c_str(), x.type);
    std::printf("%zu descs to fix\n", fix.size());
    if (a.has("--apply") && !fix.empty()) {
        const fs::path bak = upath(f + ".bak-coopdesc");
        if (!fs::exists(bak)) copy2(upath(f), bak);
        write_file_replace(upath(f), d);
        std::printf("written %s\n", f.c_str());
    } else {
        std::printf("dry run, nothing written\n");
    }
    return 0;
}

}  // namespace u3
