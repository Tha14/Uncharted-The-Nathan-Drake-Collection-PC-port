// Recipe step emblem-cap3: the per-instance cbuffer capacity of the seven Airstrip emblem paks.
//
//   u3fix emblem-cap3 --project P [--apply]
//
// Each of the seven air-multi-emblem-<name>.pak in work/overlay-r3/u3data/build/main/actor33/ps4 holds, at a fixed
// offset, the capacity (u32, little-endian) of its per-instance cbuffer: 2 on the old emblem-test overlay, 8 in a
// fresh wall-emblem build (mkemblem.py). The game needs 3 there (crashcheck G6: one feature set + 2), so 2 and 8 become
// 3. A pak that is not there yet is reported and skipped (the wall-emblem step makes it); a pak that already has 3 is
// left alone; any other value stops the step with nothing written. Dry run without --apply. Replaces set_cap3.py: the
// same files, the same lines printed.
#include "u3fix.hpp"

#include <cstdio>

namespace u3 {
namespace {

struct EmblemPak {
    const char *name;
    uint32_t offset;   // of the capacity
};
const EmblemPak EMBLEM_PAKS[] = {{"diamond", 0x6d220}, {"corr", 0x6cd80},    {"corr-blue", 0x6c740}, {"tower", 0x6c740},
                                 {"stucco", 0x6c7d0},  {"board-0", 0x6c780}, {"board-1", 0x6c780}};

}  // namespace

int emblem_cap3(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project"});
    const fs::path dir = upath(a.need("--project")) / "work" / "overlay-r3" / "u3data" / "build" / "main" / "actor33" / "ps4";
    struct Change {
        fs::path path;
        Bytes data;
    };
    std::vector<Change> changes;
    std::string report;   // printed once every pak checked out, so a problem writes nothing
    for (const EmblemPak &p : EMBLEM_PAKS) {
        const fs::path f = dir / ("air-multi-emblem-" + std::string(p.name) + ".pak");
        if (!fs::exists(f)) {
            report += std::string(p.name) + " missing (needs the wall-emblem step first)\n";
            continue;
        }
        Bytes d = read_file(f);
        if (d.size() < size_t(p.offset) + 4) throw Stop(std::string(p.name) + ": the pak is too short for the capacity value");
        uint8_t *v = d.data() + p.offset;
        const uint32_t cap = uint32_t(v[0]) | uint32_t(v[1]) << 8 | uint32_t(v[2]) << 16 | uint32_t(v[3]) << 24;
        if (cap == 3) {
            report += std::string(p.name) + " already 3\n";
            continue;
        }
        if (cap != 2 && cap != 8) throw Stop(std::string(p.name) + ": capacity " + std::to_string(cap) + ", expected 2 or 8 (nothing written)");
        v[0] = 3;
        v[1] = v[2] = v[3] = 0;
        report += std::string(p.name) + " " + std::to_string(cap) + " -> 3\n";
        changes.push_back({f, std::move(d)});
    }
    std::fputs(report.c_str(), stdout);
    if (!a.has("--apply")) {
        if (!changes.empty()) std::printf("dry run: %zu paks would change\n", changes.size());
        return 0;
    }
    for (const Change &c : changes) write_file_replace(c.path, c.data);
    return 0;
}

}  // namespace u3
