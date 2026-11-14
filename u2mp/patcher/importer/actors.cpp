// importer/actors.cpp: the actor27 rows of the Uncharted 2 import.
//
// Kinds: 'skin' (a skinned mesh on the MP hero skeleton), 'prop' (own skeleton, animations, collision, sound banks),
// 'anim' (animations only), 'havok' (a loose ragdoll: COLLISION_HAVOK_JOINTED only); anything else is unsupported.
// Skins named mp-hero* are built from their PS4 campaign twin when it has one (twin.cpp); the others are converted on
// the retail PS4 mp-hero pak of the same skeleton that shares most material names (then the longest common name prefix,
// then the smaller pak). Props are converted on the retail actor that shares most material names (a known PS4 twin
// first), keeping their own skeleton. Every written prop / skin is checked the way the PS4 loader resolves it.
#include "actor.hpp"

#include "texcodec.hpp"

namespace u2imp {

using namespace u3;

namespace {

const std::set<std::string> SKIN_TYPES = {"ARTGROUP_DATA", "FOREGROUND_SECTION", "LEVEL_PATS_1", "MATERIAL_TABLE_1", "PACKAGE_ITEM_LOOKUP", "PAK_LOGIN_TABLE",
                                          "PM_COLLECTION_2", "TAG_FLOAT", "TAG_INT", "TAG_VEC4", "VRAM_DESC", "VRAM_DESC_TABLE"};
const std::map<std::string, std::string> WHY = {{"JOINT_HIERARCHY", "own skeleton (JOINT_HIERARCHY)"},
                                                {"SKELETON_FLIPDATA", "own skeleton (SKELETON_FLIPDATA)"},
                                                {"ANIM", "animations (U2 PS3 ANIM)"},
                                                {"ANIM_RAWCHANNELS", "animations (U2 PS3 ANIM)"},
                                                {"COLLISION_DATA_FOREGROUND", "collision (COLLISION_DATA_FOREGROUND)"},
                                                {"COLLISION_DATA_BACKGROUND", "collision (COLLISION_DATA_BACKGROUND)"},
                                                {"COLLISION_HAVOK_JOINTED", "Havok jointed collision"},
                                                {"SOUND_BANK_TABLE", "SOUND_BANK_TABLE"}};
std::set<std::string> prop_types() {
    std::set<std::string> s = SKIN_TYPES;
    for (const char *t : {"JOINT_HIERARCHY", "SKELETON_FLIPDATA", "ANIM", "ANIM_RAWCHANNELS", "COLLISION_DATA_FOREGROUND", "COLLISION_DATA_BACKGROUND",
                          "SOUND_BANK_TABLE", "COLLISION_HAVOK_JOINTED"})
        s.insert(t);
    return s;
}
const std::set<std::string> ANIM_PAK_TYPES = {"ANIM", "ANIM_RAWCHANNELS", "ARTGROUP_DATA", "LEVEL_PATS_1", "PACKAGE_ITEM_LOOKUP", "PAK_LOGIN_TABLE",
                                              "MATERIAL_TABLE_1", "VRAM_DESC_TABLE"};
const std::set<std::string> HAVOK_PAK_TYPES = {"COLLISION_HAVOK_JOINTED", "ARTGROUP_DATA", "LEVEL_PATS_1", "PACKAGE_ITEM_LOOKUP", "PAK_LOGIN_TABLE",
                                               "MATERIAL_TABLE_1", "VRAM_DESC_TABLE"};
bool subset(const std::set<std::string> &a, const std::set<std::string> &b) {
    for (const std::string &x : a)
        if (!b.count(x)) return false;
    return true;
}

std::string kind(const std::set<std::string> &types) {
    if (!types.count("FOREGROUND_SECTION")) {
        if (types.count("ANIM") && subset(types, ANIM_PAK_TYPES)) return "anim";
        return types.count("COLLISION_HAVOK_JOINTED") && subset(types, HAVOK_PAK_TYPES) ? "havok" : "";
    }
    if (subset(types, SKIN_TYPES)) return "skin";
    return subset(types, prop_types()) && types.count("JOINT_HIERARCHY") ? "prop" : "";
}

std::string unsupported_why(const std::set<std::string> &types) {
    if (!kind(types).empty()) return "";
    auto joined = [](const std::set<std::string> &s) {
        std::string o;
        for (const std::string &x : s) o += (o.empty() ? "" : ", ") + x;
        return o;
    };
    if (!types.count("FOREGROUND_SECTION")) return "no FOREGROUND_SECTION (items: " + joined(types) + ")";
    const std::set<std::string> pt = prop_types();
    std::set<std::string> why;
    for (const std::string &t : types)
        if (!pt.count(t)) why.insert(WHY.count(t) ? WHY.at(t) : t);
    return why.empty() ? "" : "unsupported items: " + joined(why);
}

std::string key_of(const std::string &n) { return n.substr(0, n.rfind('/')) + ":" + n.substr(n.rfind('/') + 1); }
std::string before_comma(const std::string &s) { return s.substr(0, s.find(',')); }
std::string stem(const fs::path &p) {
    std::string s = S(p.filename());
    return s.size() > 4 ? s.substr(0, s.size() - 4) : s;
}
size_t common_prefix(const std::string &a, const std::string &b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
    return i;
}
std::vector<std::string> sorted_paks(const fs::path &dir) {
    std::vector<std::string> v;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir, ec)) v.push_back(S(e.path().filename()));
    std::sort(v.begin(), v.end());
    return v;
}

