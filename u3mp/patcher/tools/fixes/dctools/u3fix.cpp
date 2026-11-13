// dctools/u3fix.cpp: u3fix.exe, the C++ recipe steps of the Uncharted 3 import.
//
//   u3fix <step> [options]
//     remove-lab        --project P [--actors lab_actors.txt] [--apply]       (tools/fixes/no-lab)
//     remove-rpcn-page  --project P [--apply]                                 (tools/fixes/no-rpcn-menu)
//     netgame-order     --project P --net18 F [--apply | --verify] [...]      (tools/fixes/netgame-order)
//     emblem-cap3       --project P [--apply]                                 (tools/fixes/wall-emblem-test)
//     eula-box          --project P [--apply]                                 (tools/fixes/dctools)
//     orange-wall       --project P --plan F [--apply] [--restore]            (tools/fixes/orange-wall)
//     excess-wall       --project P --job D [--only-wall] [--add ID ...] [--apply] [--restore]   (tools/fixes/excess-wall)
//     enemy-stall       --project P [--apply | --restore] [--only-tag-on-enter]  (tools/fixes/enemy-stall)
//     coop-desc         --project P [--apply]                                 (tools/fixes/coop-chars)
//     vram-gap          --project P [--apply] [--restore] [--dir D]           (tools/fixes/coop-tab)
//     igc3-retargets    --ps4 D --job D                                       (tools/fixes/airport-igc3)
//     igc3-install      --project P --job D [--apply] [--restore]             (tools/fixes/airport-igc3)
//     text1-merge       --ps4 D --ps3-disc D --project P [--apply]           (tools/fixes/text1-merge)
//     coop-audit        --project P [--ps3-disc D] [--ps3-update D] [--ps3-data2 D] [--job D] [--apply | --restore]   (tools/fixes/coop-audit)
//     registry-levels   --project P --job J --ps4 D --ps3-update D --ps3-disc D overlay ...   (tools/fixes/registry)
//     registry-final    --project P --job J --ps4 D --ps3-update D --ps3-dlcmaps D --ps3-disc D --ps3-data2 D [--apply] [overlay ...]   (tools/fixes/pak23-registry)
//     hair-key          --project P [--also-hair-kind] [--apply] [net.bin]  (tools/fixes/hat-chloe)
//     levels            <port|r2|r3> --project P --job J --ps4 D --ps3-* D [--apply] [--only RE] [--strict] [--jobs N] [--keep-work]   (tools/fixes/pak23-levels)
//     actors            --project P --job J --ps4 D --ps3-disc D --ps3-update D --ps3-data2 D [--apply] [--overlays ...] [--only ...] [--allow-equivalent] [--jobs N]   (tools/fixes/actor33)
//     rebuild-am        <am|hats|igc2> --project P --job J --ps4 D --ps3-disc D --ps3-update D --ps3-data2 D [--only a,b] [--jobs N]   (tools/fixes/actor33)
//     install-am        <am|hats|igc2> --project P [--work DIR] [--only a,b] [--apply | --restore]   (tools/fixes/actor33)
//     pakinfo           PAK ... [--textures] [--noitems]             (dctools/ndpak.cpp, a maintainer check)
//
// The package builds it into u3mp/bin (tools/package_windows.sh), which the importer puts on PATH; a recipe step runs
// it as ["u3fix.exe", "<step>", ...]. Every step is a dry run without --apply and exits 0 'already applied' when
// there is nothing to do; a problem prints its reason and exits 1 with nothing written.
#include "u3fix.hpp"

#include <cstdio>
#include <windows.h>
#include <tlhelp32.h>

