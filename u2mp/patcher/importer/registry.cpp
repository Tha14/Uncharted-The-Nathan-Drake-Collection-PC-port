// importer/registry.cpp: what the PS4 game needs to see imported content, pak22_ps4.txt (the level registry) and
// sound1/config/ps4/banks.txt.
//
// pak22_ps4.txt is a list of 'key value' lines; 'level-name X ' (with the trailing space retail has) starts a level,
// then its level-id, package, sound-bank, cache, actor and package-ts lines. The PS4 file stays byte for byte, except
// that the multiplayer / co-op entries lose the 'actor' lines whose actor pak neither the PS4 game nor the overlay has
// (retail ruins-multi names four map pack 1 props the base-game Ruins never uses). Levels the PS4 file does not define
// are added in the same form from their PS3 .txt:
//   * 'package' / 'actor' / 'sound-bank' lines only when the PS4 game or the overlay has that file (dropped ones are
//     reported); a level without a level-id line gets one;
//   * the package-ts lines most retail multiplayer levels carry (the texture streaming of the retail textures the
//     converted levels reuse);
//   * no entry for a level with a package the import could not convert, nor for one none of whose packages exists.
// banks.txt: the PS4 list plus the imported banks it lacks, in name order, with the file's own line ends.
#include "u2import.hpp"

#include <algorithm>