// retail PS4 mp-hero paks with an FG section: (skeleton, material keys, size), cached per folder
struct TInfo {
    uint32_t skel = 0;
    std::set<std::string> mats, shaders;
    size_t size = 0;
};
const std::vector<std::pair<std::string, TInfo>> &template_index(const fs::path &dir, bool props) {
    static std::map<std::pair<std::string, bool>, std::vector<std::pair<std::string, TInfo>>> cache;
    const auto key = std::make_pair(S(dir), props);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::vector<std::pair<std::string, TInfo>> out;
    for (const std::string &f : sorted_paks(dir)) {
        if (!ends(f, ".pak") || (!props && !starts(f, "mp-hero"))) continue;
        try {
            const Pak q(dir / P(f));
            if (q.items_of("FOREGROUND_SECTION_2").empty()) continue;
            TInfo t;
            for (const actor::TMat &m : actor::tmpl_materials(q)) {
                t.mats.insert(before_comma(m.name));
                t.shaders.insert(m.shader);
            }
            if (!props) t.skel = skeleton_sid(q);
            t.size = q.d.size();
            out.push_back({f.substr(0, f.size() - 4), t});
        } catch (const std::exception &) {
        }
    }
    return cache[key] = out;
}

std::set<std::string> material_keys(const U2Src &s) {
    std::set<std::string> keys;
    for (const std::string &n : ps3_material_names(s))
        if (n.find('/') != std::string::npos) keys.insert(key_of(n));
    return keys;
}
size_t common(const std::set<std::string> &a, const std::set<std::string> &b) {
    size_t n = 0;
    for (const std::string &x : a) n += b.count(x);
    return n;
}

struct Choice {
    fs::path tmpl;
    size_t shared, total;
};
Choice choose_template(const fs::path &ps3_path, const fs::path &actor_dir, const std::string &exclude = "") {
    const U2Src src(ps3_path);
    const uint32_t sk = skeleton_sid(src.p);
    const std::set<std::string> keys = material_keys(src);
    const std::string base = stem(ps3_path);
    std::optional<std::tuple<size_t, size_t, long long>> best;
    std::string name;
    for (const auto &[n, t] : template_index(actor_dir, false)) {
        if (t.skel != sk || n == exclude) continue;
        const auto score = std::make_tuple(common(keys, t.mats), common_prefix(n, base), -(long long)t.size);
        if (!best || score > *best) best = score, name = n;
    }
    if (!best) throw std::runtime_error(fmt("RuntimeError: no retail PS4 actor with skeleton %08x", sk));
    return {actor_dir / P(name + ".pak"), std::get<0>(*best), keys.size()};
}

