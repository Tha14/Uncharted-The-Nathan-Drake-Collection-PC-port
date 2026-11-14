// importer/netimg.cpp: the menu images the PS4 net.pak lacks, from the PS3 net.pak, as a small PS4 package
// 'net-u2mp.pak' (pak22/ps4).
//
// The PS4 Collection's net.pak dropped what only the PS3 online menus used: the Leaderboards / Trophies mode buttons,
// the DLC map photos and a few more. A PS3 material is converted, with the textures it uses, when no PS4 material has
// its id (the string id of its '/' name; PS4 names read with ':' as '/'), and for three mode buttons whose PS3 look the
// owner chose over the redrawn PS4 ones (the runtime looks net-u2mp up first, so those win).
//
// The package is the Uncharted 3 net converter's (dctools ps4mats convert_net) with what U2 differs in:
//   * the PS3 MATERIAL_TABLE_1: one per section, the texture hashes in one shared array (on another page), no texture
//     name array (names come from the VRAM descriptors);
//   * effects: each material gets the effect most PS4 materials of its image set ('dir' of 'dir:name') use, with the
//     shader and parameters of the player-icons set's material (else the first set's);
//   * colour maps are UNORM (sRGB drew them darker);
//   * a few PS3 descriptors (the DLC map photo placeholders) share one path under several hashes; the converter needs
//     hash == id of the name, so those get the hash appended to their name, and that name's id.
// In game, gow3_pc/src/u2mp/menuimages.cpp loads 'net-u2mp' next to 'net' and falls back to its material table.
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "ps4mats.hpp"
#include "u2conv.hpp"

