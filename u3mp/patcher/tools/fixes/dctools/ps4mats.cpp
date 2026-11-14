// dctools/ps4mats.cpp: see ps4mats.hpp.
#include "ps4mats.hpp"

#include "ps3bg.hpp"
#include "ps4actor.hpp"
#include "ps4writer.hpp"
#include "texcodec.hpp"
#include "u3fix.hpp"

#include <set>

namespace u3::mats {
namespace {

const std::string PS3_COOK_MARK = "texture1/";

std::string u8(const std::string &latin1) { return bg::utf8_of(latin1); }
uint32_t sidu(const std::string &latin1) { return sid(u8(latin1)); }
bool has(const std::string &s, const std::string &x) { return s.find(x) != std::string::npos; }
bool ends_with(const std::string &s, const std::string &e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
std::string before(const std::string &s, char c) { return s.substr(0, s.find(c)); }
std::string short_of(const std::string &full3) {
    const size_t k = full3.find(PS3_COOK_MARK);
    return k == std::string::npos ? full3 : full3.substr(k + PS3_COOK_MARK.size());
}
std::shared_ptr<PtrTarget> unowned(PtrTarget &t) { return std::shared_ptr<PtrTarget>(std::shared_ptr<PtrTarget>(), &t); }

// PS3 MATERIAL_TABLE_1 (version 0x79): materials with their texture slots (texture-table index or none) and the textures
using Mat3 = NetMat;
using Tex3 = NetTex;
void read_materials(const Pak &p, std::vector<Mat3> &out, std::vector<Tex3> &texs) {
    if (!p.ps3) throw Stop("AssertionError: expected a PS3 pak");
    const auto mts = p.items_of("MATERIAL_TABLE_1");
    if (mts.empty()) return;
    const uint32_t pg = mts[0].page;
    const uint64_t b = mts[0].data_off();
    const uint32_t ver = p.pu32(pg, b);
    if (ver != 0x79) throw Stop(u3::fmt("ValueError: unsupported MATERIAL_TABLE_1 version %#x", ver));
    const uint32_t ntex = p.pu32(pg, b + 4), nmat = p.pu32(pg, b + 0xc);
    // every pointer of the table resolves into its own page
    auto at = [&](uint64_t off) { return bg::need(p, pg, off).second; };
    const uint64_t texh = at(b + 0x18), mats = at(b + 0x20), names = at(b + 0x30);
    for (uint32_t i = 0; i < ntex; i++) texs.push_back({p.pu32(pg, texh + 4ull * i), p.pstr(pg, at(names + 4ull * i))});
    for (uint32_t i = 0; i < nmat; i++) {
        const uint64_t m = at(mats + 4ull * i);
        Mat3 rec;
        rec.name = p.pstr(pg, at(m));
        if (p.pu32(pg, m + 0x18) != sidu(rec.name)) throw Stop("AssertionError: " + rec.name);
        if (const ORef r = bg::res(p, pg, m + 0x10))
            if (const ORef node = bg::res(p, pg, r->second))
                if (const ORef bb = bg::res(p, pg, node->second + 0x10)) {
                    const uint32_t n = p.pu32(pg, bb->second + 4);
                    for (uint32_t k = 0; k < n; k++) {
                        const ORef rr = bg::res(p, pg, bb->second + 0x50 + 0x10ull * k);
                        if (rr && texh <= rr->second && rr->second < texh + 4ull * ntex) rec.textures.push_back(uint32_t((rr->second - texh) / 4));
                        else rec.textures.push_back(std::nullopt);
                    }
                }
        out.push_back(std::move(rec));
    }
}

}  // namespace

// ------------------------------------------------------------------------------------------------ net.pak
Bytes convert_net(const fs::path &ps3_path, const fs::path &ref_path, const NetOptions &o) {
    const Pak src(ps3_path), ref(ref_path);
    if (!src.ps3 || ref.ps3) throw Stop("AssertionError: expected a PS3 pak and a PS4 reference");
    std::vector<Mat3> mats;
    std::vector<Tex3> mtex;
    if (o.mats && o.mtex) mats = *o.mats, mtex = *o.mtex;
    else read_materials(src, mats, mtex);
    const std::vector<PakTexture> texs = o.textures ? *o.textures : src.textures();
    std::set<uint32_t> hashes;
    for (const PakTexture &t : texs) hashes.insert(t.hash);
    for (const Tex3 &t : mtex)
        if (!hashes.count(t.hash)) throw Stop("ValueError: material texture " + t.name + " has no VRAM_DESC");
    // how the materials use each texture: slot 0 colour, the others transparency
    std::vector<std::set<std::string>> usage(mtex.size());
    for (const Mat3 &m : mats)
        for (size_t slot = 0; slot < m.textures.size(); slot++)
            if (m.textures[slot]) usage.at(*m.textures[slot]).insert(slot == 0 ? "color" : "trans");
    std::map<uint32_t, std::set<std::string>> usage_by_hash;
    for (size_t i = 0; i < mtex.size(); i++) usage_by_hash[mtex[i].hash] = usage[i];

    PakWriter w;
    Page &p0 = w.page();
    Page &p1 = w.page();
    // ---------------- textures
    Bytes vram;
    struct TexE {
        uint32_t hash;
        std::string shrt, full;
        std::shared_ptr<PakWItem> desc, name;
    };
    std::vector<TexE> tex_entries;
    auto ttab = p0.item("TEXTURE_TABLE");
    for (const PakTexture &t : texs) {
        const std::string shrt = short_of(t.path);
        if (sidu(shrt) != t.hash) throw Stop("ValueError: hash mismatch for " + shrt);
        const auto f3 = tc::ps3_format(t);
        if (!f3 || !f3->compressed) throw Stop(u3::fmt("NotImplementedError: PS3 texture format %#x (", t.gcmRemapCtl) + shrt + ")");
        const Bytes texels = src.vram_bytes(t.vramOffset, t.vramSize);
        if (tc::ps3_linear_size(t.width, t.height, t.mips, f3->bpb) != texels.size()) throw Stop("ValueError: unexpected PS3 size for " + shrt);
        const Bytes tiled = tc::ps3_to_ps4_texels(texels, t.width, t.height, t.mips, f3->bpb);
        const auto u = usage_by_hash.find(t.hash);
        const bool trans = u != usage_by_hash.end() && u->second.count("trans"), unused = u == usage_by_hash.end() || u->second.empty();
        uint32_t fmt4;
        if (f3->kind == "DXT1") {
            const bool alphaish = has(shrt, "-trans") || has(shrt, "_trans") || has(shrt, "-alpha") || has(shrt, "_alpha") || has(shrt, "-mask");
            fmt4 = trans || (unused && alphaish) || !o.srgb_color ? 0x47 : 0x48;
        } else if (f3->kind == "DXT5")
            fmt4 = trans || !o.srgb_color ? 0x4d : 0x4e;
        else if (f3->kind == "DXT3")
            fmt4 = o.srgb_color ? 0x4b : 0x4a;
        else
            throw Stop("NotImplementedError: " + f3->kind);
        while (vram.size() % 0x100) vram.push_back(0);
        const uint64_t voff = vram.size();
        vram.insert(vram.end(), tiled.begin(), tiled.end());
        auto nameb = std::make_shared<Blob>("texname");
        nameb->cstr(u8(shrt));
        auto name_item = p0.item("RAW_DATA", nameb);
        const std::string full4 = cook_prefix() + shrt;
        auto db = std::make_shared<Blob>("vramdesc");
        db->u64(0).u64(voff).u32(tiled.size()).u32(t.hash);
        db->u32(1).u32(fmt4);
        db->u32(fmt4 == 0x48 || fmt4 == 0x4b || fmt4 == 0x4e ? 0x010f0000 : 0x01000000);
        db->u32(t.mips).u32(t.width).u32(t.height).u32(0).u32(3);
        db->ptr(db, 0x40);
        db->cstr(u8(full4));
        tex_entries.push_back({t.hash, shrt, full4, p0.item("VRAM_DESC", db), name_item});
    }
    std::vector<const TexE *> order;
    for (const TexE &e : tex_entries) order.push_back(&e);
    std::stable_sort(order.begin(), order.end(), [](const TexE *a, const TexE *b) { return a->hash < b->hash; });
    auto namesb = std::make_shared<Blob>("ttnames");
    for (const TexE *e : order) namesb->ptr(e->name->data);
    auto names_item = p0.item("RAW_DATA", namesb);
    auto hashb = std::make_shared<Blob>("tthash");
    for (const TexE *e : order) hashb->u32(e->hash);
    auto hash_item = p0.item("RAW_DATA", hashb);
    {
        auto tb = ttab->data;
        tb->u64(order.size()).ptr(hash_item->data).ptr(names_item->data);
        tb->ptr(tb, 0x20);
        for (const TexE *e : order) tb->ptr(e->desc->data);
    }
    // ---------------- the effects, verbatim from the PS4 reference
    auto [span, span_items] = actor::extract_effect_span(ref);
    p0.span(span);
    // the shader, effect hash and features of the reference's effect material (U3: 'menu-logo')
    std::string shader, effect_hash;
    std::vector<std::string> eparams;
    {
        const auto gs = ref.items_of("GEOMETRY_1");
        if (gs.empty()) throw Stop("IndexError: no GEOMETRY_1 in the reference");
        const uint32_t pg = gs[0].page;
        const uint64_t b = gs[0].data_off();
        const uint32_t n = ref.pu32(pg, b + 0x48);
        const uint64_t mo = bg::need(ref, pg, b + 0x50).second;
        bool found = false;
        for (uint32_t i = 0; i < n && !found; i++) {
            const uint64_t m = mo + 0x48ull * i;
            if (before(ref.pstr(pg, bg::need(ref, pg, m).second), ',') != o.effect_material) continue;
            shader = ref.pstr(pg, bg::need(ref, pg, m + 8).second);
            effect_hash = ref.pstr(pg, bg::need(ref, pg, m + 0x18).second);
            const uint32_t np = ref.pu32(pg, m + 0x30);
            if (const ORef pl = bg::res(ref, pg, m + 0x10))
                for (uint32_t k = 0; k < np; k++) eparams.push_back(ref.pstr(pg, bg::need(ref, pg, pl->second + 8ull * k).second));
            found = true;
        }
        if (!found) throw Stop("KeyError: '" + o.effect_material + "'");
    }
    // ---------------- geometry (materials only)
    auto strs = std::make_shared<Blob>("geostrings");
    std::map<std::string, size_t> spool;
    auto S = [&](const std::string &s) -> int64_t {
        auto it = spool.find(s);
        if (it == spool.end()) it = spool.emplace(s, strs->cstr(u8(s))).first;
        return int64_t(it->second);
    };
    auto texb = std::make_shared<Blob>("geotextures");
    for (const TexE &e : tex_entries) texb->ptr(strs, S(e.full)).u32(e.hash).u32(0x200).u32(0).i32(-1).u32(0).u32(4).u64(0);
    auto paramsb = std::make_shared<Blob>("geoparams");
    for (const std::string &prm : eparams) paramsb->ptr(strs, S(prm));
    auto bindb = std::make_shared<Blob>("geobindings"), matb = std::make_shared<Blob>("geomaterials");
    size_t nmat_out = 0;
    for (const Mat3 &m : mats) {
        std::vector<uint32_t> tl;
        for (const auto &x : m.textures)
            if (x) tl.push_back(*x);
        if (tl.empty()) continue;
        const Tex3 &color = mtex.at(tl[0]), &trans = tl.size() > 1 ? mtex.at(tl[1]) : mtex.at(tl[0]);
        const size_t boff = bindb->size();
        for (const auto &[pname, tt] : {std::pair<const char *, const Tex3 *>{"g_tColorMap", &color}, {"g_tTransparencyMap", &trans}}) {
            const TexE *e = nullptr;
            for (const TexE &x : tex_entries)
                if (x.hash == tt->hash) { e = &x; break; }
            if (!e) throw Stop("IndexError: list index out of range");
            const std::string shortname = ends_with(e->shrt, ".ndb") ? e->shrt.substr(0, e->shrt.rfind('/')) : e->shrt;
            bindb->ptr(strs, S(pname)).ptr(strs, S(shortname)).ptr(strs, S(e->full));
            bindb->i32(-1).u32(0).u64(0);
        }
        matb->ptr(strs, S(m.name)).ptr(strs, S(shader)).ptr(eparams.empty() ? nullptr : paramsb, 0);
        const auto me = o.material_effect.find(m.name);
        matb->ptr(strs, S(me != o.material_effect.end() ? me->second : effect_hash)).u64(0).ptr(bindb, int64_t(boff));
        matb->u64(eparams.size()).u64(2).u64(0);
        nmat_out++;
    }
    p0.item("RAW_DATA", strs);
    p0.item("RAW_DATA", texb);
    if (!eparams.empty()) p0.item("RAW_DATA", paramsb);
    p0.item("RAW_DATA", bindb);
    p0.item("RAW_DATA", matb);
    auto gb = std::make_shared<Blob>("geometry");
    gb->u32(0x10).u32(0);
    gb->u64(0).ptr(nullptr);   // submeshes
    gb->u64(0).ptr(nullptr);   // prototypes
    gb->u64(0).ptr(nullptr);   // instances
    gb->u64(tex_entries.size()).ptr(texb);
    gb->u64(nmat_out).ptr(matb);
    if (o.materials_written) *o.materials_written = nmat_out;
    gb->u64(0).u64(0);
    auto geo = p0.item("GEOMETRY_1", gb);
    // ---------------- page 1 + login (retail menu.pak order: the EFFECT_TABLE before GEOMETRY_1)
    auto span_item = std::make_shared<PakWItem>();
    span_item->type = "EFFECT_TABLE";
    span_item->span_ref = span;
    for (const PakItem &i : span_items)
        if (i.type == "EFFECT_TABLE") {
            span_item->span_pos = i.off - span_items[0].off;
            break;
        }
    std::vector<std::shared_ptr<PakWItem>> descs;
    for (const TexE &e : tex_entries) descs.push_back(e.desc);
    make_login_and_vram_page(w, p1, {span_item, ttab, geo}, descs);
    w.vram = vram;
    Bytes data = w.build();
    for (const auto &pg : w.pages)   // break the self-reference cycles
        for (auto &part : pg->parts)
            if (auto *it = std::get_if<std::shared_ptr<PakWItem>>(&part)) (*it)->data->relocs.clear();
    return data;
}

// ------------------------------------------------------------------------------------------------ weapon skins
Bytes wpnmods(const fs::path &ps4, const fs::path &ps3, const std::string &src_skin, const std::vector<std::string> &skins, int max_tex,
              int stream_flags) {
    const Pak R(ps4), Sp(ps3);
    if (R.ps3 || !Sp.ps3) throw Stop("AssertionError: expected a PS4 pak and a PS3 pak");
    if (R.pages.size() != 2) throw Stop("AssertionError: expects a 2-page retail pak");
    const auto gs = R.items_of("GEOMETRY_1"), tts = R.items_of("TEXTURE_TABLE"), ets = R.items_of("EFFECT_TABLE");
    if (gs.empty() || tts.empty() || ets.empty()) throw Stop("IndexError: list index out of range");
    const PakItem &g = gs[0], &tt = tts[0], &et = ets[0];
    const uint64_t gb = g.data_off();
    if (g.page != 0 || tt.page != 0 || et.page != 0) throw Stop("AssertionError: tables not on page 0");
    auto rptr = [&](uint32_t pg, uint64_t off) { return bg::need(R, pg, off); };
    auto rstr = [&](uint32_t pg, uint64_t off) { const Ref r = rptr(pg, off); return R.pstr(r.first, r.second); };

    // ---- retail tables
    const uint32_t nmat = R.pu32(0, gb + 0x48), ntex = R.pu32(0, gb + 0x38);
    const auto [mpg, moff] = rptr(0, gb + 0x50);
    const auto [tpg, toff] = rptr(0, gb + 0x40);
    struct M {
        uint64_t off;
        std::string name;
    };
    std::vector<M> mats;
    for (uint32_t i = 0; i < nmat; i++) mats.push_back({moff + 0x48ull * i, rstr(mpg, moff + 0x48ull * i)});
    std::vector<size_t> srcs;
    for (size_t i = 0; i < mats.size(); i++)
        if (ends_with(before(mats[i].name, ','), ":" + src_skin)) srcs.push_back(i);
    if (srcs.size() != 1) throw Stop("AssertionError: sibling '" + src_skin + "' not found");
    const size_t src_idx = srcs[0];
    const M &src = mats[src_idx];
    const std::string head = before(src.name, ',');
    const std::string base_dir = head.substr(0, head.rfind(':')), shader = src.name.substr(src.name.rfind(',') + 1);
    std::set<std::string> existing;
    for (const M &m : mats) existing.insert(before(m.name, ','));
    const uint32_t nbind = R.pu32(mpg, src.off + 0x38);
    const auto [bpg, boff] = rptr(mpg, src.off + 0x28);
    struct B {
        uint64_t off;
        std::string param, shrt, full;
    };
    std::vector<B> binds;
    for (uint32_t k = 0; k < nbind; k++) {
        const uint64_t o = boff + 0x28ull * k;
        binds.push_back({o, rstr(bpg, o), rstr(bpg, o + 8), rstr(bpg, o + 0x10)});
    }
    const auto c1 = std::find_if(binds.begin(), binds.end(), [](const B &b) { return b.param == "g_tNdFetchColor1Map"; });
    if (c1 == binds.end()) throw Stop("AssertionError: sibling has no Color1 binding");
    const std::string src_color_full = c1->full;
    std::vector<bool> swap;   // Color1 (+ Overlay2)
    for (const B &b : binds) swap.push_back(b.full == src_color_full);
    std::vector<uint64_t> gtex;
    for (uint32_t i = 0; i < ntex; i++) gtex.push_back(toff + 0x28ull * i);
    std::optional<uint64_t> src_gtex;
    for (uint64_t o : gtex)
        if (rstr(tpg, o) == src_color_full) { src_gtex = o; break; }
    if (!src_gtex) throw Stop("AssertionError: sibling colour texture entry not found");
    const uint64_t n_tt = R.u64(R.pbase(0) + tt.data_off());
    const auto [hpg, hoff] = rptr(0, tt.data_off() + 8);
    const auto [npg, noff] = rptr(0, tt.data_off() + 0x10);
    const auto [dpg, doff] = rptr(0, tt.data_off() + 0x18);
    struct Row {
        uint32_t hash;
        ORef name, desc;
        int nw = -1;   // >= 0: the new texture
    };
    std::vector<Row> rows;
    for (uint64_t i = 0; i < n_tt; i++) rows.push_back({R.pu32(hpg, hoff + 4 * i), bg::res(R, npg, noff + 8 * i), bg::res(R, dpg, doff + 8 * i)});
    const uint32_t n_fx = R.pu32(0, et.data_off());
    const auto [epg, eoff] = rptr(0, et.data_off() + 8);
    if (n_fx != nmat) throw Stop(u3::fmt("AssertionError: effect entries %u != materials %u", n_fx, nmat));

    // ---- PS3 skin textures
    std::vector<Mat3> ms3;
    std::vector<Tex3> tx3;
    read_materials(Sp, ms3, tx3);
    std::vector<std::pair<std::string, PakTexture>> tex3;   // by path (dict: the first position, the last value)
    for (const PakTexture &t : Sp.textures()) {
        auto it = std::find_if(tex3.begin(), tex3.end(), [&](const auto &x) { return x.first == t.path; });
        if (it == tex3.end()) tex3.push_back({t.path, t});
        else it->second = t;
    }
    struct New {
        std::string skin, shrt, name, full_noext, short_noext;
        uint32_t hash, fmt, w, h, mips;
        Bytes tiled;
        std::shared_ptr<PakWItem> desc;
    };
    std::vector<New> nw;
    for (const std::string &skin : skins) {
        const Mat3 *m3 = nullptr;
        for (const Mat3 &m : ms3)
            if (m.name == base_dir + "/" + skin) { m3 = &m; break; }
        if (!m3) throw Stop("AssertionError: PS3 material " + base_dir + "/" + skin + " not found");
        if (existing.count(base_dir + ":" + skin)) continue;   // retail already has it
        std::vector<std::string> cols;
        for (const auto &t : m3->textures)
            if (t && !tx3.at(*t).name.empty() && has(tx3.at(*t).name, "-color")) cols.push_back(tx3.at(*t).name);
        if (cols.empty()) throw Stop("AssertionError: no colour texture for " + skin);
        const PakTexture *tp = nullptr;
        for (const auto &[path, x] : tex3)
            if (ends_with(path, cols[0])) { tp = &x; break; }
        if (!tp) throw Stop("IndexError: list index out of range");
        const PakTexture &t = *tp;
        const std::string shrt = short_of(t.path);
        const auto f3 = tc::ps3_format(t);
        if (!f3) throw Stop("TypeError: cannot unpack non-iterable NoneType object");
        Bytes texels = Sp.vram_bytes(t.vramOffset, t.vramSize);
        uint32_t tw = t.width, th = t.height, tm = t.mips;
        while (max_tex && std::max(tw, th) > uint32_t(max_tex) && tm > 1) {
            const auto [bw, bh] = tc::blocks(tw, th, true);
            texels.erase(texels.begin(), texels.begin() + std::ptrdiff_t(std::min<uint64_t>(uint64_t(bw) * bh * f3->bpb, texels.size())));
            tw = std::max(1u, tw >> 1);
            th = std::max(1u, th >> 1);
            tm--;
        }
        New n;
        n.tiled = tc::ps3_to_ps4_texels(texels, tw, th, tm, f3->bpb);
        if (f3->kind == "DXT1") n.fmt = 0x48;
        else if (f3->kind == "DXT5") n.fmt = 0x4e;
        else if (f3->kind == "DXT3") n.fmt = 0x4b;
        else throw Stop("KeyError: '" + f3->kind + "'");
        if (sidu(shrt) != t.hash) throw Stop("AssertionError: texture hash scheme mismatch for " + shrt);
        const std::string key = u8(base_dir + ":" + skin);
        const std::string hx = md5_hex(Bytes(key.begin(), key.end()));
        const std::string guid = hx.substr(0, 8) + "-" + hx.substr(8, 4) + "-" + hx.substr(12, 4) + "-" + hx.substr(16, 4) + "-" + hx.substr(20, 12);
        n.skin = skin;
        n.shrt = shrt;
        n.hash = t.hash;
        n.w = tw;
        n.h = th;
        n.mips = tm;
        n.name = base_dir + ":" + skin + "," + guid + "," + shader;
        nw.push_back(std::move(n));
    }
    if (nw.empty()) throw Stop("wpnmods: nothing to add");

    // ---- build
    PakWriter w;
    w.flags = uint16_t(R.h[0] >> 16);
    std::vector<std::pair<uint64_t, size_t>> fx0, fx1;
    for (const auto &f : R.fixups)
        if (f.src < 2) (f.src == 0 ? fx0 : fx1).push_back({f.off, f.dst});
    RawPage &pg0 = w.raw_page(Bytes(R.d.begin() + std::ptrdiff_t(R.pbase(0)), R.d.begin() + std::ptrdiff_t(R.pbase(0) + R.pages[0].size)), fx0);
    RawPage &pg1 = w.raw_page(Bytes(R.d.begin() + std::ptrdiff_t(R.pbase(1)), R.d.begin() + std::ptrdiff_t(R.pbase(1) + R.pages[1].size)), fx1);
    Page &pg2 = w.page();
    w.login_page = std::make_pair(size_t(R.h[2]), size_t(R.h[3]));
    const std::shared_ptr<PtrTarget> rp[2] = {unowned(pg0), unowned(pg1)};
    auto P = [&](Blob &blob, const ORef &loc) {
        if (loc) blob.ptr(rp[loc->first], int64_t(loc->second));
        else blob.u64(0);
    };
    // retail bytes [off, off + size) of page pg, pointer fields re-pointed
    auto copy_words = [&](Blob &blob, uint32_t pg, uint64_t off, uint64_t size) {
        for (uint64_t o = off; o < off + size;) {
            if (R.is_ptr(pg, uint32_t(o))) {
                P(blob, bg::res(R, pg, o));
                o += 8;
            } else {
                const uint64_t a = R.pbase(pg) + o;
                blob.raw(Bytes(R.d.begin() + std::ptrdiff_t(a), R.d.begin() + std::ptrdiff_t(a + 4)));
                o += 4;
            }
        }
    };
    auto strs = std::make_shared<Blob>("wpnstrings");
    std::map<std::string, size_t> sidx;
    auto S_ = [&](const std::string &s) -> int64_t {
        auto it = sidx.find(s);
        if (it == sidx.end()) it = sidx.emplace(s, strs->cstr(u8(s))).first;
        return int64_t(it->second);
    };
    // VRAM: retail + new
    Bytes vram(R.d.begin() + R.vram_base, R.d.begin() + R.vram_base + R.h[8]);
    std::vector<std::shared_ptr<PakWItem>> descs;
    for (New &n : nw) {
        while (vram.size() % 0x100) vram.push_back(0);
        const uint64_t voff = vram.size();
        vram.insert(vram.end(), n.tiled.begin(), n.tiled.end());
        auto db = std::make_shared<Blob>("vramdesc");
        const std::string full4 = cook_prefix() + n.shrt;
        db->u64(0).u64(voff).u32(n.tiled.size()).u32(n.hash);
        db->u32(1).u32(n.fmt).u32(n.fmt == 0x48 || n.fmt == 0x4b || n.fmt == 0x4e ? 0x010f0000 : 0x01000000);
        db->u32(n.mips).u32(n.w).u32(n.h).u32(0).u32(uint32_t(stream_flags));
        db->ptr(db, 0x40);
        db->cstr(u8(full4));
        n.desc = pg2.item("VRAM_DESC", db);
        n.full_noext = ends_with(full4, ".ndb") ? full4.substr(0, full4.size() - 4) : full4;
        n.short_noext = ends_with(n.shrt, ".ndb") ? n.shrt.substr(0, n.shrt.rfind('/')) : n.shrt;
        descs.push_back(n.desc);
    }
    w.vram = vram;
    // the VRAM desc table, appended to page 1 with an unlisted ResItem header like retail
    {
        const uint32_t vpg = R.h[9], voff = R.h[10];
        const uint64_t vdata = voff + 0x20;
        const uint32_t nv = R.pu32(vpg, vdata);
        auto vt = std::make_shared<Blob>("vramtable");
        const uint64_t ha = R.pbase(vpg) + voff;
        vt->raw(Bytes(R.d.begin() + std::ptrdiff_t(ha), R.d.begin() + std::ptrdiff_t(ha + 0x10)));
        vt->u32(8 + 8 * (nv + descs.size()));
        vt->raw(Bytes(R.d.begin() + std::ptrdiff_t(ha + 0x14), R.d.begin() + std::ptrdiff_t(ha + 0x20)));
        vt->u32(nv + descs.size()).u32(0);
        for (uint32_t k = 0; k < nv; k++) P(*vt, bg::res(R, vpg, vdata + 8 + 8ull * k));
        for (const auto &it : descs) vt->ptr(it->data, -0x20);
        pg1.append(vt);
        w.vram_table_blob = vt;
    }
    // TEXTURE_TABLE: merged, sorted by hash
    for (size_t i = 0; i < nw.size(); i++) rows.push_back({nw[i].hash, std::nullopt, std::nullopt, int(i)});
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.hash < b.hash; });
    auto hb = std::make_shared<Blob>("tthash"), nb = std::make_shared<Blob>("ttnames"), dbl = std::make_shared<Blob>("ttdesc");
    for (const Row &r : rows) {
        hb->u32(r.hash);
        if (r.nw < 0) {
            P(*nb, r.name);
            P(*dbl, r.desc);
        } else {
            nb->ptr(strs, S_(nw[size_t(r.nw)].shrt));
            dbl->ptr(nw[size_t(r.nw)].desc->data);   // the TEXTURE_TABLE points at the desc data
        }
    }
    // GEOMETRY_1 texture entries: retail + copies of the sibling colour entry
    auto gt = std::make_shared<Blob>("geotex");
    for (uint64_t o : gtex) copy_words(*gt, tpg, o, 0x28);
    for (const New &n : nw) {
        const size_t st = gt->size();
        copy_words(*gt, tpg, *src_gtex, 0x28);
        gt->relocs.erase(std::remove_if(gt->relocs.begin(), gt->relocs.end(), [&](const Blob::Reloc &r) { return r.pos == st; }), gt->relocs.end());
        gt->relocs.push_back({st, strs, S_(n.full_noext)});
        for (int k = 0; k < 4; k++) gt->b[st + 8 + size_t(k)] = uint8_t(n.hash >> (8 * k));
    }
    // materials: retail entries + aliases with private binding arrays
    auto mb = std::make_shared<Blob>("materials");
    std::vector<std::shared_ptr<Blob>> bblobs;
    for (const M &m : mats) copy_words(*mb, mpg, m.off, 0x48);
    for (const New &n : nw) {
        auto bb = std::make_shared<Blob>("bindings-" + n.skin);
        for (size_t k = 0; k < binds.size(); k++) {
            const size_t st = bb->size();
            copy_words(*bb, bpg, binds[k].off, 0x28);
            if (swap[k]) {
                bb->relocs.erase(std::remove_if(bb->relocs.begin(), bb->relocs.end(), [&](const Blob::Reloc &r) { return r.pos == st + 8 || r.pos == st + 0x10; }),
                                 bb->relocs.end());
                bb->relocs.push_back({st + 8, strs, S_(n.short_noext)});
                bb->relocs.push_back({st + 0x10, strs, S_(n.full_noext)});
                for (int q = 0; q < 4; q++) bb->b[st + 0x18 + size_t(q)] = 0xff;
                for (int q = 0; q < 4; q++) bb->b[st + 0x1c + size_t(q)] = 0;
            }
        }
        bblobs.push_back(bb);
        const size_t st = mb->size();
        copy_words(*mb, mpg, src.off, 0x48);
        mb->relocs.erase(std::remove_if(mb->relocs.begin(), mb->relocs.end(), [&](const Blob::Reloc &r) { return r.pos == st || r.pos == st + 0x28; }), mb->relocs.end());
        mb->relocs.push_back({st, strs, S_(n.name)});
        mb->relocs.push_back({st + 0x28, bb, 0});
    }
    // effect table entries: retail + copies of the sibling's entry
    auto eb = std::make_shared<Blob>("fxentries");
    for (uint32_t i = 0; i < n_fx; i++) copy_words(*eb, epg, eoff + 0x18ull * i, 0x18);
    for (size_t i = 0; i < nw.size(); i++) copy_words(*eb, epg, eoff + 0x18ull * src_idx, 0x18);
    for (const auto &b : {strs, hb, nb, dbl, gt, mb, eb}) pg2.item("RAW_DATA", b);
    for (const auto &b : bblobs) pg2.item("RAW_DATA", b);
    // the retail page-0 fields
    pg0.set_u64(gb + 0x38, ntex + nw.size());
    pg0.redirect(gb + 0x40, gt);
    pg0.set_u64(gb + 0x48, nmat + nw.size());
    pg0.redirect(gb + 0x50, mb);
    pg0.set_u64(tt.data_off(), rows.size());
    pg0.redirect(tt.data_off() + 8, hb);
    pg0.redirect(tt.data_off() + 0x10, nb);
    pg0.redirect(tt.data_off() + 0x18, dbl);
    pg0.set_u32(et.data_off(), uint32_t(n_fx + nw.size()));
    pg0.redirect(et.data_off() + 8, eb);
    Bytes out = w.build();
    for (const auto &pg : w.pages)   // break the self-reference cycles
        for (auto &part : pg->parts)
            if (auto *it = std::get_if<std::shared_ptr<PakWItem>>(&part)) (*it)->data->relocs.clear();
    return out;
}

}  // namespace u3::mats