std::string mdir(const std::string &key) {   // 'objects/props/crates' of 'objects/props/crates/crate-a:default'
    const std::string a = key.substr(0, key.find(':'));
    return a.substr(0, a.rfind('/'));
}
Choice choose_prop_template(const fs::path &ps3_path, const fs::path &actor_dir, const std::optional<std::string> &twin) {
    const U2Src src(ps3_path);
    const std::set<std::string> keys = material_keys(src);
    const std::string base = stem(ps3_path);
    std::set<std::string> kd;
    for (const std::string &k : keys) kd.insert(mdir(k));
    std::optional<std::tuple<bool, size_t, size_t, size_t, long long>> best;
    std::string name;
    for (const auto &[n, t] : template_index(actor_dir, true)) {
        bool all_sb = true;
        for (const std::string &m : t.mats) all_sb = all_sb && m.find("shadowblocker") != std::string::npos;
        // character shaders only when the materials match; not shadow blockers only
        if (starts(n, "mp-hero") || (t.shaders.count("prime-shader.fx") && !common(keys, t.mats)) || all_sb) continue;
        std::set<std::string> md;
        for (const std::string &m : t.mats) md.insert(mdir(m));
        const auto score = std::make_tuple(twin && n == *twin, common(keys, t.mats), common(kd, md), common_prefix(n, base), -(long long)t.size);
        if (!best || score > *best) best = score, name = n;
    }
    if (!best) throw std::runtime_error("RuntimeError: no retail PS4 actor with an FG section in " + S(actor_dir));
    return {actor_dir / P(name + ".pak"), std::get<1>(*best), keys.size()};
}

// props the PS4 campaign has under another name (found by content); the twin is the template, so its materials keep
// their PS4 records and remastered textures, while geometry, skeleton, animations and collision come from the PS3 pak
const std::map<std::string, std::string> PROP_TWINS = {
    {"multiplayer-generator-explode", "dig-generator-explode"}, {"multiplayer-generator", "dig-generator"},
    {"debris-hi-pot-a", "debris-wz-pot-a"},                     {"debris-hi-pot-b", "debris-wz-pot-b"},
    {"debris-hi-pot-hang-a", "debris-wz-pot-hang-a"},           {"debris-hi-pot-hang-b", "debris-wz-pot-hang-b"},
    {"debris-hi-pot-n-lid", "debris-wz-pot-n-lid"},             {"mi-24-heli-mp-lod0", "mi-24-heli-lod0"},
    {"mi-24-heli-missle-mp", "mi-24-heli-missle"},              {"mp-wrk-crash-boxcar", "wrk-box-car-02"},
    {"mp-wrk-explode-tanker", "wrk-exploding-tanker-b"},
};

std::vector<std::string> convert_prop(const fs::path &ps3_path, const fs::path &out, const std::string &k, const fs::path &actor_dir, const std::string &name) {
    if (k == "anim") {
        convert_anim_pak(ps3_path, out);
        return {"animations"};
    }
    if (k == "havok") {
        convert_havok_jointed_pak(ps3_path, out);
        return {"Havok jointed collision"};
    }
    std::optional<std::string> twin;
    if (auto it = PROP_TWINS.find(name); it != PROP_TWINS.end() && fs::is_regular_file(actor_dir / P(it->second + ".pak"))) twin = it->second;
    const Choice c = choose_prop_template(ps3_path, actor_dir, twin);
    convert_actor_pak(ps3_path, c.tmpl, out);
    std::vector<std::string> notes;
    if (twin) notes.push_back("PS4 twin " + *twin + " as template");
    notes.push_back(fmt("template %s (%zu of %zu materials by name)", S(c.tmpl.filename()).c_str(), c.shared, c.total));
    return notes;
}

