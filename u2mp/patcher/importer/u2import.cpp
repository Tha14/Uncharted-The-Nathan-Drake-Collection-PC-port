// importer/u2import.cpp: u2import.exe, the Uncharted 2 multiplayer assets the PS4 Collection lacks, brought in from the
// player's own PS3 copy and converted to the PS4 format, as an overlay folder.
//
//   u2import --ps4 <PS4 game folder> --ps3 <PS3 base folder> [--ps3-dlc <PS3 DLC folder>] --out <folder>
//            [--ps3-data <PS3 game-data folder>] [--groups A,B,C,D,E] [--speech-langs english,french,...] [--dlc-ruins]
//            [--drop-navmesh] [--only REGEX] [--ps3-elf EBOOT.elf] [--objdump PATH] [--ffmpeg PATH] [--force] [--update] [--validate]
//   u2import --shipped-id          the fingerprint of the installed import (the launcher compares it with state.json)
//
//   --ps4      the Collection's game folder (holds u2data/) or u2data itself
//   --ps3      PS3 Uncharted 2 (GOTY NPUA80698): USRDIR, its parent or build/main (archives + all-patches.psarc)
//   --ps3-dlc  folder with dlc-map1/2.psarc and dlc-patch*.psarc; without it the map packs (groups C, D) are skipped
//   --ps3-data the PS3 game-data folder (RPCS3 dev_hdd0/game or a <id>DATA2 folder): net-patch9.bin and the server's
//              map spawn paks
//   --out      the overlay: <out>/u2data/build/main/... (only converted files and the registries), <out>/state.json,
//              <out>/import.log, <out>/work (caches, removable)
//
// Rule: what the PS4 game has (skin, map, picture, sound ...) stays the PS4 one; only what it lacks is imported. Every
// row of u2mp/docs/import-list.tsv (group, key, PS3 source, size, SHA-1) is checked against the PS4 game first; PS3
// inputs are checked by SHA-1. Groups: A skins, B the Fort's missing pieces, C map pack 1, D map pack 2, E base extras
// (props, banks, music, DLC character voices), F optional (only with --groups F).
//
// The steps run like Uncharted 3's recipe: a step whose fingerprint (this program, the inputs and options, and for the
// later steps what the first step made) and outputs are unchanged since its last good run is skipped; within a step a
// converted file whose input SHA-1 and converter version are unchanged is kept. Progress: lines "@U2MP <json>".
// Nothing is written outside --out.
#include "u2conv.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdio>
#include <regex>
#include <thread>

using namespace u2imp;

std::string shipped_id_text();   // the fingerprint of the installed import (end of this file)

namespace u2imp {

// ------------------------------------------------------------------------------------------------ small text helpers
std::string sha1_hex(const Bytes &b);   // u2imp.cpp below

std::vector<std::string> splitlines(const std::string &t, bool keepends) {
    std::vector<std::string> out;
    size_t a = 0, i = 0;
    while (i < t.size()) {
        const uint8_t c = uint8_t(t[i]);
        size_t brk = 0;
        if (c == '\r') brk = i + 1 < t.size() && t[i + 1] == '\n' ? 2 : 1;
        else if (c == '\n' || c == '\v' || c == '\f' || c == 0x1c || c == 0x1d || c == 0x1e || c == 0x85) brk = 1;
        if (!brk) {
            i++;
            continue;
        }
        out.push_back(t.substr(a, (keepends ? i + brk : i) - a));
        i += brk;
        a = i;
    }
    if (a < t.size()) out.push_back(t.substr(a));
    return out;
}
namespace {
bool py_space(uint8_t c) { return c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1c && c <= 0x1f) || c == 0x85 || c == 0xa0; }
}  // namespace
std::string pystrip(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && py_space(uint8_t(s[a]))) a++;
    while (b > a && py_space(uint8_t(s[b - 1]))) b--;
    return s.substr(a, b - a);
}
std::vector<std::string> split_ws(const std::string &s, int max) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && py_space(uint8_t(s[i]))) i++;
        if (i >= s.size()) break;
        if (max >= 0 && int(out.size()) == max) {   // the rest, with its trailing white space removed
            size_t e = s.size();
            while (e > i && py_space(uint8_t(s[e - 1]))) e--;
            out.push_back(s.substr(i, e - i));
            break;
        }
        size_t j = i;
        while (j < s.size() && !py_space(uint8_t(s[j]))) j++;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}
std::string latin1_utf8(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) o += char(c);
        else o += char(0xc0 | c >> 6), o += char(0x80 | (c & 0x3f));
    }
    return o;
}
std::string utf8_latin1(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = uint8_t(s[i]);
        if (c >= 0xc0 && c < 0xc4 && i + 1 < s.size()) o += char(((c & 3) << 6) | (uint8_t(s[++i]) & 0x3f));
        else o += char(c);
    }
    return o;
}
std::string py_repr(const std::string &s) {
    const bool dq = s.find('\'') != std::string::npos && s.find('"') == std::string::npos;
    const char q = dq ? '"' : '\'';
    std::string o(1, q);
    for (unsigned char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == uint8_t(q)) o += std::string("\\") + char(q);
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20 || (c >= 0x7f && c <= 0xa0) || c == 0xad) o += fmt("\\x%02x", c);
        else if (c >= 0x80) o += char(0xc0 | c >> 6), o += char(0x80 | (c & 0x3f));
        else o += char(c);
    }
    return o + q;
}
std::string py_list(const std::vector<std::string> &v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); i++) o += (i ? ", " : "") + py_repr(v[i]);
    return o + "]";
}
std::string basename(const std::string &key) {
    const size_t s = key.find_last_of("/\\");
    return s == std::string::npos ? key : key.substr(s + 1);
}

fs::path patcher_dir() { return exe_dir().parent_path() / "patcher"; }   // u2mp/bin/u2import.exe -> u2mp/patcher
fs::path launcher_root() { return exe_dir().parent_path().parent_path(); }

}  // namespace u2imp

