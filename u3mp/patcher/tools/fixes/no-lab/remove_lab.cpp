// Recipe step remove-lab (recipe 1.15.0): no Lab maps on PC.        u3fix remove-lab --project P [--actors F] [--apply]
//
// The owner decided that the PS3 Lab maps (the 14 map-list entries whose main level starts with 'lab-') are not part
// of the PC import, whatever PS3 game data the player has (their RPCS3 DATA2 still holds the Lab psarcs). The steps
// that built them are retired (levels-lab, levels-maps-batch) and convert-actors skips the 48 Lab art groups. This
// step removes what older imports (and steps that still touch Lab lines, such as lab-registration and
// registry-final) left in the four overlays:
//   * pak23/ps4/lab-*.pak and lab34-*.pak (the Lab level paks: 70 in a 1.14 import),
//   * actor33/ps4/<lab_actors.txt>.pak (the 48 box / stadium art groups only Lab levels use),
//   * vtex1/ps4/net-levels/lab-*.dds (the Lab map pictures),
//   * every pak23_ps4.txt block whose level-name starts with 'lab' (from that line to the next level-name line).
// Kept on purpose: render-settings/game/lab.bin (a DC module modules.bin lists), the Lab playlist picture and the
// 'labicon' emblem (not maps), Lab modes on the regular maps. The net.bin side (Lab maps not listed, Lab netgames out
// of the playlists, PS3 netgame indices kept for crossplay) is step netgame-order-119, which runs after this one.
// Dry run by default; --apply deletes and rewrites (pak23_ps4.txt backed up once to work/backup-no-lab/).
// Idempotent: prints 'already applied' and exits 0 when nothing Lab is left.
#include "u3fix.hpp"

#include <algorithm>
#include <cstdio>

