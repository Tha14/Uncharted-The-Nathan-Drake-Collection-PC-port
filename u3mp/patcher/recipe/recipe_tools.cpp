// recipe_tools.cpp: the recipe's maintainer tools (hash tables and the provenance of the overlay files).
//   recipe_tools make_manifest <work-dir> <out-dir> [overlay names...]
//       inventory of the overlay folders (default overlay-r3 overlay-r2 overlay-port overlay-mp): path, size, sha256,
//       mtime -> <out-dir>/manifest.tsv and manifest.json
//   recipe_tools hash_tree <root> <out.tsv> [subdirs...]
//       path, size, sha256 of every file under root (or the given subdirs of it)
//   recipe_tools provenance
//       classifies every file of ./manifest.tsv by hash against the hash tables in the current folder (retail PS4
//       files and psarc entries, PS3 archives and loose files, backups, transfer batches, stage-assets); writes
//       ./provenance.tsv and prints per-family stats
//   recipe_tools psarc_hash <out.tsv> <psarc...>
//       appends psarc, entry, size, sha256 of every entry of each PSARC (streamed, nothing extracted to disk)
// Text files and stdout are written in text mode (CRLF on Windows), like the Python tools they replace.
// Build (MSYS2 CLANG64, from this folder):
//   clang++ -std=c++20 -O2 -static -I../tools/fixes/dctools -I../../../launcher/native recipe_tools.cpp ../tools/fixes/dctools/psarc.cpp ../tools/fixes/dctools/dc.cpp -o recipe_tools.exe
#include "psarc.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#endif

namespace u3 {
// psarc.cpp's Ps3Index needs this helper of fixlib.cpp (same definition); the tools here use it for every path
fs::path upath(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
}  // namespace u3

namespace {

namespace fs = std::filesystem;

struct Fail {
    std::string message;
};

fs::path P(const std::string &utf8) { return u3::upath(utf8); }
std::string U(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// ---------------------------------------------------------------- output (stdout in text mode, flushed per line)
void say(const std::string &line) {
    std::fputs((line + "\n").c_str(), stdout);
    std::fflush(stdout);
}

const auto t_start = std::chrono::steady_clock::now();
std::string secs() {   // int(time.time() - t0)
    return std::to_string(
        (long long)std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count());
}

// ---------------------------------------------------------------- SHA-256 of a file, read in 1 MiB chunks
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t used = 0;
    uint64_t total = 0;

    static uint32_t rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }
    void block(const uint8_t *p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++)
            w[i] = w[i - 16] + (rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] +
                   (rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ w[i - 2] >> 10);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    }
    void update(const uint8_t *p, size_t n) {
        total += n;
        while (n) {
            const size_t take = std::min(n, 64 - used);
            std::memcpy(buf + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { block(buf); used = 0; }
        }
    }
    std::string hex() {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (used != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; i++) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
        return out;
    }
};

std::string sha256_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Fail{"cannot read " + U(p)};
    Sha256 s;
    std::vector<char> chunk(1 << 20);
    while (f) {
        f.read(chunk.data(), std::streamsize(chunk.size()));
        s.update(reinterpret_cast<const uint8_t *>(chunk.data()), size_t(f.gcount()));
    }
    if (f.bad()) throw Fail{"cannot read " + U(p)};
    return s.hex();
}

// os.stat(): st_size and int(st_mtime), the float Python builds from seconds and nanoseconds (sec + nsec * 1e-9)
struct Stat {
    uint64_t size;
    long long mtime;
};
Stat stat_file(const fs::path &p) {
#ifdef _WIN32
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION info;
    const bool ok = h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &info);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) throw Fail{"cannot stat " + U(p)};
    const int64_t ticks = int64_t(uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32 | info.ftLastWriteTime.dwLowDateTime);
    const long long sec = ticks / 10000000 - 11644473600LL;
    const long nsec = long(ticks % 10000000) * 100;
    return {uint64_t(info.nFileSizeHigh) << 32 | info.nFileSizeLow, (long long)(double(sec) + nsec * 1e-9)};