namespace u3 {

fs::path upath(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

Args::Args(const std::vector<std::string> &argv, const std::set<std::string> &flags, const std::set<std::string> &values) {
    for (size_t i = 0; i < argv.size(); i++) {
        const std::string &a = argv[i];
        if (flags.count(a)) { set.insert(a); continue; }
        if (values.count(a) && i + 1 < argv.size()) { val[a] = argv[++i]; continue; }
        throw Stop("unknown or incomplete argument: " + a);
    }
}

std::string Args::need(const std::string &k) const {
    auto it = val.find(k);
    if (it == val.end() || it->second.empty()) throw Stop("missing " + k);
    return it->second;
}

std::string Args::get(const std::string &k, const std::string &def) const {
    auto it = val.find(k);
    return it == val.end() ? def : it->second;
}

// MD5 (RFC 1321), for the plans that name a pak by its md5
std::string md5_hex(const Bytes &data) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const uint64_t bits = uint64_t(data.size()) * 8;
    const size_t whole = data.size() - data.size() % 64;
    Bytes tail(data.begin() + std::ptrdiff_t(whole), data.end());
    tail.push_back(0x80);
    while (tail.size() % 64 != 56) tail.push_back(0);
    for (int i = 0; i < 8; i++) tail.push_back(uint8_t(bits >> (8 * i)));
    auto block = [&](const uint8_t *p) {
        uint32_t m[16];
        for (int i = 0; i < 16; i++) m[i] = rd32le(p + 4 * i);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const uint32_t t = d;
            d = c;
            c = b;
            const uint32_t x = a + f + K[i] + m[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    };
    for (size_t o = 0; o < whole; o += 64) block(data.data() + o);
    for (size_t o = 0; o < tail.size(); o += 64) block(tail.data() + o);
    std::string s;
    for (uint32_t v : h)
        for (int i = 0; i < 4; i++) s += fmt("%02x", (v >> (8 * i)) & 0xff);
    return s;
}

void copy2(const fs::path &src, const fs::path &dst) {
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
    fs::last_write_time(dst, fs::last_write_time(src));
}

bool game_running(bool name_contains) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;   // as the scripts: no process list, not running
    PROCESSENTRY32W e;
    e.dwSize = sizeof e;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
        std::wstring n = e.szExeFile;
        for (wchar_t &c : n) c = (c >= L'A' && c <= L'Z') ? wchar_t(c - L'A' + L'a') : c;
        found = name_contains ? n.find(L"uncharted.exe") != std::wstring::npos : n == L"uncharted.exe";
    }
    CloseHandle(snap);
    return found;
}

std::string win_path(std::string p) {
    for (char &c : p)
        if (c == '/') c = '\\';
    return p;
}

std::string pyhex(uint64_t v) { return fmt("0x%llx", (unsigned long long)v); }

json::Value read_json(const fs::path &p) {
    const Bytes b = read_file(p);
    json::Value v;
    if (!json::parse(std::string(b.begin(), b.end()), v)) throw Stop(utf8(p) + ": not valid JSON");
    return v;
}

namespace {
void dump1(std::string &o, const json::Value &v, int depth) {
    const std::string pad(size_t(depth + 1), ' '), end(size_t(depth), ' ');
    switch (v.kind) {
    case json::Value::Null: o += "null"; break;
    case json::Value::Bool: o += v.b ? "true" : "false"; break;
    case json::Value::Int: o += std::to_string(v.i); break;
    case json::Value::Float: o += json::real(v.f); break;
    case json::Value::String: json::quote(o, v.s); break;
    case json::Value::Array:
        if (v.items.empty()) { o += "[]"; break; }
        o += "[\n";
        for (size_t k = 0; k < v.items.size(); ++k) {
            o += pad;
            dump1(o, v.items[k], depth + 1);
            o += k + 1 < v.items.size() ? ",\n" : "\n";
        }
        o += end + "]";
        break;
    case json::Value::Object:
        if (v.fields.empty()) { o += "{}"; break; }
        o += "{\n";
        for (size_t k = 0; k < v.fields.size(); ++k) {
            o += pad;
            json::quote(o, v.fields[k].first);
            o += ": ";
            dump1(o, v.fields[k].second, depth + 1);
            o += k + 1 < v.fields.size() ? ",\n" : "\n";
        }
        o += end + "}";
        break;
    }
}
void need_bytes(const Bytes &d, uint64_t o, size_t n) {
    if (o + n > d.size())
        throw Stop(fmt("struct.error: %zu bytes at offset %llu need a buffer of %llu bytes, the file has %zu", n, (unsigned long long)o,
                       (unsigned long long)(o + n), d.size()));
}
}  // namespace

std::string json_dump_indent1(const json::Value &v) {
    std::string o;
    dump1(o, v, 0);
    return o;
}

uint16_t rd16le(const Bytes &d, uint64_t o) { need_bytes(d, o, 2); return uint16_t(d[o] | d[o + 1] << 8); }
uint32_t rd32le(const Bytes &d, uint64_t o) { need_bytes(d, o, 4); return rd32le(&d[o]); }
uint64_t rd64le(const Bytes &d, uint64_t o) { need_bytes(d, o, 8); return rd64le(&d[o]); }
uint32_t be32(const Bytes &d, uint64_t o) { need_bytes(d, o, 4); return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3]; }

