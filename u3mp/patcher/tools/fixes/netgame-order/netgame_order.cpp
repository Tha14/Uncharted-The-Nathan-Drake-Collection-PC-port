// Recipe step netgame-order-119: the overlay net.bin netgame table (DC 0x3b9a067d) and playlists (0xc1b0be0d) in
// PS3 1.19 order, so a PC netgame index is the PS3 index (crossplay with RPCS3 / PS3: match settings, votes, films
// and the party messages carry the index).
//
//   u3fix netgame-order --project P --net18 F [--apply | --verify] [--net net.bin] [--modules modules.bin] [--backup D]
//
// The order is the one of the player's PS3 game data net18.bin (DATA2), the table a PS3 1.19 game loads when it goes
// online (NetInit downloads it and reloads DC module 'net'). The result has exactly PS3's netgames: same count,
// order, names, maps and game types.
//   * a netgame the PC table has (same name; duplicates take the next unused PC entry) is copied whole, so every PS4
//     sub-structure and every earlier recipe fix stays (PS4 wave / AI lists, hero records in the coop-desc-fix form);
//     its scalar fields are then overwritten with the converted PS3 values: the match rules are PS3's (score / time
//     limits, Classic / Hardcore flags and loadouts: Hardcore stays Hardcore as on PS3).
//   * a netgame the PC lacks is converted from PS3 (scalars by the field map, the +0x74 hero records as
//     f0670de8 -> 3 x 8b617b51); any other pointer field stops the step.
//   * PC-only netgames (old PS4-retail Hardcore, the lab-custom-* TDM entries, old Lab Dodgeball, ...) are dropped.
//   * a PS3 map the map list lacks gets a map-list entry: +0x94 = 1 (not listed) when the PC has no paks for it (its
//     main level is not in overlay-r3's pak23_ps4.txt), else 0; its netgames keep their PS3 indices. PS3's +0x14
//     (mode bit mask) has no slot in the PS4 record, so it is not converted.
//   * playlists: PS3 order, ids, words, netgame lists with weights and pools; the PC entry of the same id is the
//     template (PS4 layout, picture string) and keeps its content-pack word (+0x2c).
//   * no Lab maps on PC (owner decision 2026-11-11): every map whose main level starts with 'lab-' gets +0x94 = 1
//     (not listed), so Custom Game never lists it. The netgames on Lab maps stay in the table at their PS3 index but
//     leave every playlist netgame list (order and weights of the others as on PS3). Playlist pools are matchmaking
//     pool ids, not netgames, so they stay PS3's. A playlist left with no netgame (PS3's 0x5242014b
//     net-playlists/lab) keeps its place and gets content pack 0xff: PS4's playlist enum (Find Game) lists an entry
//     only when byte 0x17c3a40[+0x2c] is set, and nothing sets 0xff.
// Same order on every PC (recipe 1.15.0): the map list is rebuilt as exactly net18's maps in net18 order (PC entries
// copied whole, missing ones converted; maps only the PC has are dropped).
// Append-only (DcEdit): the original bytes stay, new arrays go after them; only the table headers change.
// modules.bin: the size word of module 'net' (found by name) is set to the new net.bin size.
// Dry run by default; --apply writes (net.bin / modules.bin backed up once to work/backup-netgame-order/; the log
// netgame_order.log.json there lists the Lab maps hidden, the Lab netgames and per playlist what was removed).
// Idempotent: exits 0 'already applied' when the table already equals net18's with the Lab rules (check() finds
// nothing). --verify: only report the differences (exit 1 when there are any).
// The same table rules are in dctools/ng119.py, which the older net.bin steps (classic.py, maps-batch/apply_maps.py)
// still read to leave such a table alone; keep both in step until those are C++ too.
#include "u3fix.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace u3 {
namespace {

// ---------------------------------------------------------------- table layout (PS3 1.19 BE 4-byte / PS4 LE 8-byte)
constexpr uint32_t NG = 0x3b9a067d, MAPS = 0x0dec97a4, PL = 0xc1b0be0d;
constexpr uint64_t NG3 = 0xe4, NG4 = 0xd0, MAP3 = 0x58, MAP4 = 0x98, PL3 = 0x5c, PL4 = 0x68;
constexpr uint32_t NG_OBJ = 0xf0670de8, NG_SUB = 0x8b617b51;
constexpr uint32_t PL_LIST_T = 0xea8e8c7e, PL_POOL_T = 0x7b085cda, MAP_LIST_T = 0xd14c6ffc;
constexpr uint32_t COOP_ADV = 11, CLASSIC = 0x13;
constexpr uint64_t MAP_NOT_LISTED = 0x94, MAP_MAIN3 = 0x18, MAP_MAIN4 = 0x28, PL_PACK = 0x2c;
constexpr uint32_t HIDDEN_PACK = 0xff;
constexpr int NONE = -1;

struct Pair { uint64_t k3; int64_t k4; };

// PS3 netgame scalar word -> PS4 offset; NONE = no PS4 slot (must be zero, except DROPPED_CLASSIC on Classic)
std::vector<Pair> make_map() {
    std::vector<Pair> m;
    for (uint64_t k = 0; k < 0x48; k += 4) m.push_back({k, int64_t(k)});
    const Pair more[] = {{0x50, 0x58}, {0x54, 0x5c}, {0x58, 0x60}, {0x5c, 0x64}, {0x60, NONE}, {0x64, 0x6c},
                         {0x68, 0x68}, {0x6c, NONE}, {0x70, NONE}, {0x78, 0x78}, {0x7c, 0x7c}};
    m.insert(m.end(), std::begin(more), std::end(more));
    for (uint64_t k = 0x84; k < 0xbc; k += 4) m.push_back({k, int64_t(k + 4)});
    for (uint64_t k = 0xc4; k < 0xe4; k += 4) m.push_back({k, NONE});
    return m;
}
const std::vector<Pair> MAP = make_map();
const std::vector<Pair> PTRS = {{0x48, 0x48}, {0x4c, 0x50}, {0x74, 0x70}, {0x80, 0x80}, {0xbc, 0xc0}, {0xc0, 0xc8}};
bool dropped_classic(uint64_t k3) { return k3 == 0x6c || k3 == 0x70; }

std::vector<Pair> make_pl_map() {
    std::vector<Pair> m = {{0x0, 0x0}, {0x4, 0x4}, {0x8, 0x8}, {0xc, 0xc}, {0x14, 0x18}, {0x18, 0x1c},
                           {0x1c, 0x20}, {0x20, 0x24}, {0x24, 0x28}, {0x30, 0x38}, {0x34, 0x3c}};
    for (uint64_t k = 0x3c; k < 0x5c; k += 4) m.push_back({k, int64_t(k + 0xc)});
    return m;
}
const std::vector<Pair> PL_MAP = make_pl_map();
const std::set<uint64_t> PL_KEEP_PC = {0x2c};
const std::set<uint64_t> PL_PTRS3 = {0x10, 0x2c, 0x38}, PL_PTRS4 = {0x10, 0x30, 0x40};
// PS3 map entry -> PS4: strings (+0x30 the sub-level list goes to +0x58) and scalar words
const std::vector<Pair> MAP_STR = {{0xc, 0x10}, {0x10, 0x18}, {0x18, 0x28}, {0x20, 0x38}, {0x28, 0x48}, {0x40, 0x70}, {0x48, 0x80}};
const std::vector<Pair> MAP_U32 = {{0x0, 0x0}, {0x4, 0x4}, {0x8, 0x8}, {0x1c, 0x30}, {0x24, 0x40}, {0x2c, 0x50},
                                   {0x38, 0x68}, {0x3c, 0x6c}, {0x44, 0x78}, {0x4c, 0x88}, {0x50, 0x90}, {0x54, 0x94}};
const std::set<std::string> DROP_LEVELS = {"cargo-plane-static"};   // not a PS4 level
constexpr uint64_t MAP3_PS3_ONLY = 0x14;                             // PS3 map mode bit mask: no PS4 slot

bool has_key(const std::vector<Pair> &m, uint64_t k3) {
    for (const Pair &p : m) if (p.k3 == k3) return true;
    return false;
}

bool is_lab(const std::string &level) { return level.rfind("lab-", 0) == 0; }

struct Table { uint64_t hdr; uint32_t n; uint64_t arr; };

Table table(const Dc &c, uint32_t s = NG) {
    auto it = c.bysid.find(s);
    if (it == c.bysid.end()) throw Stop(c.name + ": no symbol " + hex8(s));
    uint64_t o = it->second.second;
    return {o, c.u32(o), c.ptr(o + c.ps)};
}

// {map id: main level name} of the map list (PS3 or PS4); the last entry of an id wins
std::map<uint32_t, std::string> map_mains(const Dc &c) {
    Table t = table(c, MAPS);
    uint64_t step = c.ps == 8 ? MAP4 : MAP3, mo = c.ps == 8 ? MAP_MAIN4 : MAP_MAIN3;
    std::map<uint32_t, std::string> r;
    for (uint32_t i = 0; i < t.n; i++) {
        uint64_t e = t.arr + i * step;
        r[c.u32(e)] = c.isptr(e + mo) ? c.str_or_empty(c.ptr(e + mo)) : std::string();
    }
    return r;
}

struct LabInfo {
    std::set<uint32_t> maps;
    std::map<uint32_t, uint32_t> netgames;   // PS3 index -> name
    std::set<uint32_t> names;
};

// the Lab maps of net18 b's map list and of the PC table c, the PS3 netgames on them and their names
LabInfo lab_netgames(const Dc &b, const Dc &c) {
    LabInfo L;
    for (const auto &[m, s] : map_mains(b)) if (is_lab(s)) L.maps.insert(m);
    for (const auto &[m, s] : map_mains(c)) if (is_lab(s)) L.maps.insert(m);
    Table t = table(b);
    for (uint32_t i = 0; i < t.n; i++)
        if (L.maps.count(b.u32(t.arr + i * NG3 + 4))) L.netgames[i] = b.u32(t.arr + i * NG3);
    for (const auto &kv : L.netgames) L.names.insert(kv.second);
    std::set<uint32_t> clash;
    for (uint32_t i = 0; i < t.n; i++)
        if (!L.netgames.count(i) && L.names.count(b.u32(t.arr + i * NG3))) clash.insert(b.u32(t.arr + i * NG3));
    if (!clash.empty()) {
        std::string s;
        for (uint32_t x : clash) s += (s.empty() ? "'" : ", '") + hex8(x) + "'";
        throw std::invalid_argument("netgame names on Lab and non-Lab maps: [" + s + "]");
    }
    return L;
}

// the +0x94 word a PC map entry must have: 1 for a Lab map, 1 kept, anything else 0
uint32_t not_listed_want(const std::string &main, uint32_t value) { return is_lab(main) || value == 1 ? 1 : 0; }

// the map list every PC must have: exactly net18's maps in its order [(map id, PS3 slot)]
std::vector<std::pair<uint32_t, uint32_t>> map_order(const Dc &b) {
    Table t = table(b, MAPS);
    std::vector<std::pair<uint32_t, uint32_t>> r;
    for (uint32_t i = 0; i < t.n; i++) r.push_back({b.u32(t.arr + i * MAP3), i});
    return r;
}

using Items = std::vector<std::pair<uint32_t, uint32_t>>;   // (netgame name, weight)

Items playlist_list(const Dc &b, uint64_t m3) {
    uint64_t l3 = b.ptr(m3 + 0x2c);
    Items r;
    for (uint32_t k = 0; k < b.u32(l3); k++) r.push_back({b.u32(b.ptr(l3 + 4) + 8 * k), b.u32(b.ptr(l3 + 4) + 8 * k + 4)});
    return r;
}

// PS3's netgame list of playlist m3 without the Lab netgames; hidden = it had netgames and none is left
Items pc_playlist_list(const Dc &b, uint64_t m3, const std::set<uint32_t> &lab_names, bool &hidden) {
    Items L3 = playlist_list(b, m3), L;
    for (const auto &x : L3) if (!lab_names.count(x.first)) L.push_back(x);
    hidden = !L3.empty() && L.empty();
    return L;
}

using Rec = std::vector<std::vector<uint32_t>>;

Rec hero_records(const Dc &c, uint64_t t, uint64_t step) {
    Rec r(3);
    for (uint64_t j = 0; j < 3; j++)
        for (uint64_t w = 0; w < 9; w++) r[j].push_back(c.u32(c.ptr(t + step * j) + 4 * w));
    return r;
}

Rec reversed_word0(Rec r) {   // the form coop-desc-fix leaves on Co-op Adventure netgames: word 0 in PS3 byte order
    for (auto &x : r) x[0] = __builtin_bswap32(x[0]);
    return r;
}

const char *tf(bool v) { return v ? "True" : "False"; }

// every difference between the PC net.bin c and the table it must have after this step (net18 b); empty = done
std::vector<std::string> check(const Dc &c, const Dc &b) {
    std::vector<std::string> err;
    LabInfo L;
    try {
        L = lab_netgames(b, c);
    } catch (const std::invalid_argument &x) {
        return {x.what()};
    }
    if (c.reloc + 4 > c.d.size() || c.reloc + 4 + rd32le(&c.d[c.reloc]) != c.d.size()) err.push_back("container size");
    for (uint64_t p : c.ptrs) {
        uint64_t v = c.ptr(p);
        if (!(0 < v && v < c.reloc)) {
            err.push_back(fmt("pointer %llx -> %llx", (unsigned long long)p, (unsigned long long)v));
            break;
        }
    }
    Table t = table(c), t3 = table(b);
    if (t.n != t3.n) err.push_back(fmt("netgame count %u, PS3 %u", t.n, t3.n));
    for (uint32_t i = 0; i < std::min(t.n, t3.n); i++) {
        uint64_t g = t.arr + i * NG4, s = t3.arr + i * NG3;
        if (c.u32(g) != b.u32(s) || c.u32(g + 4) != b.u32(s + 4) || c.u32(g + 0xc) != b.u32(s + 0xc)) {
            err.push_back(fmt("index %u: %08x map %08x type %d, PS3 %08x map %08x type %d", i, c.u32(g), c.u32(g + 4),
                              int(c.u32(g + 0xc)), b.u32(s), b.u32(s + 4), int(b.u32(s + 0xc))));
            continue;
        }
        for (const Pair &p : MAP)
            if (p.k4 != NONE && c.u32(g + p.k4) != b.u32(s + p.k3))
                err.push_back(fmt("index %u %08x: +%llx = %x, PS3 +%llx = %x", i, c.u32(g), (unsigned long long)p.k4,
                                  c.u32(g + p.k4), (unsigned long long)p.k3, b.u32(s + p.k3)));
        for (const Pair &p : PTRS)
            if (c.isptr(g + p.k4) != b.isptr(s + p.k3))
                err.push_back(fmt("index %u %08x: pointer +%llx present %s, PS3 +%llx %s", i, c.u32(g), (unsigned long long)p.k4,
                                  tf(c.isptr(g + p.k4)), (unsigned long long)p.k3, tf(b.isptr(s + p.k3))));
        if (c.isptr(g + 0x70) && b.isptr(s + 0x74)) {
            Rec r4 = hero_records(c, c.ptr(g + 0x70), 8), r3 = hero_records(b, b.ptr(s + 0x74), 4);
            if (r4 != r3 && !(b.u32(s + 0xc) == COOP_ADV && r4 == reversed_word0(r3)))
                err.push_back(fmt("index %u %08x: co-op hero records differ from PS3", i, c.u32(g)));
        }
    }
    Table tm = table(c, MAPS), tm3 = table(b, MAPS);
    std::set<uint32_t> maps;
    std::vector<uint32_t> map_list;
    for (uint32_t i = 0; i < tm.n; i++) { maps.insert(c.u32(tm.arr + i * MAP4)); map_list.push_back(c.u32(tm.arr + i * MAP4)); }
    for (uint32_t i = 0; i < tm3.n; i++)
        if (!maps.count(b.u32(tm3.arr + i * MAP3))) err.push_back("PS3 map " + hex8(b.u32(tm3.arr + i * MAP3)) + " is not in the map list");
    std::vector<uint32_t> want;
    for (const auto &m : map_order(b)) want.push_back(m.first);
    if (map_list != want) err.push_back("map list is not exactly the net18 maps in net18 order");
    for (uint32_t i = 0; i < tm.n; i++) {
        uint32_t v = c.u32(tm.arr + i * MAP4 + MAP_NOT_LISTED);
        if (v > 1 || (L.maps.count(c.u32(tm.arr + i * MAP4)) && v != 1))
            err.push_back(fmt("map %08x +0x94 = %#x (Lab maps 1, others 0 or 1)", c.u32(tm.arr + i * MAP4), v));
    }
    for (uint32_t i = 0; i < t.n; i++) {
        uint32_t m = c.u32(t.arr + i * NG4 + 4);
        if (m && !maps.count(m)) err.push_back(fmt("netgame %u map %08x is not in the map list", i, m));
    }
    std::set<uint32_t> names;
    for (uint32_t i = 0; i < t.n; i++) names.insert(c.u32(t.arr + i * NG4));
    Table tp = table(c, PL), tp3 = table(b, PL);
    if (tp.n != tp3.n) err.push_back(fmt("playlist count %u, PS3 %u", tp.n, tp3.n));
    for (uint32_t j = 0; j < std::min(tp.n, tp3.n); j++) {
        uint64_t m = tp.arr + j * PL4, m3 = tp3.arr + j * PL3;
        if (c.u32(m) != b.u32(m3)) {
            err.push_back(fmt("playlist %u: %08x, PS3 %08x", j, c.u32(m), b.u32(m3)));
            continue;
        }
        for (const Pair &p : PL_MAP)
            if (!PL_KEEP_PC.count(p.k4) && c.u32(m + p.k4) != b.u32(m3 + p.k3))
                err.push_back(fmt("playlist %08x +%llx", c.u32(m), (unsigned long long)p.k4));
        uint64_t l = c.ptr(m + 0x30);
        Items Lc;
        for (uint32_t k = 0; k < c.u32(l); k++) Lc.push_back({c.u32(c.ptr(l + 8) + 8 * k), c.u32(c.ptr(l + 8) + 8 * k + 4)});
        bool hidden;
        Items L3 = pc_playlist_list(b, m3, L.names, hidden);
        if (Lc != L3) err.push_back(fmt("playlist %08x netgame list", c.u32(m)));
        if ((c.u32(m + PL_PACK) == HIDDEN_PACK) != hidden)
            err.push_back(fmt("playlist %08x content pack %#x (%s)", c.u32(m), c.u32(m + PL_PACK), hidden ? "must be hidden" : "hidden"));
        bool unknown = false;
        for (const auto &x : Lc) if (!names.count(x.first)) unknown = true;
        if (unknown) err.push_back(fmt("playlist %08x names a netgame that is not in the table", c.u32(m)));
        if (c.isptr(m + 0x40) != b.isptr(m3 + 0x38)) {
            err.push_back(fmt("playlist %08x pool presence", c.u32(m)));
        } else if (b.isptr(m3 + 0x38)) {
            uint64_t p = c.ptr(m + 0x40), p3 = b.ptr(m3 + 0x38);
            std::vector<uint32_t> a4, a3;
            for (uint32_t k = 0; k < c.u32(p); k++) a4.push_back(c.u32(c.ptr(p + 8) + 4 * k));
            for (uint32_t k = 0; k < b.u32(p3); k++) a3.push_back(b.u32(b.ptr(p3 + 4) + 4 * k));
            if (a4 != a3) err.push_back(fmt("playlist %08x pool", c.u32(m)));
        }
    }
    return err;
}

// ---------------------------------------------------------------- the step log (JSON, as the earlier versions wrote it)
struct J {
    enum K { Null, Bool, Int, Str, Arr, Obj } k = Null;
    bool b = false;
    long long i = 0;
    std::u32string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;