namespace u3 {
namespace {

const char *const OVERLAYS[] = {"overlay-r3", "overlay-r2", "overlay-port", "overlay-mp"};

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// lab-*, lab34-*, lab<digit>... (case-insensitive 'lab', then '-' or a digit)
bool lab_level(const std::string &n) {
    return n.size() >= 4 && lower(n[0]) == 'l' && lower(n[1]) == 'a' && lower(n[2]) == 'b' &&
           (n[3] == '-' || (n[3] >= '0' && n[3] <= '9'));
}

bool ends_with(const std::string &s, const std::string &e) {
    return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

// whitespace of a latin-1 text line, as Python's str.split()
bool space(unsigned char c) {
    return c == ' ' || (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f) || c == 0x85 || c == 0xa0;
}

std::vector<std::string> words(const std::string &ln, size_t max) {
    std::vector<std::string> w;
    size_t i = 0;
    while (i < ln.size() && w.size() < max) {
        while (i < ln.size() && space((unsigned char)ln[i])) i++;
        size_t j = i;
        while (j < ln.size() && !space((unsigned char)ln[j])) j++;
        if (j > i) w.push_back(ln.substr(i, j - i));
        i = j;
    }
    return w;
}

std::set<std::string> lab_actors(const fs::path &list) {
    Bytes b = read_file(list);
    std::string text(b.begin(), b.end()), ln;
    std::set<std::string> r;
    auto add = [&](std::string s) {
        if (!s.empty() && s[0] == '#') return;
        size_t a = 0, z = s.size();
        while (a < z && space((unsigned char)s[a])) a++;
        while (z > a && space((unsigned char)s[z - 1])) z--;
        if (z > a) r.insert(s.substr(a, z - a));
    };
    for (char c : text) {
        if (c == '\n') { add(ln); ln.clear(); }
        else ln += c;
    }
    add(ln);
    return r;
}

std::vector<std::string> names_in(const fs::path &dir) {
    std::vector<std::string> n;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return n;
    for (const auto &e : fs::directory_iterator(dir)) n.push_back(utf8(e.path().filename()));
    std::sort(n.begin(), n.end());
    return n;
}

struct Registry {
    std::string overlay;
    fs::path path;
    std::string text;
    std::vector<std::string> removed;
};

// text without the blocks of Lab levels (from a Lab level-name line to the next level-name line)
std::string strip_registry(const std::string &text, std::vector<std::string> &removed) {
    std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string out;
    bool drop = false, first = true;
    size_t pos = 0;
    while (true) {
        size_t e = text.find(nl, pos);
        std::string ln = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        auto w = words(ln, 2);
        if (!w.empty() && w[0] == "level-name") {
            drop = w.size() > 1 && lab_level(w[1]);
            if (drop) removed.push_back(w[1]);
        }
        if (!drop) {
            if (!first) out += nl;
            out += ln;
            first = false;
        }
        if (e == std::string::npos) break;
        pos = e + nl.size();
    }
    return out;
}

}  // namespace

int remove_lab(const std::vector<std::string> &argv) {
    Args a(argv, {"--apply"}, {"--project", "--actors"});
    std::string project = a.need("--project");
    fs::path work = upath(project + "/work/");
    fs::path bk = upath(project + "/work/backup-no-lab/");
    std::string actors_list = a.get("--actors", "lab_actors.txt");
    std::set<std::string> actors = lab_actors(upath(actors_list));

    std::vector<std::pair<fs::path, std::string>> files;   // (path, path relative to work/)
    std::vector<Registry> registries;
    for (const char *ov : OVERLAYS) {
        std::string main_rel = std::string(ov) + "/u3data/build/main/";
        for (const auto &n : names_in(work / upath(main_rel + "pak23/ps4/")))
            if (ends_with(n, ".pak") && lab_level(n)) files.push_back({work / upath(main_rel + "pak23/ps4/" + n), main_rel + "pak23/ps4/" + n});
        for (const auto &n : names_in(work / upath(main_rel + "actor33/ps4/")))
            if (ends_with(n, ".pak") && actors.count(n.substr(0, n.size() - 4)))
                files.push_back({work / upath(main_rel + "actor33/ps4/" + n), main_rel + "actor33/ps4/" + n});
        for (const auto &n : names_in(work / upath(main_rel + "vtex1/ps4/net-levels/"))) {
            std::string l = n;
            std::transform(l.begin(), l.end(), l.begin(), lower);
            if (ends_with(n, ".dds") && l.rfind("lab-", 0) == 0)
                files.push_back({work / upath(main_rel + "vtex1/ps4/net-levels/" + n), main_rel + "vtex1/ps4/net-levels/" + n});
        }
        fs::path reg = work / upath(main_rel + "pak23_ps4.txt");
        std::error_code ec;
        if (fs::is_regular_file(reg, ec)) {
            Bytes b = read_file(reg);
            Registry r{ov, reg, {}, {}};
            r.text = strip_registry(std::string(b.begin(), b.end()), r.removed);
            if (!r.removed.empty()) registries.push_back(std::move(r));
        }
    }
    if (files.empty() && registries.empty()) {
        std::printf("already applied: no Lab level, actor, picture or registry block in the overlays\n");
        return 0;
    }
    for (const auto &f : files) std::printf("remove %s\n", f.second.c_str());
    for (const auto &r : registries) {
        std::string list;
        for (size_t i = 0; i < r.removed.size(); i++) list += (i ? ", " : "") + r.removed[i];
        std::printf("%s pak23_ps4.txt: %d Lab level blocks removed (%s)\n", r.overlay.c_str(), int(r.removed.size()), list.c_str());
    }
    if (!a.has("--apply")) {
        std::printf("dry run: %d files, %d registries; nothing changed\n", int(files.size()), int(registries.size()));
        return 0;
    }
    fs::create_directories(bk);
    for (const auto &r : registries) {
        copy_once(r.path, bk / upath(r.overlay + "-pak23_ps4.txt"));
        write_file_replace(r.path, Bytes(r.text.begin(), r.text.end()));
    }
    for (const auto &f : files) fs::remove(f.first);
    std::printf("applied: %d Lab files removed, %d registries cleaned\n", int(files.size()), int(registries.size()));
    return 0;
}

}  // namespace u3
