// importer/ps3keep.cpp: the Multiplayer tab's opt-in "keep only the PS3 files updates need" (only to save disk space).
//
// <data>/ps3-kept holds just the PS3 files the import reads, in the layout of the player's PS3 folders:
//   ps3-kept/<id>/<folder name>/...   one per PS3 folder (id = folder name + hash of its path), e.g. .../USRDIR/...
//   ps3-kept/<id>/PARAM.SFO           a file next to that folder (PS3_GAME/PARAM.SFO beside USRDIR)
//   ps3-kept/elf/EBOOT.elf            the decrypted EBOOT.elf when it is not in a PS3 folder
//   ps3-kept/manifest.json            what is kept, how (hard link or verified copy), complete or not
// Which files: the ones the imports recorded (<data>/ps3-reads.txt) plus ps3-needed.tsv (what a full import reads, traced
// 2026-11-09). On the same drive the files are hard links; on another drive they are copied and compared with the
// source. Later imports add the kept folders after the player's own: the player's folders win while they exist.
#include "ps3keep.hpp"

#include "recipe.hpp"

#include <windows.h>

#include <fstream>
#include <set>

namespace u3imp {

namespace {

const char *KEPT = "ps3-kept";
const char *READS = "ps3-reads.txt";

std::string root_id(const fs::path &root) {
    std::string r = S(fs::absolute(root).lexically_normal());
    while (!r.empty() && (r.back() == '\\' || r.back() == '/')) r.pop_back();
    const std::string base = S(P(r).filename());
    // os.path.normcase: lower case, '/' -> '\'
    std::string nc = lower(r);
    for (char &c : nc)
        if (c == '/') c = '\\';
    return (base.empty() ? "root" : base) + "-" + sha256_hex(nc).substr(0, 8);
}
std::string win(std::string s) {
    for (char &c : s)
        if (c == '/') c = '\\';
    return s;
}
std::string abspath(const fs::path &p) {
    std::error_code ec;
    return win(S(fs::absolute(p, ec).lexically_normal()));
}
std::string norm(const std::string &p) { return win(S(P(p).lexically_normal())); }

}  // namespace

Value ps3keep_manifest(const fs::path &data) {
    if (auto v = try_json(data / KEPT / "manifest.json"); v && v->is_object()) return *v;
    return Value::object();
}

std::vector<fs::path> ps3keep_roots(const fs::path &data) {
    const Value m = ps3keep_manifest(data);
    std::vector<fs::path> out;
    if (!(m.get("complete") && m.get("complete")->truthy())) return out;
    std::vector<std::pair<std::string, std::string>> roots;
    if (const Value *r = m.get("roots"))
        for (const auto &[rid, name] : r->fields) roots.push_back({rid, name.s});
    std::sort(roots.begin(), roots.end());
    for (const auto &[rid, name] : roots) out.push_back(data / KEPT / P(rid) / P(name));
    std::error_code ec;
    if (fs::is_directory(data / KEPT / "elf", ec)) out.push_back(data / KEPT / "elf");
    std::vector<fs::path> keep;
    for (const fs::path &p : out)
        if (fs::is_directory(p, ec)) keep.push_back(p);
    return keep;
}

void ps3keep_record(const std::vector<fs::path> &roots, const fs::path &data) { record_reads(roots, data / READS); }

namespace {

// ps3-needed.tsv mapped onto the PS3 folders found in roots: absolute paths (missing ones left out)
std::set<std::string> needed(const std::vector<fs::path> &roots, Inputs &inp) {
    inp.add_ps3_u3(roots);
    std::set<std::string> out;
    const fs::path nf = patcher_dir() / "ps3-needed.tsv";
    std::error_code ec;
    if (!fs::is_regular_file(nf, ec)) return out;
    std::string t = read_text(nf);
    for (std::string line : split(t, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (strip(line).empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        const std::string kind = line.substr(0, tab), rel = line.substr(tab + 1);
        auto it = inp.ps3.find(kind);
        if (it != inp.ps3.end()) out.insert(norm(S(it->second) + "\\" + rel));
    }
    for (const auto &[k, d] : inp.ps3)   // every folder's PARAM.SFO (names the inputs in the log)
        if (auto sfo = find_sfo(d)) out.insert(abspath(*sfo));
    // no Game of the Year disc (its goty-dlc.psarc stands in for the DLC map packs): keep the packs installed on their own
    auto dl = inp.ps3.find("dlcmaps"), di = inp.ps3.find("disc");
    if (dl != inp.ps3.end() && (di == inp.ps3.end() || dl->second != di->second) &&
        !fs::exists((di == inp.ps3.end() ? fs::path() : di->second) / "goty-dlc.psarc", ec))
        for (int n = 1; n <= 4; n++) out.insert(S(dl->second / P("dlc-map" + std::to_string(n) + ".psarc")));
    return out;
}

// where p goes under ps3-kept (relative), and the (id, folder name) of its PS3 folder
std::pair<std::string, std::optional<std::pair<std::string, std::string>>> place(const std::string &p, const std::vector<std::string> &roots, const std::string &elf) {
    const std::string q = lower(p);
    for (const std::string &r : roots) {
        const std::string rn = lower(r);
        if (starts(q, rn + "\\")) {
            const std::string rid = root_id(P(r)), name = S(P(r).filename());
            std::string rel = p.substr(r.size() + 1);
            for (char &c : rel)
                if (c == '\\') c = '/';
            return {rid + "/" + name + "/" + rel, std::pair{rid, name}};
        }
    }
    for (const std::string &r : roots)
        if (lower(S(P(p).parent_path())) == lower(S(P(r).parent_path()))) {
            const std::string rid = root_id(P(r)), name = S(P(r).filename());
            return {rid + "/" + S(P(p).filename()), std::pair{rid, name}};
        }
    if (!elf.empty() && lower(abspath(P(elf))) == q) return {"elf/" + S(P(p).filename()), std::nullopt};
    return {"", std::nullopt};
}

std::string copy_verified(const fs::path &src, const fs::path &dst) {
    const fs::path tmp = P(S(dst) + ".part");
    Sha256 h;
    {
        std::ifstream fi(src, std::ios::binary);
        std::ofstream fo(tmp, std::ios::binary | std::ios::trunc);
        std::vector<char> buf(8 << 20);
        while (fi) {
            fi.read(buf.data(), std::streamsize(buf.size()));
            const std::streamsize n = fi.gcount();
            if (n <= 0) break;
            h.update(buf.data(), size_t(n));
            fo.write(buf.data(), n);
        }
        if (!fo) throw u3::Stop("cannot write " + S(tmp));
    }
    const std::string want = h.hex();
    std::error_code ec;
    if (sha256_file(tmp) != want) {
        fs::remove(tmp, ec);
        throw u3::Stop("the copy of " + S(src) + " differs from the original");
    }
    fs::rename(tmp, dst);
    fs::last_write_time(dst, fs::last_write_time(src, ec), ec);   // shutil.copystat
    return want;
}

bool same_file(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}

int64_t mtime_ns(const fs::path &p) {
    // os.stat().st_mtime_ns: nanoseconds since 1970
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    const int64_t t = int64_t(uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32 | d.ftLastWriteTime.dwLowDateTime);
    return (t - 116444736000000000LL) * 100;
}

}  // namespace

Value ps3keep_keep(const fs::path &data, const std::vector<fs::path> &roots0, const std::string &elf, const LogFn &lg, const EventFn &ev) {
    const fs::path kept = data / KEPT;
    const std::string kept_nc = lower(abspath(kept));
    std::vector<std::string> roots;
    std::error_code ec;
    for (const fs::path &r : roots0) {
        if (r.empty() || !fs::is_directory(r, ec)) continue;
        std::string a = abspath(r);
        while (!a.empty() && (a.back() == '\\' || a.back() == '/')) a.pop_back();
        if (starts(lower(abspath(r)), kept_nc + "\\")) continue;
        roots.push_back(a);
    }
    Value m = ps3keep_manifest(data);
    m.setdefault("roots", Value::object());
    m.setdefault("files", Value::object());
    std::set<std::string> want;
    if (fs::is_regular_file(data / READS, ec))
        for (std::string l : split(read_text(data / READS), '\n')) {
            l = strip(l);
            if (!l.empty()) want.insert(l);
        }
    Inputs inp;
    std::vector<fs::path> rp;
    for (const auto &r : roots) rp.push_back(P(r));
    for (const std::string &n : needed(rp, inp)) want.insert(n);
    if (!elf.empty() && fs::is_regular_file(P(elf), ec)) want.insert(abspath(P(elf)));
    struct Item {
        std::string p, rel;
        std::optional<std::pair<std::string, std::string>> root;
    };
    std::vector<Item> plan;
    for (const std::string &p : want) {   // sorted, as Python's sorted(set)
        if (!fs::is_regular_file(P(p), ec)) continue;
        auto [rel, root] = place(p, roots, elf);
        if (!rel.empty()) plan.push_back({p, rel, root});
    }
    if (plan.empty() && !(m.get("complete") && m.get("complete")->truthy()))
        return obj({{"complete", Value::boolean(false)}, {"error", str("no PS3 folder found to keep files from")}});
    fs::create_directories(kept);
    // space for the files that cannot be hard links (another drive)
    uint64_t need = 0;
    const std::string kd = lower(S(kept.root_name()));
    for (const Item &it : plan)
        if (lower(S(P(it.p).root_name())) != kd && !fs::exists(kept / P(it.rel), ec)) need += fs::file_size(P(it.p), ec);
    const auto sp = fs::space(kept, ec);
    if (double(need) > double(sp.available) - 1e9) {
        Value st = m.get("state") ? *m.get("state") : Value::object();
        st.set("complete", Value::boolean(m.get("complete") && m.get("complete")->truthy()));
        st.set("error", str(fmt("%.1f GB to copy, %.1f GB free on %s", need / 1e9, sp.available / 1e9, (S(kept.root_name()) + "\\").c_str())));
        return st;
    }
    if (ev) ev(obj({{"phase", str("ps3keep")}, {"status", str("running")}, {"note", str(fmt("Keeping the PS3 files the import needs (%zu files)...", plan.size()))}}));
    lg(fmt("PS3 files: keeping %zu file(s) in %s", plan.size(), S(kept).c_str()));
    int linked = 0, copied = 0, same = 0;
    Value &files = *m.get("files");
    Value &mroots = *m.get("roots");
    for (const Item &it : plan) {
        const fs::path src = P(it.p), dst = kept / P(it.rel);
        const uint64_t size = fs::file_size(src, ec);
        const Value *rec = files.get(it.rel);
        if (fs::exists(dst, ec)) {
            if (same_file(src, dst)) {
                same++;
                files.set(it.rel, obj({{"size", num((long long)size)}, {"how", str("link")}}));
                if (it.root) mroots.set(it.root->first, str(it.root->second));
                continue;
            }
            if (fs::file_size(dst, ec) == size && rec && rec->get("mtime") && rec->get("mtime")->i == mtime_ns(src) && rec->get("sha256") &&
                rec->get("sha256")->truthy()) {
                same++;
                if (it.root) mroots.set(it.root->first, str(it.root->second));
                continue;
            }
            fs::remove(dst, ec);
        }
        fs::create_directories(dst.parent_path());
        if (CreateHardLinkW(dst.c_str(), src.c_str(), nullptr)) {
            files.set(it.rel, obj({{"size", num((long long)size)}, {"how", str("link")}}));
            linked++;
        } else {
            const std::string h = copy_verified(src, dst);
            files.set(it.rel, obj({{"size", num((long long)size)}, {"how", str("copy")}, {"mtime", num(mtime_ns(src))}, {"sha256", str(h)}}));
            copied++;
        }
        if (it.root) mroots.set(it.root->first, str(it.root->second));
    }
    // files kept earlier whose PS3 original is gone stay (they are then the only copy)
    std::vector<std::string> gone;
    for (const auto &[rel, f] : files.fields)
        if (!fs::is_regular_file(kept / P(rel), ec)) gone.push_back(rel);
    for (const auto &rel : gone) files.remove(rel);
    // the kept folders alone must be found as the same PS3 inputs
    std::vector<std::string> lost;
    {
        Inputs alone;
        std::vector<fs::path> kr;
        for (const auto &[rid, name] : mroots.fields) kr.push_back(kept / P(rid) / P(name.s));
        alone.add_ps3_u3(kr);
        for (const auto &[k, d] : inp.ps3)
            if (!alone.ps3.count(k)) lost.push_back(k);
    }
    uint64_t size = 0, extra = 0;
    for (const auto &[rel, f] : files.fields) {
        size += uint64_t(f.get("size")->i);
        if (f.get("how") && f.get("how")->s == "copy") extra += uint64_t(f.get("size")->i);
    }
    std::string lost_s;
    for (const auto &l : lost) lost_s += (lost_s.empty() ? "" : ", ") + l;
    m.set("complete", Value::boolean(lost.empty()));
    m.set("state", obj({{"complete", Value::boolean(lost.empty())},
                        {"files", num((long long)files.fields.size())},
                        {"size", num((long long)size)},
                        {"copied_size", num((long long)extra)},
                        {"folder", str(S(kept))},
                        {"error", str(lost.empty() ? "" : "the kept files are not found as: " + lost_s)}}));
    write_text(kept / "manifest.json", dump_indent1(m));
    lg(fmt("PS3 files: %zu kept (%.1f GB; %d new hard links, %d verified copies, %d unchanged)", files.fields.size(), size / 1e9, linked, copied, same) +
       (lost.empty() ? "" : "; NOT complete: " + m.get("state")->get("error")->s));
    if (ev)
        ev(obj({{"phase", str("ps3keep")},
                {"status", str(lost.empty() ? "ok" : "failed")},
                {"note", str(lost.empty() ? fmt("PS3 files kept: %.1f GB. Your PS3 Uncharted 3 folders are no longer needed.", size / 1e9)
                                          : "PS3 files: " + m.get("state")->get("error")->s)}}));
    return *m.get("state");
}

void ps3keep_drop(const fs::path &data, const LogFn &lg) {
    const fs::path kept = data / KEPT;
    std::error_code ec;
    if (fs::exists(kept, ec)) {
        rmtree(kept);
        lg("PS3 files: the kept copy was removed (" + S(kept) + ")");
    }
}

}  // namespace u3imp