namespace {

const char *CONV_VERSION = "1";        // bump when a converter changes its output: those files are converted again
const char *SKIN_VERSION = "twin2";    // mp-hero skins: PS4-twin build first; skins converted before it are redone
const char *ACTOR_VERSION = "u2ag2";   // actor27 paks: U2 artgroup / lookup / LEVEL_PATS layout, props, anims
// PS3 speech language -> PS4 speech archive (<name>_ps4.psarc). NPUA80698 is the North American release: its Spanish /
// Portuguese tracks are taken as the Latin American Spanish / Brazilian Portuguese the PS4 game has.
const std::map<std::string, std::string> SPEECH_LANGS = {{"english", "english"}, {"french", "french"}, {"spanish", "sam-spanish"}, {"portuguese", "brazilian"}};
const std::string OUT_MAIN = "u2data/build/main";
// PS3 sources of the GOTY release (NPUA80698 1.10: base archives + all-patches.psarc) and the map packs
bool goty(const std::string &source) { return source == "patch" || source == "base" || source == "dlc-map2" || source == "dlc-map1"; }

bool isfile(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
bool isdir(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
long long fsize(const fs::path &p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? -1 : (long long)n;
}
double seconds_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }
std::string join_notes(const std::vector<std::string> &v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? "; " : "") + v[i];
    return s;
}
std::string cut(const std::string &s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }
std::string text_of(const Value *v) { return v && v->kind == Value::String ? v->s : ""; }

// a JSON value with every object's keys sorted (json.dump(sort_keys=True))
Value sorted_keys(const Value &v) {
    Value o = v;
    if (o.kind == Value::Object) {
        std::stable_sort(o.fields.begin(), o.fields.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (auto &f : o.fields) f.second = sorted_keys(f.second);
    } else if (o.kind == Value::Array)
        for (auto &x : o.items) x = sorted_keys(x);
    return o;
}

struct Args {
    std::string ps4, ps3, ps3_dlc, ps3_data, out, groups = "A,B,C,D,E", speech_langs, list, only, ffmpeg, ps3_elf, objdump;
    bool dlc_ruins = false, no_dlc_ruins = false, drop_navmesh = false, no_verify = false, update = false, force = false, validate = false;
};

struct Row {
    std::map<std::string, std::string> f;
    const std::string &operator[](const std::string &k) const {
        static const std::string none;
        const auto it = f.find(k);
        return it == f.end() ? none : it->second;
    }
};

std::vector<Row> read_list(const fs::path &path) {
    std::vector<Row> rows;
    const std::string t = read_text(path);
    std::vector<std::string> lines;
    size_t a = 0;
    for (size_t i = 0; i <= t.size(); i++)
        if (i == t.size() || t[i] == '\n') {
            if (i > a || i < t.size()) lines.push_back(t.substr(a, i - a));
            a = i + 1;
        }
    if (lines.empty()) return rows;
    auto cells = [](std::string l) {
        if (!l.empty() && l.back() == '\r') l.pop_back();   // a CRLF checkout
        return split(l, '\t');
    };
    const auto head = cells(lines[0]);
    for (size_t i = 1; i < lines.size(); i++) {
        const auto v = cells(lines[i]);
        if (v.size() < head.size()) continue;
        Row r;
        for (size_t k = 0; k < head.size(); k++) r.f[head[k]] = v[k];
        rows.push_back(r);
    }
    return rows;
}

// the folder name of a PS3 title (4 capitals and 5 digits), the last one in the path
std::string title_id(const std::string &path) {
    std::string p = S(fs::absolute(P(path)));
    std::replace(p.begin(), p.end(), '\\', '/');
    const auto parts = split(p, '/');
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        const std::string &x = *it;
        if (x.size() == 9 && std::all_of(x.begin(), x.begin() + 4, [](char c) { return c >= 'A' && c <= 'Z'; }) &&
            std::all_of(x.begin() + 4, x.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return x;
    }
    return "";
}

std::vector<std::string> listdir_sorted(const fs::path &d) {
    std::vector<std::string> v;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(d, ec)) v.push_back(S(e.path().filename()));
    std::sort(v.begin(), v.end());
    return v;
}
std::string upper(std::string s) {
    for (char &c : s) c = char(toupper(uint8_t(c)));
    return s;
}

class Importer {
public:
    explicit Importer(const Args &a) : a(a) {
        out = fs::absolute(P(a.out));
        main = out / P(OUT_MAIN);
        work = out / "work";
        state_file = out / "state.json";
        if (auto s = try_json(state_file); s && s->is_object()) state = *s;
        else state = Value::object();
        prev_status = text_of(state.get("status"));
        if (!state.get("items") || !state.get("items")->is_object()) state.set("items", Value::object());
    }
    int run();

private:
    Args a;
    fs::path out, main, work, state_file;
    Value state;
    std::string prev_status;
    std::unique_ptr<PS3> ps3;
    std::unique_ptr<PS4> ps4;
    std::set<std::string> music_refs_, langs;
    std::string ffmpeg;
    bool dlc_ruins = false;
    std::vector<Row> rows;
    std::map<std::string, std::string> level_txt;   // level -> its PS3 .txt (latin-1)
    std::string exe_digest, base_digest;
    FILE *logf = nullptr;
    Context ctx;

    Value &items() { return *state.get("items"); }
    Value *item(const std::string &key) { return items().get(key); }

    void lg(const std::string &text) {
        log(text);
        if (logf) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            std::fprintf(logf, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, text.c_str());
            std::fflush(logf);
        }
    }

    void save() {
        const fs::path tmp = S(state_file) + ".tmp";
        write_text(tmp, dump_indent1(sorted_keys(state)));
        // a sync client or a virus scanner may hold state.json for a moment: retry
        for (int i = 0; i < 40; i++) {
            if (MoveFileExW(tmp.wstring().c_str(), state_file.wstring().c_str(), MOVEFILE_REPLACE_EXISTING)) return;
            if (i == 39) throw std::runtime_error("PermissionError: cannot replace " + S(state_file));
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    fs::path out_path(const std::string &rel) {
        const fs::path p = main / P(rel);
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        return p;
    }

    void set_item(const std::string &key, const std::string &status, const std::string &group, std::vector<std::pair<std::string, Value>> kw) {
        Value it = Value::object();
        it.set("status", str(status));
        it.set("group", str(group));
        for (auto &[k, v] : kw) it.set(k, v);
        it.set("conv", str(CONV_VERSION));
        items().set(key, it);
    }

    bool done_before(const std::string &key, const std::string &src_sha) {
        const Value *it = item(key);
        if (a.force || !it || text_of(it->get("conv")) != CONV_VERSION || text_of(it->get("src_sha1")) != src_sha) return false;
        const std::string k = lower(key);
        if (starts(k, "actor27/mp-hero") && text_of(it->get("skin")) != SKIN_VERSION) return false;
        if (starts(k, "actor27/") && text_of(it->get("actor")) != ACTOR_VERSION) return false;
        if (text_of(it->get("status")) != "ok") return false;
        const fs::path o = out / P(text_of(it->get("out")));
        const Value *sz = it->get("size");
        return isfile(o) && sz && sz->kind == Value::Int && fsize(o) == sz->i;
    }

    // a converted file: status ok, its output and size
    void record(const std::string &key, const std::string &group, const std::string &rel, std::vector<std::pair<std::string, Value>> kw) {
        const std::string o = OUT_MAIN + "/" + rel;
        kw.insert(kw.begin(), {"out", str(o)});
        set_item(key, "ok", group, kw);
        Value &it = *item(key);
        const std::string k = lower(key);
        if (starts(k, "actor27/mp-hero")) it.set("skin", str(SKIN_VERSION));
        if (starts(k, "actor27/")) it.set("actor", str(ACTOR_VERSION));
        it.set("size", num(fsize(out / P(o))));
    }

    // (entry, bytes) of the PS3 file of a row, checked by SHA-1; throws std::out_of_range with the reason
    std::pair<const Entry *, Bytes> source(const Row &row) {
        auto cands = ps3->candidates(row["key"]);
        if (cands.empty()) throw std::out_of_range("not in the given PS3 folders (needs " + row["ps3_source"] + ")");
        const std::string want = row["sha1"];
        std::vector<std::string> mism;
        // the listed source first, then the GOTY copies, the BCUS title updates (dlc-patch*) last
        std::stable_sort(cands.begin(), cands.end(), [&](const Entry *x, const Entry *y) {
            const auto kx = std::make_pair(x->source != row["ps3_source"], !goty(x->source)), ky = std::make_pair(y->source != row["ps3_source"], !goty(y->source));
            return kx < ky;
        });
        for (const Entry *e : cands) {
            Bytes data = e->read();
            if (want.empty() || a.no_verify || sha1_hex(data) == want) return {e, std::move(data)};
            mism.push_back(e->where());
        }
        std::string w;
        for (size_t i = 0; i < mism.size() && i < 3; i++) w += (i ? "; " : "") + mism[i];
        throw std::out_of_range("SHA-1 differs from the import list in " + w);
    }

    // "" when PS4 lacks the file, else why it stays the PS4 one
    std::string ps4_has(const Row &row) {
        const std::string k = lower(row["key"]);
        if (starts(k, "sound1/speech/")) {
            const std::string lang = split(k, '/')[2];
            const auto l4 = SPEECH_LANGS.find(lang);
            if (l4 == SPEECH_LANGS.end() || !ps4->speech.count(l4->second)) return "no PS4 speech language for " + lang;
            if (!langs.count(l4->second)) return "language not selected";
            return ps4->speech.at(l4->second).count(basename(k)) ? "PS4 " + l4->second + " speech has it" : "";
        }
        if (starts(k, "sound1/music/")) {
            if (ps4->has(k)) return "PS4 has it";
            const auto name = music_name(basename(k).substr(0, basename(k).size() - 5));
            if (!name) return "PS4 music settings do not use it";
            if (ps4->has("sound1/music/" + *name + ".xvag")) return "PS4 music settings do not use it (PS4 plays its own renumbered copy " + *name + ")";
            return "";
        }
        if ((k == "pak22/ruins-multi.pak" || k == "pak22/ruins-multi-ingame10.pak") && dlc_ruins) return "";
        if (ends(k, ".txt") && starts(k, "pak22/")) return "";
        return ps4->has(k) ? "PS4 has it" : "";
    }

    // the track name the PS4 music settings use for a PS3 track: the same name, else the one name with the same title
    // after the track number (PS4 renumbered the tracks)
    std::optional<std::string> music_name(const std::string &stem) {
        const std::string s = lower(stem);
        for (const std::string &r : music_refs_)
            if (lower(r) == s) return r;
        auto title = [](const std::string &x) {
            if (x.size() > 1) {
                size_t i = 0;
                while (i < x.size() && isdigit(uint8_t(x[i]))) i++;
                if (i > 0 && i < x.size() && x[i] == '_') return x.substr(i + 1);
            }
            return x;
        };
        std::vector<std::string> same;
        for (const std::string &r : music_refs_)
            if (title(lower(r)) == title(s)) same.push_back(r);
        if (same.size() == 1) return same[0];
        return std::nullopt;
    }

    // ---------------------------------------------------------------------------------------------- converters
    Converted conv_xvag(const Row &row, const Entry &entry, const Bytes &data) {
        const std::string k = row["key"];
        std::string rel;
        if (starts(lower(k), "sound1/speech/")) rel = "sound1/speech/" + SPEECH_LANGS.at(split(lower(k), '/')[2]) + "/" + basename(entry.key);
        else if (starts(lower(k), "sound1/music/")) {
            const std::string b = basename(entry.key);
            rel = "sound1/music/" + music_name(b.substr(0, b.size() - 5)).value_or("None") + ".xvag";
        } else
            rel = k.substr(0, k.rfind('/')) + "/" + basename(entry.key);
        write_bytes(out_path(rel), xvag_convert(data));
        return {rel, {}};
    }

    Converted conv_bank(const Row &, const Entry &entry, const Bytes &data) {
        std::string note;
        const Bytes o = bank_convert(data, ffmpeg, note);
        const std::string rel = "sound1/banks/ps4/" + basename(entry.key);
        write_bytes(out_path(rel), o);
        return {rel, {note}};
    }

    fs::path src_file(const Entry &entry, const Bytes &data) {
        const fs::path d = work / "src";
        std::error_code ec;
        fs::create_directories(d, ec);
        const fs::path p = d / P(basename(entry.key));
        if (!(isfile(p) && fsize(p) == (long long)data.size())) write_bytes(p, data);
        return p;
    }

    Converted conv_actor(const Row &, const Entry &entry, const Bytes &data) { return convert_actor(ctx, entry, src_file(entry, data)); }

    Converted conv_level(const Row &, const Entry &entry, const Bytes &data) {
        const fs::path src = src_file(entry, data);
        const std::string rel = "pak22/ps4/" + basename(entry.key);
        return {rel, convert_level(ctx, src, out_path(rel))};
    }

    // ---------------------------------------------------------------------------------------------- steps
    std::string step_fingerprint(const std::string &sid, bool first);
    bool step_outputs_ok(const std::string &sid);
    void step_items();
    std::vector<fs::path> ps3_data_roots();
    std::vector<fs::path> ps3_data_candidates();
    std::optional<fs::path> ps3_data_pak22();
    std::optional<fs::path> ps3_data_file(const std::string &name);
    void net_spawn_paks();
    void net_patch();
    void net_images();
    void registries();
    void texts();
    int finish(const std::string &status, std::vector<std::pair<std::string, Value>> kw = {});
};

int Importer::run() {
    std::error_code ec;
    fs::create_directories(out, ec);
    logf = _wfopen((out / "import.log").wstring().c_str(), L"ab");
    lg("=== u2mp import " + now_text() + " (converter " + CONV_VERSION + ")");
    state.set("status", str("running"));
    state.set("started", str(now_text()));
    state.set("pid", num(GetCurrentProcessId()));
    state.set("inputs", obj({{"ps4", str(a.ps4)}, {"ps3", str(a.ps3)}, {"ps3_dlc", str(a.ps3_dlc)}}));
    state.set("conv", str(CONV_VERSION));
    state.set("updating", Value::boolean(a.update));
    state.remove("error");
    state.remove("failed");
    save();
    try {
        ps4 = std::make_unique<PS4>(a.ps4);
        ps3 = std::make_unique<PS3>(a.ps3, a.ps3_dlc);
    } catch (const std::runtime_error &e) {
        return finish("failed", {{"error", str(e.what())}});
    }
    std::string arcs;
    for (size_t i = 0; i < ps3->archives.size(); i++) arcs += (i ? ", " : "") + basename(ps3->archives[i]);
    lg(fmt("PS3 archives: %zu (%s)", ps3->archives.size(), arcs.c_str()));
    // the decrypted PS3 EBOOT.elf is required: no import without it
    const std::string elf = !a.ps3_elf.empty() ? a.ps3_elf : S(ps3->base_main.parent_path().parent_path() / "EBOOT.elf");
    bool ok = false;
    if (FILE *f = _wfopen(P(elf).wstring().c_str(), L"rb")) {
        char m[4] = {};
        ok = std::fread(m, 1, 4, f) == 4 && !std::memcmp(m, "\x7f" "ELF", 4);
        std::fclose(f);
    }
    if (!ok) {
        const std::string why = "No decrypted PS3 Uncharted 2 EBOOT.elf of your own copy of the game: add it, then import again";
        if (a.update && (prev_status == "imported" || prev_status == "partial")) {
            // an automatic update of a working import: not applied, the current files stay playable
            state.set("update_failed_for", str(shipped_id_text()));
            return finish(prev_status, {{"error", str("Update not applied: " + why)}});
        }
        return finish("failed", {{"error", str(why)}});
    }
    lg("PS3 EBOOT.elf: " + elf);
    music_refs_ = ps4->music_refs();
    if (!a.speech_langs.empty())
        for (const std::string &l : split(a.speech_langs, ',')) langs.insert(strip(l));
    else
        for (const auto &[k, v] : SPEECH_LANGS) langs.insert(v);
    dlc_ruins = a.dlc_ruins && !a.ps3_dlc.empty() && !a.no_dlc_ruins;
    ffmpeg = find_ffmpeg(a.ffmpeg);
    lg("ffmpeg: " + (ffmpeg.empty() ? std::string("not found (banks with MP3 samples are skipped)") : ffmpeg));
    ctx.ps3 = ps3.get();
    ctx.ps4 = ps4.get();
    ctx.work = work;
    ctx.main = main;
    ctx.ps4_game = a.ps4;
    ctx.ps3_elf = elf;
    ctx.objdump = a.objdump;
    ctx.drop_navmesh = a.drop_navmesh;
    std::set<char> groups;
    for (const std::string &g : split(a.groups, ','))
        if (!strip(g).empty()) groups.insert(char(toupper(uint8_t(strip(g)[0]))));
    for (const Row &r : read_list(P(a.list)))
        if (!r["group"].empty() && groups.count(r["group"][0])) rows.push_back(r);
    if (!a.only.empty()) {
        const std::regex re(a.only);
        std::vector<Row> keep;
        for (const Row &r : rows)
            if (std::regex_search(r["key"], re)) keep.push_back(r);
        rows = keep;
    }
    auto order = [](const Row &r) {
        const std::string top = split(r["key"], '/')[0];
        return top == "sound1" ? 0 : top == "actor27" ? 1 : top == "pak22" ? 2 : 3;
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const Row &x, const Row &y) {
        return std::make_tuple(order(x), x["group"], x["key"]) < std::make_tuple(order(y), y["group"], y["key"]);
    });
    std::string gl;
    for (char g : groups) gl += (gl.empty() ? "" : ",") + std::string(1, g);
    lg(fmt("%zu import-list rows in groups %s", rows.size(), gl.c_str()));
    exe_digest = sha256_file(exe_dir() / "u2import.exe");
    const auto t0 = std::chrono::steady_clock::now();
    struct Step {
        std::string id, title;
        std::function<void()> fn;
        bool first;
    };
    const std::vector<Step> steps = {
        {"import-list", fmt("Files from the PS3 copy: sounds, skins, actors and levels (%zu rows)", rows.size()), [this] { step_items(); }, true},
        {"net-images", "Menu images of the multiplayer maps", [this] { net_images(); }, false},
        {"net-spawn-paks", "Spawn paks of the multiplayer maps (PS3 game data)", [this] { net_spawn_paks(); }, false},
        {"net-patch", "net-patch9: the PS3 server live-data DC module", [this] { net_patch(); }, false},
        {"registries", "pak22_ps4.txt and banks.txt: the imported levels and banks", [this] { registries(); }, false},
        {"texts", "Text banks with the multiplayer strings", [this] { texts(); }, false},
    };
    if (!state.get("steps") || !state.get("steps")->is_object()) state.set("steps", Value::object());
    std::vector<std::string> failed;
    for (size_t i = 0; i < steps.size(); i++) {
        const Step &s = steps[i];
        event(obj({{"phase", str("recipe")}, {"step", num((long long)(i + 1))}, {"total", num((long long)(steps.size()))}, {"id", str(s.id)}, {"title", str(s.title)},
                   {"status", str("running")}}));
        lg(fmt("[%zu/%zu] %s - %s", i + 1, steps.size(), s.id.c_str(), s.title.c_str()));
        const std::string fp = step_fingerprint(s.id, s.first);
        Value *old = state.get("steps")->get(s.id);
        if (!a.force && old && text_of(old->get("fp")) == fp && text_of(old->get("status")) == "ok" && step_outputs_ok(s.id)) {
            lg("        no change (already applied)");
            event(obj({{"phase", str("recipe")}, {"id", str(s.id)}, {"status", str("ok")}, {"note", str("no change")}}));
            continue;
        }
        const auto t = std::chrono::steady_clock::now();
        try {
            s.fn();
            const double sec = seconds_since(t);
            Value r = obj({{"fp", str(fp)}, {"status", str("ok")}});
            Value secs;
            secs.kind = Value::Float;
            secs.f = std::round(sec * 10) / 10;
            r.set("seconds", secs);
            state.get("steps")->set(s.id, r);
            lg(fmt("        ok  %.1f s", sec));
            event(obj({{"phase", str("recipe")}, {"id", str(s.id)}, {"status", str("ok")}, {"note", str(fmt("ok  %.1f s", sec))}}));
        } catch (const std::exception &e) {   // one step failing does not stop the others
            failed.push_back(s.id);
            const std::string note = cut(e.what(), 400);
            state.get("steps")->set(s.id, obj({{"fp", str(fp)}, {"status", str("failed")}, {"note", str(note)}}));
            lg(fmt("        FAILED after %.1f s: %s", seconds_since(t), cut(e.what(), 300).c_str()));
            event(obj({{"phase", str("recipe")}, {"id", str(s.id)}, {"status", str("failed")}, {"note", str(note)}}));
        }
        save();
    }
    std::map<std::string, long long> counts;
    for (const auto &[k, it] : items().fields) counts[text_of(it.get("status"))]++;
    Value c = Value::object();
    std::string cl;
    for (const auto &[k, n] : counts) {
        c.set(k, num(n));
        cl += (cl.empty() ? "" : ", ") + k + " " + std::to_string(n);
    }
    state.set("counts", c);
    std::string fl;
    for (const std::string &f : failed) fl += (fl.empty() ? "" : ", ") + f;
    lg(fmt("done in %.0f s: %s; steps failed: %s", seconds_since(t0), cl.c_str(), fl.empty() ? "none" : fl.c_str()));
    if (a.validate) state.set("validation", validate(*ps3, *ps4, ctx, work / "validate", [this](const std::string &s) { lg(s); }));
    state.set("recipe_id", str(shipped_id_text()));
    Value fa = Value::object();
    fa.kind = Value::Array;
    for (const std::string &f : failed) fa.items.push_back(str(f));
    if (std::find(failed.begin(), failed.end(), "import-list") != failed.end())
        return finish("failed", {{"failed", fa}, {"error", str("step import-list failed: see the import log")}});
    const long long bad = counts["failed"] + counts["missing"];
    return finish(bad || counts["unsupported"] || !failed.empty() ? "partial" : "imported", {{"failed", fa}});
}

// sha256 over the step id, this program, the inputs and options, and (first step) the import list or (later steps) what
// the first step made: its item records and the level lists, taken once right after it
std::string Importer::step_fingerprint(const std::string &sid, bool first) {
    Sha256 h;
    h.update(sid.data(), sid.size());
    h.update("\0", 1);
    h.update(exe_digest.data(), exe_digest.size());
    Value arcs = Value::object();
    arcs.kind = Value::Array;
    for (const std::string &p : ps3->archives) {
        std::error_code ec;
        const auto t = fs::last_write_time(P(p), ec);
        arcs.items.push_back(obj({{"name", str(basename(p))}, {"size", num(fsize(P(p)))}, {"mtime", num(ec ? 0 : (long long)t.time_since_epoch().count())}}));
    }
    const Value opts = obj({{"conv", str(CONV_VERSION)}, {"skin", str(SKIN_VERSION)}, {"actor", str(ACTOR_VERSION)}, {"ps4", str(a.ps4)}, {"ps3", str(a.ps3)},
                            {"dlc", str(a.ps3_dlc)}, {"data", str(a.ps3_data)}, {"elf", str(a.ps3_elf)}, {"groups", str(a.groups)},
                            {"langs", str(a.speech_langs)}, {"dlc_ruins", Value::boolean(a.dlc_ruins)}, {"no_dlc_ruins", Value::boolean(a.no_dlc_ruins)},
                            {"navmesh", Value::boolean(a.drop_navmesh)}, {"only", str(a.only)}, {"no_verify", Value::boolean(a.no_verify)}, {"archives", arcs}});
    const std::string o = dumps(opts, true);
    h.update(o.data(), o.size());
    if (first) {
        std::string t = read_text(P(a.list));
        t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
        const std::string d = sha256_hex(t);
        h.update(d.data(), d.size());
    } else {
        if (base_digest.empty()) {
            Value its = Value::object();
            for (const Row &r : rows)
                if (const Value *it = item(r["key"])) its.set(r["key"], *it);
            Value lt = Value::object();
            for (const auto &[k, v] : level_txt) lt.set(k, str(latin1_utf8(v)));
            Value both = Value::object();
            both.kind = Value::Array;
            both.items = {its, lt};
            base_digest = sha256_hex(dumps(both, true));
        }
        h.update(base_digest.data(), base_digest.size());
    }
    return h.hex();
}

// a skipped step must still have its files: the import-list step's converted files and its level lists (the later
// steps write few files and are cheap to redo)
bool Importer::step_outputs_ok(const std::string &sid) {
    if (sid != "import-list") return true;
    for (const auto &[k, it] : items().fields)
        if (text_of(it.get("status")) == "ok" && !text_of(it.get("out")).empty()) {
            const fs::path p = out / P(text_of(it.get("out")));
            const Value *sz = it.get("size");
            if (!isfile(p) || !sz || fsize(p) != sz->i) return false;
        }
    const auto lt = try_json(work / "level-txt.json");
    if (!lt || !lt->is_object()) return false;
    level_txt.clear();
    for (const auto &[k, v] : lt->fields) level_txt[k] = utf8_latin1(v.s);
    return true;
}

// step import-list: every import-list row the PS4 game lacks, converted (unchanged inputs keep their files)
void Importer::step_items() {
    level_txt.clear();
    for (size_t n = 0; n < rows.size(); n++) {
        const Row &row = rows[n];
        const std::string key = row["key"], kl = lower(key), group = row["group"];
        if (n % 50 == 0 || !starts(kl, "sound1/speech/"))
            event(obj({{"phase", str("item")}, {"n", num((long long)(n + 1))}, {"total", num((long long)(rows.size()))}, {"key", str(key)}}));
        const std::string have = ps4_has(row);
        if (!have.empty()) {
            const std::string st = starts(have, "PS4 ") && have.find("do not use") == std::string::npos ? "ps4" : "skipped";
            set_item(key, st, group, {{"note", str(have)}});
            continue;
        }
        const Entry *entry = nullptr;
        Bytes data;
        try {
            std::tie(entry, data) = source(row);
        } catch (const std::out_of_range &e) {
            if ((group[0] == 'C' || group[0] == 'D') && a.ps3_dlc.empty() && ps3->candidates(key).empty()) {
                // map pack rows without --ps3-dlc (the launcher's GOTY-only import): not part of this import
                set_item(key, "skipped", group, {{"note", str(std::string("map pack ") + group[0] + " not given (--ps3-dlc): GOTY-only import")}});
                continue;
            }
            set_item(key, "missing", group, {{"note", str(e.what())}});
            lg("MISSING " + key + ": " + e.what());
            continue;
        }
        const std::string src_sha = !row["sha1"].empty() ? row["sha1"] : sha1_hex(data);
        if (ends(kl, ".txt") && starts(kl, "pak22/")) {
            const std::string b = basename(key);
            level_txt[b.substr(0, b.size() - 4)] = std::string(data.begin(), data.end());
            set_item(key, "ok", group, {{"note", str("registry input")}, {"src", str(entry->source)}, {"src_sha1", str(src_sha)}});
            continue;
        }
        if (done_before(key, src_sha)) {
            item(key)->set("src", str(entry->source));   // same bytes; the archive they are read from now
            continue;
        }
        std::function<Converted(const Row &, const Entry &, const Bytes &)> fn;
        if (ends(kl, ".xvag")) fn = [this](const Row &r, const Entry &e, const Bytes &d) { return conv_xvag(r, e, d); };
        else if (ends(kl, ".bnk")) fn = [this](const Row &r, const Entry &e, const Bytes &d) { return conv_bank(r, e, d); };
        else if (starts(kl, "actor27/") && ends(kl, ".pak")) fn = [this](const Row &r, const Entry &e, const Bytes &d) { return conv_actor(r, e, d); };
        else if (starts(kl, "pak22/") && ends(kl, ".pak")) fn = [this](const Row &r, const Entry &e, const Bytes &d) { return conv_level(r, e, d); };
        else {
            set_item(key, "skipped", group, {{"note", str("optional (" + split(key, '/')[0] + "): not referenced by the PS4 game")}});
            continue;
        }
        const auto t = std::chrono::steady_clock::now();
        try {
            const Converted c = fn(row, *entry, data);
            const std::string notes = join_notes(c.notes);
            record(key, group, c.rel, {{"src", str(entry->source)}, {"src_sha1", str(src_sha)}, {"note", str(cut(notes, 400))}});
            if (!starts(kl, "sound1/speech/")) lg(fmt("ok      %s <- %s (%.1fs) %s", key.c_str(), entry->source.c_str(), seconds_since(t), cut(notes, 200).c_str()));
        } catch (const Unsupported &e) {
            set_item(key, "unsupported", group, {{"note", str(cut(e.what(), 400))}, {"src", str(entry->source)}});
            lg("UNSUPPORTED " + key + ": " + e.what());
        } catch (const std::exception &e) {
            set_item(key, "failed", group, {{"src", str(entry->source)}, {"note", str(cut(e.what(), 400))}});
            lg("FAILED " + key + ": " + cut(e.what(), 300));
        }
        if (n % 20 == 0) save();
    }
    std::error_code ec;
    fs::create_directories(work, ec);
    Value lt = Value::object();
    for (const auto &[k, v] : level_txt) lt.set(k, str(latin1_utf8(v)));
    write_text(work / "level-txt.json", dumps(lt));
    save();
}

// --ps3-data as given, plus (when it is RPCS3's dev_hdd0/game or holds <id>DATA2 folders) the title's own DATA2 folder
// first and then the other DATA2 folders in it
std::vector<fs::path> Importer::ps3_data_roots() {
    std::vector<fs::path> roots;
    const fs::path d = P(a.ps3_data);
    if (a.ps3_data.empty() || !isdir(d)) return roots;
    roots.push_back(d);
    const std::string title = title_id(a.ps3);
    for (const fs::path &base : {d, d / "game", d / "dev_hdd0" / "game"}) {
        if (!isdir(base)) continue;
        std::vector<std::string> subs;
        for (const std::string &x : listdir_sorted(base))
            if (ends(upper(x), "DATA2") && isdir(base / P(x))) subs.push_back(x);
        if (!title.empty() && std::find(subs.begin(), subs.end(), title + "DATA2") != subs.end()) roots.push_back(base / P(title + "DATA2"));
        for (const std::string &x : subs)
            if (x != title + "DATA2") roots.push_back(base / P(x));
    }
    return roots;
}

// the data roots, then (from the PS3 folders up to dev_hdd0/game) the title's DATA2 folder and every other DATA2 folder
std::vector<fs::path> Importer::ps3_data_candidates() {
    std::vector<fs::path> cands = ps3_data_roots();
    const std::string title = title_id(a.ps3);
    for (const std::string &root : {a.ps3_dlc, a.ps3}) {
        if (root.empty()) continue;
        fs::path p = fs::absolute(P(root));
        for (int i = 0; i < 6; i++) {
            if (lower(S(p.filename())) == "game") {
                if (!title.empty()) cands.push_back(p / P(title + "DATA2"));
                for (const std::string &x : listdir_sorted(p))
                    if (ends(upper(x), "DATA2")) cands.push_back(p / P(x));
                break;
            }
            p = p.parent_path();
        }
    }
    return cands;
}

// pak22 of the PS3 game-data folder (what PS3 1.10 downloaded from the server). GOTY's own NPUA80698DATA2 has none (the
// server is offline); the packages are the server's map data, byte-identical in BCUS98123DATA2 and BCES00757DATA2.
std::optional<fs::path> Importer::ps3_data_pak22() {
    for (const fs::path &c : ps3_data_candidates())
        for (const std::string sub : {"", "USRDIR", "USRDIR/build/main", "build/main"}) {
            const fs::path d = !sub.empty() || !ends(lower(S(c)), "pak22") ? (sub.empty() ? c / "pak22" : c / P(sub) / "pak22") : c;
            if (!isdir(d)) continue;
            for (const std::string &f : listdir_sorted(d))
                if (ends(f, "-pak-ingame10.pak")) return d;
        }
    return std::nullopt;
}

std::optional<fs::path> Importer::ps3_data_file(const std::string &name) {
    for (const fs::path &c : ps3_data_candidates())
        for (const fs::path &f : {c / P(name), c / "USRDIR" / P(name)})
            if (isfile(f)) return f;
    return std::nullopt;
}

// pak22/ps4/<map>-pak-ingame10.pak: each multiplayer map's spawners, CTF flags and territories. The game asks for the
// level '<map>-pak'; PS3 1.10 downloaded these packages as 1-<map>-pak-ingame10.pak (neither disc has them, PS4 ships only
// stage-multi-pak-ingame10). Converted like any -ingame10 data pak, registered as level '<map>-pak'.
void Importer::net_spawn_paks() {
    const auto d = ps3_data_pak22();
    std::vector<std::string> found;
    if (d)
        for (const std::string &f : listdir_sorted(*d))
            if (ends(f, "-pak-ingame10.pak")) found.push_back(f);
    if (d) lg("net spawn paks: from " + S(*d) + " (server map data, the same for every PS3 version)");
    else lg("net spawn paks: no PS3 game-data folder with <map>-pak-ingame10.pak (set --ps3-data): multiplayer maps have no player spawners");
    for (const std::string &f : found) {
        // optional "<digits>-" in front, then "<level>-pak-ingame10.pak"
        std::string n = f;
        size_t i = 0;
        while (i < n.size() && isdigit(uint8_t(n[i]))) i++;
        if (i > 0 && i < n.size() && n[i] == '-') n = n.substr(i + 1);
        const std::string level = n.substr(0, n.size() - std::string("-ingame10.pak").size());
        if (level.size() <= 4) continue;
        const std::string key = "pak22/" + level + "-ingame10.pak", rel = "pak22/ps4/" + level + "-ingame10.pak";
        if (ps4->has(key)) {   // stage-multi-pak: PS4 ships it
            level_txt[level] = "level-id " + level + "\npackage " + level + "-ingame\n";
            set_item(key, "ps4", "A", {{"note", str("PS4 has it")}});
            continue;
        }
        const fs::path src = *d / P(f);
        const std::string src_sha = sha1_hex(read_bytes(src));
        if (!done_before(key, src_sha)) {
            const auto t = std::chrono::steady_clock::now();
            std::vector<std::string> notes;
            try {
                notes = convert_level(ctx, src, out_path(rel));
            } catch (const std::exception &e) {
                set_item(key, "failed", "A", {{"src", str("ps3-data")}, {"note", str(cut(e.what(), 400))}});
                lg("FAILED " + key + ": " + cut(e.what(), 300));
                continue;
            }
            record(key, "A", rel, {{"src", str("ps3-data")}, {"src_sha1", str(src_sha)}, {"note", str(cut(join_notes(notes), 400))}});
            lg(fmt("ok      %s <- ps3-data %s (%.1fs) %s", key.c_str(), f.c_str(), seconds_since(t), notes.empty() ? "" : notes[0].c_str()));
        }
        if (isfile(out_path(rel))) level_txt[level] = "level-id " + level + "\npackage " + level + "-ingame\n";
    }
    size_t maps = 0;
    for (const auto &[k, v] : level_txt) maps += ends(k, "-pak");
    lg(fmt("net spawn paks: %zu maps", maps));
    save();
}

// dc1/ps4/net-patch9.bin: the PS3 server live-data DC module (weapon sets the PS4 net.bin names but does not define,
// playlists ...). PS3 1.10 downloaded it into the game-data folder and bound its symbols at NetInit; the runtime
// (netpatch.cpp) loads the converted file the same way.
void Importer::net_patch() {
    const std::string key = "dc1/net-patch9.bin", rel = "dc1/ps4/net-patch9.bin";
    const auto src = ps3_data_file("net-patch9.bin");
    if (!src) {
        set_item(key, "missing", "A", {{"note", str("no PS3 game-data folder with net-patch9.bin (NPUA80698DATA2; set --ps3-data)")}});
        lg("MISSING " + key + ": no PS3 game-data folder with net-patch9.bin (start the PS3 GOTY once in RPCS3 or set --ps3-data): match start asserts on weapon set 0x8cf8e1cc");
        return;
    }
    const Bytes data = read_bytes(*src);
    const std::string src_sha = sha1_hex(data);
    if (done_before(key, src_sha) && text_of(item(key)->get("dc")) == NETPATCH_VERSION) return;
    const auto t = std::chrono::steady_clock::now();
    Bytes conv;
    std::string note;
    try {
        conv = convert_net_patch(ctx, data, note);
    } catch (const std::exception &e) {
        set_item(key, "failed", "A", {{"src", str("ps3-data")}, {"note", str(cut(e.what(), 400))}});
        lg("FAILED " + key + ": " + cut(e.what(), 300));
        return;
    }
    write_bytes(out_path(rel), conv);
    record(key, "A", rel, {{"src", str("ps3-data")}, {"src_sha1", str(src_sha)}, {"note", str(note)}, {"dc", str(NETPATCH_VERSION)}});
    lg(fmt("ok      %s <- %s (%.1fs) %s", key.c_str(), S(*src).c_str(), seconds_since(t), note.c_str()));
    save();
}

// pak22/ps4/net-u2mp.pak: the PS3 net.pak menu images the PS4 net.pak lacks, registered as its own level / package
void Importer::net_images() {
    const std::string out_name = NETIMG_OUT_NAME, key = "pak22/" + out_name;
    // the GOTY all-patches net.pak first (the newer BCUS dlc-patch7 one adds only images nothing names)
    auto c = ps3->candidates("pak22/net.pak");
    std::stable_sort(c.begin(), c.end(), [](const Entry *x, const Entry *y) { return goty(x->source) > goty(y->source); });
    const fs::path ps4_net = ps4->pak_dir / "net.pak";
    if (c.empty() || !isfile(ps4_net)) {
        set_item(key, "missing", "A", {{"note", str("no PS3 or PS4 net.pak")}});
        return;
    }
    const Bytes data = c[0]->read();
    const std::string src_sha = sha1_hex(data);
    const fs::path o = out_path("pak22/ps4/" + out_name);
    if (!done_before(key, src_sha) || !item(key)->get("netimg") || item(key)->get("netimg")->kind != Value::Int || item(key)->get("netimg")->i != NETIMG_VERSION) {
        const fs::path src = work / "ps3-net.pak";
        std::error_code ec;
        fs::create_directories(work, ec);
        write_bytes(src, data);
        const auto t = std::chrono::steady_clock::now();
        std::vector<std::string> names;
        try {
            names = convert_net_images(src, ps4_net, o);
        } catch (const std::exception &e) {
            fs::remove(src, ec);
            set_item(key, "failed", "A", {{"src", str(c[0]->source)}, {"note", str(cut(e.what(), 400))}});
            lg("FAILED " + key + ": " + cut(e.what(), 300));
            return;
        }
        fs::remove(src, ec);
        if (names.empty()) return;
        record(key, "A", "pak22/ps4/" + out_name,
               {{"src", str(c[0]->source)}, {"src_sha1", str(src_sha)}, {"note", str(fmt("%zu menu images", names.size()))}, {"netimg", num(NETIMG_VERSION)}});
        lg(fmt("ok      %s <- %s (%.1fs) %zu menu images", key.c_str(), c[0]->source.c_str(), seconds_since(t), names.size()));
    }
    if (isfile(o)) {
        const std::string lv = out_name.substr(0, out_name.size() - 4);
        level_txt[lv] = "level-id " + lv + "\npackage " + lv + "\n";
    }
}

// pak22_ps4.txt and banks.txt of the overlay (from the PS4 files + what was converted)
void Importer::registries() {
    // the levels of the net-images / net-spawn-paks steps: a resumed import skips those steps when nothing changed, so
    // they come back from the item records here
    std::map<std::string, std::string> lt = level_txt;
    for (const auto &[k, it] : items().fields) {
        if (!starts(k, "pak22/") || !ends(k, "-pak-ingame10.pak")) continue;
        const std::string level = k.substr(6, k.size() - 6 - std::string("-ingame10.pak").size());
        const std::string st = text_of(it.get("status"));
        if ((st == "ok" || st == "ps4") && (st == "ps4" || isfile(out / P(text_of(it.get("out"))))))
            lt.emplace(level, "level-id " + level + "\npackage " + level + "-ingame\n");
    }
    const std::string net_u2mp = std::string(NETIMG_OUT_NAME).substr(0, std::string(NETIMG_OUT_NAME).size() - 4);
    if (isfile(main / "pak22" / "ps4" / NETIMG_OUT_NAME)) lt.emplace(net_u2mp, "level-id " + net_u2mp + "\npackage " + net_u2mp + "\n");
    auto in_overlay = [this](const std::string &rel) { return isfile(main / P(rel)); };
    auto have_pak = [&](const std::string &name) {
        for (const std::string &n : package_file_names(name))
            if (ps4->has("pak22/" + n + ".pak") || in_overlay("pak22/ps4/" + n + ".pak")) return true;
        return false;
    };
    auto have_actor = [&](const std::string &name) { return ps4->has("actor27/" + name + ".pak") || in_overlay("actor27/ps4/" + name + ".pak"); };
    auto have_bank = [&](const std::string &name) { return ps4->has("sound1/banks/" + name + ".bnk") || in_overlay("sound1/banks/ps4/" + name + ".bnk"); };
    const std::string text = read_text(ps4->registry_path);
    std::set<std::string> failed;
    for (const auto &[k, it] : items().fields) {
        const std::string st = text_of(it.get("status"));
        if (starts(k, "pak22/") && ends(k, ".pak") && (st == "unsupported" || st == "failed" || st == "missing")) {
            const std::string b = basename(k);
            failed.insert(b.substr(0, b.size() - 4));
        }
    }
    const RegistryResult r = build_registry(text, lt, have_pak, have_actor, have_bank, failed);
    const fs::path p = out_path("pak22_ps4.txt");
    if (!a.only.empty() && lt.empty())
        lg("pak22_ps4.txt: unchanged (--only run without level rows)");
    else {
        std::error_code ec;
        if (!r.added.empty()) write_text(p, r.text);
        else if (isfile(p)) fs::remove(p, ec);
        Value added = Value::object(), skipped = Value::object(), dropped = Value::object();
        added.kind = Value::Array;
        for (const std::string &n : r.added) added.items.push_back(str(latin1_utf8(n)));
        for (const auto &[n, why] : r.skipped) skipped.set(latin1_utf8(n), str(latin1_utf8(why)));
        for (const auto &[n, lines] : r.dropped) {
            Value l = Value::object();
            l.kind = Value::Array;
            for (const auto &[k, v] : lines) l.items.push_back(str(latin1_utf8(k + " " + v)));
            dropped.set(latin1_utf8(n), l);
        }
        state.set("registry", obj({{"added", added}, {"skipped", skipped}, {"dropped", dropped}}));
        lg(latin1_utf8(fmt("pak22_ps4.txt: %zu levels added %s; %zu not added", r.added.size(), py_list(r.added).c_str(), r.skipped.size())));
        for (const auto &[n, why] : r.skipped) lg(latin1_utf8("        not added " + n + ": " + why));
        for (const auto &[n, lines] : r.dropped) {
            std::string s;
            for (size_t i = 0; i < lines.size(); i++) s += (i ? ", " : "") + lines[i].first + " " + lines[i].second;
            lg(latin1_utf8("        dropped from " + n + " (no such file): " + s));
        }
    }
    const fs::path bank_dir = main / "sound1" / "banks" / "ps4";
    std::vector<std::string> new_banks;
    if (isdir(bank_dir))
        for (const std::string &f : listdir_sorted(bank_dir))
            if (ends(f, ".bnk")) new_banks.push_back(f);
    if (!new_banks.empty() && isfile(ps4->banks_txt)) {
        const auto [btxt, add] = build_banks_txt(read_text(ps4->banks_txt), new_banks);
        if (!add.empty()) write_text(out_path("sound1/config/ps4/banks.txt"), btxt);
        Value av = Value::object();
        av.kind = Value::Array;
        for (const std::string &b : add) av.items.push_back(str(b));
        state.set("banks_txt", av);
        lg(fmt("banks.txt: %zu banks added %s", add.size(), py_list(add).c_str()));
    }
    save();
}

// text1/ps4/<lang>.<category> (common, networking) for every PS4 language: the PS4 bank completed with the GOTY texts
// it lacks (texts.cpp); the game reads its banks from core.psarc and the runtime answers misses from these files
void Importer::texts() {
    const fs::path core = ps4->main / "core.psarc";
    if (!isfile(core)) return;
    Psarc arc(core);
    std::map<std::string, std::vector<std::pair<int, Bytes>>> banks;   // category -> (language, PS4 bank), archive order
    for (size_t i = 0; i < arc.names.size(); i++) {
        const std::string &n = arc.names[i];
        if (!starts(n, "text1/ps4/")) continue;
        const std::string rest = n.substr(10);
        const size_t dot = rest.find('.');
        if (dot == std::string::npos || dot == 0) continue;
        const std::string num_s = rest.substr(0, dot), cat = rest.substr(dot + 1);
        if (!std::all_of(num_s.begin(), num_s.end(), [](char c) { return c >= '0' && c <= '9'; }) || (cat != "networking" && cat != "common")) continue;
        const int lang = std::stoi(num_s);
        auto &v = banks[cat];
        const Bytes b = arc.read(i + 1);
        auto it = std::find_if(v.begin(), v.end(), [&](const auto &x) { return x.first == lang; });
        if (it != v.end()) it->second = b;
        else v.push_back({lang, b});
    }
    Value added = Value::object(), counts = Value::object();
    long long n_goty = 0, n_related = 0;
    std::vector<std::string> left;
    for (const auto &[category, ps4b] : banks) {
        std::map<int, std::vector<Bytes>> gb;
        for (const auto &[lang, _] : ps4b) {
            auto &list = gb[lang];
            for (const Entry *e : ps3->candidates("text1/" + std::to_string(lang) + "." + category))
                if (goty(e->source)) list.push_back(e->read());
        }
        for (const auto &[lang, res] : texts_complete(ps4b, gb)) {
            const std::string rel = "text1/ps4/" + std::to_string(lang) + "." + category;
            const fs::path o = main / P(rel);
            const TextCounts &n = res.second;
            counts.set(rel, obj({{"goty", num(n.goty)}, {"related", num(n.related)}, {"left", num(n.left)}}));
            n_goty += n.goty;
            n_related += n.related;
            if (!res.first) {
                std::error_code ec;
                if (isfile(o)) fs::remove(o, ec);
                continue;
            }
            write_bytes(out_path(rel), *res.first);
            added.set(rel, num(n.goty + n.related));
        }
    }
    std::vector<std::pair<std::string, long long>> lefts;
    for (const auto &[rel, c] : counts.fields)
        if (c.get("left")->i) lefts.push_back({rel, c.get("left")->i});
    std::sort(lefts.begin(), lefts.end());
    std::string ls;
    for (const auto &[rel, n] : lefts) ls += (ls.empty() ? "" : ", ") + basename(rel) + " " + std::to_string(n);
    state.set("text", added);
    state.set("text_sources", counts);
    state.remove("text_title_update");
    lg(fmt("text banks: %zu written (%lld texts from GOTY, %lld from the related language; still English: %s)", added.fields.size(), n_goty, n_related,
           ls.empty() ? "none" : ls.c_str()));
    save();
}

int Importer::finish(const std::string &status, std::vector<std::pair<std::string, Value>> kw) {
    for (auto &[k, v] : kw) state.set(k, v);
    state.set("status", str(status));
    state.set("finished", str(now_text()));
    state.remove("pid");
    state.remove("updating");
    // an automatic update (--update) that failed is not started again at every launcher start for the same import version
    if (status == "failed" && a.update) state.set("update_failed_for", str(shipped_id_text()));
    else if (status == "imported" || status == "partial") state.remove("update_failed_for");
    save();
    event(obj({{"phase", str("done")}, {"status", str(status)}, {"error", str(text_of(state.get("error")))}}));
    lg("status: " + status + " " + text_of(state.get("error")));
    if (logf) std::fclose(logf);
    logf = nullptr;
    return status == "imported" || status == "partial" ? 0 : 1;
}

}  // namespace

namespace u2imp {

std::string sha1_hex(const Bytes &b) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    uint8_t d[20];
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    for (size_t o = 0; o < b.size(); o += 1u << 30)
        BCryptHashData(h, const_cast<PUCHAR>(b.data() + o), ULONG(std::min<size_t>(b.size() - o, 1u << 30)), 0);
    BCryptFinishHash(h, d, sizeof d, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    std::string s;
    for (uint8_t x : d) s += fmt("%02x", x);
    return s;
}

}  // namespace u2imp

// the fingerprint of the installed import: this program, the import list and the bank envelope table it reads
std::string shipped_id_text() {
    Sha256 h;
    auto add = [&](const std::string &name, const fs::path &p, bool text) {
        if (!fs::is_regular_file(p)) return;
        std::string t = read_text(p);
        if (text) t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());   // a CRLF checkout gives the release zip's value
        const std::string d = sha256_hex(t);
        h.update(name.data(), name.size());
        h.update("\0", 1);
        h.update(d.data(), d.size());
    };
    add("u2import.exe", exe_dir() / "u2import.exe", false);
    add("import-list.tsv", patcher_dir().parent_path() / "docs" / "import-list.tsv", true);
    add("adsr_env.json", launcher_root() / "u3mp" / "patcher" / "tools" / "fixes" / "sound-banks" / "adsr_env.json", true);
    return h.hex();
}

