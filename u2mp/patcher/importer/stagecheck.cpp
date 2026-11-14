// importer/stagecheck.cpp: the check of a converted Uncharted 2 stage (background) pak before it is kept: what the PS4
// loader resolves, plus the stage structure.
//
//   * ps4check (actors.cpp), where the item types U2 stages carry besides the U3 ones (LIGHT_TABLE_1,
//     COLLISION_DATA_HAVOK_BG, VIS_INFO_1, WATER_DATA, LEVEL_OFFSET_1) are fine, and so are cube maps (VRAM type 2:
//     their 6 faces make the tiled size differ, retail stages carry them too)
//   * the login order is the U2 retail one: the types in the order of U2_LOGIN
//   * VIS_INFO_1 version 7 walks the way the game reads it: dictionary symbols in range, the 4 buffers present, every
//     region's planes and face vertices, every node of its tree reachable, every leaf stream ending in the end code
//     within nWords * 4 decoded bytes, every G entry with its bits
//   * COLLISION_DATA_HAVOK_BG keeps the U2 layout (stage.cpp): every pointer inside the item, aligned, nothing overrun,
//     indices below the vertex count (the U3 Havok packfile check does not apply: the U2 header has no packfile at +70)
//   * item types within the retail stage set; pages at most 0x200000 bytes; VRAM directly after the last page
//   * GEOMETRY_1 version 0x10, flags 0: every submesh material in the material table, indices below the vertex count,
//     stream sizes = vertex count * stride; prototype submesh names resolve; instances name a prototype, their
//     submeshes resolve and their stream sizes fit the submesh's vertex count
//   * a stage carrying its own lightmaps binds every g_tLightMap* parameter to them
#include "actor.hpp"
#include "visinfo.hpp"