    static J boolean(bool v) { J j; j.k = Bool; j.b = v; return j; }
    static J num(long long v) { J j; j.k = Int; j.i = v; return j; }
    static J latin1(const std::string &v) { J j; j.k = Str; for (unsigned char ch : v) j.s += char32_t(ch); return j; }
    static J utf8s(const std::string &v);
    static J arr() { J j; j.k = Arr; return j; }
    static J obj() { J j; j.k = Obj; return j; }
    J &add(const std::string &key, J v) { o.push_back({key, std::move(v)}); return *this; }
    J &push(J v) { a.push_back(std::move(v)); return *this; }
    const J *get(const std::string &key) const {
        for (const auto &kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string dump(int indent = 0) const;
};

J J::utf8s(const std::string &v) {
    J j;
    j.k = Str;
    for (size_t p = 0; p < v.size();) {
        unsigned char c = v[p];
        char32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int q = 1; q < n && p + q < v.size(); q++) cp = cp << 6 | (v[p + q] & 0x3f);
        j.s += cp;
        p += n;
    }
    return j;
}

std::string json_str(const std::u32string &s) {
    std::string r = "\"";
    auto u = [&](unsigned v) { r += fmt("\\u%04x", v); };
    for (char32_t c : s) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else if (c == '\b') r += "\\b";
        else if (c == '\f') r += "\\f";
        else if (c >= 0x20 && c <= 0x7e) r += char(c);
        else if (c > 0xffff) { char32_t v = c - 0x10000; u(0xd800 | (v >> 10)); u(0xdc00 | (v & 0x3ff)); }
        else u(unsigned(c));
    }
    return r + "\"";
}

std::string J::dump(int ind) const {   // as Python json.dumps(..., indent=1)
    std::string pad(ind + 1, ' '), end(ind, ' ');
    switch (k) {
    case Null: return "null";
    case Bool: return b ? "true" : "false";
    case Int: return std::to_string(i);
    case Str: return json_str(s);
    case Arr: {
        if (a.empty()) return "[]";
        std::string r = "[\n";
        for (size_t x = 0; x < a.size(); x++) r += pad + a[x].dump(ind + 1) + (x + 1 < a.size() ? ",\n" : "\n");
        return r + end + "]";
    }
    case Obj: {
        if (o.empty()) return "{}";
        std::string r = "{\n";
        for (size_t x = 0; x < o.size(); x++)
            r += pad + json_str(J::latin1(o[x].first).s) + ": " + o[x].second.dump(ind + 1) + (x + 1 < o.size() ? ",\n" : "\n");
        return r + end + "}";
    }
    }
    return "null";
}

std::string pct_x(uint64_t v) { return v ? fmt("0x%llx", (unsigned long long)v) : "0"; }   // Python '%#x'

// ---------------------------------------------------------------- inputs
bool py_space(unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f) || c == 0x85 || c == 0xa0; }

