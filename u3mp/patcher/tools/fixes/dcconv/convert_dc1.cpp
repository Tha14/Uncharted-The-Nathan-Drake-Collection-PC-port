// Recipe step convert-dc1: the PS3 1.19 DC modules the overlays carry as whole-file conversions (ss-multiplayer state
// scripts, ai-*, destruction, player-settings, render-settings) converted to PS4 DC with dctools/dcconv.cpp, plus
// their .dci (the PS3 .dci with the size field set to the PS4 .bin size). Replaces convert_dc1.py.
//   u3fix convert-dc1 --project P --job J --schema S --ps4 D --ps3-disc D --ps3-update D --ps3-data2 D
//                     [--apply] [--only NAME[,NAME]] [--report FILE] [--force-files] [--all]
// dc1-convert.tsv (in the job folder): overlay, PS4 path, PS3 layer, the sha256 the converted file must have (the
// 2026-10-27 cloud conversion) and the converter's state for it (identical / different). Layouts come from two schemas:
// S = tools/dcconv/schema-oldconv.json (learned from the 2026-10-27 conversions: offsets and type ids only) and the
// retail schema learned from the player's own PS3 disc + PS4 Collection DC pairs (once, cached in
// work/_dcconv/schema-retail.json). Only the files the converter reproduces exactly are written (--all: every file,
// best effort). Dry run by default; files already present are left alone (later fixes patch some in place).
#include "dcconv.hpp"
#include "proc.hpp"
#include "psarc.hpp"
#include "u3fix.hpp"

#include <regex>

