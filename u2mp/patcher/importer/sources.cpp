// importer/sources.cpp: where each file of the Uncharted 2 import comes from.
//
// PS3: the player's GOTY copy (build/main: the base archives, the speech archives, pak22.txt; USRDIR/all-patches.psarc)
// and an optional DLC folder (dlc-map1/2, dlc-patch2/3/5/7 as archives or unpacked folders, speech update archives).
// Precedence as the PS3 game mounts them, newest first: dlc-patch7 > 5 > 3 > 2 > patch > dlc-map2 > dlc-map1 > base.
// A member's key is its path under build/main without the dlc folder; a member without a folder gets the archive's
// folder ('actor27.psarc' -> 'actor27/'). Keys compare lower-case; the member's own spelling names the output.
// PS4: the Collection's u2data/build/main: which files it has (by key), its speech names, registries, music settings.
#include "u2import.hpp"

#include <algorithm>

namespace u2imp {
namespace {

int rank(const std::string &source) {
    static const std::map<std::string, int> R = {{"dlc-patch7", 9}, {"dlc-patch5", 8}, {"dlc-patch3", 7}, {"dlc-patch2", 6}, {"patch", 5},
                                                 {"dlc-map2", 4},   {"dlc-map1", 3},   {"base", 1}};
    const auto it = R.find(source);
    return it == R.end() ? 0 : it->second;
}

// the archive's member name as a key
std::string norm_member(const std::string &name, const std::string &prefix) {
    std::string n = name;
    std::replace(n.begin(), n.end(), '\\', '/');
    n.erase(0, n.find_first_not_of('/') == std::string::npos ? n.size() : n.find_first_not_of('/'));
    if (starts(n, "build/main/")) n = n.substr(11);
    // 'dlc-map<d>/' or 'dlc-patch<d>/' in front
    for (const char *t : {"dlc-map", "dlc-patch"}) {
        const size_t l = std::char_traits<char>::length(t);
        if (n.size() > l + 1 && n.compare(0, l, t) == 0 && isdigit(uint8_t(n[l])) && n[l + 1] == '/') {
            n = n.substr(l + 2);
            break;
        }
    }
    if (n.find('/') == std::string::npos && !prefix.empty()) n = prefix + n;
    return n;
}

bool isdir(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool isfile(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
std::vector<std::string> listdir_sorted(const fs::path &d) {
    std::vector<std::string> v;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(d, ec)) v.push_back(S(e.path().filename()));
    std::sort(v.begin(), v.end());
    return v;
}

}  // namespace

Bytes Entry::read() const {
    if (!path.empty()) return read_bytes(path);
    return arc->read(index);
}
std::string Entry::where() const { return !path.empty() ? S(path) : arc_path + " :: " + name; }

std::optional<fs::path> build_main(const fs::path &root) {
    for (const fs::path &c : {root, root / "build" / "main", root / "USRDIR" / "build" / "main"}) {
        const fs::path n = c.lexically_normal();
        fs::path last = n.filename(), parent = n.parent_path().filename();
        if (last.empty()) {   // a trailing separator
            last = n.parent_path().filename();
            parent = n.parent_path().parent_path().filename();
        }
        if (isdir(c) && S(last) == "main" && S(parent) == "build") return c;
    }
    return std::nullopt;
}

PS3::PS3(const std::string &base, const std::string &dlc) {
    const auto bm = build_main(P(base));
    if (!bm) throw std::runtime_error("no PS3 Uncharted 2 build/main folder under " + base);
    base_main = *bm;
    const fs::path usrdir = base_main.parent_path().parent_path();
    for (const auto &[rel, prefix] : std::vector<std::pair<std::string, std::string>>{{"actor27.psarc", "actor27/"},
                                                                                      {"pak22.psarc", "pak22/"},
                                                                                      {"dc1.psarc", "dc1/"},
                                                                                      {"text1.psarc", "text1/"},
                                                                                      {"sound1/banks.psarc", "sound1/banks/"},
                                                                                      {"sound1/music.psarc", "sound1/music/"},
                                                                                      {"sound1/streams.psarc", "sound1/streams/"}})
        add_arc(base_main / P(rel), "base", prefix);
    const fs::path sp = base_main / "sound1" / "speech";
    if (isdir(sp))
        for (const std::string &f : listdir_sorted(sp))
            if (ends(f, ".psarc")) add_arc(sp / P(f), "base", "sound1/speech/" + split(f.substr(0, f.size() - 6), '-')[0] + "/");
    if (isfile(base_main / "pak22.txt")) {
        Entry e;
        e.key = e.name = "pak22.txt";
        e.source = "base";
        e.path = base_main / "pak22.txt";
        put(e);
    }
    add_arc(usrdir / "all-patches.psarc", "patch", "");
    if (!dlc.empty()) {
        const fs::path dm = build_main(P(dlc)).value_or(P(dlc));
        int found = 0;
        for (const char *tag : {"dlc-map1", "dlc-map2", "dlc-patch2", "dlc-patch3", "dlc-patch5", "dlc-patch7"}) {
            const fs::path arc = dm / (std::string(tag) + ".psarc");
            if (isfile(arc)) {
                add_arc(arc, tag, "");
                found++;
            } else if (isdir(dm / tag)) {
                add_tree(dm / tag, tag);
                found++;
            }
        }
        if (!found) log("note: no dlc-map / dlc-patch archives under " + dlc);
        const fs::path dsp = dm / "sound1" / "speech";   // the disc copy's speech update archives: DLC character lines
        if (isdir(dsp))
            for (const std::string &f : listdir_sorted(dsp))
                if (ends(f, ".psarc") && f.find("update") != std::string::npos) add_arc(dsp / P(f), "dlc-patch2", "sound1/speech/" + split(f, '-')[0] + "/");
    }
}

void PS3::put(Entry e) { by_key[lower(e.key)].push_back(std::move(e)); }

void PS3::add_arc(const fs::path &path, const std::string &source, const std::string &prefix) {
    if (!isfile(path)) return;
    std::shared_ptr<Psarc> p;
    try {
        p = std::make_shared<Psarc>(path);
    } catch (const std::exception &ex) {
        log("warning: cannot read " + S(path) + ": " + ex.what());
        return;
    }
    archives.push_back(S(path));
    for (size_t i = 0; i < p->names.size(); i++) {
        Entry e;
        e.key = norm_member(p->names[i], prefix);
        e.name = p->names[i];
        e.source = source;
        e.arc = p;
        e.index = i + 1;
        e.arc_path = S(path);
        put(std::move(e));
    }
}

void PS3::add_tree(const fs::path &root, const std::string &source) {
    std::error_code ec;
    for (const auto &de : fs::recursive_directory_iterator(root, ec)) {
        if (!de.is_regular_file()) continue;
        std::string key = S(fs::relative(de.path(), root));
        std::replace(key.begin(), key.end(), '\\', '/');
        Entry e;
        e.key = e.name = key;
        e.source = source;
        e.path = de.path();
        put(std::move(e));
    }
}

std::vector<const Entry *> PS3::candidates(const std::string &key) const {
    std::vector<const Entry *> v;
    const auto it = by_key.find(lower(key));
    if (it != by_key.end())
        for (const Entry &e : it->second) v.push_back(&e);
    std::stable_sort(v.begin(), v.end(), [](const Entry *a, const Entry *b) { return rank(a->source) > rank(b->source); });
    return v;
}

PS4::PS4(const std::string &game) {
    const fs::path g = P(game);
    const fs::path u = isdir(g / "u2data") ? g / "u2data" : g;
    main = u / "build" / "main";
    if (!isfile(main / "pak22_ps4.txt")) throw std::runtime_error("no Uncharted 2 PS4 data (u2data/build/main/pak22_ps4.txt) under " + game);
    pak_dir = main / "pak22" / "ps4";
    actor_dir = main / "actor27" / "ps4";
    bank_dir = main / "sound1" / "banks" / "ps4";
    for (const auto &[sub, key] : std::vector<std::pair<std::string, std::string>>{{"pak22/ps4", "pak22/"},
                                                                                  {"actor27/ps4", "actor27/"},
                                                                                  {"sound1/banks/ps4", "sound1/banks/"},
                                                                                  {"sound1/streams", "sound1/streams/"},
                                                                                  {"sound1/music", "sound1/music/"}}) {
        const fs::path d = main / P(sub);
        if (isdir(d))
            for (const std::string &f : listdir_sorted(d)) files[lower(key + f)] = d / P(f);
    }
    const fs::path sp = main / "sound1" / "speech";
    if (isdir(sp))
        for (const std::string &f : listdir_sorted(sp))
            if (ends(f, "_ps4.psarc")) {
                std::set<std::string> names;
                try {
                    Psarc a(sp / P(f));
                    for (const std::string &n : a.names) names.insert(lower(basename(n)));
                } catch (const std::exception &) {
                }
                speech[f.substr(0, f.size() - 10)] = names;
            }
    registry_path = main / "pak22_ps4.txt";
    banks_txt = main / "sound1" / "config" / "ps4" / "banks.txt";
    music_settings = main / "dc1" / "ps4" / "music-settings.bin";
}

// the track names the PS4 music settings name: 'NNN_<word>' or 'MP_<word>' (at least 2 more word characters), not
// inside a longer word
std::set<std::string> PS4::music_refs() const {
    std::set<std::string> refs;
    if (!isfile(music_settings)) return refs;
    const Bytes d = read_bytes(music_settings);
    auto word = [&](size_t i) { return i < d.size() && (isalnum(d[i]) || d[i] == '_'); };
    size_t i = 0;
    while (i < d.size()) {
        if (i > 0 && word(i - 1)) {
            i++;
            continue;
        }
        size_t j = 0;
        if (i + 4 <= d.size() && isdigit(d[i]) && isdigit(d[i + 1]) && isdigit(d[i + 2]) && d[i + 3] == '_') j = i + 4;
        else if (i + 3 <= d.size() && d[i] == 'M' && d[i + 1] == 'P' && d[i + 2] == '_') j = i + 3;
        if (j) {
            size_t k = j;
            while (word(k)) k++;
            if (k - j >= 2) {
                refs.insert(std::string(d.begin() + long(i), d.begin() + long(k)));
                i = k;
                continue;
            }
        }
        i++;
    }
    return refs;
}

}  // namespace u2imp