// ------------------------------------------------------------------------------------------------ the loader's view
}  // namespace

// what the PS4 loader resolves: pak structure, known item types, the VRAM descriptors (format, tiled size, id = id of
// the short path, type 1), the texture table, the effect table (version 6 or 7, entries pointing at EFFECT items), and
// every texture entry, material effect and texture binding of GEOMETRY_1 (version 0x10)
std::vector<std::string> ps4check(const Pak &p) {
    static const std::set<std::string> KNOWN = {"TEXTURE_TABLE", "VRAM_DESC", "RAW_DATA", "EFFECT_TABLE", "EFFECT", "GEOMETRY_1", "LEVEL_INFO_1",
                                                "LEVEL_PATS_1", "DEBUG_INFO_1", "ATTACHED_FG_MAPPING", "TAG_FLOAT", "TAG_INT", "SPLINE_TABLE"};
    std::vector<std::string> errs = p.validate();
    for (const PakItem &it : p.items)
        if (!KNOWN.count(it.type)) errs.push_back("unknown item type " + it.type);
    std::set<uint32_t> by_hash;
    for (const PakTexture &t : p.textures()) {
        const auto f = tc::DXGI.find(t.format);
        if (f == tc::DXGI.end()) {
            errs.push_back(fmt("unknown format 0x%x %s", t.format, t.path.c_str()));
            continue;
        }
        const bool comp = starts(f->second.first, "BC");
        const uint64_t exp = tc::ps4_tiled_size(t.width, t.height, t.mips, f->second.second, comp);
        if (exp != t.vramSize) errs.push_back(fmt("tiled size 0x%x != 0x%llx for %s", t.vramSize, (unsigned long long)exp, t.path.c_str()));
        const size_t k = t.path.find("texture2/");
        const std::string shrt = k == std::string::npos ? t.path : t.path.substr(k + 9);
        if (u3::sid(bg::utf8_of(shrt)) != t.hash) errs.push_back("hash != sid(short) " + shrt);
        if (t.type != 1) errs.push_back(fmt("type %u", t.type));
        by_hash.insert(t.hash);
    }
    for (const PakItem &tt : p.items_of("TEXTURE_TABLE")) {
        const uint64_t b = tt.data_off();
        const uint32_t n = p.pu32(tt.page, b);
        const ORef hp = bg::res(p, tt.page, b + 8), np_ = bg::res(p, tt.page, b + 0x10), dp = bg::res(p, tt.page, b + 0x18);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t h = p.pu32(bg::need(hp).first, hp->second + 4ull * i);
            const Ref nm = bg::need(p, bg::need(np_).first, np_->second + 8ull * i), d = bg::need(p, bg::need(dp).first, dp->second + 8ull * i);
            const uint32_t dh = rd32le(p.d, p.pbase(d.first) + d.second + 0x14);
            if (dh != h || u3::sid(bg::utf8_of(p.pstr(nm.first, nm.second))) != h) errs.push_back(fmt("TEXTURE_TABLE entry %u inconsistent", i));
        }
    }
    std::set<uint64_t> eff;
    for (const PakItem &et : p.items_of("EFFECT_TABLE")) {
        const uint64_t b = et.data_off();
        const uint32_t n = p.pu32(et.page, b), ver = p.pu32(et.page, b + 4);
        if (ver != 6 && ver != 7) errs.push_back(fmt("EFFECT_TABLE version %u", ver));
        const ORef arr = bg::res(p, et.page, b + 8);
        const Ref fx = bg::need(p, et.page, b + 0x10);   // the effect names
        const uint32_t nfx = p.pu32(fx.first, fx.second);
        for (uint32_t k = 0; k < nfx; k++) (void)bg::need(p, fx.first, fx.second + 8 + 8ull * k);
        for (uint32_t i = 0; i < n; i++) {
            const uint64_t e = bg::need(arr).second + 0x18ull * i;
            const Ref ep = bg::need(p, arr->first, e);
            if (p.item_at(ep.first, uint32_t(ep.second - 0x20)).type != "EFFECT") errs.push_back(fmt("effect entry %u does not point at an EFFECT", i));
            eff.insert(rd64le(p.d, p.pbase(arr->first) + e + 0x10));
        }
    }
    for (const PakItem &g : p.items_of("GEOMETRY_1")) {
        const uint32_t pg = g.page;
        const uint64_t b = g.data_off();
        if (p.pu32(pg, b) != 0x10) errs.push_back(fmt("GEOMETRY_1 version 0x%x", p.pu32(pg, b)));
        const uint32_t ntex = p.pu32(pg, b + 0x38);
        const ORef tp = bg::res(p, pg, b + 0x40);
        std::set<uint32_t> texmap;
        for (uint32_t i = 0; i < ntex; i++) {
            const uint64_t e = bg::need(tp).second + 0x28ull * i;
            const Ref fr = bg::need(p, tp->first, e);
            const std::string full = p.pstr(fr.first, fr.second);
            const uint32_t h = p.pu32(tp->first, e + 8);
            texmap.insert(u3::sid(bg::utf8_of(full)));
            if (!by_hash.count(h)) errs.push_back(fmt("geometry texture %s (%08x) not in this pak", full.c_str(), h));
        }
        const uint32_t nmat = p.pu32(pg, b + 0x48);
        const ORef mp = bg::res(p, pg, b + 0x50);
        for (uint32_t i = 0; i < nmat; i++) {
            const uint64_t m = bg::need(mp).second + 0x48ull * i;
            const Ref nr = bg::need(p, mp->first, m);
            const std::string name = p.pstr(nr.first, nr.second);
            if (const ORef eh = bg::res(p, mp->first, m + 0x18)) {
                const uint64_t h = std::stoull(p.pstr(eh->first, eh->second), nullptr, 16);
                if (!eff.count(h)) errs.push_back(fmt("material %s: effect %016llX missing", name.c_str(), (unsigned long long)h));
            }
            const uint32_t nb = p.pu32(mp->first, m + 0x38);
            const ORef bp = bg::res(p, mp->first, m + 0x28);
            for (uint32_t k = 0; k < nb; k++) {
                const Ref fr = bg::need(p, bg::need(bp).first, bp->second + 0x28ull * k + 0x10);
                const std::string full = p.pstr(fr.first, fr.second);
                if (!texmap.count(u3::sid(bg::utf8_of(full)))) errs.push_back("Could not find texture '" + full + "' in material instance '" + name + "'");
            }
        }
    }
    return errs;
}