#else
    struct stat st;
    if (::stat(p.c_str(), &st)) throw Fail{"cannot stat " + U(p)};
    return {uint64_t(st.st_size), (long long)(double(st.st_mtim.tv_sec) + st.st_mtim.tv_nsec * 1e-9)};
#endif
}

// os.walk(top) (top down, symlinked folders not entered, unreadable folders skipped): every file in the order Python
// gets them, a folder's files first, then its subfolders; rel is the path below top joined with '/'
template <class F>
void walk(const fs::path &top, const std::string &rel, F &&file) {
    std::error_code ec;
    fs::directory_iterator it(top, ec), end;
    if (ec) return;
    std::vector<fs::path> dirs, files;
    for (; it != end; it.increment(ec)) {
        if (ec) return;
        std::error_code e2;
        (it->is_directory(e2) ? dirs : files).push_back(it->path());
    }
    for (const fs::path &f : files) file(f, rel.empty() ? U(f.filename()) : rel + "/" + U(f.filename()));
    for (const fs::path &d : dirs) {
        std::error_code e2;
        if (fs::is_symlink(fs::symlink_status(d, e2))) continue;
        walk(d, rel.empty() ? U(d.filename()) : rel + "/" + U(d.filename()), file);
    }
}

// os.path.relpath(sub, root) with '\' -> '/' ("." when they are the same folder)
std::string relpath(const fs::path &sub, const fs::path &root) {
    const fs::path r = fs::absolute(sub).lexically_normal().lexically_relative(fs::absolute(root).lexically_normal());
    std::string s = U(r.empty() ? fs::path(".") : r);
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// os.path.basename (ntpath on Windows: after the drive, the last '/' or '\')
std::string basename(std::string p) {
#ifdef _WIN32
    if (p.size() >= 2 && p[1] == ':') p = p.substr(2);
    const size_t k = p.find_last_of("/\\");
#else
    const size_t k = p.find_last_of('/');
#endif
    return k == std::string::npos ? p : p.substr(k + 1);
}

// json.dumps(text) with ensure_ascii: everything outside ' '..'~' as \uXXXX (surrogate pairs above U+FFFF)
std::string json_str(const std::string &s) {
    std::string o = "\"";
    char b[16];
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        uint32_t cp = c;
        int n = 1;
        if (c >= 0xf0 && i + 3 < s.size()) { cp = (c & 7u) << 18 | (s[i + 1] & 63u) << 12 | (s[i + 2] & 63u) << 6 | (s[i + 3] & 63u); n = 4; }
        else if (c >= 0xe0 && i + 2 < s.size()) { cp = (c & 15u) << 12 | (s[i + 1] & 63u) << 6 | (s[i + 2] & 63u); n = 3; }
        else if (c >= 0xc0 && i + 1 < s.size()) { cp = (c & 31u) << 6 | (s[i + 1] & 63u); n = 2; }
        i += n;
        if (cp == '"') o += "\\\"";
        else if (cp == '\\') o += "\\\\";
        else if (cp == '\n') o += "\\n";
        else if (cp == '\r') o += "\\r";
        else if (cp == '\t') o += "\\t";
        else if (cp == '\b') o += "\\b";
        else if (cp == '\f') o += "\\f";
        else if (cp >= 0x20 && cp <= 0x7e) o += char(cp);
        else if (cp > 0xffff) {
            cp -= 0x10000;
            std::snprintf(b, sizeof b, "\\u%04x\\u%04x", 0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
            o += b;
        } else {
            std::snprintf(b, sizeof b, "\\u%04x", cp);
            o += b;
        }
    }
    return o + "\"";
}

std::ofstream create(const fs::path &p, std::ios::openmode mode = std::ios::out | std::ios::trunc) {
    std::ofstream f(p, mode);   // text mode: '\n' is written as the platform's line end, like Python's open(p, "w")
    if (!f) throw Fail{"cannot write " + U(p)};
    return f;
}

// ---------------------------------------------------------------- make_manifest
int make_manifest(const std::vector<std::string> &a) {
    if (a.size() < 2) throw Fail{"usage: recipe_tools make_manifest <work-dir> <out-dir> [overlay names...]"};
    const fs::path work = P(a[0]), out = P(a[1]);
    std::vector<std::string> names(a.begin() + 2, a.end());
    if (names.empty()) names = {"overlay-r3", "overlay-r2", "overlay-port", "overlay-mp"};
    struct Row {
        std::string overlay, path, sha256;
        uint64_t size;
        long long mtime;
    };
    std::vector<Row> rows;
    for (const std::string &ov : names) {
        walk(work / P(ov), "", [&](const fs::path &p, const std::string &rel) {
            const std::string sha = sha256_file(p);
            const Stat st = stat_file(p);
            rows.push_back({ov, rel, sha, st.size, st.mtime});
        });
        say(ov + " " + std::to_string(rows.size()) + " files so far " + secs() + " s");
    }
    fs::create_directories(out);
    {
        std::ofstream f = create(out / "manifest.tsv");
        f << "overlay\tpath\tsize\tsha256\tmtime\n";
        for (const Row &r : rows) f << r.overlay << '\t' << r.path << '\t' << r.size << '\t' << r.sha256 << '\t' << r.mtime << '\n';
    }
    {
        std::ofstream f = create(out / "manifest.json");   // json.dump(rows, f, indent=0)
        if (rows.empty()) f << "[]";
        else {
            f << "[\n";
            for (size_t i = 0; i < rows.size(); i++) {
                const Row &r = rows[i];
                f << "{\n\"overlay\": " << json_str(r.overlay) << ",\n\"path\": " << json_str(r.path) << ",\n\"size\": " << r.size
                  << ",\n\"sha256\": " << json_str(r.sha256) << ",\n\"mtime\": " << r.mtime << "\n}" << (i + 1 < rows.size() ? ",\n" : "\n");
            }
            f << "]";
        }
    }
    say("done " + std::to_string(rows.size()) + " files " + secs() + " s");
    return 0;
}

// ---------------------------------------------------------------- hash_tree
int hash_tree(const std::vector<std::string> &a) {
    if (a.size() < 2) throw Fail{"usage: recipe_tools hash_tree <root> <out.tsv> [subdirs...]"};
    const fs::path root = P(a[0]);
    std::vector<std::string> subs(a.begin() + 2, a.end());
    if (subs.empty()) subs = {"."};
    size_t n = 0;
    std::ofstream f = create(P(a[1]));
    f << "path\tsize\tsha256\n";
    for (const std::string &s : subs) {
        const fs::path top = root / P(s);
        std::string pre = relpath(top, root);
        if (pre == ".") pre.clear();
        walk(top, pre, [&](const fs::path &p, const std::string &rel) {
            const std::string sha = sha256_file(p);
            f << rel << '\t' << stat_file(p).size << '\t' << sha << '\n';
            if (++n % 500 == 0) say(std::to_string(n) + " " + secs() + " s");
        });
    }
    f.close();
    say("done " + std::to_string(n) + " " + secs() + " s");
    return 0;
}

// ---------------------------------------------------------------- provenance
// a text file as Python's open(p, encoding='utf-8') gives it: "\r\n" and "\r" read as "\n"; nullopt when missing
std::optional<std::string> read_text(const fs::path &p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Fail{"cannot read " + U(p)};
    const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string t;
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] != '\r') t += raw[i];
        else {
            t += '\n';
            if (i + 1 < raw.size() && raw[i + 1] == '\n') i++;
        }
    }
    return t;
}