namespace u2imp {
namespace {

using Line = std::pair<std::string, std::string>;
struct Parsed {
    std::map<std::string, std::vector<Line>> levels;
    std::vector<std::string> order;   // first appearance
};

Parsed parse(const std::string &text) {
    Parsed p;
    std::vector<Line> *cur = nullptr;
    for (const std::string &line : splitlines(text)) {
        const auto parts = split_ws(pystrip(line), 1);
        if (parts.size() != 2) continue;
        const std::string k = parts[0], v = pystrip(parts[1]);
        if (k == "level-name") {
            if (!p.levels.count(v)) p.order.push_back(v);
            cur = &p.levels[v];
            continue;
        }
        if (cur) cur->push_back({k, v});
    }
    return p;
}

// the package-ts lines of the '*multi*' levels, the most common set (the first one of a tie, in file order)
std::vector<std::string> mp_package_ts(const Parsed &p) {
    std::vector<std::pair<std::vector<std::string>, int>> count;
    for (const std::string &n : p.order) {
        if (n.find("multi") == std::string::npos) continue;
        std::vector<std::string> ts;
        for (const auto &[k, v] : p.levels.at(n))
            if (k == "package-ts") ts.push_back(v);
        auto it = std::find_if(count.begin(), count.end(), [&](const auto &c) { return c.first == ts; });
        if (it == count.end()) count.push_back({ts, 1});
        else it->second++;
    }
    const std::vector<std::string> *best = nullptr;
    int n = 0;
    for (const auto &[ts, c] : count)
        if (c > n) best = &ts, n = c;
    return best ? *best : std::vector<std::string>{};
}

bool mp_level(const std::string &name) { return name.find("multi") != std::string::npos || name.find("coop") != std::string::npos; }

std::string prune_actors(const std::string &ps4_text, const std::function<bool(const std::string &)> &have_actor,
                         std::map<std::string, std::vector<Line>> &pruned) {
    std::string out, cur;
    for (const std::string &line : splitlines(ps4_text, true)) {
        const auto parts = split_ws(pystrip(line), 1);
        if (parts.size() == 2 && parts[0] == "level-name") cur = pystrip(parts[1]);
        else if (parts.size() == 2 && parts[0] == "actor" && !cur.empty() && mp_level(cur) && !have_actor(pystrip(parts[1]))) {
            pruned[cur + " (PS4 entry)"].push_back({"actor", pystrip(parts[1])});
            continue;
        }
        out += line;
    }
    return out;
}

std::vector<Line> lines_of(const std::string &name, const std::string &ps3_txt) {
    Parsed p = parse("level-name " + name + "\n" + ps3_txt);
    return p.levels[name];
}

std::string level_block(const std::string &name, const std::string &ps3_txt, const std::function<bool(const std::string &)> &have_pak,
                        const std::function<bool(const std::string &)> &have_actor, const std::function<bool(const std::string &)> &have_bank,
                        const std::vector<std::string> &ts, std::vector<Line> &dropped) {
    std::vector<std::string> out = {"level-name " + name + " "};
    bool seen_id = false;
    for (const std::string &line : splitlines(ps3_txt)) {
        const auto parts = split_ws(pystrip(line), 1);
        if (parts.size() != 2) continue;
        const std::string k = parts[0], v = pystrip(parts[1]);
        if (k == "level-name") continue;
        if (k == "level-id") seen_id = true;
        const bool ok = k == "package" ? have_pak(v) : k == "actor" ? have_actor(v) : k == "sound-bank" ? have_bank(v) : true;
        if (ok) out.push_back(k + " " + v);
        else dropped.push_back({k, v});
    }
    if (!seen_id) out.insert(out.begin() + 1, "level-id " + name);
    for (const std::string &t : ts) out.push_back("package-ts " + t);
    std::string s;
    for (size_t i = 0; i < out.size(); i++) s += (i ? "\n" : "") + out[i];
    return s + "\n";
}

std::string kv_list(const std::vector<Line> &v, size_t max) {
    std::vector<std::string> s;
    for (size_t i = 0; i < v.size() && i < max; i++) s.push_back(v[i].first + " " + v[i].second);
    return py_list(s);
}

}  // namespace

std::vector<std::string> package_file_names(const std::string &pkg) {
    std::vector<std::string> names = {pkg};
    if (ends(pkg, "-ingame")) names.push_back(pkg + "10");
    return names;
}

RegistryResult build_registry(const std::string &ps4_text_in, const std::map<std::string, std::string> &new_levels,
                              const std::function<bool(const std::string &)> &have_pak,
                              const std::function<bool(const std::string &)> &have_actor,
                              const std::function<bool(const std::string &)> &have_bank, const std::set<std::string> &failed_paks) {
    RegistryResult r;
    const Parsed levels = parse(ps4_text_in);
    const std::vector<std::string> ts = mp_package_ts(levels);
    const std::string ps4_text = prune_actors(ps4_text_in, have_actor, r.dropped);
    r.text = ends(ps4_text, "\n") ? ps4_text : ps4_text + "\n";
    for (const auto &[name, txt] : new_levels) {   // sorted by name
        const auto lv = levels.levels.find(name);
        if (lv != levels.levels.end()) {
            std::vector<Line> mine;
            for (const Line &l : lv->second)
                if (l.first != "package-ts") mine.push_back(l);
            const std::vector<Line> ps3 = lines_of(name, txt);
            std::vector<Line> extra, gone;
            for (const Line &l : ps3)
                if (std::find(mine.begin(), mine.end(), l) == mine.end()) extra.push_back(l);
            for (const Line &l : mine)
                if (std::find(ps3.begin(), ps3.end(), l) == ps3.end()) gone.push_back(l);
            r.skipped[name] = "the PS4 registry already defines it (" +
                              (extra.empty() && gone.empty() ? std::string("same lines as the PS3 entry")
                                                             : fmt("PS3 differs: %zu lines only PS3 has %s, %zu only PS4 has %s", extra.size(),
                                                                   kv_list(extra, 3).c_str(), gone.size(), kv_list(gone, 3).c_str())) +
                              ")";
            continue;
        }
        std::vector<std::string> pk;
        for (const Line &l : lines_of(name, txt))
            if (l.first == "package") pk.push_back(l.second);
        if (pk.empty()) pk = {name};   // no package line (the-fort-multi-wide-2): the level's own pak
        std::vector<std::string> bad;
        for (const std::string &v : pk)
            for (const std::string &n : package_file_names(v))
                if (failed_paks.count(n)) {
                    bad.push_back(v);
                    break;
                }
        auto joined = [](const std::vector<std::string> &v) {
            std::string s;
            for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + v[i];
            return s;
        };
        if (!bad.empty()) {
            r.skipped[name] = "its package " + joined(bad) + " was not converted";
            continue;
        }
        if (std::none_of(pk.begin(), pk.end(), have_pak)) {
            r.skipped[name] = "none of its packages (" + joined(pk) + ") was converted or is on PS4";
            continue;
        }
        std::vector<Line> d;
        r.text += level_block(name, txt, have_pak, have_actor, have_bank, ts, d);
        r.added.push_back(name);
        if (!d.empty()) r.dropped[name] = d;
    }
    return r;
}

std::pair<std::string, std::vector<std::string>> build_banks_txt(const std::string &ps4_text, const std::vector<std::string> &new_banks) {
    std::set<std::string> have;
    for (const std::string &l : splitlines(ps4_text))
        if (!pystrip(l).empty()) have.insert(lower(pystrip(l)));
    std::vector<std::string> sorted = new_banks, add;
    std::sort(sorted.begin(), sorted.end());
    for (const std::string &b : sorted)
        if (!have.count(lower(b))) add.push_back(b);
    const std::string nl = ps4_text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string text = ps4_text.empty() || ends(ps4_text, "\n") ? ps4_text : ps4_text + nl;
    for (const std::string &b : add) text += b + nl;
    return {text, add};
}

}  // namespace u2imp