// level names the PC registry defines (overlay-r3 pak23_ps4.txt 'level-name' lines); empty when it is missing
std::set<std::string> pc_levels(const fs::path &registry) {
    std::set<std::string> r;
    std::error_code ec;
    if (!fs::is_regular_file(registry, ec)) return r;
    Bytes b = read_file(registry);
    std::string text(b.begin(), b.end());
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = pos;
        while (e < text.size() && text[e] != '\n' && text[e] != '\r') e++;
        std::string ln = text.substr(pos, e - pos);
        if (ln.rfind("level-name ", 0) == 0) {
            size_t a = 10;
            while (a < ln.size() && py_space((unsigned char)ln[a])) a++;
            size_t z = ln.size();
            while (z > a && py_space((unsigned char)ln[z - 1])) z--;
            if (z > a) r.insert(ln.substr(a, z - a));
        }
        if (e >= text.size()) break;
        pos = e + ((text[e] == '\r' && e + 1 < text.size() && text[e + 1] == '\n') ? 2 : 1);
    }
    return r;
}

// the description of a net18.bin version in tools/dcconv/net18-versions.json ("accepted": {sha256: text}); the log only
std::string accepted_net18(const std::string &sha) {
    const char *dc = std::getenv("DCCONV");   // tools/dcconv, set by the recipe driver
    if (!dc || !*dc) return "version list not found";
    fs::path p = upath(dc) / "net18-versions.json";
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return "version list not found";
    Bytes b = read_file(p);
    std::string t(b.begin(), b.end());
    size_t acc = t.find("\"accepted\"");
    if (acc == std::string::npos) return "version list not found";
    size_t at = t.find("\"" + sha + "\"", acc);
    if (at == std::string::npos) return "not in tools/dcconv/net18-versions.json";
    size_t q = t.find('"', t.find(':', at + sha.size() + 2));
    std::string v;
    for (size_t x = q + 1; x < t.size() && t[x] != '"'; x++) {
        if (t[x] == '\\' && x + 1 < t.size()) x++;
        v += t[x];
    }
    return v;
}