// csv.DictReader(text, delimiter='\t') (Excel dialect: '"' quotes a field, '""' inside it is one '"'); blank rows skipped
// A row shorter than the header has None in its last columns (nullopt here); a column the header lacks is a KeyError.
using Field = std::optional<std::string>;
struct Table {
    std::map<std::string, size_t> col;
    std::vector<std::vector<std::string>> rows;
    bool has(const std::string &name) const { return col.count(name) != 0; }
    Field operator()(const std::vector<std::string> &row, const std::string &name) const {   // row[name]
        const auto c = col.find(name);
        if (c == col.end()) throw Fail{"KeyError: '" + name + "'"};
        return c->second < row.size() ? Field(row[c->second]) : std::nullopt;
    }
};
std::string str(const Field &f) { return f ? *f : "None"; }   // str(value)
Table dict_reader(const std::string &text) {
    enum { RECORD, FIELD, IN_FIELD, QUOTED, QUOTE_IN_QUOTED } st = RECORD;
    std::vector<std::vector<std::string>> all;
    std::vector<std::string> row;
    std::string field;
    auto save = [&] { row.push_back(field); field.clear(); };
    auto finish = [&] { all.push_back(row); row.clear(); st = RECORD; };
    for (char c : text) {
        switch (st) {
        case RECORD:
            if (c == '\n') { all.push_back({}); break; }
            st = FIELD;
            [[fallthrough]];
        case FIELD:
            if (c == '\n') { save(); finish(); }
            else if (c == '"') st = QUOTED;
            else if (c == '\t') save();
            else { field += c; st = IN_FIELD; }
            break;
        case IN_FIELD:
            if (c == '\n') { save(); finish(); }
            else if (c == '\t') { save(); st = FIELD; }
            else field += c;
            break;
        case QUOTED:
            if (c == '"') st = QUOTE_IN_QUOTED;
            else field += c;
            break;
        case QUOTE_IN_QUOTED:
            if (c == '"') { field += c; st = QUOTED; }
            else if (c == '\t') { save(); st = FIELD; }
            else if (c == '\n') { save(); finish(); }
            else { field += c; st = IN_FIELD; }
            break;
        }
    }
    if (st != RECORD) { save(); finish(); }
    Table t;
    if (all.empty()) return t;
    for (size_t i = 0; i < all[0].size(); i++) t.col[all[0][i]] = i;
    for (size_t i = 1; i < all.size(); i++)
        if (!all[i].empty()) t.rows.push_back(std::move(all[i]));
    return t;
}