namespace {

// ps4check without what retail U2 PS4 skins show too (unknown item types, the tiled size of env-reflection cube maps),
// plus the prototypes' scope-3 cbuffer capacity
std::vector<std::string> check(const fs::path &path) {
    const Pak p(path);
    std::vector<std::string> errs;
    for (const std::string &e : ps4check(p))
        if (!starts(e, "unknown item type") && !(starts(e, "tiled size") && e.find("-env-reflection") != std::string::npos)) errs.push_back(e);
    // prototypes stating fewer scope-3 cbuffers than the pak declares
    Bytes copy = p.d;
    fix_cbuffer_capacity(copy);
    if (copy != p.d) errs.push_back("a prototype's scope-3 cbuffer capacity is below the pak's cbuffers (AddCb overflows the instance list)");
    return errs;
}

void checked_replace(const fs::path &tmp, const fs::path &out) {
    const auto errs = check(tmp);
    std::error_code ec;
    if (!errs.empty()) {
        fs::remove(tmp, ec);
        std::string s;
        for (size_t i = 0; i < errs.size() && i < 3; i++) s += (i ? "; " : "") + errs[i];
        throw std::runtime_error("RuntimeError: ps4check: " + s);
    }
    fs::rename(tmp, out, ec);
    if (ec) throw std::runtime_error("cannot write " + S(out) + ": " + ec.message());
}

}  // namespace