bool py_space(unsigned char c) { return c == 0x20 || (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f) || c == 0x85 || c == 0xa0; }

std::vector<std::string> py_split(const std::string &s) {
    std::vector<std::string> w;
    size_t i = 0;
    while (true) {
        while (i < s.size() && py_space((unsigned char)s[i])) i++;
        if (i >= s.size()) return w;
        size_t j = i;
        while (j < s.size() && !py_space((unsigned char)s[j])) j++;
        w.push_back(s.substr(i, j - i));
        i = j;
    }
}

std::string py_rstrip(const std::string &s) {
    size_t n = s.size();
    while (n && py_space((unsigned char)s[n - 1])) n--;
    return s.substr(0, n);
}

std::vector<std::string> py_splitlines(const std::string &s) {   // LF, CR, CRLF, VT, FF, FS, GS, RS and NEL (0x85) end a line
    std::vector<std::string> out;
    size_t a = 0, i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0x0a || c == 0x0d || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d || c == 0x1e || c == 0x85) {
            out.push_back(s.substr(a, i - a));
            i += (c == 0x0d && i + 1 < s.size() && s[i + 1] == 0x0a) ? 2 : 1;
            a = i;
        } else {
            i++;
        }
    }
    if (a < s.size()) out.push_back(s.substr(a));
    return out;
}
}  // namespace u3

namespace {
std::string narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
}  // namespace

int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> a;
    for (int i = 2; i < argc; i++) a.push_back(narrow(argv[i]));
    std::string step = argc > 1 ? narrow(argv[1]) : "";
    try {
        if (step == "remove-lab") return u3::remove_lab(a);
        if (step == "remove-rpcn-page") return u3::remove_rpcn_page(a);
        if (step == "netgame-order") return u3::netgame_order(a);
        if (step == "emblem-cap3") return u3::emblem_cap3(a);
        if (step == "eula-box") return u3::eula_box(a);
        if (step == "orange-wall") return u3::orange_wall(a);
        if (step == "excess-wall") return u3::excess_wall(a);
        if (step == "enemy-stall") return u3::enemy_stall(a);
        if (step == "coop-desc") return u3::coop_desc(a);
        if (step == "vram-gap") return u3::vram_gap(a);
        if (step == "igc3-retargets") return u3::igc3_retargets(a);
        if (step == "igc3-install") return u3::igc3_install(a);
        if (step == "text1-merge") return u3::text1_merge(a);
        if (step == "coop-audit") return u3::coop_audit(a);
        if (step == "registry-levels") return u3::registry_levels(a);
        if (step == "registry-final") return u3::registry_final(a);
        if (step == "hair-key") return u3::hair_key(a);
        if (step == "levels") return u3::levels(a);
        if (step == "actors") return u3::actors(a);
        if (step == "rebuild-am") return u3::rebuild_am(a);
        if (step == "install-am") return u3::install_am(a);
        if (step == "convert-dc1") return u3::convert_dc1(a);
        if (step == "dc-recipes") return u3::dc_recipes(a);
        if (step == "sound-banks") return u3::sound_banks(a);
        if (step == "part-modules") return u3::part_modules(a);
        if (step == "actors-hk") return u3::actors_hk(a);
        if (step == "coop-vo") return u3::coop_vo(a);
        if (step == "coop-voice2") return u3::coop_voice2(a);
        if (step == "elim-pic") return u3::elim_pic(a);
        if (step == "classic") return u3::classic(a);
        if (step == "skins-net-names") return u3::skins_net_names(a);
        if (step == "shade-npc") return u3::shade_npc(a);
        if (step == "facility-collapse") return u3::facility_collapse(a);
        if (step == "pakinfo") return u3::pakinfo(a);
        std::fprintf(stderr, "usage: u3fix <step> [options] (the steps: see u3fix.cpp)\n");
        return 2;
    } catch (const u3::Stop &e) {
        std::fflush(stdout);
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    } catch (const std::exception &e) {
        std::fflush(stdout);
        std::fprintf(stderr, "%s: %s\n", step.c_str(), e.what());
        return 1;
    }
}