// ---------------------------------------------------------------- the rebuild
Bytes build(const fs::path &net, const Dc &b, const fs::path &registry, J &log) {
    Dc a(net);
    if (a.ps != 8 || b.ps != 4) throw Stop("expected a PS4 net.bin and a PS3 net18.bin");
    DcEdit e(net);
    std::map<std::string, uint64_t> strs;

    auto string = [&](const std::string &s) -> uint64_t {   // an existing string of the original part, else appended
        auto it = strs.find(s);
        if (it != strs.end()) return it->second;
        std::string enc = s + '\0';
        auto find = [&](size_t from) -> int64_t {
            if (from > a.d.size()) return -1;
            auto f = std::search(a.d.begin() + static_cast<std::ptrdiff_t>(from), a.d.end(), enc.begin(), enc.end());
            return f == a.d.end() ? -1 : int64_t(f - a.d.begin());
        };
        int64_t pos = find(0);
        while (pos >= 0 && uint64_t(pos) < a.reloc) {
            if (pos == 0 || a.d[pos - 1] == 0) break;
            pos = find(size_t(pos) + 1);
        }
        uint64_t r = (pos < 0 || uint64_t(pos) >= a.reloc) ? e.add_string(s) : uint64_t(pos);
        strs[s] = r;
        return r;
    };
    auto obj = [&](uint32_t t, size_t size) -> uint64_t {   // PS4 object: 8-byte header [0, type]
        e.align(8);
        uint64_t h = e.alloc(8, 1);
        e.raw32(h + 4, t);
        return e.alloc(size, 1);
    };
    auto obj4 = [&](uint32_t t, const std::vector<uint32_t> &w) -> uint64_t {   // 4-byte header [type] + words
        e.align(4);
        uint64_t h = e.alloc(4, 1);
        e.raw32(h, t);
        uint64_t o = e.alloc(4 * w.size(), 1);
        for (size_t k = 0; k < w.size(); k++) e.raw32(o + 4 * k, w[k]);
        return o;
    };
    auto put = [&](uint64_t o, uint32_t v) {
        if (e.ptrs.count(o & ~uint64_t(7))) throw Stop(fmt("internal: scalar write over a pointer slot at %llx", (unsigned long long)o));
        e.raw32(o, v);
    };

    // ---------------------------------------------------------------- map list in PS3 order, Lab maps hidden
    Table tm = table(a, MAPS);
    std::map<uint32_t, uint32_t> pc_at;
    std::vector<uint32_t> pc_list;
    for (uint32_t i = 0; i < tm.n; i++) { pc_at[a.u32(tm.arr + i * MAP4)] = i; pc_list.push_back(a.u32(tm.arr + i * MAP4)); }
    auto order = map_order(b);
    LabInfo L;
    try {
        L = lab_netgames(b, a);
    } catch (const std::invalid_argument &x) {
        throw Stop(x.what());
    }
    auto mains4 = map_mains(a);
    J maps_added = J::arr(), lab_hidden = J::arr(), dropped_maps = J::arr();
    std::vector<uint32_t> order_ids;
    std::set<uint32_t> map_ids;
    for (const auto &m : order) { order_ids.push_back(m.first); map_ids.insert(m.first); }
    bool order_changed = pc_list != order_ids;
    {   // in the order of first appearance in the PC list
        std::set<uint32_t> seen;
        for (uint32_t m : pc_list)
            if (seen.insert(m).second && !map_ids.count(m)) dropped_maps.push(J::latin1(hex8(m) + " " + mains4[m]));
    }
    bool hide = false;
    for (const auto &[m, slot3] : order) {
        (void)slot3;
        if (!pc_at.count(m)) continue;
        uint32_t v = a.u32(tm.arr + pc_at[m] * MAP4 + MAP_NOT_LISTED);
        if (v != not_listed_want(mains4[m], v)) hide = true;
    }
    if (order_changed || hide) {
        std::set<std::string> levels = pc_levels(registry);
        uint64_t am3 = table(b, MAPS).arr;
        uint64_t na = e.alloc(MAP4 * order.size(), 16);
        for (size_t slot = 0; slot < order.size(); slot++) {
            uint32_t mid = order[slot].first, i3 = order[slot].second;
            uint64_t m = na + MAP4 * slot;
            if (pc_at.count(mid)) {   // the PC entry, copied whole (PS4 sub-structures and earlier fixes kept)
                uint32_t i = pc_at[mid];
                e.copy_words(tm.arr + i * MAP4, m, MAP4);
                uint32_t was = a.u32(tm.arr + i * MAP4 + MAP_NOT_LISTED);
                uint32_t w = not_listed_want(mains4[mid], was);
                if (w != was) put(m + MAP_NOT_LISTED, w);
                if (is_lab(mains4[mid]) && was != 1)
                    lab_hidden.push(J::obj().add("id", J::latin1(hex8(mid))).add("main", J::latin1(mains4[mid]))
                                        .add("slot", J::num(slot)).add("was", J::num(was)));
                continue;
            }
            uint64_t src = am3 + i3 * MAP3;
            for (uint64_t k = 0; k < MAP3; k += 4) {
                if (b.isptr(src + k) && !has_key(MAP_STR, k) && k != 0x30)
                    throw Stop(fmt("PS3 map %08x: unexpected pointer at +%llx", b.u32(src), (unsigned long long)k));
                bool known = has_key(MAP_STR, k) || has_key(MAP_U32, k) || k == 0x30 || k == MAP3_PS3_ONLY;
                if (!known && b.u32(src + k))
                    throw Stop(fmt("PS3 map %08x: unexpected value %x at +%llx", b.u32(src), b.u32(src + k), (unsigned long long)k));
            }
            if (!b.isptr(src + 0x30)) throw Stop(fmt("PS3 map %08x has no sub-level list", b.u32(src)));
            uint64_t l3 = b.ptr(src + 0x30);
            if (b.u32(l3 - 4) != MAP_LIST_T) throw Stop(fmt("PS3 map %08x: sub-level list type %08x", b.u32(src), b.u32(l3 - 4)));
            std::vector<std::string> sub;
            for (uint32_t k = 0; k < b.u32(l3); k++) {
                std::string s = b.str_or_empty(b.ptr(b.ptr(l3 + 4) + 8 * k));
                if (!DROP_LEVELS.count(s)) sub.push_back(s);
            }
            uint64_t items = e.alloc(16 * sub.size(), 8);
            for (size_t k = 0; k < sub.size(); k++) {
                e.putptr(items + 16 * k, string(sub[k]));
                e.raw32(items + 16 * k + 8, sid(sub[k]));
            }
            uint64_t lst = obj(MAP_LIST_T, 16);
            e.raw32(lst, uint32_t(sub.size()));
            e.putptr(lst + 8, items);
            for (const Pair &p : MAP_U32) put(m + p.k4, b.u32(src + p.k3));
            for (const Pair &p : MAP_STR) {
                if (b.isptr(src + p.k3)) e.putptr(m + p.k4, string(b.str_or_empty(b.ptr(src + p.k3))));
                else if (b.u32(src + p.k3))
                    throw Stop(fmt("PS3 map %08x: +%llx is a value, not a string", b.u32(src), (unsigned long long)p.k3));
            }
            e.putptr(m + 0x58, lst);
            std::string main = b.isptr(src + 0x18) ? b.str_or_empty(b.ptr(src + 0x18)) : std::string();
            uint32_t not_listed = (!main.empty() && levels.count(main) && !is_lab(main)) ? 0 : 1;
            put(m + MAP_NOT_LISTED, not_listed);
            maps_added.push(J::obj().add("id", J::latin1(hex8(mid))).add("ps3_slot", J::num(i3)).add("main", J::latin1(main))
                                .add("sublevels", J::num(sub.size())).add("not_listed", J::num(not_listed))
                                .add("lab", J::boolean(is_lab(main))));
        }
        e.put32(tm.hdr, uint32_t(order.size()));
        e.putptr(tm.hdr + 8, na);
    }
    log.add("maps_added", maps_added).add("lab_maps_hidden", lab_hidden).add("map_order_changed", J::boolean(order_changed))
        .add("pc_only_maps_dropped", dropped_maps);

    // ---------------------------------------------------------------- netgames in PS3 order
    Table t4 = table(a), t3 = table(b);
    for (uint32_t i = 0; i < t4.n; i++) {
        uint64_t g = t4.arr + i * NG4;
        std::string bad;
        for (uint64_t k = 0; k < NG4; k += 8) {
            bool slot = false;
            for (const Pair &p : PTRS) if (uint64_t(p.k4) == k) slot = true;
            if (a.isptr(g + k) && !slot) bad += (bad.empty() ? "" : ",") + fmt("%llx", (unsigned long long)k);
        }
        if (!bad.empty()) throw Stop(fmt("PC netgame %u (%08x): unexpected pointer at +%s", i, a.u32(g), bad.c_str()));
    }
    std::map<uint32_t, std::vector<uint32_t>> pc_by;
    for (uint32_t i = 0; i < t4.n; i++) pc_by[a.u32(t4.arr + i * NG4)].push_back(i);
    std::set<uint32_t> used;
    struct Plan { bool pc; uint32_t i3, i4; };
    std::vector<Plan> plan;
    for (uint32_t i3 = 0; i3 < t3.n; i3++) {
        uint64_t src = t3.arr + i3 * NG3;
        std::string bad;
        for (uint64_t k = 0; k < NG3; k += 4)
            if (b.isptr(src + k) && !has_key(PTRS, k)) bad += (bad.empty() ? "" : ",") + fmt("%llx", (unsigned long long)k);
        if (!bad.empty()) throw Stop(fmt("PS3 netgame %u (%08x): unexpected pointer at +%s", i3, b.u32(src), bad.c_str()));
        for (const Pair &p : MAP)
            if (p.k4 == NONE && b.u32(src + p.k3) && !(dropped_classic(p.k3) && b.u32(src + 0xc) == CLASSIC))
                throw Stop(fmt("PS3 netgame %u (%08x): +%llx = %x has no PC field", i3, b.u32(src), (unsigned long long)p.k3, b.u32(src + p.k3)));
        if (b.u32(src + 4) && !map_ids.count(b.u32(src + 4)))
            throw Stop(fmt("PS3 netgame %u (%08x): map %08x is in no map list", i3, b.u32(src), b.u32(src + 4)));
        int64_t free = -1;
        auto f = pc_by.find(b.u32(src));
        if (f != pc_by.end())
            for (uint32_t i : f->second) if (!used.count(i)) { free = i; break; }
        if (free >= 0) { used.insert(uint32_t(free)); plan.push_back({true, i3, uint32_t(free)}); }
        else plan.push_back({false, i3, 0});
    }
    uint64_t na = e.alloc(NG4 * plan.size(), 16);
    int n_pc = 0, n_conv = 0;
    std::map<std::string, int> changed;
    for (size_t j = 0; j < plan.size(); j++) {
        uint64_t g = na + NG4 * j, src = t3.arr + plan[j].i3 * NG3;
        if (plan[j].pc) {
            uint64_t pc = t4.arr + plan[j].i4 * NG4;
            for (const Pair &p : PTRS)
                if (a.isptr(pc + p.k4) != b.isptr(src + p.k3))
                    throw Stop(fmt("netgame %08x: pointer +%llx on PC %s, PS3 +%llx %s: the PC sub-structure does not match PS3",
                                   b.u32(src), (unsigned long long)p.k4, tf(a.isptr(pc + p.k4)), (unsigned long long)p.k3,
                                   tf(b.isptr(src + p.k3))));
            e.copy_words(pc, g, NG4);
            for (const Pair &p : MAP) {
                if (p.k4 == NONE) continue;
                uint32_t v = b.u32(src + p.k3);
                if (e.u32(g + p.k4) != v) {
                    changed[fmt("+%02llx", (unsigned long long)p.k4)]++;
                    put(g + p.k4, v);
                }
            }
            n_pc++;
        } else {
            bool other = false;
            for (const Pair &p : PTRS) if (p.k3 != 0x74 && b.isptr(src + p.k3)) other = true;
            if (!b.isptr(src + 0x74) || other)
                throw Stop(fmt("PS3 netgame %u (%08x) is not on the PC and has pointers other than the +0x74 hero records: "
                               "needs a sub-structure converter", plan[j].i3, b.u32(src)));
            for (const Pair &p : MAP) if (p.k4 != NONE) put(g + p.k4, b.u32(src + p.k3));
            uint64_t tt = b.ptr(src + 0x74);
            if (b.u32(tt - 4) != NG_OBJ) throw Stop(fmt("PS3 netgame %08x: +0x74 object type %08x", b.u32(src), b.u32(tt - 4)));
            std::vector<uint64_t> subs;
            for (uint64_t k = 0; k < 3; k++) {
                uint64_t r = b.ptr(tt + 4 * k);
                if (b.u32(r - 4) != NG_SUB) throw Stop(fmt("PS3 netgame %08x: hero record type %08x", b.u32(src), b.u32(r - 4)));
                std::vector<uint32_t> w;
                for (uint64_t x = 0; x < 9; x++) w.push_back(b.u32(r + 4 * x));
                if (b.u32(src + 0xc) == COOP_ADV) w[0] = __builtin_bswap32(w[0]);   // the coop-desc-fix form
                subs.push_back(obj4(NG_SUB, w));
            }
            uint64_t tobj = obj(NG_OBJ, 24);
            for (uint64_t k = 0; k < 3; k++) e.putptr(tobj + 8 * k, subs[k]);
            e.putptr(g + 0x70, tobj);
            n_conv++;
        }
    }
    e.put32(t4.hdr, uint32_t(plan.size()));
    e.putptr(t4.hdr + 8, na);
    J dropped = J::arr(), converted = J::arr(), chg = J::obj();
    int n_dropped = 0;
    for (uint32_t i = 0; i < t4.n; i++)
        if (!used.count(i)) { dropped.push(J::latin1(fmt("%u:%08x", i, a.u32(t4.arr + i * NG4)))); n_dropped++; }
    for (const Plan &p : plan)
        if (!p.pc) converted.push(J::latin1(fmt("%u:%08x", p.i3, b.u32(t3.arr + p.i3 * NG3))));
    for (const auto &kv : changed) chg.add(kv.first, J::num(kv.second));
    log.add("netgames", J::obj().add("total", J::num(plan.size())).add("ps3", J::num(t3.n)).add("pc_before", J::num(t4.n))
                            .add("copied_from_pc", J::num(n_pc)).add("converted", J::num(n_conv)).add("pc_only_dropped", J::num(n_dropped)));
    log.add("ps3_values_changed_fields", chg).add("pc_only_dropped", dropped).add("converted", converted)
        .add("indices_over_255", J::num(plan.size() > 256 ? plan.size() - 256 : 0));

    // ---------------------------------------------------------------- playlists in PS3 order
    Table tp4 = table(a, PL), tp3 = table(b, PL);
    std::map<uint32_t, uint64_t> pc_pl;
    for (uint32_t i = 0; i < tp4.n; i++) pc_pl[a.u32(tp4.arr + i * PL4)] = tp4.arr + i * PL4;
    std::set<uint32_t> names;
    for (uint32_t i = 0; i < t3.n; i++) names.insert(b.u32(t3.arr + i * NG3));
    uint64_t npl = e.alloc(PL4 * tp3.n, 16);
    J plog = J::arr();
    int removed_total = 0;
    J hidden_ids = J::arr();
    for (uint32_t j = 0; j < tp3.n; j++) {
        uint64_t m3 = tp3.arr + j * PL3, m = npl + PL4 * j;
        uint32_t pid = b.u32(m3);
        auto tp = pc_pl.find(pid);
        if (tp == pc_pl.end()) throw Stop(fmt("PS3 playlist %08x has no PC entry to take the PS4 layout from", pid));
        uint64_t tpl = tp->second;
        std::set<uint64_t> p3, p4;
        for (uint64_t k = 0; k < PL3; k += 4) if (b.isptr(m3 + k)) p3.insert(k);
        for (uint64_t k = 0; k < PL4; k += 8) if (a.isptr(tpl + k)) p4.insert(k);
        bool ok = std::includes(PL_PTRS3.begin(), PL_PTRS3.end(), p3.begin(), p3.end()) && p3.count(0x2c) &&
                  std::includes(PL_PTRS4.begin(), PL_PTRS4.end(), p4.begin(), p4.end()) && p4.count(0x30);
        if (!ok) {
            auto list = [](const std::set<uint64_t> &s) {
                std::string r = "[";
                for (uint64_t x : s) r += (r.size() > 1 ? ", " : "") + std::to_string(x);
                return r + "]";
            };
            throw Stop(fmt("playlist %08x: unexpected pointers PS3 %s PC %s", pid, list(p3).c_str(), list(p4).c_str()));
        }
        uint64_t l3 = b.ptr(m3 + 0x2c);
        if (b.u32(l3 - 4) != PL_LIST_T) throw Stop(fmt("PS3 playlist %08x: list type %08x", pid, b.u32(l3 - 4)));
        e.copy_words(tpl, m, PL4);
        for (const Pair &p : PL_MAP) if (!PL_KEEP_PC.count(p.k4)) put(m + p.k4, b.u32(m3 + p.k3));
        Items items3 = playlist_list(b, m3);
        bool hidden;
        Items items = pc_playlist_list(b, m3, L.names, hidden);
        uint32_t pack = a.u32(tpl + PL_PACK);
        if (hidden) put(m + PL_PACK, HIDDEN_PACK);
        else if (pack == HIDDEN_PACK) put(m + PL_PACK, 1);
        uint64_t it = e.alloc(std::max<size_t>(8 * items.size(), 8), 8);
        for (size_t k = 0; k < items.size(); k++) { e.raw32(it + 8 * k, items[k].first); e.raw32(it + 8 * k + 4, items[k].second); }
        uint64_t lst = obj(PL_LIST_T, 16);
        e.raw32(lst, uint32_t(items.size()));
        e.putptr(lst + 8, it);
        e.putptr(m + 0x30, lst);
        if (p3.count(0x38)) {
            uint64_t q3 = b.ptr(m3 + 0x38);
            if (b.u32(q3 - 4) != PL_POOL_T) throw Stop(fmt("PS3 playlist %08x: pool type %08x", pid, b.u32(q3 - 4)));
            uint32_t pn = b.u32(q3);
            uint64_t pit3 = b.ptr(q3 + 4);
            uint64_t pit = e.alloc(std::max<size_t>(4 * size_t(pn), 8), 8);
            for (uint32_t k = 0; k < pn; k++) e.raw32(pit + 4 * k, b.u32(pit3 + 4 * k));
            uint64_t pool = obj(PL_POOL_T, 16);
            e.raw32(pool, pn);
            e.putptr(pool + 8, pit);
            e.putptr(m + 0x40, pool);
        } else if (p4.count(0x40)) {   // PS3 has no pool here: none on the PC either
            e.ptrs.erase(m + 0x40);
            wr64le(&e.d[m + 0x40], 0);
        }
        std::string missing;
        for (const auto &x : items3)
            if (!names.count(x.first)) missing += (missing.empty() ? "'" : ", '") + hex8(x.first) + "'";
        if (!missing.empty()) throw Stop(fmt("PS3 playlist %08x names netgames PS3 does not have: [%s]", pid, missing.c_str()));
        J removed = J::arr();
        long long w3 = 0, w = 0;
        for (const auto &x : items3) { w3 += x.second; if (L.names.count(x.first)) { removed.push(J::latin1(hex8(x.first))); removed_total++; } }
        for (const auto &x : items) w += x.second;
        uint32_t shown_pack = hidden ? HIDDEN_PACK : (pack == HIDDEN_PACK ? 1 : pack);
        if (hidden) hidden_ids.push(J::latin1(hex8(pid)));
        plog.push(J::obj().add("id", J::latin1(hex8(pid))).add("name", J::latin1(p3.count(0x10) ? b.str_or_empty(b.ptr(m3 + 0x10)) : ""))
                      .add("netgames_ps3", J::num(items3.size())).add("netgames", J::num(items.size())).add("lab_removed", removed)
                      .add("weight_ps3", J::num(w3)).add("weight", J::num(w)).add("pack", J::latin1(pct_x(shown_pack)))
                      .add("hidden", J::boolean(hidden)).add("pool", J::num(p3.count(0x38) ? b.u32(b.ptr(m3 + 0x38)) : 0)));
    }
    e.put32(tp4.hdr, tp3.n);
    e.putptr(tp4.hdr + 8, npl);
    log.add("playlists", plog);
    J labmaps = J::arr(), labng = J::arr();
    for (uint32_t x : L.maps) labmaps.push(J::latin1(hex8(x)));   // ascending ids = sorted hex strings
    for (const auto &kv : L.netgames) labng.push(J::latin1(fmt("%u:%08x", kv.first, kv.second)));
    long long hidden_maps = (long long)lab_hidden.a.size();
    for (const J &x : maps_added.a) if (x.get("lab")->b) hidden_maps++;
    log.add("lab", J::obj().add("maps", labmaps).add("netgames_kept_unlisted", labng).add("maps_hidden", J::num(hidden_maps))
                       .add("playlist_entries_removed", J::num(removed_total)).add("playlists_hidden", hidden_ids));

    Bytes out = e.build();
    // nothing but the table headers (and the reloc offset) may differ in the original part
    std::vector<bool> allowed(a.reloc, false);
    for (uint64_t x = 8; x < 16 && x < a.reloc; x++) allowed[x] = true;
    for (const auto &[o, ln] : e.patched)
        for (uint64_t x = o; x < o + ln && x < a.reloc; x++) allowed[x] = true;
    for (uint64_t x = 0; x < a.reloc; x++)
        if (out[x] != a.d[x] && !allowed[x]) throw Stop(fmt("internal: original bytes changed at %llx", (unsigned long long)x));
    for (uint64_t p : a.ptrs)
        if (!e.ptrs.count(p)) throw Stop(fmt("internal: pointer bits lost at %llx", (unsigned long long)p));
    return out;
}

}  // namespace