namespace u3 {

int convert_dc1(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--force-files", "--all"}, {"--project", "--job", "--schema", "--ps4", "--ps3-disc", "--ps3-update", "--ps3-data2", "--only", "--report"});
    const std::string project = a.need("--project"), ps4 = a.need("--ps4");
    std::vector<std::array<std::string, 5>> rows;
    {
        const std::string t = text_file(upath(a.need("--job")) / "dc1-convert.tsv");
        size_t b = 0;
        for (size_t k = 0; k <= t.size(); k++) {
            if (k < t.size() && t[k] != '\n') continue;
            std::string l = t.substr(b, k - b);
            b = k + 1;
            if (py_split(l).empty() || l[0] == '#') continue;
            std::vector<std::string> c;
            size_t s = 0;
            for (size_t j = 0; j <= l.size(); j++)
                if (j == l.size() || l[j] == '\t') { c.push_back(l.substr(s, j - s)); s = j + 1; }
            if (c.size() != 5) throw Stop("ValueError: dc1-convert.tsv row: " + l);
            rows.push_back({c[0], c[1], c[2], c[3], c[4]});
        }
    }
    if (a.val.count("--only")) {
        std::vector<std::string> only;
        const std::string on = a.get("--only", "");
        size_t s = 0;
        for (size_t j = 0; j <= on.size(); j++)
            if (j == on.size() || on[j] == ',') { only.push_back(on.substr(s, j - s)); s = j + 1; }
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const auto &r) {
                       for (const std::string &x : only)
                           if (r[1].find(x) != std::string::npos) return false;
                       return true;
                   }), rows.end());
    }
    Ps3Index idx(a.get("--ps3-disc", ""), a.get("--ps3-update", ""), a.get("--ps3-data2", ""));
    // the retail schema: learned once from the PS3 disc modules whose PS4 counterpart has as many pointer slots
    std::string retail;
    const fs::path cache = upath(project) / "work" / "_dcconv" / "schema-retail.json";
    if (fs::exists(cache))
        retail = text_file(cache);
    else {
        const std::string ret = ps4 + "/u3data/build/main/";
        std::vector<std::pair<Bytes, Bytes>> pairs;
        for (const std::string &rel : idx.names()) {
            if (rel.size() < 4 || rel.compare(rel.size() - 4, 4, ".bin") || !fs::exists(upath(ret + rel))) continue;
            const auto d = idx.read(rel, "disc");
            if (!d) continue;
            Bytes d4 = read_file(upath(ret + rel));
            if (dcconv::slot_count(*d) == dcconv::slot_count(d4)) pairs.push_back({*d, std::move(d4)});
        }
        std::printf("learning the retail schema from %zu PS3/PS4 pairs (one time)\n", pairs.size());
        std::fflush(stdout);
        retail = dcconv::learn(pairs, [](const std::string &m) { std::printf("%s\n", m.c_str()); std::fflush(stdout); });
        fs::create_directories(cache.parent_path());
        write_file_replace(cache, Bytes(retail.begin(), retail.end()));
    }
    const dcconv::Converter C({text_file(upath(a.need("--schema"))), retail});
    std::vector<std::pair<std::string, int>> tot;
    auto count = [&](const std::string &k) {
        for (auto &x : tot)
            if (x.first == k) { x.second++; return; }
        tot.push_back({k, 1});
    };
    std::vector<std::string> rep_lines;
    static const std::regex DCI(R"(^\(([^ ]+) \((\d+)\))");
    for (const auto &[ov, rel4, layer, want, state] : rows) {
        const fs::path dst = upath(project + "/work/" + ov + "/u3data/build/main/" + rel4);
        std::string rel3 = rel4;
        if (const size_t k = rel3.find("dc2/part-module/"); k != std::string::npos) rel3.replace(k, 16, "dc1/ps4/");
        auto put_dci = [&](size_t n) {   // the overlays carry .dci only for the ss-multiplayer scripts
            if (("/" + rel4).find("/ss-multiplayer/") == std::string::npos) return;
            const std::string dr = rel3.substr(0, rel3.size() - 4) + ".dci";
            auto dci3 = idx.read(dr, layer);
            if (!dci3) dci3 = idx.read(dr);   // the .dci may live in another layer than the .bin
            if (!dci3) return;
            const std::string s(dci3->begin(), dci3->end());
            const std::string r = std::regex_replace(s, DCI, "($1 (" + std::to_string(n) + ")", std::regex_constants::format_first_only);
            const fs::path dp = upath(utf8(dst).substr(0, utf8(dst).size() - 4) + ".dci");
            const Bytes rb(r.begin(), r.end());
            if (!fs::exists(dp) || read_file(dp) != rb) write_file_replace(dp, rb);
        };
        if (fs::exists(dst)) {
            // present: this step's own output or a later fix of it (enemy-stall / coop-audit patch these in place)
            const bool at = sha_file(dst) == want;
            if (at || !a.has("--force-files")) {
                count(at ? "already" : "present-kept");
                if (a.has("--apply")) put_dci(size_t(fs::file_size(dst)));
                continue;
            }
        }
        if (state != "identical" && !a.has("--all")) {
            count("skipped-unverified");
            rep_lines.push_back("SKIPPED " + ov + " " + rel4 + " (converter not exact for it; --all writes it)");
            continue;
        }
        const auto d3 = idx.read(rel3, layer);
        if (!d3) {
            count("no-source");
            rep_lines.push_back("NO-SOURCE " + layer + " " + rel3);
            continue;
        }
        const Bytes out = C.convert(*d3);
        const bool ok = sha256_hex(out) == want;
        count(ok ? "identical" : "different");
        rep_lines.push_back(std::string(ok ? "IDENTICAL " : "DIFFERENT ") + ov + " " + rel4);
        if (a.has("--apply") && (ok || a.has("--all"))) {
            fs::create_directories(dst.parent_path());
            write_file_replace(dst, out);
            put_dci(out.size());
        }
    }
    std::string d = "{";
    for (size_t i = 0; i < tot.size(); i++) d += (i ? ", '" : "'") + tot[i].first + "': " + std::to_string(tot[i].second);
    std::printf("convert-dc1: %s} %s\n", d.c_str(), a.has("--apply") ? "" : "(dry run, nothing written)");
    for (const std::string &l : rep_lines)
        if (l.rfind("IDENTICAL", 0) != 0) std::printf("  %s\n", l.c_str());
    if (a.val.count("--report")) {
        std::string t;
        for (const std::string &l : rep_lines) t += l + "\n";
        write_text(upath(a.get("--report", "")), t);
    }
    return 0;
}

}  // namespace u3