size_t chars(const std::string &s) {   // code points of UTF-8 text
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xc0) != 0x80;
    return n;
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> v;
    size_t a = 0;
    for (size_t k; (k = s.find(sep, a)) != std::string::npos; a = k + 1) v.push_back(s.substr(a, k - a));
    v.push_back(s.substr(a));
    return v;
}

int provenance(const std::vector<std::string> &) {
    const auto mtext = read_text("manifest.tsv");
    if (!mtext) throw Fail{"FileNotFoundError: [Errno 2] No such file or directory: 'manifest.tsv'"};
    const Table man = dict_reader(*mtext);
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> idx;   // sha -> [(class, source)]
    auto add = [&](const char *f, const char *cls, const std::string &prefix) {
        const auto t = read_text(f);
        if (!t) return;
        const Table tab = dict_reader(*t);
        for (const auto &r : tab.rows) {
            const Field h = tab.has("sha256") ? tab(r, "sha256") : Field("");   // r.get('sha256', '')
            if (!h) throw Fail{"TypeError: object of type 'NoneType' has no len()"};
            if (chars(*h) != 64) continue;
            const Field path = tab(r, "path");
            if (!path && !prefix.empty()) throw Fail{"TypeError: can only concatenate str (not \"NoneType\") to str"};
            idx[*h].push_back({cls, prefix + str(path)});
        }
    };
    auto addpsarc = [&](const char *f, const char *cls) {
        const auto t = read_text(f);
        if (!t) return;
        std::vector<std::string> lines = split(*t, '\n');
        if (lines.back().empty()) lines.pop_back();   // the text after the last line end is not a line
        for (const std::string &line : lines) {
            const auto p = split(line, '\t');
            if (p.size() == 4 && chars(p[3]) == 64) idx[p[3]].push_back({cls, basename(p[0]) + "::" + p[1]});
        }
    };
    add("retail-ps4-hashes.tsv", "ps4-retail", "");
    add("retail-ps4-u2data-sound.tsv", "ps4-retail", "");
    addpsarc("retail-ps4-psarc-entries.tsv", "ps4-retail-psarc");
    addpsarc("ps3-hashes/u3-pak23.tsv", "ps3-u3-disc-psarc");
    addpsarc("ps3-hashes/u3-actor-bin.tsv", "ps3-u3-disc-psarc");
    addpsarc("ps3-hashes/u3-goty.tsv", "ps3-u3-goty-psarc");
    addpsarc("ps3-hashes/u3-streams.tsv", "ps3-u3-streams-psarc");
    addpsarc("ps3-hashes/u3-rpcs3.tsv", "ps3-u3-patch-dlc-psarc");
    addpsarc("ps3-hashes/u2.tsv", "ps3-u2-psarc");
    add("ps3-hashes/u3-disc-loose.tsv", "ps3-u3-disc-loose", "");
    add("ps3-hashes/rpcs3-loose.tsv", "ps3-rpcs3-loose", "");
    add("backup-hashes-work.tsv", "backup", "work/");
    add("backup-hashes-d.tsv", "backup", "D-backups/");
    add("transfer-hashes.tsv", "transfer-batch", "");
    add("stage-assets-hashes.tsv", "stage-assets", "");
    const auto rtext = read_text("retail-ps4-hashes.tsv");
    if (!rtext) throw Fail{"FileNotFoundError: [Errno 2] No such file or directory: 'retail-ps4-hashes.tsv'"};
    std::set<std::string> rpath;
    {
        const Table r = dict_reader(*rtext);
        for (const auto &row : r.rows) {   // {r['path']: r['sha256'] ...}
            const Field p = r(row, "path");
            r(row, "sha256");
            if (p) rpath.insert(*p);
        }
    }
    static const char *PRIO[] = {"ps4-retail", "ps4-retail-psarc", "ps3-u3-disc-loose", "ps3-u3-disc-psarc", "ps3-u3-goty-psarc",
                                 "ps3-u3-streams-psarc", "ps3-u3-patch-dlc-psarc", "ps3-rpcs3-loose", "ps3-u2-psarc",
                                 "transfer-batch", "stage-assets", "backup"};
    std::map<std::pair<std::string, std::string>, long long> stats;
    std::vector<std::pair<std::string, long long>> tot;   // class -> count, in the order the classes first appear
    std::string out;   // provenance.tsv is written after every row is classified
    static const std::vector<std::pair<std::string, std::string>> none;
    for (const auto &m : man.rows) {
        const Field overlay = man(m, "overlay");
        if (!overlay) throw Fail{"TypeError: unsupported operand type(s) for +: 'NoneType' and 'str'"};
        const Field path = man(m, "path");
        if (!path) throw Fail{"AttributeError: 'NoneType' object has no attribute 'split'"};
        const auto parts = split(*path, '/');
        std::string fam = *overlay + ":";
        for (size_t i = 2; i < 5 && i < parts.size(); i++) fam += (i > 2 ? "/" : "") + parts[i];
        const Field sha = man(m, "sha256");
        const auto hit = sha ? idx.find(*sha) : idx.end();
        const auto &hits = hit != idx.end() ? hit->second : none;
        const std::pair<std::string, std::string> *best = nullptr;
        for (const char *c : PRIO) {
            for (const auto &x : hits)
                if (x.first == c) { best = &x; break; }
            if (best) break;
        }
        const std::string cls = best ? best->first : std::string("derived-or-converted") + (rpath.count(*path) ? "(retail path exists)" : "");
        stats[{fam, cls}]++;
        auto t = std::find_if(tot.begin(), tot.end(), [&](const auto &e) { return e.first == cls; });
        if (t == tot.end()) tot.push_back({cls, 1});
        else t->second++;
        std::string all;
        for (size_t i = 0; i < hits.size() && i < 4; i++) all += (i ? ";" : "") + hits[i].first + ":" + hits[i].second;
        out += *overlay + '\t' + *path + '\t' + str(man(m, "size")) + '\t' + str(sha) + '\t' + cls + '\t' + (best ? best->second : "") +
               '\t' + all + '\n';
    }
    {
        std::ofstream f = create("provenance.tsv");
        f << "overlay\tpath\tsize\tsha256\tclass\tbest_source\tall_matches\n" << out;
    }
    char num[32];
    for (const auto &[k, v] : stats) {   // f'{v:6d}  {k[0]:45s} {k[1]}'
        std::snprintf(num, sizeof num, "%6lld", v);
        const size_t w = chars(k.first);
        say(std::string(num) + "  " + k.first + std::string(w < 45 ? 45 - w : 0, ' ') + " " + k.second);
    }
    say("");
    std::stable_sort(tot.begin(), tot.end(), [](const auto &x, const auto &y) { return x.second > y.second; });   // most_common()
    for (const auto &[k, v] : tot) {
        std::snprintf(num, sizeof num, "%6lld", v);
        say(std::string(num) + " " + k);
    }
    return 0;
}