int netgame_order(const std::vector<std::string> &argv) {
    Args A(argv, {"--apply", "--verify"}, {"--project", "--net", "--modules", "--net18", "--backup", "--registry"});
    std::string project = A.get("--project", "");
    std::string dir = project + "/work/overlay-r3/u3data/build/main/dc1/ps4/";
    if (project.empty() && (!A.val.count("--net") || !A.val.count("--modules") || !A.val.count("--backup")))
        throw Stop("missing --project");
    fs::path net = upath(A.get("--net", dir + "net.bin")), modules = upath(A.get("--modules", dir + "modules.bin"));
    fs::path backup = upath(A.get("--backup", project + "/work/backup-netgame-order/"));
    fs::path registry = upath(A.get("--registry", project + "/work/overlay-r3/u3data/build/main/pak23_ps4.txt"));
    std::string net18s = A.need("--net18");
    fs::path net18 = upath(net18s);
    std::error_code ec;
    if (!fs::is_regular_file(net18, ec)) throw Stop("PS3 net18.bin not found: " + net18s);
    Bytes n18 = read_file(net18);
    std::string h = sha256_hex(n18);
    std::printf("net18.bin %s sha256 %s (%s)\n", net18s.c_str(), h.c_str(), accepted_net18(h).c_str());
    Dc b(std::move(n18), utf8(net18));
    std::vector<std::string> errs = check(Dc(net), b);
    if (A.has("--verify")) {
        for (size_t i = 0; i < errs.size() && i < 60; i++) std::printf("  %s\n", errs[i].c_str());
        std::printf("differences to net18: %d\n", int(errs.size()));
        return errs.empty() ? 0 : 1;
    }
    if (errs.empty()) {
        std::printf("already applied: the netgame table and playlists equal net18.bin (%u netgames), Lab maps and netgames hidden\n",
                    table(b).n);
        return 0;
    }
    std::printf("%d difference(s) to net18.bin, first: %s\n", int(errs.size()), errs[0].c_str());
    Bytes before = read_file(net);
    J log = J::obj();
    log.add("net18", J::utf8s(net18s)).add("net18_sha256", J::latin1(h))
        .add("net_before", J::obj().add("size", J::num(before.size())).add("sha256", J::latin1(sha256_hex(before))));
    Bytes out = build(net, b, registry, log);
    uint64_t slot = module_size_slot(Dc(modules), "net");
    Bytes mod = read_file(modules);
    uint64_t old = rd64le(&mod[slot]);
    if (old != before.size())
        throw Stop(fmt("modules.bin: module net size word %llu (at %#llx) is not the net.bin size %llu", (unsigned long long)old,
                       (unsigned long long)slot, (unsigned long long)before.size()));
    wr64le(&mod[slot], out.size());
    {   // the result must load and equal net18 before anything is written
        Dc c(out, "rebuilt net.bin");
        std::vector<std::string> left = check(c, b);
        if (!left.empty()) {
            for (size_t i = 0; i < left.size() && i < 20; i++) std::printf("  ERROR %s\n", left[i].c_str());
            throw Stop(fmt("the rebuilt table still differs from net18.bin in %d place(s); nothing written", int(left.size())));
        }
    }
    log.add("net_after", J::obj().add("size", J::num(out.size())).add("sha256", J::latin1(sha256_hex(out))));
    log.add("modules_net_size_slot", J::latin1(pct_x(slot)));
    J s = J::obj();
    for (const auto &kv : log.o)
        if (kv.first != "converted" && kv.first != "pc_only_dropped" && kv.first != "playlists" && kv.first != "lab_maps_hidden")
            s.add(kv.first, kv.second);
    std::printf("%s\n", s.dump().c_str());
    auto latin = [](const J &x) { std::string r; for (char32_t c : x.s) r += char(c); return r; };
    std::string dl;
    for (const J &x : log.get("pc_only_dropped")->a) dl += (dl.empty() ? "" : " ") + latin(x);
    std::printf("dropped PC-only netgames: %s\n", dl.c_str());
    std::string hid, add;
    for (const J &x : log.get("lab_maps_hidden")->a) hid += (hid.empty() ? "" : " ") + latin(*x.get("id")) + " " + latin(*x.get("main"));
    for (const J &x : log.get("maps_added")->a)
        if (x.get("lab")->b) add += (add.empty() ? "" : " ") + latin(*x.get("id")) + " " + latin(*x.get("main"));
    std::printf("Lab maps hidden (+0x94 = 1): %s | added hidden: %s\n", hid.empty() ? "-" : hid.c_str(), add.empty() ? "-" : add.c_str());
    for (const J &p : log.get("playlists")->a) {
        if (p.get("lab_removed")->a.empty() && !p.get("hidden")->b) continue;
        std::string rm;
        for (const J &x : p.get("lab_removed")->a) rm += (rm.empty() ? "" : " ") + latin(x);
        std::printf("playlist %s %s: %lld -> %lld netgames (removed %s), weight %lld -> %lld%s\n", latin(*p.get("id")).c_str(),
                    latin(*p.get("name")).c_str(), p.get("netgames_ps3")->i, p.get("netgames")->i, rm.c_str(), p.get("weight_ps3")->i,
                    p.get("weight")->i, p.get("hidden")->b ? (", hidden (content pack " + latin(*p.get("pack")) + ")").c_str() : "");
    }
    if (!A.has("--apply")) {
        std::printf("dry run: net.bin %llu -> %llu, modules.bin net size %llu -> %llu; nothing written\n", (unsigned long long)old,
                    (unsigned long long)out.size(), (unsigned long long)old, (unsigned long long)out.size());
        return 0;
    }
    fs::create_directories(backup);
    copy_once(net, backup / "net.bin");
    copy_once(modules, backup / "modules.bin");
    write_file_replace(net, out);
    write_file_replace(modules, mod);
    std::string text = log.dump();
    write_file_replace(backup / "netgame_order.log.json", Bytes(text.begin(), text.end()));
    std::printf("applied: net.bin %llu -> %llu (%u netgames, PS3 order), modules.bin net size %llu -> %llu\n", (unsigned long long)old,
                (unsigned long long)out.size(), table(b).n, (unsigned long long)old, (unsigned long long)out.size());
    return 0;
}

}  // namespace u3
