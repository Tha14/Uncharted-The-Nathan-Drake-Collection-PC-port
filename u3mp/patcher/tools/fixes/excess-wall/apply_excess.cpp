// Recipe step excess-wall: hide excess companion-level triangles (the recipe runs it for the Airstrip wall only).
//   u3fix excess-wall --project P --job D [--only-wall] [--add ID ...] [--apply] [--restore]
//
// D is the job folder with air-wall.json (the Airstrip wall: pak, md5, bytes, tris) and plan.json (per pak: md5, bytes,
// clusters with a tier and tris). Without --only-wall the clusters of tier 'default' (and those named with --add) are
// taken too. Each triangle [offset, a, b, c] becomes a, a, a; the file size does not change. Each pak must have the md5
// and size of its plan and every triangle must read a, b, c. Added in C++, because a recipe update reruns the step on the
// finished import: a pak whose md5 differs counts as done when its listed triangles are all collapsed, or when its
// backup in work/backup-excess/ (which only this step makes) is the plan's input.
// Dry run without --apply; --apply backs each pak up once to work/backup-excess/ (never replaced). --restore [--apply]
// copies every backup back. Replaces apply_excess.py (same messages; paths printed as it printed them).
#include "u3fix.hpp"

#include <algorithm>
#include <cstdio>

namespace u3 {
namespace {

struct Tri {
    uint64_t off;
    int64_t a, b, c;
};
// a pak's triangles by offset, in first-seen order (a later entry for the same offset replaces the values)
struct PakWork {
    std::string md5;
    int64_t bytes = 0;
    std::vector<Tri> tris;
    std::map<uint64_t, size_t> at;
    void add(const json::Value &t) {
        const Tri x{uint64_t(t.items.at(0).i), t.items.at(1).i, t.items.at(2).i, t.items.at(3).i};
        auto it = at.find(x.off);
        if (it != at.end()) tris[it->second] = x;
        else {
            at[x.off] = tris.size();
            tris.push_back(x);
        }
    }
};

const std::string &text(const json::Value *v, const char *what) {
    if (!v || v->kind != json::Value::String) throw Stop(std::string("plan: no ") + what);
    return v->s;
}

}  // namespace

int excess_wall(const std::vector<std::string> &argv) {
    std::vector<std::string> rest, add;
    for (size_t i = 0; i < argv.size(); i++) {   // --add takes every following argument up to the next --option
        if (argv[i] == "--add") {
            while (i + 1 < argv.size() && argv[i + 1].rfind("--", 0) != 0) add.push_back(argv[++i]);
            continue;
        }
        rest.push_back(argv[i]);
    }
    const Args a(rest, {"--apply", "--restore", "--only-wall"}, {"--project", "--job"});
    const bool apply = a.has("--apply");
    const std::string project = a.need("--project"), job = a.need("--job");
    const std::string root = win_path(project) + "\\work/", pk = root + "overlay-r3/u3data/build/main/pak23/ps4/", bk = root + "backup-excess/";
    if (a.has("--restore")) {
        if (fs::is_directory(upath(bk))) {
            std::vector<std::string> names;
            for (const auto &e : fs::directory_iterator(upath(bk))) names.push_back(utf8(e.path().filename()));
            std::sort(names.begin(), names.end());
            for (const std::string &f : names) {
                std::printf("restore %s\n", f.c_str());
                if (apply) copy2(upath(bk + f), upath(pk + f));
            }
        }
        if (!apply) std::printf("dry run: nothing written (use --apply)\n");
        return 0;
    }
    const json::Value wall = read_json(upath(job + "/air-wall.json"));
    std::vector<std::pair<std::string, PakWork>> work;   // insertion order; sorted by pak name before use
    auto entry = [&](const std::string &pak, const std::string &md5, int64_t bytes) -> PakWork & {
        for (auto &w : work)
            if (w.first == pak) return w.second;   // dict.setdefault: the first plan of a pak stays
        work.push_back({pak, PakWork{}});
        work.back().second.md5 = md5;
        work.back().second.bytes = bytes;
        return work.back().second;
    };
    if (!a.has("--only-wall") && fs::exists(upath(job + "/plan.json"))) {
        const json::Value plan = read_json(upath(job + "/plan.json"));
        for (const auto &[pak, e] : plan.fields) {
            const json::Value *clusters = e.get("clusters");
            if (!clusters) throw Stop("plan.json: " + pak + " has no clusters");
            for (const auto &[cid, c] : clusters->fields) {
                const bool take = text(c.get("tier"), "tier") == "default" || std::find(add.begin(), add.end(), cid) != add.end();
                if (!take) continue;
                PakWork &w = entry(pak, text(e.get("md5"), "md5"), e.get("bytes") ? e.get("bytes")->i : -1);
                for (const json::Value &t : c.get("tris")->items) w.add(t);
            }
        }
    }
    const std::string &wpak = text(wall.get("pak"), "pak"), &wmd5 = text(wall.get("md5"), "md5");
    PakWork &w = entry(wpak, wmd5, wall.get("bytes") ? wall.get("bytes")->i : -1);
    if (w.md5 != wmd5) throw Stop("plan and wall list disagree on " + wpak);
    for (const json::Value &t : wall.get("tris")->items) w.add(t);
    std::sort(work.begin(), work.end(), [](const auto &x, const auto &y) { return x.first < y.first; });

    std::vector<std::pair<std::string, Bytes>> todo;
    for (auto &[pak, e] : work) {
        Bytes d = read_file(upath(pk + pak));
        if (md5_hex(d) != e.md5 || int64_t(d.size()) != e.bytes) {
            bool collapsed = !e.tris.empty();
            for (const Tri &t : e.tris)
                collapsed = collapsed && t.off + 6 <= d.size() && rd16le(d, t.off) == t.a && rd16le(d, t.off + 2) == t.a && rd16le(d, t.off + 4) == t.a;
            if (collapsed) {
                std::printf("already applied in %s: every listed triangle is collapsed\n", pak.c_str());
                continue;
            }
            if (fs::exists(upath(bk + pak)) && md5_hex(read_file(upath(bk + pak))) == e.md5) {
                std::printf("already applied in %s: the backup is the plan's input (later steps changed the pak since)\n", pak.c_str());
                continue;
            }
            throw Stop("pak changed since the plan was built: " + pak);
        }
        for (const Tri &t : e.tris) {
            if (rd16le(d, t.off) != t.a || rd16le(d, t.off + 2) != t.b || rd16le(d, t.off + 4) != t.c)
                throw Stop("index mismatch in " + pak + " at " + pyhex(t.off));
            d[t.off + 2] = d[t.off + 4] = d[t.off];
            d[t.off + 3] = d[t.off + 5] = d[t.off + 1];
        }
        std::printf("collapse %zu triangles in %s\n", e.tris.size(), pak.c_str());
        todo.push_back({pak, std::move(d)});
    }
    if (!apply) {
        std::printf("dry run: nothing written (use --apply)\n");
        return 0;
    }
    if (!todo.empty()) fs::create_directories(upath(bk));
    for (const auto &[pak, d] : todo) {
        if (!fs::exists(upath(bk + pak))) copy2(upath(pk + pak), upath(bk + pak));
        write_file_replace(upath(pk + pak), d);
        std::printf("written %s backup %s\n", pak.c_str(), (bk + pak).c_str());
    }
    return 0;
}

}  // namespace u3