int wmain(int argc, wchar_t **argv) {
    g_event_prefix = "@U2MP ";
    u3::g_u2 = true;   // the Uncharted 3 converters write for Uncharted 2 (set before any thread starts)
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> v;
    for (int i = 1; i < argc; i++) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        v.push_back(s);
    }
    try {
        if (!v.empty() && v[0] == "--shipped-id") {
            log(shipped_id_text());
            return 0;
        }
        // the launcher: u2import --overlay <game folder> <overlay folder> [--update <update folder>]; prints the overlay folder
        if (!v.empty() && v[0] == "--overlay") {
            if (v.size() < 3) {
                std::fputs("usage: u2import --overlay <game folder> <overlay folder> [--update <folder>]\n", stderr);
                return 2;
            }
            std::optional<fs::path> upd;
            if (v.size() >= 5 && v[3] == "--update" && !v[4].empty()) upd = P(v[4]);
            log(S(build_overlay(P(v[1]), P(v[2]), upd)));
            return 0;
        }
        Args a;
        a.list = S(patcher_dir().parent_path() / "docs" / "import-list.tsv");
        const std::map<std::string, std::string *> vals = {{"--ps4", &a.ps4},         {"--ps3", &a.ps3},       {"--ps3-dlc", &a.ps3_dlc}, {"--ps3-data", &a.ps3_data},
                                                           {"--out", &a.out},         {"--groups", &a.groups}, {"--speech-langs", &a.speech_langs},
                                                           {"--list", &a.list},       {"--only", &a.only},     {"--ffmpeg", &a.ffmpeg},
                                                           {"--ps3-elf", &a.ps3_elf}, {"--objdump", &a.objdump}};
        const std::map<std::string, bool *> flags = {{"--dlc-ruins", &a.dlc_ruins}, {"--no-dlc-ruins", &a.no_dlc_ruins}, {"--drop-navmesh", &a.drop_navmesh},
                                                     {"--no-verify", &a.no_verify}, {"--update", &a.update},             {"--force", &a.force},
                                                     {"--validate", &a.validate}};
        for (size_t i = 0; i < v.size(); i++) {
            if (flags.count(v[i])) *flags.at(v[i]) = true;
            else if (vals.count(v[i]) && i + 1 < v.size()) {
                std::string *target = vals.at(v[i]);
                *target = v[++i];
            }
            else {
                std::fprintf(stderr, "u2import: unknown argument %s\n", v[i].c_str());
                return 2;
            }
        }
        if (a.ps4.empty() || a.ps3.empty() || a.out.empty()) {
            std::fprintf(stderr, "usage: u2import --ps4 <folder> --ps3 <folder> --out <folder> [options] | --shipped-id\n");
            return 2;
        }
        return Importer(a).run();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "u2import: %s\n", e.what());
        return 1;
    }
}
