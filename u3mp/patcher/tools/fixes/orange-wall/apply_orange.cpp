// Recipe step orange-wall: hide the duplicate control tower that air-multi-strip.pak draws beside the real one on
// Airstrip.   u3fix orange-wall --project P --plan orange-plan.json [--apply] [--restore]
//
// The plan (orange-plan.json: pak, md5, bytes, md5_after, tris = [offset, a, b, c]) lists index-buffer triangles of
// overlay-r3 pak23/ps4/<pak>; each becomes a, a, a (collapsed, so it draws nothing); the file size does not change.
// The pak must be the one the plan was made from (md5 and size) and every listed triangle must still read a, b, c.
// Already applied: the md5 the plan records after the step, or (added in C++: a recipe update reruns the step on the
// finished import, where later steps have rebuilt the pak) a backup in work/backup-orange/ that is the plan's input,
// which only this step makes. Dry run without --apply; --apply backs the pak up
// once to work/backup-orange/ (an existing backup is never replaced). --restore [--apply] copies the backup back.
// Replaces apply_orange.py (same messages).
#include "u3fix.hpp"

#include <cstdio>

namespace u3 {

int orange_wall(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--restore"}, {"--project", "--plan"});
    const std::string root = a.need("--project") + "/work/";
    const json::Value plan = read_json(upath(a.need("--plan")));
    const json::Value *pak = plan.get("pak");
    if (!pak || pak->kind != json::Value::String) throw Stop("orange-plan.json: no pak");
    const std::string target = root + "overlay-r3/u3data/build/main/pak23/ps4/" + pak->s, backup = root + "backup-orange/" + pak->s;
    const bool apply = a.has("--apply");
    if (a.has("--restore")) {
        std::printf("restore %s -> %s\n", backup.c_str(), target.c_str());
        if (!fs::exists(upath(backup))) throw Stop("no backup");
        if (apply) copy2(upath(backup), upath(target));
        else std::printf("dry run: nothing written (use --apply)\n");
        return 0;
    }
    Bytes data = read_file(upath(target));
    const std::string md5 = md5_hex(data);
    const json::Value *after = plan.get("md5_after");
    if (after && after->kind == json::Value::String && md5 == after->s) {
        std::printf("already applied: the duplicate tower is collapsed\n");
        return 0;
    }
    const json::Value *tris = plan.get("tris"), *want = plan.get("md5"), *bytes = plan.get("bytes");
    if (!tris || tris->kind != json::Value::Array || !want || !bytes) throw Stop("orange-plan.json: no md5, bytes or tris");
    if (md5 != want->s || int64_t(data.size()) != bytes->i) {
        if (fs::exists(upath(backup)) && md5_hex(read_file(upath(backup))) == want->s) {
            std::printf("already applied: the backup is the plan's input, so this import collapsed the tower (later steps changed the pak since)\n");
            return 0;
        }
        throw Stop("pak changed since the plan was built: " + target);
    }
    int n = 0;
    for (const json::Value &t : tris->items) {
        const uint64_t off = uint64_t(t.items.at(0).i);
        const int64_t x = t.items.at(1).i, y = t.items.at(2).i, z = t.items.at(3).i;
        if (rd16le(data, off) != x || rd16le(data, off + 2) != y || rd16le(data, off + 4) != z) throw Stop("index mismatch at " + pyhex(off));
        data[off + 2] = data[off + 4] = data[off];
        data[off + 3] = data[off + 5] = data[off + 1];
        n++;
    }
    std::printf("collapse %d triangles in %s\n", n, pak->s.c_str());
    if (!apply) {
        std::printf("dry run: nothing written (use --apply)\n");
        return 0;
    }
    fs::create_directories(upath(backup).parent_path());
    if (!fs::exists(upath(backup))) copy2(upath(target), upath(backup));   // an earlier pass already saved this input state: keep it
    write_file_replace(upath(target), data);
    std::printf("written; backup at %s\n", backup.c_str());
    return 0;
}

}  // namespace u3