namespace u2imp {

using namespace u3;

namespace {

const std::vector<std::string> U2_LOGIN = {"LEVEL_INFO_1", "LEVEL_PATS_1", "DEBUG_INFO_1", "EFFECT_TABLE", "TEXTURE_TABLE", "LEVEL_OFFSET_1",
                                           "VIS_INFO_1",   "GEOMETRY_1",   "LIGHT_TABLE_1", "COLLISION_DATA_HAVOK_BG", "WATER_DATA"};
const std::set<std::string> STAGE_TYPES = {"TEXTURE_TABLE", "VRAM_DESC", "RAW_DATA", "EFFECT_TABLE", "EFFECT", "GEOMETRY_1", "LEVEL_INFO_1",
                                           "LEVEL_PATS_1", "DEBUG_INFO_1", "ATTACHED_FG_MAPPING", "TAG_FLOAT", "LIGHT_TABLE_1",
                                           "COLLISION_DATA_HAVOK_BG", "VIS_INFO_1", "WATER_DATA", "LEVEL_OFFSET_1"};

struct Bad : std::runtime_error {   // a VIS_INFO_1 walk failure: ValueError('<what>')
    explicit Bad(const std::string &what) : std::runtime_error("ValueError('" + what + "')") {}
};

Bytes slice(const Pak &p, const Ref &a, size_t n) {   // up to n bytes from a, fewer at the end of the file
    const uint64_t s = std::min<uint64_t>(p.pbase(a.first) + a.second, p.d.size());
    const uint64_t e = std::min<uint64_t>(s + n, p.d.size());
    return Bytes(p.d.begin() + std::ptrdiff_t(s), p.d.begin() + std::ptrdiff_t(e));
}

// ------------------------------------------------------------------------------------------------ VIS_INFO_1 version 7
void vis_walk(const Pak &p, const PakItem &it) {
    const Ref h{it.page, it.data_off()};
    auto U = [&](const Ref &a, uint64_t k = 0) { return p.pu32(a.first, a.second + k); };
    auto R = [&](const Ref &a, uint64_t k = 0) { return bg::res(p, a.first, a.second + k); };
    if (U(h) != 7) throw Bad("version");
    const uint32_t nreg = U(h, 4), nwords = U(h, 0xb0), ndict = U(h, 0xb4), ng = U(h, 0xc0);
    const ORef dr = R(h, 0xb8);
    std::vector<std::pair<uint32_t, uint32_t>> dic;
    for (uint32_t i = 0; i < ndict; i++) {
        const Ref d = bg::need(dr);
        const uint64_t a = p.pbase(d.first) + d.second + 4ull * i;
        dic.emplace_back(p.u16(a), p.u16(a + 2));
    }
    for (const auto &[l, r] : dic)
        if (l > 0x100 + ndict || r > 0x100 + ndict) throw Bad("dict symbol out of range");
    for (int k = 0; k < 4; k++)
        if (!R(h, 0x18 + 0x28ull * k) && nwords) throw Bad(fmt("buffer %d missing", k));
    const ORef regs = R(h, 8);
    for (uint32_t i = 0; i < nreg; i++) {
        const Ref rg{bg::need(regs).first, regs->second + 0x40ull * i};
        const uint32_t npl = U(rg, 0x30), nfa = U(rg, 0x34);
        if (npl && !R(rg, 0)) throw Bad("planes");
        const ORef fa = R(rg, 0x10);
        for (uint32_t j = 0; j < nfa; j++)
            if (!R(bg::need(fa), 16ull * j + 8)) throw Bad("face verts");
        const ORef nd = R(rg, 8);
        if (!nd) continue;
        std::set<int64_t> seen;
        std::vector<int64_t> todo = {0};
        while (!todo.empty()) {
            const int64_t n = todo.back();
            todo.pop_back();
            if (!seen.insert(n).second) continue;
            const uint64_t o = nd->second + 0x18ull * uint64_t(n);
            const uint32_t a = p.pu32(nd->first, o);
            const int32_t b = int32_t(p.pu32(nd->first, o + 4));
            if (const ORef sp = bg::res(p, nd->first, o + 0x10)) {
                size_t nout = 0;
                huff_len(slice(p, *sp, 0x10000), dic, nout);
                if (nout > 4ull * nwords) throw Bad("leaf stream too long");
            } else {
                const int64_t left = int64_t(int32_t(a)) >> 2;
                for (int64_t c : {left, int64_t(b)})
                    if (c >= 0) todo.push_back(c);
            }
        }
    }
    const ORef g = R(h, 0xc8);
    for (uint32_t j = 0; j < ng; j++)
        if (!R(bg::need(g), 16ull * j + 8)) throw Bad("G bits");
}

// ------------------------------------------------------------------------------------------------ U2 collision
std::vector<std::string> collision_problems(const Pak &q, const PakItem &it) {
    std::vector<std::string> errs;
    const uint32_t pg = it.page;
    const uint64_t b = it.data_off(), end = b + it.size;
    auto P = [&](uint64_t o, const std::string &what, uint64_t align = 1) -> std::optional<uint64_t> {
        const ORef r = q.resolve(pg, uint32_t(o));
        if (!r) return std::nullopt;
        if (r->first != pg || !(b <= r->second && r->second < end)) {
            errs.push_back(what + " points outside the item");
            return std::nullopt;
        }
        if (r->second % align) errs.push_back(fmt("%s not %llu-aligned", what.c_str(), (unsigned long long)align));
        return r->second;
    };
    const uint64_t base = q.pbase(pg);
    if (q.pu32(pg, b) != 3) errs.push_back(fmt("version %u", q.pu32(pg, b)));
    if (b % 0x80) errs.push_back("item data not 128-aligned");
    const uint32_t n = q.pu32(pg, b + 0x18);
    const auto A = P(b + 8, "A"), Sp = P(b + 0x10, "subparts");
    for (uint32_t k = 0; k < ((A && Sp) ? n : 0); k++) {
        const auto s = P(*Sp + 8ull * k, fmt("subpart %u", k), 16);
        if (!s) continue;
        const uint32_t ntri = q.pu32(pg, *s + 0x20), nv = q.pu32(pg, *s + 0x30);
        const auto v = P(*s + 0x28, fmt("vertices %u", k), 0x80), ix = P(*s + 0x38, fmt("indices %u", k), 0x80);
        const uint32_t vstride = q.u16(base + *s + 0x40), istride = q.u16(base + *s + 0x48), stype = q.d.at(base + *s + 0x4a);
        if (v && *v + uint64_t(vstride) * nv > end) errs.push_back(fmt("vertices %u overrun", k));
        if (ix && ntri) {
            const uint32_t isz = stype == 2 ? 2 : 4;
            const uint64_t last = *ix + uint64_t(istride) * (ntri - 1) + 3 * isz;
            if (last > end)
                errs.push_back(fmt("indices %u overrun", k));
            else {
                uint32_t mx = 0;
                for (uint32_t t = 0; t < ntri; t++)
                    for (uint32_t c = 0; c < 3; c++) {
                        const uint64_t a = base + *ix + uint64_t(istride) * t + uint64_t(isz) * c;
                        mx = std::max(mx, isz == 2 ? uint32_t(q.u16(a)) : q.u32(a));
                    }
                if (mx >= nv) errs.push_back(fmt("subpart %u index %u >= %u vertices", k, mx, nv));
            }
        }
        const uint64_t a = *A + 0x18ull * k;
        const auto x = P(a, fmt("material index %u", k));
        if (x && *x + ntri > end) errs.push_back(fmt("material index %u overrun", k));
        const auto y = P(a + 8, fmt("materials %u", k));
        if (y && *y + 12ull * q.pu32(pg, a + 0x10) > end) errs.push_back(fmt("materials %u overrun", k));
        if (errs.size() > 20) break;
    }
    if (const auto m = P(b + 0x20, "MOPP"))
        if (const auto c = P(*m + 0x10, "MOPP code", 0x80); c && *c + q.pu32(pg, *m + 0x18) > end) errs.push_back("MOPP code overrun");
    if (const auto w = P(b + 0x28, "welding", 0x80); w && *w + 2ull * q.pu32(pg, b + 0x30) > end) errs.push_back("welding overrun");
    return errs;
}

// ------------------------------------------------------------------------------------------------ GEOMETRY_1
struct Stream {
    uint32_t slot, stride, size;
};
// the vertex streams of a submesh: {u16 1, u16 stride, u32 size, ptr format {slot, 0, size, type}, ptr data}; data is
// float x3 (type 6), half x2 (0x22) or half x4 (0x0a)
std::vector<Stream> streams(const Pak &q, const ORef &ref, uint32_t n) {
    std::vector<Stream> out;
    for (uint32_t k = 0; k < n; k++) {
        const Ref at{bg::need(ref).first, ref->second + 0x18ull * k};
        const uint64_t b = q.pbase(at.first) + at.second;
        const uint32_t stride = q.u16(b + 2), size = q.u32(b + 4);
        const Ref f = bg::need(q, at.first, at.second + 8);
        const uint64_t fb = q.pbase(f.first) + f.second;
        const uint32_t slot = q.d.at(fb), type = q.d.at(fb + 3);
        if (bg::res(q, at.first, at.second + 0x10)) {
            static const std::map<uint32_t, uint32_t> ELEM = {{6, 12}, {0x22, 4}, {0x0a, 8}};
            const auto e = ELEM.find(type);
            if (e == ELEM.end()) throw std::runtime_error(fmt("KeyError: vertex stream type %u", type));
            if (size % e->second) throw std::runtime_error(fmt("ValueError: vertex stream size %u", size));
        }
        out.push_back({slot, stride, size});
    }
    return out;
}

struct Submesh {
    std::string name, mat;
    uint32_t nv = 0, ni = 0, max_index = 0;
    std::vector<Stream> st;
};

}  // namespace

// the problems of the converted stage pak at `path` ([] = keep it)
std::vector<std::string> stage_check(const fs::path &path) {
    const Pak p(path);
    std::string stage = S(path.filename());
    stage = stage.substr(0, stage.size() - 4);
    // the textures by path (a later one of the same path replaces the earlier, in its place)
    std::vector<PakTexture> texs;
    for (const PakTexture &t : p.textures()) {
        auto same = std::find_if(texs.begin(), texs.end(), [&](const PakTexture &x) { return x.path == t.path; });
        if (same != texs.end()) *same = t;
        else texs.push_back(t);
    }
    std::vector<std::string> errs;
    for (const std::string &e : ps4check(p)) {
        if (e == "unknown item type LIGHT_TABLE_1" || e == "unknown item type COLLISION_DATA_HAVOK_BG" || e == "unknown item type VIS_INFO_1" ||
            e == "unknown item type WATER_DATA" || e == "unknown item type LEVEL_OFFSET_1")
            continue;
        if (e == "type 2") continue;   // a cube map (6 faces)
        if (starts(e, "tiled size")) {
            const auto m = std::find_if(texs.begin(), texs.end(), [&](const PakTexture &t) { return e.find(t.path) != std::string::npos; });
            if (m != texs.end() && m->type == 2) continue;
        }
        errs.push_back(e);
    }
    for (const PakItem &it : p.items_of("VIS_INFO_1")) {
        try {
            vis_walk(p, it);
        } catch (const std::exception &ex) {
            errs.push_back(std::string("VIS_INFO_1: ") + ex.what());
        }
    }
    for (const PakItem &it : p.items)
        if (!STAGE_TYPES.count(it.type)) errs.push_back("item type " + it.type + " not in retail stages");
    for (size_t i = 0; i < p.pages.size(); i++)
        if (p.pages[i].size > 0x200000) errs.push_back(fmt("page %zu too large 0x%x", i, p.pages[i].size));
    if (p.pages.empty() || p.vram_base != int64_t(p.pages.back().file) + p.pages.back().size) errs.push_back("VRAM base not at end of pages");

    const auto geos = p.items_of("GEOMETRY_1");
    if (geos.empty()) throw std::runtime_error("IndexError: no GEOMETRY_1");
    const uint32_t pg = geos[0].page;
    const uint64_t b = geos[0].data_off(), gb = p.pbase(pg) + b;
    const uint32_t ver = p.pu32(pg, b), flags = p.pu32(pg, b + 4);
    if (ver != 0x10 || flags != 0) errs.push_back(fmt("GEOMETRY_1 ver/flags 0x%x/%u", ver, flags));
    // submeshes (0x88 bytes): +08 name*, +20 nv, ni, n streams, n streams 2, +30 streams*, +38 streams 2*, +40 u16 indices*, +48 material*
    std::vector<Submesh> subs;
    std::map<std::string, size_t> sub_by_name;
    const uint64_t nsub = p.u64(gb + 8);
    const ORef sref = bg::res(p, pg, b + 0x10);
    for (uint64_t k = 0; k < nsub; k++) {
        const uint32_t sp = bg::need(sref).first;
        const uint64_t o = sref->second + 0x88 * k, sb = p.pbase(sp) + o;
        Submesh s;
        const Ref nm = bg::need(p, sp, o + 8);
        s.name = p.pstr(nm.first, nm.second);
        s.nv = p.u32(sb + 0x20);
        s.ni = p.u32(sb + 0x24);
        const uint32_t ns = p.u32(sb + 0x28), ns2 = p.u32(sb + 0x2c);
        s.st = streams(p, bg::res(p, sp, o + 0x30), ns);
        if (ns2)
            for (const Stream &x : streams(p, bg::res(p, sp, o + 0x38), ns2)) s.st.push_back(x);
        const Ref ib = bg::need(p, sp, o + 0x40);
        const Bytes idx = slice(p, ib, 2ull * s.ni);
        for (size_t i = 0; i + 1 < idx.size(); i += 2) s.max_index = std::max<uint32_t>(s.max_index, idx[i] | idx[i + 1] << 8);
        const Ref mr = bg::need(p, sp, o + 0x48);
        s.mat = p.pstr(mr.first, mr.second);
        sub_by_name[s.name] = subs.size();
        subs.push_back(std::move(s));
    }
    // materials (0x48 bytes): +00 name*, +28 bindings* (0x28 each: +00 parameter*, +10 full texture path*), +38 count
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> mats;
    std::vector<std::string> mat_order;
    const uint64_t nmat = p.u64(gb + 0x48);
    const ORef mp = bg::res(p, pg, b + 0x50);
    for (uint64_t i = 0; i < nmat; i++) {
        const uint64_t o = bg::need(mp).second + 0x48 * i;
        const Ref nr = bg::need(p, mp->first, o);
        const std::string nm = p.pstr(nr.first, nr.second);
        const uint32_t nb = p.pu32(mp->first, o + 0x38);
        const ORef br = bg::res(p, mp->first, o + 0x28);
        std::vector<std::pair<std::string, std::string>> bl;
        for (uint32_t k = 0; k < (br ? nb : 0); k++) {
            const Ref par = bg::need(p, br->first, br->second + 0x28ull * k), full = bg::need(p, br->first, br->second + 0x28ull * k + 0x10);
            bl.emplace_back(p.pstr(par.first, par.second), p.pstr(full.first, full.second));
        }
        if (!mats.count(nm)) mat_order.push_back(nm);
        mats[nm] = std::move(bl);
    }
    for (const Submesh &s : subs) {
        if (!mats.count(s.mat)) errs.push_back("submesh " + s.name + " material " + s.mat + " missing");
        if (s.ni && s.max_index >= s.nv) errs.push_back("submesh " + s.name + " index out of range");
        for (const Stream &x : s.st)
            if (x.size != uint64_t(x.stride) * s.nv) errs.push_back(fmt("submesh %s stream slot %u size 0x%x != nv*stride", s.name.c_str(), x.slot, x.size));
    }
    // prototypes (+18 count, +20 list*): +04 submesh count, +10 name*, +18 submesh names* (0x10 each)
    std::set<std::string> protos;
    const uint64_t nproto = p.u64(gb + 0x18);
    const ORef pl = bg::res(p, pg, b + 0x20);
    for (uint64_t i = 0; i < nproto; i++) {
        const Ref r = bg::need(p, bg::need(pl).first, pl->second + 8 * i);
        const Ref nm = bg::need(p, r.first, r.second + 0x10);
        protos.insert(p.pstr(nm.first, nm.second));
        const uint32_t ns = p.pu32(r.first, r.second + 4);
        const ORef sn = bg::res(p, r.first, r.second + 0x18);
        for (uint32_t k = 0; k < ns; k++) {
            const Ref x = bg::need(p, bg::need(sn).first, sn->second + 0x10ull * k);
            const std::string name = p.pstr(x.first, x.second);
            if (!sub_by_name.count(name)) errs.push_back("prototype submesh name " + name + " unresolved");
        }
    }
    // instances (+28 count, +30 list*): +08 prototype name*, +20 submesh count, +30 submeshes* (0x50 each: +00 name*,
    // +10 stream count, +18 streams*)
    const uint64_t ninst = p.u64(gb + 0x28);
    const ORef il = bg::res(p, pg, b + 0x30);
    for (uint64_t i = 0; i < ninst; i++) {
        const Ref r = bg::need(p, bg::need(il).first, il->second + 8 * i);
        const Ref pn = bg::need(p, r.first, r.second + 8);
        const std::string proto = p.pstr(pn.first, pn.second);
        const uint32_t ns = p.pu32(r.first, r.second + 0x20);
        const ORef sr = bg::res(p, r.first, r.second + 0x30);
        if (!protos.count(proto)) errs.push_back("instance prototype " + proto + " missing");
        for (uint32_t k = 0; k < ns; k++) {
            const uint64_t so = bg::need(sr).second + 0x50ull * k;
            const Ref x = bg::need(p, sr->first, so);
            const std::string name = p.pstr(x.first, x.second);
            const uint32_t nst = p.pu32(sr->first, so + 0x10);
            const std::vector<Stream> st = nst ? streams(p, bg::res(p, sr->first, so + 0x18), nst) : std::vector<Stream>{};
            const auto f = sub_by_name.find(name);
            if (f == sub_by_name.end()) {
                errs.push_back("instsub " + name + " unresolved");
                continue;
            }
            for (const Stream &y : st)
                if (y.size != uint64_t(y.stride) * subs[f->second].nv) errs.push_back(fmt("instsub %s slot %u size mismatch", name.c_str(), y.slot));
        }
    }
    // lightmaps of this stage
    const std::string lm = "/lightmaps/" + stage + "/";
    if (std::any_of(texs.begin(), texs.end(), [&](const PakTexture &t) { return t.path.find(lm) != std::string::npos; }))
        for (const std::string &nm : mat_order)
            for (const auto &[par, full] : mats[nm])
                if (starts(par, "g_tLightMap") && full.find(lm) == std::string::npos)
                    errs.push_back("material " + nm.substr(0, 50) + " " + par + " -> " + (full.size() > 60 ? full.substr(full.size() - 60) : full) +
                                   " (not the stage lightmap)");

    // the U2 retail login order
    std::vector<std::string> lt;
    for (const auto &[lpg, off] : p.login) lt.push_back(p.item_at(lpg, off).type);
    std::vector<std::string> want;
    for (const std::string &t : U2_LOGIN)
        for (const std::string &x : lt)
            if (x == t) want.push_back(t);
    if (lt != want) errs.push_back("U2 login order " + py_list(lt));
    for (const PakItem &it : p.items_of("COLLISION_DATA_HAVOK_BG"))
        for (const std::string &x : collision_problems(p, it)) errs.push_back("collision: " + x);
    return errs;
}

}  // namespace u2imp