namespace u2imp {

const char *NETIMG_OUT_NAME = "net-u2mp.pak";
const char *NETIMG_VERSION = "3";   // bump when the materials or their formats change: the import converts again

namespace {

using u3::Pak;
using u3::mats::NetMat;
using u3::mats::NetTex;

const std::set<std::string> PREFER_PS3 = {"general/net/mode-buttons/matchmaking", "general/net/mode-buttons/customgame",
                                          "general/net/mode-buttons/cinema"};
const std::string PS3_COOK_MARK = "texture1/";

uint32_t sid_l1(const std::string &latin1) { return u3::sid(u3::bg::utf8_of(latin1)); }
std::pair<uint32_t, uint64_t> need(const Pak &p, uint32_t pg, uint64_t off) {
    const auto r = p.resolve(pg, uint32_t(off));
    if (!r) throw std::runtime_error("TypeError: cannot unpack non-iterable NoneType object");
    return {r->first, r->second};
}

struct Mat {
    std::string name;
    uint32_t sid;
    std::vector<std::optional<uint32_t>> textures;
};

// the materials and the texture list of the PS3 net.pak (U2 layout)
void read_materials_u2(const Pak &p, std::vector<Mat> &out, std::vector<NetTex> &hashes) {
    if (!p.ps3) throw std::runtime_error("AssertionError: not a PS3 pak");
    std::map<uint32_t, std::string> vram;
    for (const auto &t : p.textures()) vram[t.hash] = t.path;
    std::optional<std::pair<uint32_t, uint64_t>> texh;
    for (const auto &it : p.items_of("MATERIAL_TABLE_1")) {
        const uint32_t pg = it.page;
        const uint64_t b = it.data_off();
        if (p.pu32(pg, b) != 0x79) throw std::runtime_error(fmt("ValueError: unsupported MATERIAL_TABLE_1 version %#x", p.pu32(pg, b)));
        const uint32_t nmat = p.pu32(pg, b + 0xc);
        const auto th = p.resolve(pg, uint32_t(b + 0x18));
        const auto ma = need(p, pg, b + 0x20);
        if (!texh) texh = th;
        else if (th != texh) throw std::runtime_error("ValueError: material tables with different texture hash arrays");
        for (uint32_t i = 0; i < nmat; i++) {
            const auto [mp, mo] = need(p, ma.first, ma.second + 4ull * i);
            const auto nr = need(p, mp, mo);
            Mat rec;
            rec.name = p.pstr(nr.first, nr.second);
            rec.sid = sid_l1(rec.name);
            if (p.pu32(mp, mo + 0x18) != rec.sid) throw std::runtime_error("ValueError: material id mismatch: " + rec.name);
            const auto r = p.resolve(mp, uint32_t(mo + 0x10));
            const auto node = r ? p.resolve(r->first, uint32_t(r->second)) : std::nullopt;
            const auto bb = node ? p.resolve(node->first, uint32_t(node->second + 0x10)) : std::nullopt;
            if (bb)
                for (uint32_t k = 0; k < p.pu32(bb->first, bb->second + 4); k++) {
                    const auto rr = p.resolve(bb->first, uint32_t(bb->second + 0x50 + 0x10ull * k));
                    const bool ok = rr && texh && rr->first == texh->first && rr->second >= texh->second && (rr->second - texh->second) % 4 == 0;
                    rec.textures.push_back(ok ? std::optional<uint32_t>(uint32_t((rr->second - texh->second) / 4)) : std::nullopt);
                }
            out.push_back(std::move(rec));
        }
    }
    int64_t n = 0;
    for (const Mat &m : out)
        for (const auto &t : m.textures)
            if (t) n = std::max<int64_t>(n, int64_t(*t) + 1);
    for (int64_t i = 0; i < n; i++) {
        const uint32_t h = p.pu32(texh->first, texh->second + 4ull * uint64_t(i));
        const auto v = vram.find(h);
        hashes.push_back({h, v != vram.end() ? v->second : fmt("<%08x>", h)});
    }
}

// the PS4 net.pak materials: (name before the first ',', effect hash) in table order
std::vector<std::pair<std::string, std::string>> ps4_materials(const Pak &q) {
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) throw std::runtime_error("IndexError: no GEOMETRY_1 in the PS4 net.pak");
    const uint32_t pg = gs[0].page;
    const uint64_t b = gs[0].data_off();
    const uint64_t mo = need(q, pg, b + 0x50).second;
    std::vector<std::pair<std::string, std::string>> v;
    for (uint32_t i = 0; i < q.pu32(pg, b + 0x48); i++) {
        const uint64_t m = mo + 0x48ull * i;
        std::string nm = q.pstr(pg, need(q, pg, m).second);
        nm = nm.substr(0, nm.find(','));
        v.push_back({nm, q.pstr(pg, need(q, pg, m + 0x18).second)});
    }
    return v;
}

}  // namespace