// ---------------------------------------------------------------- psarc_hash
// Python's repr() of a str (quote = "'" unless the text has ' and no ") or of bytes (b'...'), for the skip messages
std::string py_repr(const std::string &s, bool bytes) {
    const char q = s.find('\'') != std::string::npos && s.find('"') == std::string::npos ? '"' : '\'';
    std::string o = bytes ? "b" : "";
    o += q;
    char b[8];
    for (unsigned char c : s) {
        if (c == '\\' || c == q) o += std::string("\\") + char(c);
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20 || c == 0x7f || (bytes && c > 0x7f)) {
            std::snprintf(b, sizeof b, "\\x%02x", c);
            o += b;
        } else o += char(c);
    }
    return o + q;
}

// why the Python tool skipped an archive it could not open, in its words; empty when the header is a PSARC header
std::string psarc_open_error(const std::string &pth) {
    std::error_code ec;
    const fs::path p = P(pth);
    if (!fs::exists(p, ec)) return "[Errno 2] No such file or directory: " + py_repr(pth, false);
    if (fs::is_directory(p, ec)) return "[Errno 13] Permission denied: " + py_repr(pth, false);
    std::ifstream f(p, std::ios::binary);
    char h[32];
    f.read(h, 32);
    if (f.gcount() != 32) return "unpack requires a buffer of 32 bytes";
    if (std::memcmp(h, "PSAR", 4)) return py_repr(std::string(h, 4), true);
    return "";
}

