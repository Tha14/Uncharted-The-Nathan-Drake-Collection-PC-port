// Recipe step map-lighting (by 33martyr, PR #3): multiplayer map lighting fixes in the overlay-r3 stage paks, in place,
// no size change. Replaces apply_map_lighting.py (recipe 1.13.0 form: every pak).
//  1. Sun shadow blockers: an instance submesh whose material name contains shadowblocker, ik-only or canopy-shadow
//     must be shadow-only, as retail's hidden blockers are: +0x20 |= 0x10000 (ik-only 0x1010000) when the bit is not
//     set, +0x24 |= 0x101. Without it the blocker is drawn in the colour pass (invisible) and never into the sun
//     shadow pass, and the sun floods interiors (London Underground library).
//  2. Fort (fort-multi, fort-multi-stage), from the PS3 fragment programs:
//     * ground/stone-steps: Colour1/Colour2 and Normal1/Normal2 bindings swapped (PS3 draws the stone layer), env
//       strength 0.05, sun specular 0, metalness 0;
//     * fort-mlti-wall-plaster-cream(-emblem) without a border tint: Colour2 bound to Colour1's texture and
//       g_colorTint2 = PS3's g_diffuseColorBorderTint (0.349, 0.335, 0.311);
//     * fort-multi-stage: fort-mlti-wall-plaster-cream-emblem, fort-mlti-symbols, dig-ship-barrel metalness 0.
//     A value block shared by two materials is left alone.
//   u3fix map-lighting --work W [--apply]
// Dry run by default; idempotent. Backup of every changed pak: <work>/backup-map-lighting/<pak> (never overwritten).
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "u3fix.hpp"

#include <cstring>

namespace u3 {
namespace {

struct Write {
    uint64_t off;
    Bytes old, nw;
    std::string desc;
};
Bytes le32(uint32_t v) { return Bytes{uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; }
Bytes f32s(const std::vector<float> &v) {
    Bytes o(4 * v.size());
    std::memcpy(o.data(), v.data(), o.size());
    return o;
}
bool has(const std::string &s, const char *x) { return s.find(x) != std::string::npos; }

std::vector<Write> blocker_writes(const Pak &q) {
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) return {};
    const uint32_t pg = gs[0].page;
    const uint64_t b = gs[0].data_off();
    const ORef il = bg::res(q, pg, b + 0x30);
    const uint64_t n = q.u64(q.pbase(pg) + b + 0x28);
    if (!il) return {};
    const uint64_t nsub = q.u64(q.pbase(pg) + b + 8);
    const Ref sref = bg::need(q, pg, b + 0x10);
    std::map<std::string, std::string> mat;
    for (uint64_t k = 0; k < nsub; k++) {
        const uint64_t o = sref.second + 0x88 * k;
        const Ref nr = bg::need(q, sref.first, o + 8);
        const ORef mr = bg::res(q, sref.first, o + 0x48);
        const std::string mn = mr ? q.pstr(mr->first, mr->second) : "";
        mat[q.pstr(nr.first, nr.second)] = mn.substr(0, mn.find(','));
    }
    std::vector<Write> W;
    for (uint64_t i = 0; i < n; i++) {
        const Ref r = bg::need(q, il->first, il->second + 8 * i);
        const uint32_t ns = q.pu32(r.first, r.second + 0x20);
        const ORef sr = bg::res(q, r.first, r.second + 0x30);
        if (!sr) continue;
        for (uint32_t k = 0; k < ns; k++) {
            const uint64_t o = sr->second + 0x50ull * k, ab = q.pbase(sr->first) + o;
            const Ref nr = bg::need(q, sr->first, o);
            auto mi = mat.find(q.pstr(nr.first, nr.second));
            const std::string m = mi == mat.end() ? "" : mi->second;
            if (!(has(m, "shadowblocker") || has(m, "ik-only") || has(m, "canopy-shadow"))) continue;
            const uint32_t f = rd32le(q.d, ab + 0x20), f2 = rd32le(q.d, ab + 0x24);
            const uint32_t n20 = (f & 0x10000) ? f : f | (has(m, "ik-only") ? 0x1010000u : 0x10000u);
            if (n20 != f) W.push_back({ab + 0x20, le32(f), le32(n20), "blocker " + m});
            if ((f2 | 0x101) != f2) W.push_back({ab + 0x24, le32(f2), le32(f2 | 0x101), "blocker " + m});
        }
    }
    return W;
}

struct Mat {
    std::map<std::string, uint64_t> params, binds;
    std::map<std::string, std::string> bindpath;
};
// name -> parameters (file offset of each value), bindings (file offset of each record) and binding paths, in the
// order of first appearance (a later material of the same name replaces the values)
std::vector<std::pair<std::string, Mat>> materials(const Pak &q) {
    std::vector<std::pair<std::string, Mat>> out;
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) return out;
    const uint32_t pg = gs[0].page;
    const uint64_t b = gs[0].data_off();
    const uint32_t n = q.pu32(pg, b + 0x48);
    const ORef r = bg::res(q, pg, b + 0x50);
    if (!r) return out;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t o = r->second + uint64_t(i) * 0x48;
        const Ref nr = bg::need(q, r->first, o);
        std::string nm = q.pstr(nr.first, nr.second);
        nm = nm.substr(0, nm.find(','));
        const uint64_t cb = q.pbase(r->first) + o + 0x30;
        const uint32_t npar = rd32le(q.d, cb + 4), nb = rd32le(q.d, cb + 8);
        Mat M;
        if (const ORef pr = bg::res(q, r->first, o + 0x20))
            for (uint32_t k = 0; k < npar; k++) {
                const uint64_t po = pr->second + 0x18ull * k;
                if (const ORef vr = bg::res(q, pr->first, po + 8)) {
                    const Ref pn = bg::need(q, pr->first, po);
                    M.params[q.pstr(pn.first, pn.second)] = q.pbase(vr->first) + vr->second;
                }
            }
        if (const ORef br = bg::res(q, r->first, o + 0x28))
            for (uint32_t k = 0; k < nb; k++) {
                const uint64_t bo = br->second + 0x28ull * k;
                const Ref bn = bg::need(q, br->first, bo);
                const std::string name = q.pstr(bn.first, bn.second);
                M.binds[name] = q.pbase(br->first) + bo;
                const ORef fr = bg::res(q, br->first, bo + 0x10);
                M.bindpath[name] = fr ? q.pstr(fr->first, fr->second) : "";
            }
        auto it = std::find_if(out.begin(), out.end(), [&](const auto &e) { return e.first == nm; });
        if (it != out.end()) it->second = std::move(M);
        else out.push_back({nm, std::move(M)});
    }
    return out;
}