std::vector<std::string> convert_net_images(const fs::path &ps3_net, const fs::path &ps4_net, const fs::path &out) {
    const Pak src(ps3_net), q(ps4_net);
    std::vector<Mat> all;
    std::vector<NetTex> tex;
    read_materials_u2(src, all, tex);
    const auto ps4m = ps4_materials(q);
    std::set<uint32_t> have;
    for (const auto &[nm, eh] : ps4m) {
        std::string s = nm;
        std::replace(s.begin(), s.end(), ':', '/');
        have.insert(sid_l1(s));
    }
    std::vector<Mat> keep;
    for (const Mat &m : all)
        if ((!have.count(m.sid) || PREFER_PS3.count(m.name)) && std::any_of(m.textures.begin(), m.textures.end(), [](const auto &t) { return t.has_value(); }))
            keep.push_back(m);
    if (keep.empty()) {
        log("net images: the PS4 net.pak has every PS3 net material");
        return {};
    }
    // the effect of each PS4 image set: the one most of its materials use (the first of a tie), with its first material
    std::vector<std::pair<std::pair<std::string, std::string>, int>> count;   // (set, effect) -> uses, first-use order
    std::map<std::pair<std::string, std::string>, std::string> first;
    for (const auto &[nm, eh] : ps4m) {
        const std::pair<std::string, std::string> key{nm.substr(0, nm.find(':')), eh};
        auto it = std::find_if(count.begin(), count.end(), [&](const auto &c) { return c.first == key; });
        if (it == count.end()) count.push_back({key, 1});
        else it->second++;
        first.emplace(key, nm);
    }
    std::stable_sort(count.begin(), count.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    std::vector<std::pair<std::string, std::pair<std::string, std::string>>> effects;   // set -> (material, effect), in that order
    for (const auto &[key, n] : count)
        if (std::none_of(effects.begin(), effects.end(), [&](const auto &e) { return e.first == key.first; }))
            effects.push_back({key.first, {first[key], key.second}});
    auto effect_of = [&](const std::string &set) -> const std::pair<std::string, std::string> * {
        for (const auto &e : effects)
            if (e.first == set) return &e.second;
        return nullptr;
    };
    if (effects.empty()) throw std::runtime_error("StopIteration: the PS4 net.pak has no materials");
    const auto *dflt = effect_of("general/net/player-icons");
    if (!dflt) dflt = &effects.front().second;
    u3::mats::NetOptions o;
    o.effect_material = dflt->first;
    o.srgb_color = false;
    for (const Mat &m : keep) {
        const auto *e = effect_of(m.name.substr(0, m.name.rfind('/') == std::string::npos ? m.name.size() : m.name.rfind('/')));
        o.material_effect[m.name] = (e ? e : dflt)->second;
    }
    // the kept materials' textures only, renumbered
    std::set<uint32_t> used_s;
    for (const Mat &m : keep)
        for (const auto &t : m.textures)
            if (t) used_s.insert(*t);
    const std::vector<uint32_t> used(used_s.begin(), used_s.end());
    std::map<uint32_t, uint32_t> remap;
    for (size_t i = 0; i < used.size(); i++) remap[used[i]] = uint32_t(i);
    std::vector<NetMat> mats;
    for (const Mat &m : keep) {
        NetMat nm{m.name, {}};
        for (const auto &t : m.textures) nm.textures.push_back(t ? std::optional<uint32_t>(remap.at(*t)) : std::nullopt);
        mats.push_back(std::move(nm));
    }
    std::vector<NetTex> mtex;
    for (uint32_t i : used) mtex.push_back(tex.at(i));
    std::set<uint32_t> want;
    for (const NetTex &t : mtex) want.insert(t.hash);
    // descriptors whose name does not hash to their id get the id appended to the name
    std::map<uint32_t, std::pair<std::string, uint32_t>> rename;
    const auto ps3tex = src.textures();
    for (const auto &t : ps3tex) {
        const size_t k = t.path.find(PS3_COOK_MARK);
        const std::string shrt = k == std::string::npos ? t.path : t.path.substr(k + PS3_COOK_MARK.size());
        if (want.count(t.hash) && sid_l1(shrt) != t.hash) {
            const std::string nw = ends(shrt, ".ndb") ? shrt.substr(0, shrt.size() - 4) + fmt("-%08x.ndb", t.hash) : shrt + fmt("-%08x", t.hash);
            rename[t.hash] = {t.path.substr(0, t.path.size() - shrt.size()) + nw, sid_l1(nw)};
        }
    }
    for (NetTex &t : mtex)
        if (const auto r = rename.find(t.hash); r != rename.end()) t.name = r->second.first, t.hash = r->second.second;
    std::vector<u3::PakTexture> textures;
    for (auto t : ps3tex)
        if (want.count(t.hash)) {
            if (const auto r = rename.find(t.hash); r != rename.end()) t.path = r->second.first, t.hash = r->second.second;
            textures.push_back(t);
        }
    o.mats = &mats;
    o.mtex = &mtex;
    o.textures = &textures;
    size_t written = 0;
    o.materials_written = &written;
    const Bytes data = u3::mats::convert_net(ps3_net, ps4_net, o);
    const fs::path tmp = S(out) + ".tmp";
    write_bytes(tmp, data);
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) throw std::runtime_error("cannot write " + S(out) + ": " + ec.message());
    log(fmt("net images: %s: %zu materials, %zu textures, %zu bytes", S(out.filename()).c_str(), written, textures.size(), data.size()));
    std::vector<std::string> names;
    for (const NetMat &m : mats) names.push_back(m.name);
    return names;
}

}  // namespace u2imp