Converted convert_actor(const Context &c, const Entry &entry, const fs::path &src) {
    const U2Src pak(src);
    const std::set<std::string> types = pak.types();
    if (const std::string why = unsupported_why(types); !why.empty()) throw Unsupported(why);
    const std::string k = kind(types), b = basename(entry.key), name = lower(b.substr(0, b.size() - 4));
    const std::string rel = "actor27/ps4/" + b;
    const fs::path out = c.main / P(rel), tmp = S(out) + ".tmp";
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    havok_configure(c);
    if (k != "skin") {   // props, animation paks, loose ragdolls
        const auto notes = convert_prop(src, tmp, k, c.ps4->actor_dir, name);
        if (k == "prop") checked_replace(tmp, out);
        else {
            fs::rename(tmp, out, ec);
            if (ec) throw std::runtime_error("cannot write " + S(out) + ": " + ec.message());
        }
        return {rel, notes};
    }
    // skins: the PS4 campaign twin first
    std::optional<std::string> twin_note;
    if (starts(lower(b), "mp-hero")) {
        const auto t = TWINS.find(name);
        if (t == TWINS.end() || !c.ps4->has("actor27/" + t->second + ".pak")) twin_note = "no PS4 campaign twin";
        else {
            const auto cands = c.ps3->candidates("actor27/" + t->second + ".pak");
            if (cands.empty()) twin_note = "no PS3 copy of the twin " + t->second;
            else {
                const fs::path tsrc = c.work / "src" / P(basename(cands[0]->key));
                const Bytes td = cands[0]->read();
                if (!(fs::is_regular_file(tsrc, ec) && fs::file_size(tsrc, ec) == td.size())) write_bytes(tsrc, td);
                std::variant<TwinStats, std::string> r;
                try {
                    r = twin_build(src, tsrc, c.ps4->files.at("actor27/" + t->second + ".pak"), tmp);
                } catch (const std::exception &e) {   // the PS3 conversion still works: noted, not fatal
                    fs::remove(tmp, ec);
                    fs::remove(S(tmp) + ".conv", ec);
                    r = std::string("twin build error ") + std::string(e.what()).substr(0, 200);
                }
                if (const auto *st = std::get_if<TwinStats>(&r)) {
                    const auto errs = check(tmp);
                    if (errs.empty()) {
                        fs::rename(tmp, out, ec);
                        if (ec) throw std::runtime_error("cannot write " + S(out) + ": " + ec.message());
                        return {rel, {st->note()}};
                    }
                    fs::remove(tmp, ec);
                    std::string s;
                    for (size_t i = 0; i < errs.size() && i < 2; i++) s += (i ? "; " : "") + errs[i];
                    twin_note = "twin build failed ps4check: " + s;
                } else
                    twin_note = std::get<std::string>(r);
            }
        }
    }
    const Choice ch = choose_template(src, c.ps4->actor_dir);
    convert_actor_pak(src, ch.tmpl, tmp);
    if (twin_note) rekey_artgroup(tmp);   // skins: the retail artgroup keys
    checked_replace(tmp, out);
    std::vector<std::string> notes;
    if (twin_note) notes.push_back("PS3 (" + *twin_note + ")");
    notes.push_back(fmt("template %s (%zu of %zu materials by name)", S(ch.tmpl.filename()).c_str(), ch.shared, ch.total));
    return {rel, notes};
}

// ------------------------------------------------------------------------------------------------ for validate.cpp
fs::path skin_template(const fs::path &ps3_path, const fs::path &actor_dir, const std::string &exclude) {
    return choose_template(ps3_path, actor_dir, exclude).tmpl;
}
std::vector<std::string> actor_check(const fs::path &path) { return check(path); }

}  // namespace u2imp
