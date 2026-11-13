// dctools/stageplan.cpp: see stageplan.hpp.
#include "stageplan.hpp"

#include "inflate.h"   // launcher/native/inflate.h: gunzip, crc32
#include "proc.hpp"
#include "texcodec.hpp"
#include "u3fix.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <unordered_map>

namespace u3::stage {

// ------------------------------------------------------------------------------------------------ names and roles
namespace {

bool is_hex16(const std::string &s, size_t a) {
    if (a + 16 > s.size()) return false;
    for (size_t i = a; i < a + 16; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return true;
}

bool ends_with(const std::string &s, const std::string &e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
bool starts_with(const std::string &s, const std::string &e) { return s.compare(0, e.size(), e) == 0; }
bool has(const std::string &s, const char *x) { return s.find(x) != std::string::npos; }

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::vector<std::string> split(const std::string &s, char c) {
    std::vector<std::string> v;
    size_t a = 0;
    for (size_t i = 0;; i++)
        if (i == s.size() || s[i] == c) {
            v.push_back(s.substr(a, i - a));
            if (i == s.size()) break;
            a = i + 1;
        }
    return v;
}

bool rx(const std::string &s, const std::regex &r) { return std::regex_search(s, r); }

}  // namespace

std::string short_of(const std::string &full) {
    // re.search(r'texture[12]/(.*?)/[0-9A-F]{16}(\.ndb)?$'): the first 'texture1/' or 'texture2/' that the anchored
    // tail '/<16 hex>' or '/<16 hex>.ndb' follows
    size_t tail = 0;
    if (full.size() >= 21 && ends_with(full, ".ndb") && full[full.size() - 21] == '/' && is_hex16(full, full.size() - 20)) tail = 21;
    else if (full.size() >= 17 && full[full.size() - 17] == '/' && is_hex16(full, full.size() - 16)) tail = 17;
    if (!tail) return full;
    for (size_t i = full.find("texture"); i != std::string::npos; i = full.find("texture", i + 1)) {
        if (i + 9 > full.size() || (full[i + 7] != '1' && full[i + 7] != '2') || full[i + 8] != '/') continue;
        if (i + 9 + tail > full.size()) break;
        return full.substr(i + 9, full.size() - tail - (i + 9));
    }
    return full;
}

std::optional<std::string> tex_role(const std::string &shrt) {
    static const std::vector<std::pair<std::string, std::regex>> ROLE_RE = {
        {"normal", std::regex(R"(-normals?\b|-normal-|normal\.)")}, {"detail", std::regex("-detail")}, {"spec", std::regex("-spec")},
        {"mask", std::regex("-mask")}, {"blend", std::regex("-blend")}, {"trans", std::regex(R"(-trans|-alpha|-opacity)")},
        {"env", std::regex("refle|env")}, {"color", std::regex(R"(-color|-diffuse|-col\b)")}};
    const std::string s = lower(shrt);
    if (has(s, "lightmap") || has(s, "missingtexture") || has(s, ".atgi/") || has(s, "indirect.rgbmax")) return std::nullopt;
    for (const auto &[role, r] : ROLE_RE)
        if (rx(s, r)) return role;
    return std::string("color");
}

std::optional<std::string> tex_role_final(const std::string &shrt) {
    static const std::vector<std::pair<std::string, std::regex>> ROLE_FINAL_RE = {
        {"normal", std::regex(R"(-normals?(?=[-._]))")}, {"detail", std::regex(R"(-detail(?=[-._]))")}, {"spec", std::regex(R"(-spec(?=[-._]))")},
        {"mask", std::regex(R"(-mask(?=[-._]))")}, {"blend", std::regex(R"(-blend(?=[-._]))")},
        {"trans", std::regex(R"(-(trans\w*|alpha|opacity)(?=[-._]))")}, {"env", std::regex(R"((-env(?=[-._])|refle))")},
        {"color", std::regex(R"(-(color|diffuse|col)(?=[-._]))")}};
    static const std::regex COLOR_END(R"(-(color|diffuse|col)\.[a-z]+$)");
    if (!tex_role(shrt)) return std::nullopt;
    std::string b = lower(shrt);
    if (const size_t k = b.rfind('/'); k != std::string::npos) b = b.substr(k + 1);
    std::string role = "color";
    for (const auto &[r, re] : ROLE_FINAL_RE)
        if (rx(b, re)) {
            role = r;
            break;
        }
    if ((role == "blend" || role == "detail" || role == "env" || role == "mask") && rx(b, COLOR_END)) role = "color";
    return role;
}

std::optional<std::string> param_role(const std::string &p) {
    static const std::pair<const char *, const char *> PARAM_ROLE[] = {{"Normal", "normal"}, {"Detail", "detail"}, {"Mask", "mask"},
                                                                       {"Blend", "blend"}, {"Transparency", "trans"}, {"Environment", "env"},
                                                                       {"Color", "color"}, {"Spec", "spec"}, {"LightMap", "lightmap"}};
    for (const auto &[k, r] : PARAM_ROLE)
        if (has(p, k)) return std::string(r);
    return std::nullopt;
}

std::string ps4key(const std::string &n) {
    const size_t k = n.rfind('/');
    if (k == std::string::npos) throw Stop("ValueError: not enough values to unpack (expected 2, got 1)");
    return n.substr(0, k) + ":" + n.substr(k + 1);
}

// ------------------------------------------------------------------------------------------------ survey (mkres)
namespace {

// one JSON value as json.dumps writes it (', ' / ': ' separators, non-ASCII as \uXXXX of the latin-1 text)
void jstr(std::string &o, const std::string &latin1) { json::quote(o, bg::utf8_of(latin1)); }

std::string gzip_stored(const std::string &data) {   // a gzip member with stored deflate blocks
    std::string o("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    size_t i = 0;
    do {
        const size_t n = std::min<size_t>(65535, data.size() - i);
        o += char(i + n == data.size() ? 1 : 0);
        o += char(n & 0xff);
        o += char(n >> 8);
        o += char(~n & 0xff);
        o += char((~n >> 8) & 0xff);
        o.append(data, i, n);
        i += n;
    } while (i < data.size());
    const uint32_t crc = inflate::crc32(0, reinterpret_cast<const unsigned char *>(data.data()), data.size()), sz = uint32_t(data.size());
    for (int k = 0; k < 4; k++) o += char(crc >> (8 * k));
    for (int k = 0; k < 4; k++) o += char(sz >> (8 * k));
    return o;
}

std::string survey_line(const Pak &q, const std::string &fn) {
    std::string o = "{\"pak\": ";
    jstr(o, fn);
    o += ", \"size\": " + std::to_string(q.d.size()) + ", \"nfx\": " + std::to_string(q.items_of("EFFECT").size()) + ", \"mats\": [";
    bool firstm = true;
    for (const PakItem &g : q.items_of("GEOMETRY_1")) {
        const uint32_t pg = g.page;
        const uint64_t b = g.data_off();
        const uint32_t flags = q.pu32(pg, b + 4);
        const uint64_t n = q.u64(q.pbase(pg) + b + 0x48);
        const ORef r = bg::res(q, pg, b + 0x50);
        if (!r) continue;
        const auto [mp, mo] = *r;
        for (uint64_t i = 0; i < n; i++) {
            const uint64_t o_ = mo + i * 0x48;
            auto S = [&](uint64_t off, std::string &out) {
                const ORef s = bg::res(q, mp, o_ + off);
                if (s) jstr(out, q.pstr(s->first, s->second));
                else out += "null";
            };
            const uint64_t mb = q.pbase(mp) + o_;
            const uint32_t nf = rd32le(q.d, mb + 0x30), nb = rd32le(q.d, mb + 0x38);
            rd32le(q.d, mb + 0x3c);
            std::string m = "{\"n\": ";
            S(0, m);
            m += ", \"s\": ";
            S(8, m);
            m += ", \"h\": ";
            S(0x18, m);
            m += ", \"f\": [";
            const ORef fr = bg::res(q, mp, o_ + 0x10);
            for (uint32_t k = 0; k < (fr ? nf : 0); k++) {
                const Ref s = bg::need(q, fr->first, fr->second + 8 * k);
                if (k) m += ", ";
                jstr(m, q.pstr(s.first, s.second));
            }
            m += "], \"b\": [";
            const ORef br = bg::res(q, mp, o_ + 0x28);
            for (uint32_t k = 0; k < (br ? nb : 0); k++) {
                const uint64_t bo = br->second + 0x28 * k;
                const Ref a = bg::need(q, br->first, bo), c = bg::need(q, br->first, bo + 8);
                if (k) m += ", ";
                m += "[";
                jstr(m, q.pstr(a.first, a.second));
                m += ", ";
                jstr(m, q.pstr(c.first, c.second));
                m += "]";
            }
            m += "], \"fl\": " + std::to_string(flags) + "}";
            if (!firstm) o += ", ";
            o += m;
            firstm = false;
        }
    }
    return o + "]}";
}

}  // namespace

void make_res(const fs::path &out, const fs::path &pak_dir) {
    std::vector<std::string> names;
    for (const auto &e : fs::directory_iterator(pak_dir)) {
        const std::string fn = utf8(e.path().filename());
        if (ends_with(fn, ".pak") && !ends_with(fn, "-ingame12.pak")) names.push_back(fn);
    }
    std::sort(names.begin(), names.end());
    std::string mats, tex;
    for (const std::string &fn : names) {
        try {
            const Pak q(pak_dir / upath(fn));
            mats += survey_line(q, fn) + "\n";
            for (const PakTexture &x : q.textures())
                tex += fn + "\t" + bg::utf8_of(x.path) + "\t" + std::to_string(x.width) + "\t" + std::to_string(x.height) + "\t" + std::to_string(x.format) + "\t" +
                       std::to_string(x.mips) + "\t" + std::to_string(x.vramOffset) + "\t" + std::to_string(x.vramSize) + "\t0\t0\t0\r\n";
        } catch (const std::exception &ex) {
            std::string l = "{\"pak\": ";
            jstr(l, fn);
            l += ", \"err\": ";
            jstr(l, ex.what());
            mats += l + "}\n";
        }
    }
    fs::create_directories(out);
    const std::string gz = gzip_stored(mats);
    write_file_replace(out / "ps4mats.jsonl.gz", Bytes(gz.begin(), gz.end()));
    write_file_replace(out / "ps4tex.tsv", Bytes(tex.begin(), tex.end()));
}

// ------------------------------------------------------------------------------------------------ the survey, loaded
namespace {

std::string latin1_of(const std::string &u) {   // the UTF-8 text of the JSON survey back to the paks' latin-1 bytes
    std::string o;
    for (size_t i = 0; i < u.size(); i++) {
        const unsigned char c = (unsigned char)u[i];
        if (c >= 0xc2 && c <= 0xc3 && i + 1 < u.size()) {
            o += char(((c & 3) << 6) | ((unsigned char)u[i + 1] & 0x3f));
            i++;
        } else
            o += char(c);
    }
    return o;
}

struct SMat {
    std::string n, s, h;
    std::vector<std::string> f;
    std::vector<std::pair<std::string, std::string>> b;
    // derived (the fallback scoring)
    bool prime = false, water = false;
    unsigned r4 = 0, c4 = 0;   // role / class bits
    bool tmus1a = false, wind = false, leaf = false;
    size_t fsid = 0;   // feature set id
};
struct SEnt {
    std::string pak;
    uint64_t size;
    const SMat *m;
};
struct SKey {
    std::string k, kdir;
    std::vector<std::string> kc;
    bool shadowblocker;
    std::vector<SEnt> ents;
};
// a feature set's facts for the v2 / v2.3 penalties
struct FeatSet {
    std::vector<std::string> feats;   // unique
    std::vector<int> bad;             // first BAD_FEATS weight per feature (0 = none)
    std::vector<bool> dist, incand, addenv;
    unsigned layers = 0;
    bool blend12 = false, normalmap = false, anydist = false, anyinc = false, blendfactor = false, uv2 = false, uv34 = false, vcol = false;
    bool border = false;
};
struct Survey {
    std::vector<SKey> keys;
    std::map<std::string, size_t> key_at;
    std::vector<std::unique_ptr<SMat>> mats;
    std::map<std::string, uint64_t> paksize;
    std::vector<FeatSet> fsets;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> tex;   // short -> [(pak, full)]
};

const unsigned R_NORMAL = 1, R_DETAIL = 2, R_SPEC = 4, R_MASK = 8, R_BLEND = 16, R_TRANS = 32, R_ENV = 64, R_COLOR = 128;
unsigned role_bit(const std::string &r) {
    static const std::map<std::string, unsigned> m = {{"normal", R_NORMAL}, {"detail", R_DETAIL}, {"spec", R_SPEC}, {"mask", R_MASK},
                                                      {"blend", R_BLEND},   {"trans", R_TRANS},   {"env", R_ENV},   {"color", R_COLOR}};
    auto it = m.find(r);
    return it == m.end() ? 0 : it->second;
}
const unsigned C_CUT = 1, C_BLEND = 2, C_LEAF = 4;
int popc(unsigned v) { return __builtin_popcount(v); }

bool layer_of(const std::string &x, int &d) {   // re.match(r'COLOR_\d_', x) -> int(x.split('_')[1])
    if (x.size() >= 8 && starts_with(x, "COLOR_") && x[6] >= '0' && x[6] <= '9' && x[7] == '_') {
        d = x[6] - '0';
        return true;
    }
    return false;
}

FeatSet feat_set(const std::vector<std::string> &uniq) {
    // BAD_FEATS: (alternatives, weight); '_BLACK$' is the only anchored one
    static const std::vector<std::pair<std::vector<const char *>, int>> BAD = {
        {{"\x01_BLACK"}, 25}, {{"MASK_COLOR_MAP"}, 20}, {{"FACING_TINT"}, 12}, {{"BORDER_TINT"}, 12}, {{"_DSR_", "DIFFUSE_SUBSURFACE"}, 10},
        {{"DISTORTION"}, 15}, {{"CAUSTICS"}, 15}, {{"VERTEX_OVERLAY", "VERTEX_MASK", "SLOPE"}, 12}, {{"INSTANCE_TINT", "INSTANCE_OFFSET"}, 12},
        {{"TINT_WITH_MASK"}, 12}, {{"APPLY_OVERLAY_TEXTURE", "OVERLAY_CONTROL", "OVERLAY_MASK"}, 12},
        {{"TIME_SCROLL", "TILT_SCROLL", "UV_FLOW"}, 15}, {{"OFFSET_MAPPING"}, 6}, {{"PICKUP_SHIMMER"}, 30}, {{"SHADOW_PROXY"}, 30},
        {{"CLOTH", "SUBSURF"}, 10}, {{"RIPPLES"}, 15}, {{"PARTIALLY_CONVERTED"}, 3}, {{"GLASS_FRESNEL"}, 10}, {{"TRANSLUCEN"}, 6},
        {{"EARTH_CURVATURE", "SPHERIZE"}, 20}};
    FeatSet fs;
    fs.feats = uniq;
    for (const std::string &x : uniq) {
        int w = 0;
        for (const auto &[alts, wt] : BAD) {
            bool hit = false;
            for (const char *a : alts) hit = hit || (a[0] == '\x01' ? ends_with(x, a + 1) : has(x, a));
            if (hit) {
                w = wt;
                break;
            }
        }
        fs.bad.push_back(w);
        fs.dist.push_back(has(x, "DISTORTION"));
        fs.incand.push_back(has(x, "INCANDESCENCE"));
        fs.addenv.push_back(has(x, "ADD_ENVIRONMENT"));
        int d;
        if (layer_of(x, d)) fs.layers |= 1u << d;
        fs.blend12 = fs.blend12 || starts_with(x, "COLOR_1_BLEND_COLOR_2");
        fs.normalmap = fs.normalmap || has(x, "NORMAL_1_USE_NORMAL_MAP") || has(x, "NORMAL_MAP_USE_SHARED");
        fs.anydist = fs.anydist || has(x, "DISTORTION");
        fs.anyinc = fs.anyinc || has(x, "INCANDESCENCE");
        fs.blendfactor = fs.blendfactor || starts_with(x, "BLEND_FACTOR");
        fs.uv2 = fs.uv2 || has(x, "USE_UV2") || has(x, "USE_SET_2") || has(x, "OFFSET_MAPPING_USE_UV2");
        fs.uv34 = fs.uv34 || has(x, "USE_UV3") || has(x, "USE_UV4");
        fs.vcol = fs.vcol || has(x, "VERTEX_COLOR") || has(x, "VERTEX_AMBIENT") || has(x, "VERTEX_OVERLAY") || has(x, "VERTEX_MASK") || has(x, "_VERTEX_");
        fs.border = fs.border || has(x, "BORDER_TINT");
    }
    return fs;
}

std::mutex g_survey_lock;
std::map<std::string, std::shared_ptr<Survey>> g_surveys;

std::shared_ptr<Survey> load_survey(const fs::path &res) {
    std::lock_guard<std::mutex> lock(g_survey_lock);
    const std::string key = utf8(res);
    if (auto it = g_surveys.find(key); it != g_surveys.end()) return it->second;
    auto S = std::make_shared<Survey>();
    const Bytes gz = read_file(res / "ps4mats.jsonl.gz");
    const std::string text = inflate::gunzip(std::string(gz.begin(), gz.end()));
    std::map<std::vector<std::string>, size_t> fset_at;
    size_t a = 0;
    while (a < text.size()) {
        size_t e = text.find('\n', a);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(a, e - a);
        a = e + 1;
        if (line.empty()) continue;
        json::Value d;
        if (!json::parse(line, d)) throw Stop("ps4mats.jsonl.gz: a line that is not JSON");
        const json::Value *mats = d.get("mats");
        if (!mats) continue;
        const std::string pak = latin1_of(d.get("pak")->s);
        const json::Value *sz = d.get("size");
        const uint64_t size = sz ? uint64_t(sz->i) : 0;
        for (const json::Value &m : mats->items) {
            auto sm = std::make_unique<SMat>();
            auto str = [&](const char *k) { const json::Value *v = m.get(k); return v && v->kind == json::Value::String ? latin1_of(v->s) : std::string(); };
            sm->n = str("n");
            sm->s = str("s");
            sm->h = str("h");
            if (const json::Value *f = m.get("f"))
                for (const json::Value &x : f->items) sm->f.push_back(latin1_of(x.s));
            if (const json::Value *b = m.get("b"))
                for (const json::Value &x : b->items) sm->b.push_back({latin1_of(x.items.at(0).s), latin1_of(x.items.at(1).s)});
            sm->prime = sm->s == "prime-shader.fx";
            sm->water = sm->s == "water.fx";
            for (const auto &bd : sm->b)
                if (auto r = param_role(bd.first); r && *r != "lightmap" && *r != "env") sm->r4 |= role_bit(*r);
            std::set<std::string> fset(sm->f.begin(), sm->f.end());
            if (fset.count("TRANSPARENCY_CUTOFF")) sm->c4 |= C_CUT;
            if (fset.count("TRANSPARENCY_ALPHA_BLEND")) sm->c4 |= C_BLEND;
            if (fset.count("GLOBAL_LEAF")) sm->c4 |= C_LEAF;
            sm->leaf = fset.count("GLOBAL_LEAF") != 0;
            sm->tmus1a = fset.count("TRANSPARENCY_MAP_USE_SHARED_TEXTURE_1_A") != 0;
            for (const std::string &x : fset)
                for (const char *w : {"GLOBAL_PIVOT_WIND", "BP_TREE_ANIM", "ND_TREE_ANIM"}) sm->wind = sm->wind || has(x, w);
            std::vector<std::string> uniq(fset.begin(), fset.end());
            auto fi = fset_at.find(uniq);
            if (fi == fset_at.end()) {
                fi = fset_at.emplace(uniq, S->fsets.size()).first;
                S->fsets.push_back(feat_set(uniq));
            }
            sm->fsid = fi->second;
            const std::string k = split(sm->n, ',')[0];
            auto ki = S->key_at.find(k);
            if (ki == S->key_at.end()) {
                ki = S->key_at.emplace(k, S->keys.size()).first;
                SKey sk;
                sk.k = k;
                sk.kdir = split(k, ':')[0];
                sk.kc = split(sk.kdir, '/');
                sk.shadowblocker = has(k, "shadowblocker");
                S->keys.push_back(std::move(sk));
            }
            S->keys[ki->second].ents.push_back({pak, size, sm.get()});
            S->paksize[pak] = size;
            S->mats.push_back(std::move(sm));
        }
    }
    const Bytes tsv = read_file(res / "ps4tex.tsv");
    const std::string t(tsv.begin(), tsv.end());
    a = 0;
    while (a < t.size()) {
        size_t e = t.find('\n', a);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(a, e - a);
        a = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> f = split(line, '\t');
        if (f.size() < 11) continue;
        const std::string full = latin1_of(f[1]);
        S->tex[short_of(full)].push_back({latin1_of(f[0]), full});
    }
    g_surveys[key] = S;
    return S;
}

// ------------------------------------------------------------------------------------------------ PS3 side of the plan
std::set<uint32_t> ps3_alpha_textures(const Pak &p) {   // PS3 DXT1 textures with > 5 % punch-through blocks in the top mip
    std::set<uint32_t> out;
    for (const PakTexture &t : p.textures()) {
        const auto f = tc::ps3_format(t);
        if (!f || !f->compressed) continue;
        const uint64_t bw = (t.width + 3) / 4, bh = (t.height + 3) / 4;
        const Bytes d = p.vram_bytes(t.vramOffset, bw * bh * f->bpb);
        if (f->kind != "DXT1") continue;
        if (d.size() % 8) throw Stop("ValueError: cannot reshape array");
        const size_t n = d.size() / 8;
        size_t hit = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t *b = &d[i * 8];
            const uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
            const uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
            bool has3 = false;
            for (int k = 0; k < 16; k++) has3 = has3 || ((idx >> (2 * k)) & 3) == 3;
            if (c0 <= c1 && has3) hit++;
        }
        if (n && double(hit) / double(n) > 0.05) out.insert(t.hash);
    }
    return out;
}

std::map<std::string, std::set<int>> ps3_material_attrs(const Pak &p) {
    std::map<std::string, std::set<int>> out;
    std::vector<bg::Proto> C;
    try {
        C = bg::collection(p);
    } catch (const std::exception &) {
        return out;
    }
    for (const bg::Proto &c : C)
        for (size_t k = 0; k < c.blocks.size(); k++) {
            const std::string m = c.mats[k] ? *c.mats[k] : std::string("\x01None");
            auto &s = out[m];
            try {
                const Ref f = bg::need(p, c.blocks[k].first, c.blocks[k].second + 0x28);
                for (const bg::Attr &a : bg::read_format(p, f.first, f.second)) s.insert(a.id);
            } catch (const std::exception &) {
            }
        }
    return out;
}

bool colp(const std::string &k, int &d) {   // re.match(r'g_color(Tint|Brightness)(\d)$', k)
    for (const char *pre : {"g_colorTint", "g_colorBrightness"}) {
        const size_t n = std::strlen(pre);
        if (k.size() == n + 1 && starts_with(k, pre) && k[n] >= '0' && k[n] <= '9') {
            d = k[n] - '0';
            return true;
        }
    }
    return false;
}
bool uvp(const std::string &k, int &d) {   // re.match(r'g_uvScale(\d)$', k)
    if (k.size() == 10 && starts_with(k, "g_uvScale") && k[9] >= '0' && k[9] <= '9') {
        d = k[9] - '0';
        return true;
    }
    return false;
}

// the per-material facts of the fallback scoring
struct M3 {
    std::string n, low, fdir, top;
    std::vector<std::string> ncomp;
    unsigned r3 = 0, c3 = 0;
    std::set<int> attrs3;
    bool inc3 = false, dis3 = false, emissive = false, two3 = false, nrm3 = false;
    std::vector<std::string> scr3;
    std::vector<std::string> strong;   // ps3_strong, the keys
    size_t ns3 = 0;                    // PS3 constants minus the generic ones
    std::unordered_map<size_t, int> cache;   // feature set id -> v2 + v2.3 score
};

int v2_v23(const M3 &m, const FeatSet &f) {
    int pen = 0;
    const bool blend3 = (m.r3 & R_BLEND) || m.two3;
    if (m.two3 && !f.blend12) pen -= 45;
    if (m.nrm3 && !f.normalmap) pen -= 15;
    for (const std::string &need : m.scr3) {
        bool any = false;
        for (const std::string &x : f.feats) any = any || starts_with(x, need);
        if (!any) pen -= 15;
    }
    if (m.dis3 && !f.anydist) pen -= 15;
    if (m.inc3 && !f.anyinc) pen -= 15;
    const unsigned allowed = blend3 ? 6u : 2u;   // {1, 2} / {1}
    pen -= 10 * popc(f.layers & ~allowed);
    if (blend3 && !(f.layers & 4)) pen -= 10;
    if (!blend3 && f.blendfactor) pen -= 10;
    for (size_t i = 0; i < f.feats.size(); i++) {
        if (f.bad[i]) {
            bool skip = m.dis3 && f.dist[i];
            for (const std::string &need : m.scr3) skip = skip || starts_with(f.feats[i], need);
            if (!skip) pen -= f.bad[i];
        }
        if (f.incand[i] && !m.emissive) pen -= 15;
        if (f.addenv[i] && !(m.r3 & R_ENV)) pen -= 4;
    }
    if (!m.attrs3.empty()) {
        if (f.uv2 && !m.attrs3.count(0x12)) pen -= 40;
        if (f.uv34) pen -= 40;
        if (f.vcol && !m.attrs3.count(0x15)) pen -= 40;
    }
    // v2.3: strong PS3 constants the template cannot carry
    if (!m.strong.empty()) {
        int sc = 0;
        unsigned proven = 0;
        bool border_st = false;
        for (const std::string &k : m.strong) {
            int d;
            std::string need;
            if (colp(k, d)) {
                need = "COLOR_" + std::to_string(d) + "_APPLY_COLOR_TINT";
                proven |= 1u << d;
            } else if (uvp(k, d))
                need = "UV_" + std::to_string(d) + "_APPLY_SCALE";
            else
                need = "BORDER_TINT";
            if (k == "g_diffuseColorBorderTint") border_st = true;
            bool carried = false;
            for (const std::string &x : f.feats) carried = carried || has(x, need.c_str());
            if (!carried) sc -= k == "g_diffuseColorBorderTint" ? 6 : 12;
        }
        sc += 10 * popc(f.layers & ~6u & proven);
        if (border_st && f.border) sc += 12;
        pen += sc;
    }
    return pen;
}

// waterscore.py: a retail water.fx template against a PS3 water material: constant names (packed names split),
// texture roles (diffuse colour, diffuse mask, foam, env cube, shadow map), a water program
std::set<std::string> split_names(const std::vector<std::string> &names) {
    static const std::regex PART("^[a-z][A-Z0-9]");
    std::set<std::string> out;
    for (const std::string &k : names) {
        std::vector<std::string> parts;
        if (starts_with(k, "g_")) parts = split(k.substr(2), '_');
        bool all = parts.size() > 1;
        for (const std::string &s : parts) all = all && std::regex_search(s, PART);
        if (all)
            for (const std::string &s : parts) out.insert("g_" + s);
        else
            out.insert(k);
    }
    return out;
}

int water_score(const std::vector<std::string> &p3names, const std::vector<std::string> &texnames, const std::vector<std::string> &tparams,
                const std::set<std::string> &tbinds, const std::vector<std::string> &tfeats) {
    const std::set<std::string> P = split_names(p3names), T = split_names(tparams);
    int both = 0;
    for (const std::string &x : P) both += int(T.count(x));
    int s = 2 * both - int(T.size() - both) - int(P.size() - both);
    static const std::regex DIFF(R"(-(color|diffuse|col)\.)"), DMASK(R"(-(mask|trans\w*|alpha)\.)");
    bool ti[5] = {false, false, false, false, false};   // diff, dmask, foam, env, shadow
    for (const std::string &n0 : texnames) {
        if (has(n0, "missingtexture")) continue;
        const std::string n = lower(n0);
        ti[2] = ti[2] || has(n, "foam");
        ti[0] = ti[0] || (std::regex_search(n, DIFF) && !has(n, "foam"));
        ti[1] = ti[1] || (std::regex_search(n, DMASK) && !has(n, "foam") && !has(n, "default-mask") && !has(n, "shadow"));
        ti[3] = ti[3] || ends_with(n, ".dds");
        ti[4] = ti[4] || has(n, "shadow");
    }
    const std::set<std::string> f(tfeats.begin(), tfeats.end());
    const bool tt[5] = {tbinds.count("g_tDiffuseColorMap") != 0, tbinds.count("g_tNdFetchDiffuseMaskMap") != 0, f.count("ND_FOAM") != 0,
                        f.count("ND_SPECULAR_ENVMAP") != 0, f.count("ND_WATER_TEXTURE_SHADOWS") != 0};
    const int wt[5] = {8, 5, 4, 3, 2};
    for (int k = 0; k < 5; k++) s += ti[k] == tt[k] ? wt[k] : -wt[k];
    if (!f.count("ND_WATER_REFRACTIONS")) s -= 100;   // not a water surface program
    return s;
}
}  // namespace

const PlanMat *Plan::get(const std::string &n) const {
    for (const auto &e : mats)
        if (e.first == n) return &e.second;
    return nullptr;
}

Plan make_plan(const fs::path &ps3path, const fs::path &res, const fs::path &water_templates) {
    std::optional<json::Value> wtpl;
    static const std::regex FOLIAGE("plant|leaf|leaves|vine|card(?!board)|grass|moss|brom|bush|fern|ivy|palm-frond|frond|shrub|weed|reed|"
                                    "flower|hedge|branch|twig|canopy|willow|banyan");
    static const std::regex GROUNDISH("floor|tile|ground|terrain|road|path|stone|rock|brick|wall|concrete|dirt|sand|mud|plaster|"
                                      "pavement|cobble|street|roof|step|stair|trim|wood-plank|planks|metal|marble|tarmac|gravel");
    static const std::regex BLENDNAME(R"(glass|window|-trans\b)");
    const Pak p(ps3path);
    Plan plan;
    std::string stage = utf8(ps3path.filename());
    stage = stage.substr(0, stage.size() - 4);
    plan.stage = stage;
    const std::set<std::string> exclude = {stage + ".pak"};
    const std::shared_ptr<Survey> S = load_survey(res);
    const std::vector<bg::Proto> C = bg::collection(p);
    std::vector<std::string> names;
    auto add_name = [&](const std::string &n) {
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    };
    for (const bg::Proto &c : C)
        for (const bg::Group &g : c.groups) add_name(g.mat);
    const std::vector<std::string> wm = bg::water_materials(p);
    const std::set<std::string> water(wm.begin(), wm.end());
    for (const std::string &n : wm) add_name(n);
    std::vector<bg::Material3> mats3;
    std::vector<bg::Texture3> texs3;
    bg::read_materials(p, mats3, texs3);
    std::map<std::string, const bg::Material3 *> m3;
    for (const bg::Material3 &m : mats3) m3[m.name] = &m;
    const std::map<std::string, bg::Params> P3 = bg::all_material_params(p);
    const std::map<std::string, std::set<int>> A3 = ps3_material_attrs(p);
    auto p3_of = [&](const std::string &n) {
        auto it = P3.find(n);
        return it == P3.end() ? bg::Params() : it->second;
    };
    auto roles3 = [&](const std::string &n) {
        unsigned r = 0;
        auto it = m3.find(n);
        if (it == m3.end()) return r;
        for (uint32_t i : it->second->textures)
            if (auto x = tex_role(texs3.at(i).name)) r |= role_bit(*x);
        return r;
    };
    std::map<std::string, int> used;
    std::map<std::string, PlanMat> out;
    std::vector<std::string> order;
    auto put = [&](const std::string &n, PlanMat e) {
        order.push_back(n);
        out[n] = std::move(e);
    };
    auto plan_mat = [&](const std::string &n, const SEnt &c, const std::string &key, bool exact) {
        PlanMat e;
        e.key = key;
        e.pak = c.pak;
        e.exact = exact;
        e.name = c.m->n;
        e.binds = c.m->b;
        e.ps3params = p3_of(n);
        return e;
    };
    // exact first (greedy: prefer paks that cover many materials)
    std::map<std::string, std::vector<SEnt>> cand;
    std::map<std::string, int> cover;
    for (const std::string &n : names) {
        std::vector<SEnt> cs, all;
        if (auto ki = S->key_at.find(ps4key(n)); ki != S->key_at.end())
            for (const SEnt &c : S->keys[ki->second].ents)
                if (!exclude.count(c.pak)) {
                    all.push_back(c);
                    if (c.m->prime) cs.push_back(c);
                }
        cand[n] = cs.empty() ? all : cs;
        std::set<std::string> pks;
        for (const SEnt &c : cand[n]) pks.insert(c.pak);
        for (const std::string &pk : pks) cover[pk]++;
    }
    for (const std::string &n : names) {
        const auto &cs = cand[n];
        if (cs.empty()) continue;
        const SEnt *best = nullptr;
        for (const SEnt &c : cs)
            if (!best || std::make_pair(cover[c.pak], -int64_t(c.size)) > std::make_pair(cover[best->pak], -int64_t(best->size))) best = &c;
        put(n, plan_mat(n, *best, ps4key(n), true));
        used[best->pak]++;
    }
    // fallbacks
    const std::set<uint32_t> alpha_tex = ps3_alpha_textures(p);
    for (const std::string &n : names) {
        if (out.count(n)) continue;
        if (water.count(n)) {
            // the closest water.fx material: common directory, same variant, smaller pak
            struct WB {
                int common;
                bool same;
                int64_t negsz;
                const SKey *k;
                const SEnt *c;
            };
            std::optional<WB> wbest;
            const std::vector<std::string> nc = [&] { auto v = split(n, '/'); v.pop_back(); return v; }();
            const std::string nv = n.substr(n.rfind('/') + 1);
            if (!water_templates.empty() && fs::exists(water_templates)) {
                // the sky converter: score every retail water.fx template against the PS3 material (waterscore.py)
                if (!wtpl) wtpl = read_json(water_templates);
                std::vector<std::string> tx3;
                if (auto mi3 = m3.find(n); mi3 != m3.end())
                    for (uint32_t i : mi3->second->textures) {
                        if (i >= texs3.size()) continue;
                        static const std::regex HT(R"(/[0-9A-F]{16}\.ndb.*$)");
                        const std::string s = std::regex_replace(texs3[i].name, HT, "");
                        tx3.push_back(s.substr(s.rfind('/') == std::string::npos ? 0 : s.rfind('/') + 1));
                    }
                std::vector<std::pair<std::string, const SEnt *>> cands;   // template key -> its smallest retail pak
                for (const SKey &k : S->keys)
                    for (const SEnt &c : k.ents) {
                        if (exclude.count(c.pak) || !c.m->water || !wtpl->get(bg::utf8_of(k.k))) continue;
                        auto ci = std::find_if(cands.begin(), cands.end(), [&](const auto &x) { return x.first == k.k; });
                        if (ci == cands.end()) cands.push_back({k.k, &c});
                        else if (c.size < ci->second->size) ci->second = &c;
                    }
                std::map<std::string, uint64_t> psz;
                for (const auto &[k, c] : cands) psz[c->pak] = c->size;
                if (!cands.empty()) {
                    std::vector<std::string> p3names;
                    for (const auto &[k, v] : p3_of(n)) p3names.push_back(k);
                    const std::string *bestk = nullptr;
                    std::tuple<int, int, int, int64_t, std::string> bestkey;
                    std::set<std::string> tb(tx3.begin(), tx3.end());
                    for (const auto &[k, c] : cands) {
                        const json::Value &t = *wtpl->get(bg::utf8_of(k));
                        std::vector<std::string> tparams, tfeats;
                        std::set<std::string> tbinds, tbn;
                        if (const json::Value *v = t.get("params"))
                            for (const json::Value &x : v->items) tparams.push_back(latin1_of(x.s));
                        if (const json::Value *v = t.get("feats"))
                            for (const json::Value &x : v->items) tfeats.push_back(latin1_of(x.s));
                        if (const json::Value *v = t.get("binds"))
                            for (const json::Value &x : v->items) {
                                tbinds.insert(latin1_of(x.items.at(0).s));
                                const std::string s1 = latin1_of(x.items.at(1).s);
                                tbn.insert(s1.substr(s1.rfind('/') == std::string::npos ? 0 : s1.rfind('/') + 1));
                            }
                        const int s = water_score(p3names, tx3, tparams, tbinds, tfeats);
                        int tov = 0;
                        for (const std::string &x : tb) tov += int(tbn.count(x));
                        const std::vector<std::string> kc = split(split(k, ':')[0], '/');
                        int common = 0;
                        for (size_t i = 0; i < std::min(nc.size(), kc.size()) && nc[i] == kc[i]; i++) common++;
                        auto pi = psz.find(c->pak);
                        const auto key = std::make_tuple(s, tov, common, -int64_t(pi == psz.end() ? 0 : pi->second), k);
                        if (!bestk || key > bestkey) {
                            bestk = &k;
                            bestkey = key;
                        }
                    }
                    const SEnt *c = std::find_if(cands.begin(), cands.end(), [&](const auto &x) { return x.first == *bestk; })->second;
                    const SKey *sk = &S->keys[S->key_at.at(*bestk)];
                    wbest = WB{0, false, 0, sk, c};
                }
            }
            if (!wbest)
                for (const SKey &k : S->keys)
                for (const SEnt &c : k.ents) {
                    if (exclude.count(c.pak) || !c.m->water) continue;
                    int common = 0;
                    for (size_t i = 0; i < std::min(nc.size(), k.kc.size()) && nc[i] == k.kc[i]; i++) common++;
                    const size_t kp = k.k.rfind(':');
                    const bool same = nv == (kp == std::string::npos ? k.k : k.k.substr(kp + 1));
                    const WB w{common, same, -int64_t(c.size), &k, &c};
                    if (!wbest || std::make_tuple(w.common, w.same, w.negsz) > std::make_tuple(wbest->common, wbest->same, wbest->negsz)) wbest = w;
                }
            if (wbest) {
                PlanMat e = plan_mat(n, *wbest->c, wbest->k->k, false);
                e.has_roles = true;
                e.cls = {"water"};
                put(n, e);
                used[wbest->c->pak]++;
                continue;
            }
        }
        M3 m;
        m.n = n;
        m.low = lower(n);
        m.r3 = roles3(n);
        const auto mi = m3.find(n);
        bool col_alpha = false;
        if (mi != m3.end())
            for (uint32_t i : mi->second->textures) {
                const bg::Texture3 &t = texs3.at(i);
                const auto r = tex_role(short_of(t.name));
                if (r == "trans" || (r == "color" && alpha_tex.count(t.hash))) col_alpha = true;
            }
        const size_t sl = m.low.rfind('/');
        if (rx(m.low, FOLIAGE) && !rx(sl == std::string::npos ? m.low : m.low.substr(sl + 1), GROUNDISH)) m.c3 |= C_LEAF;
        if (rx(m.low, BLENDNAME)) m.c3 |= C_BLEND;
        else if (col_alpha) m.c3 |= C_CUT;
        m.fdir = n.substr(0, n.rfind('/'));
        {
            const auto v = split(n, '/');
            m.top = v[0] + (v.size() > 1 ? "/" + v[1] : "");
            m.ncomp.assign(v.begin(), v.end() - 1);
        }
        if (auto a = A3.find(n); a != A3.end()) m.attrs3 = a->second;
        const bg::Params p3 = p3_of(n);
        {   // ps3_needs, ps3_two_layers, ps3_strong
            static const std::regex SCROLL(R"(^g_timescroll[uv](\d)$)"), FLOW("flowtime|flowuv|uvflow");
            std::string ks;
            for (const auto &[k, v] : p3) {
                const std::string kl = lower(k);
                ks += (ks.empty() ? "" : " ") + kl;
                std::smatch sm;
                std::string need;
                if (std::regex_search(kl, sm, SCROLL)) need = "UV_" + sm[1].str() + "_APPLY_TIME_SCROLL";
                else if (rx(kl, FLOW)) need = "UV_FLOW";
                if (!need.empty() && std::find(m.scr3.begin(), m.scr3.end(), need) == m.scr3.end()) m.scr3.push_back(need);
                if (k == "g_colorTint2" || k == "g_colorBrightness2" || k == "g_blendSoftness_blendOffset_linearBlendOffset") m.two3 = true;
                int d;
                if (colp(k, d)) {
                    bool strong = false;
                    if (starts_with(k, "g_colorTint"))
                        for (size_t j = 0; j < std::min<size_t>(3, v.size()); j++) strong = strong || std::fabs(v[j] - 1) > 0.02;
                    else
                        strong = std::fabs(v.at(0) - 1) > 0.02;
                    if (strong) m.strong.push_back(k);
                } else if (uvp(k, d) && std::fabs(v.at(0) - 1) > 0.02)
                    m.strong.push_back(k);
                else if (k == "g_diffuseColorBorderTint")
                    m.strong.push_back(k);
                if (k != "g_specularBrightness" && k != "g_specularExponentMax") m.ns3++;
            }
            m.inc3 = has(ks, "incand");
            m.dis3 = has(ks, "distort");
            static const std::regex EMISSIVE("lamp|light|fire|glow|neon|screen|incand|emissive|sky");
            m.emissive = m.inc3 || rx(m.low, EMISSIVE);
            m.nrm3 = (m.r3 & R_NORMAL) != 0;
        }
        struct Best {
            int64_t score2, negsz;
            const SKey *k;
            const SEnt *c;
        };
        std::optional<Best> best;
        for (const SKey &k : S->keys) {
            if (k.shadowblocker && !has(n, "shadowblocker")) continue;
            int common = 0;
            for (size_t i = 0; i < std::min(m.ncomp.size(), k.kc.size()) && m.ncomp[i] == k.kc[i]; i++) common++;
            int dirsc = 0;
            if (k.kdir == m.fdir) dirsc = 8;
            else if (starts_with(k.k, m.top)) dirsc = 1 + std::max(0, common - 2);
            for (const SEnt &c : k.ents) {
                if (exclude.count(c.pak) || !c.m->prime) continue;
                const SMat &sm = *c.m;
                int score = popc(m.r3 & sm.r4) * 2 - popc(sm.r4 & ~m.r3) - popc(m.r3 & ~sm.r4);
                score += ((m.c3 & C_CUT) != 0) == ((sm.c4 & C_CUT) != 0) ? 12 : -12;
                score += ((m.c3 & C_BLEND) != 0) == ((sm.c4 & C_BLEND) != 0) ? 8 : -8;
                score += ((m.c3 & C_LEAF) != 0) == ((sm.c4 & C_LEAF) != 0) ? 4 : -2;
                if ((m.c3 & C_CUT) && !(m.r3 & R_TRANS) && sm.tmus1a) score += 6;
                score += dirsc;
                if (((m.r3 & R_TRANS) != 0) != ((sm.r4 & R_TRANS) != 0)) score -= 5;
                score -= 3 * int(m.ns3);   // no feature rule file: the template's predicted params are empty
                if (sm.wind) score -= 40;
                if (sm.leaf && !(m.c3 & C_LEAF)) score -= 30;
                auto ci = m.cache.find(sm.fsid);
                if (ci == m.cache.end()) ci = m.cache.emplace(sm.fsid, v2_v23(m, S->fsets[sm.fsid])).first;
                score += ci->second;
                score -= 2 * popc(sm.r4 & ~m.r3);
                const int64_t score2 = int64_t(score) * 2 + (used[c.pak] > 0 ? 1 : 0);
                if (!best || std::make_pair(score2, -int64_t(c.size)) > std::make_pair(best->score2, best->negsz)) best = Best{score2, -int64_t(c.size), &k, &c};
            }
        }
        if (!best) throw Stop("no fallback material for " + n);
        PlanMat e = plan_mat(n, *best->c, best->k->k, false);
        e.has_roles = true;
        for (const char *r : {"blend", "color", "detail", "env", "mask", "normal", "spec", "trans"})
            if (m.r3 & role_bit(r)) e.roles.push_back(r);
        for (const char *c : {"blend", "cut", "leaf"})
            if (m.c3 & (std::string(c) == "blend" ? C_BLEND : std::string(c) == "cut" ? C_CUT : C_LEAF)) e.cls.push_back(c);
        put(n, e);
        used[best->c->pak]++;
    }
    // textures: every PS3 texture and every texture bound by a chosen PS4 material
    std::vector<std::string> shorts;
    for (const PakTexture &t : p.textures()) shorts.push_back(short_of(t.path));
    for (const std::string &n : order)
        for (const auto &b : out[n].binds) shorts.push_back(b.second);
    std::set<std::string> want_done;
    std::set<std::pair<std::string, std::string>> want;
    for (const std::string &s : shorts) {
        if (want_done.count(s)) continue;
        auto ti = S->tex.find(s);
        if (ti == S->tex.end()) continue;
        const std::pair<std::string, std::string> *best = nullptr;
        std::pair<int, int64_t> bk{};
        for (const auto &x : ti->second) {
            auto ps = S->paksize.find(x.first);
            const std::pair<int, int64_t> k{used[x.first] > 0 ? 1 : 0, -int64_t(ps == S->paksize.end() ? (1ull << 40) : ps->second)};
            if (!best || k > bk) {
                best = &x;
                bk = k;
            }
        }
        want_done.insert(s);
        want.insert(*best);
    }
    std::set<std::pair<std::string, std::string>> rm;
    for (const std::string &n : order) rm.insert({out[n].pak, out[n].key});
    plan.req_mats.assign(rm.begin(), rm.end());
    plan.req_tex.assign(want.begin(), want.end());
    for (const std::string &n : order) plan.mats.push_back({n, out[n]});
    return plan;
}

std::string plan_json(const Plan &pl) {
    using json::Value;
    auto text = [](const std::string &s) { return Value::text(bg::utf8_of(s)); };
    auto arr = []() { Value v; v.kind = Value::Array; return v; };
    Value root = Value::object();
    root.set("stage", text(pl.stage));
    Value mats = Value::object();
    for (const auto &[n, e] : pl.mats) {
        Value m = Value::object();
        m.set("key", text(e.key));
        m.set("pak", text(e.pak));
        m.set("exact", Value::boolean(e.exact));
        if (!e.has_bindmap) {   // the sky converter's re-pointed entries drop name, binds, roles and cls
            m.set("name", text(e.name));
            Value b = arr();
            for (const auto &x : e.binds) {
                Value pr = arr();
                pr.items.push_back(text(x.first));
                pr.items.push_back(text(x.second));
                b.items.push_back(pr);
            }
            m.set("binds", b);
        }
        if (e.has_roles && !e.has_bindmap) {
            Value r = arr(), c = arr();
            for (const auto &x : e.roles) r.items.push_back(text(x));
            for (const auto &x : e.cls) c.items.push_back(text(x));
            m.set("roles", r);
            m.set("cls", c);
        }
        Value pp = Value::object();
        for (const auto &[k, v] : e.ps3params) {
            Value a = arr();
            for (double x : v) {
                Value f;
                f.kind = Value::Float;
                f.f = x;
                a.items.push_back(f);
            }
            pp.set(bg::utf8_of(k), a);
        }
        m.set("ps3params", pp);
        if (e.has_bindmap) {
            Value bm = Value::object();
            for (const auto &[k, v] : e.bindmap) bm.set(bg::utf8_of(k), text(v));
            m.set("bindmap", bm);
        }
        mats.fields.push_back({bg::utf8_of(n), m});
    }
    root.set("mats", mats);
    Value req = Value::object();
    for (const auto *lst : {&pl.req_mats, &pl.req_tex}) {
        Value a = arr();
        for (const auto &x : *lst) {
            Value pr = arr();
            pr.items.push_back(text(x.first));
            pr.items.push_back(text(x.second));
            a.items.push_back(pr);
        }
        req.set(lst == &pl.req_mats ? "mats" : "tex", a);
    }
    root.set("requests", req);
    return json_dump_indent1(root);
}

// ------------------------------------------------------------------------------------------------ fxextract
namespace {

struct XPak {
    std::unique_ptr<Pak> q;
    std::map<uint32_t, std::vector<const PakItem *>> idx;   // page -> items by offset
    bool has_tex = false;
    std::map<std::string, PakTexture> tex;
    std::map<std::string, Bytes> gtex;
    bool has_tex2 = false;
    std::map<std::string, PakTexture> tex2;
};

const PakItem *container(const XPak &x, uint32_t pg, uint64_t off) {
    auto it = x.idx.find(pg);
    if (it == x.idx.end()) return nullptr;
    const auto &L = it->second;
    auto u = std::upper_bound(L.begin(), L.end(), off, [](uint64_t o, const PakItem *i) { return o < i->off; });
    if (u == L.begin()) return nullptr;
    const PakItem *c = *(u - 1);
    return c->off <= off && off < c->data_off() + c->size ? c : nullptr;
}

std::string S_(const Pak &q, uint32_t pg, uint64_t off) {
    const ORef r = bg::res(q, pg, off);
    if (!r) throw Stop("fxextract: a null string pointer");
    return q.pstr(r->first, r->second);
}

Bytes slice(const Pak &q, uint64_t a, uint64_t n) {
    const uint64_t s = std::min<uint64_t>(a, q.d.size()), e = std::min<uint64_t>(a + n, q.d.size());
    return Bytes(q.d.begin() + s, q.d.begin() + std::max(s, e));
}

std::vector<std::string> techs(const Pak &q) {
    const auto ets = q.items_of("EFFECT_TABLE");
    if (ets.empty()) throw Stop("IndexError: no EFFECT_TABLE");
    const Ref t = bg::need(q, ets[0].page, ets[0].data_off() + 0x18);
    const uint32_t n = q.pu32(t.first, t.second);
    std::vector<std::string> out;
    for (uint32_t i = 0; i < n; i++) out.push_back(S_(q, t.first, t.second + 8 + 8 * i));
    return out;
}

std::vector<FxItem> closure(const XPak &x, const PakItem &eit) {
    const Pak &q = *x.q;
    std::vector<const PakItem *> seq = {&eit};
    std::map<std::pair<uint32_t, uint32_t>, size_t> ids = {{{eit.page, eit.off}, 0}};
    std::vector<FxItem> out;
    for (size_t k = 0; k < seq.size(); k++) {
        const PakItem &it = *seq[k];
        FxItem f;
        for (uint64_t oo = it.data_off(); oo < uint64_t(it.data_off()) + it.size; oo += 8) {
            const ORef r = bg::res(q, it.page, oo);
            if (!r) continue;
            const PakItem *c = container(x, r->first, r->second);
            if (!c) throw Stop("ValueError: loose pointer");
            auto ii = ids.find({c->page, c->off});
            if (ii == ids.end()) {
                ii = ids.emplace(std::make_pair(c->page, c->off), seq.size()).first;
                seq.push_back(c);
            }
            f.ptrs.push_back({oo - it.data_off(), ii->second, int64_t(r->second) - int64_t(c->data_off())});
        }
        f.type = it.type;
        f.name = it.name;
        f.off = it.data_off();
        f.page = it.page;
        f.data = slice(q, q.pbase(it.page) + it.data_off(), it.size);
        out.push_back(std::move(f));
    }
    return out;
}

std::optional<MatRec> material(XPak &x, const std::string &key) {
    const Pak &q = *x.q;
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) throw Stop("IndexError: no GEOMETRY_1");
    const uint32_t pg = gs[0].page;
    const uint64_t b = gs[0].data_off();
    const uint64_t n = q.u64(q.pbase(pg) + b + 0x48);
    const Ref mr = bg::need(q, pg, b + 0x50);
    for (uint64_t i = 0; i < n; i++) {
        const uint32_t mp = mr.first;
        const uint64_t o = mr.second + i * 0x48;
        const std::string name = S_(q, mp, o);
        if (split(name, ',')[0] != key) continue;
        const uint64_t mb = q.pbase(mp) + o;
        const uint32_t nf = rd32le(q.d, mb + 0x30), npar = rd32le(q.d, mb + 0x34), nb = rd32le(q.d, mb + 0x38);
        MatRec rec;
        rec.name = name;
        rec.shader = S_(q, mp, o + 8);
        rec.hash = S_(q, mp, o + 0x18);
        rec.x3 = rd32le(q.d, mb + 0x3c);
        rec.tail[0] = rd32le(q.d, mb + 0x40);
        rec.tail[1] = rd32le(q.d, mb + 0x44);
        if (const ORef fr = bg::res(q, mp, o + 0x10))
            for (uint32_t k = 0; k < nf; k++) {
                const Ref s = bg::need(q, fr->first, fr->second + 8 * k);
                rec.feats.push_back(q.pstr(s.first, s.second));
            }
        if (const ORef pr = bg::res(q, mp, o + 0x20))
            for (uint32_t k = 0; k < npar; k++) {
                const uint64_t po = pr->second + 0x18 * k;
                const Ref vr = bg::need(q, pr->first, po + 8);
                const uint32_t cnt = rd32le(q.d, q.pbase(pr->first) + po + 0x10), xx = rd32le(q.d, q.pbase(pr->first) + po + 0x14);
                rec.params.push_back({S_(q, pr->first, po), cnt, xx, slice(q, q.pbase(vr.first) + vr.second, 4ull * std::max<uint32_t>(1, cnt))});
            }
        if (const ORef br = bg::res(q, mp, o + 0x28))
            for (uint32_t k = 0; k < nb; k++) {
                const uint64_t bo = br->second + 0x28 * k;
                rec.binds.push_back({S_(q, br->first, bo), S_(q, br->first, bo + 8), S_(q, br->first, bo + 0x10)});
            }
        // the effect
        const auto ets = q.items_of("EFFECT_TABLE");
        if (ets.empty()) throw Stop("IndexError: no EFFECT_TABLE");
        const uint32_t ne = q.pu32(ets[0].page, ets[0].data_off());
        const Ref arr = bg::need(q, ets[0].page, ets[0].data_off() + 8);
        const uint64_t h = std::stoull(rec.hash, nullptr, 16);
        for (uint32_t k = 0; k < ne; k++) {
            const uint64_t eo = arr.second + 0x18ull * k;
            if (q.u64(q.pbase(arr.first) + eo + 0x10) != h) continue;
            const Ref ep = bg::need(q, arr.first, eo);
            const PakItem *eit = container(x, ep.first, ep.second);
            if (!eit) throw Stop("AttributeError: effect outside an item");
            rec.has_fx = true;
            rec.fx_idx = q.pu32(arr.first, eo + 8);
            rec.fx_x = q.pu32(arr.first, eo + 12);
            rec.fx_eoff = int64_t(ep.second) - int64_t(eit->data_off());
            rec.fx = closure(x, *eit);
            break;
        }
        // textures bound by the material: VRAM_DESC (+ resident texels) and the GEOMETRY_1 texture entry
        if (!x.has_tex) {
            x.has_tex = true;
            for (const PakTexture &t : q.textures()) x.tex[t.path] = t;
            const uint64_t nt = q.u64(q.pbase(pg) + b + 0x38);
            if (const ORef tr = bg::res(q, pg, b + 0x40))
                for (uint64_t k = 0; k < nt; k++) {
                    const uint64_t to = tr->second + 0x28 * k;
                    x.gtex[S_(q, tr->first, to)] = slice(q, q.pbase(tr->first) + to + 8, 0x20);
                }
        }
        for (const MatBind &bnd : rec.binds) {
            const std::string &full = bnd.full;
            auto ti = x.tex.find(full + ".ndb");
            if (ti == x.tex.end()) ti = x.tex.find(full);
            MatTex e;
            if (auto g = x.gtex.find(full); g != x.gtex.end()) e.gtex = g->second;
            if (ti != x.tex.end() && !has(bnd.param, "LightMap") && ti->second.vramSize <= 0x80000) {
                const PakTexture &t = ti->second;
                e.has_desc = true;
                e.path = t.path;
                e.desc = slice(q, q.pbase(t.page) + t.off + 0x20, 0x40);
                e.texels = q.vram_bytes(t.vramOffset, t.vramSize);
            }
            auto ei = std::find_if(rec.textures.begin(), rec.textures.end(), [&](const auto &p) { return p.first == full; });
            if (ei == rec.textures.end()) rec.textures.push_back({full, e});
            else ei->second = e;
        }
        return rec;
    }
    return std::nullopt;
}

}  // namespace

Bundle extract(const fs::path &D, const Plan &plan) {
    std::set<std::string> tp;
    for (const auto &[pk, key] : plan.req_mats) tp.insert(pk);
    return extract_req(D, plan.req_mats, plan.req_tex, std::vector<std::string>(tp.begin(), tp.end()));
}

std::vector<std::string> pak_techs(const fs::path &pak) { return techs(Pak(pak)); }

Bundle extract_req(const fs::path &D, const std::vector<std::pair<std::string, std::string>> &req_mats,
                   const std::vector<std::pair<std::string, std::string>> &req_tex, const std::vector<std::string> &req_techs) {
    Bundle out;
    std::map<std::string, XPak> cache;
    auto P = [&](const std::string &fn) -> XPak & {
        if (!cache.count(fn)) {
            if (cache.size() > 6) cache.clear();
            XPak x;
            x.q = std::make_unique<Pak>(D / upath(fn));
            for (const PakItem &it : x.q->items) x.idx[it.page].push_back(&it);
            for (auto &[pg, L] : x.idx) std::stable_sort(L.begin(), L.end(), [](const PakItem *a, const PakItem *b) { return a->off < b->off; });
            cache.emplace(fn, std::move(x));
        }
        return cache.at(fn);
    };
    for (const std::string &fn : req_techs) out.techs[fn] = techs(*P(fn).q);
    for (const auto &[fn, key] : req_mats) {
        XPak &x = P(fn);
        std::optional<MatRec> r = material(x, key);
        if (!r) throw Stop("bundle errors: no material " + key + " in " + fn);
        out.mats.push_back({{fn, key}, std::move(*r)});
        if (!out.techs.count(fn)) out.techs[fn] = techs(*x.q);
    }
    for (const auto &[fn, path] : req_tex) {
        try {
            XPak &x = P(fn);
            if (!x.has_tex2) {
                x.has_tex2 = true;
                for (const PakTexture &c : x.q->textures()) x.tex2[c.path] = c;   // the last of a path wins (a dict)
            }
            auto ti = x.tex2.find(path);
            if (ti == x.tex2.end()) continue;
            const PakTexture tt = ti->second;
            BundleTex b;
            b.pak = fn;
            b.path = path;
            b.desc = slice(*x.q, x.q->pbase(tt.page) + tt.off + 0x20, 0x40);
            b.texels = x.q->vram_bytes(tt.vramOffset, tt.vramSize);
            out.tex.push_back(std::move(b));
        } catch (const std::exception &) {
        }
    }
    return out;
}

std::vector<BundleTex> default_lightmaps(const fs::path &pak_dir) {
    static const char *PATHS[] = {
        "d:/dev/CP9/Data/U3/CookData/ND/build/texture2/art/general/textures/default/default-lightmap0.tga/121EDD38E3116400.ndb",
        "d:/dev/CP9/Data/U3/CookData/ND/build/texture2/art/general/textures/default/default-lightmap1.tga/B1C390E18A2F0DF2.ndb"};
    const Pak q(pak_dir / "grave-02-hollowhulk-front-low.pak");
    std::vector<BundleTex> out;
    const std::vector<PakTexture> ts = q.textures();
    for (const char *path : PATHS) {
        const PakTexture *t = nullptr;
        for (const PakTexture &c : ts)
            if (c.path == path) t = &c;
        if (!t) throw Stop(std::string("default lightmaps not found in grave-02-hollowhulk-front-low.pak: ") + path);
        BundleTex b;
        b.pak = "fort-multi-stage.pak";
        b.path = path;
        b.desc = slice(q, q.pbase(t->page) + t->off + 0x20, 0x40);   // the converter keeps desc[:0x38] and rewrites +0..+0x14
        b.texels = q.vram_bytes(t->vramOffset, t->vramSize);
        out.push_back(std::move(b));
    }
    return out;
}

}  // namespace u3::stage