std::vector<Write> fort_edits(const Pak &q, const std::string &pak) {
    const auto M = materials(q);
    std::map<uint64_t, int> users;
    for (const auto &[nm, m] : M)
        for (const auto &[pn, a] : m.params) users[a]++;
    std::vector<Write> E;
    auto mat = [&](const std::string &nm) -> const Mat & {
        for (const auto &e : M)
            if (e.first == nm) return e.second;
        throw Stop("KeyError: " + nm);
    };
    auto find = [&](const std::string &suffix) -> std::optional<std::string> {
        std::vector<std::string> hits;
        for (const auto &e : M)
            if (e.first.size() >= suffix.size() && e.first.compare(e.first.size() - suffix.size(), suffix.size(), suffix) == 0) hits.push_back(e.first);
        return hits.size() == 1 ? std::optional<std::string>(hits[0]) : std::nullopt;
    };
    auto tail = [](const std::string &nm) { return nm.substr(nm.rfind('/') + 1); };
    auto bytes = [&](uint64_t a, size_t n) { return Bytes(q.d.begin() + std::ptrdiff_t(a), q.d.begin() + std::ptrdiff_t(a + n)); };
    auto setf = [&](const std::optional<std::string> &nm, const std::string &pn, const std::vector<float> &vals) {
        if (!nm || !mat(*nm).params.count(pn)) return;
        const uint64_t a = mat(*nm).params.at(pn);
        if (users[a] != 1) return;   // a shared value block: leave it
        const Bytes old = bytes(a, 4 * vals.size()), nw = f32s(vals);
        if (old != nw) E.push_back({a, old, nw, tail(*nm) + " " + pn});
    };
    auto bind_copy = [&](const std::string &nm, const std::string &dst, const std::string &src) {   // dst bind -> src's texture
        const uint64_t d0 = mat(nm).binds.at(dst), s0 = mat(nm).binds.at(src);
        const Bytes old = bytes(d0 + 8, 16), nw = bytes(s0 + 8, 16);
        if (old != nw) E.push_back({d0 + 8, old, nw, tail(nm) + " " + dst + " -> " + src + " texture"});
    };
    if (const auto steps = find("ground/stone-steps:default")) {
        const Mat &S = mat(*steps);
        auto bp = S.bindpath.find("g_tNdFetchColor2Map");
        if (!has(bp == S.bindpath.end() ? "" : bp->second, "stone-step-color")) {   // not yet swapped
            for (const auto &[x_, y_] : std::vector<std::pair<std::string, std::string>>{{"g_tNdFetchColor1Map", "g_tNdFetchColor2Map"}, {"g_tNdFetchNormal1Map", "g_tNdFetchNormal2Map"}}) {
                const uint64_t x = S.binds.at(x_), y = S.binds.at(y_);
                const Bytes ox = bytes(x + 8, 16), oy = bytes(y + 8, 16);
                E.push_back({x + 8, ox, oy, "stone-steps " + x_ + " <-> " + y_});
                E.push_back({y + 8, oy, ox, ""});
            }
        }
        setf(steps, "g_specularEnvironmentStrength", {0.05f});
        setf(steps, "g_specularBrightness", {0.0f});
        setf(steps, "g_specularMetalness", {0.0f});
    }
    for (const char *suffix : {"fort/fort-mlti-wall-plaster-cream:default", "fort/fort-mlti-wall-plaster-cream-emblem:default"}) {
        const auto nm = find(suffix);
        if (nm && !mat(*nm).params.count("g_diffuseColorBorderTint") && mat(*nm).params.count("g_colorTint2")) {
            bind_copy(*nm, "g_tNdFetchColor2Map", "g_tNdFetchColor1Map");
            setf(nm, "g_colorTint2", {0.34935078f, 0.33464769f, 0.31090671f});
        }
    }
    if (pak == "fort-multi-stage.pak")
        for (const char *suffix : {"fort/fort-mlti-wall-plaster-cream-emblem:default", "fort/fort-mlti-symbols:default", "wood/dig-ship-barrel:default"})
            setf(find(suffix), "g_specularMetalness", {0.0f});
    return E;
}

}  // namespace