int psarc_hash(const std::vector<std::string> &a) {
    if (a.empty()) throw Fail{"usage: recipe_tools psarc_hash <out.tsv> <psarc...>"};
    std::ofstream fh = create(P(a[0]), std::ios::out | std::ios::app);
    for (size_t k = 1; k < a.size(); k++) {
        const std::string &pth = a[k];
        std::unique_ptr<u3::Psarc> p;
        if (const std::string why = psarc_open_error(pth); !why.empty()) {
            say("skip " + pth + " " + why);
            continue;
        }
        try {
            p = std::make_unique<u3::Psarc>(P(pth));
        } catch (const std::exception &e) {
            say("skip " + pth + " " + e.what());
            continue;
        }
        for (size_t i = 0; i < p->names.size(); i++) {
            u3::Bytes d;
            try {
                d = p->read(i + 1);
            } catch (const std::exception &e) {
                fh << pth << '\t' << p->names[i] << "\t-1\tERR " << e.what() << '\n';
                continue;
            }
            Sha256 s;
            s.update(d.data(), d.size());
            fh << pth << '\t' << p->names[i] << '\t' << d.size() << '\t' << s.hex() << '\n';
        }
        fh.flush();
        say("done " + basename(pth) + " " + std::to_string(p->names.size()) + " entries " + secs() + " s");
    }
    return 0;
}

std::vector<std::string> arguments(int argc, char **argv) {
#ifdef _WIN32
    (void)argc, (void)argv;
    int n = 0;
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::string> v;
    for (int i = 0; i < n; i++) v.push_back(U(fs::path(w[i])));
    LocalFree(w);
    return v;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

}  // namespace

int main(int argc, char **argv) {
    const std::vector<std::string> args = arguments(argc, argv);
    const std::string cmd = args.size() > 1 ? args[1] : "";
    const std::vector<std::string> rest(args.begin() + std::min<size_t>(2, args.size()), args.end());
    try {
        if (cmd == "make_manifest") return make_manifest(rest);
        if (cmd == "hash_tree") return hash_tree(rest);
        if (cmd == "provenance") return provenance(rest);
        if (cmd == "psarc_hash") return psarc_hash(rest);
        std::fputs("usage: recipe_tools make_manifest|hash_tree|provenance|psarc_hash ...\n", stderr);
        return 2;
    } catch (const Fail &e) {
        std::fprintf(stderr, "%s\n", e.message.c_str());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
    }
    return 1;
}