int map_lighting(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--work"});
    const bool apply = a.has("--apply");
    const std::string work = a.need("--work"), root = work + "/overlay-r3/u3data/build/main/pak23/ps4", bak = work + "/backup-map-lighting";
    if (!fs::is_directory(upath(root))) throw Stop("no " + root);
    std::vector<std::string> names;
    for (const auto &e : fs::directory_iterator(upath(root)))
        if (e.path().extension() == ".pak") names.push_back(utf8(e.path().filename()));
    std::sort(names.begin(), names.end());
    int changed = 0;
    for (const std::string &name : names) {
        const fs::path path = upath(root) / upath(name);
        Bytes d;
        std::vector<Write> W;
        try {
            d = read_file(path);
            const Pak q(d, name);
            W = blocker_writes(q);
            if (name == "fort-multi.pak" || name == "fort-multi-stage.pak") {
                const auto F = fort_edits(q, name);
                W.insert(W.end(), F.begin(), F.end());
            }
        } catch (const std::exception &e) {
            std::printf("skip (unreadable) %s %s\n", name.c_str(), e.what());
            continue;
        }
        if (W.empty()) continue;
        changed++;
        std::printf("%s %s: %zu writes\n", apply ? "patch" : "would patch", name.c_str(), W.size());
        for (const Write &w : W)
            if (!w.desc.empty()) std::printf("   %s %s\n", pyhex(w.off).c_str(), w.desc.c_str());
        if (!apply) continue;
        fs::create_directories(upath(bak));
        if (!fs::exists(upath(bak) / upath(name))) fs::copy_file(path, upath(bak) / upath(name));
        for (const Write &w : W) {
            if (w.off + w.old.size() > d.size() || !std::equal(w.old.begin(), w.old.end(), d.begin() + std::ptrdiff_t(w.off)))
                throw Stop("unexpected bytes in " + name + " at " + pyhex(w.off));
            std::copy(w.nw.begin(), w.nw.end(), d.begin() + std::ptrdiff_t(w.off));
        }
        write_file_replace(path, d);
    }
    if (changed) std::printf("%d paks %s\n", changed, apply ? "patched" : "to patch");
    else std::printf("already applied (nothing to change)\n");
    return 0;
}

}  // namespace u3
