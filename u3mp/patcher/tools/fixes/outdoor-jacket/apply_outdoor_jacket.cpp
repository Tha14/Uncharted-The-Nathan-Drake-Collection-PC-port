// Recipe step outdoor-jacket: the multiplayer Outdoor (Treasure Hunter) Jacket A, mp-kit-hunter-torso-jacket-a.pak in
// overlay-r3, as the hat-colour step leaves it, draws the cloth decay map instead of its colour map and has no normals
// or tangents on its two visible meshes (submeshes 1 and 3). Replaces apply_outdoor_jacket.py and
// actor-materials/repair_hunter_jacket.py.
//   * the colour map: the PS3 jacket's DXT1 colour map from mip 1 on (256x256, 9 mips) as a new PS4 texture (VRAM
//     appended, a VRAM_DESC on a new page, the texture table and the geometry texture list with one more entry), and
//     the visible material's Color1 binding pointed at it;
//   * the basis: per vertex the area-weighted face normals and UV tangents / bitangents of its triangles (no welding),
//     normal normalised, tangent made orthogonal to it and normalised, handedness from the bitangent; two vertices of
//     submesh 1 used only by zero-area triangles take the basis of their position / UV twins; vertices no triangle uses
//     get (0, 0, 1) / (1, 0, 0). Written as float16x4 streams 2 and 3 of the mesh.
// Every original page is kept byte for byte except the redirected pointers and counts. The inputs are pinned (the
// hat-colour build, the jacket before hat-colour as the donor of nothing but its check, the PS3 disc jacket) and so is
// the result.
//   u3fix outdoor-jacket --project P --ps3-disc D
// Backup of the replaced pak: work/backup-outdoor-jacket/ (with state.json).
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "ps4writer.hpp"
#include "psarc.hpp"
#include "texcodec.hpp"
#include "u3fix.hpp"

#include <array>
#include <cmath>
#include <cstring>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace u3 {
namespace actor {
uint16_t f16b(double v);
}

namespace {

const std::string NAME = "mp-kit-hunter-torso-jacket-a";
const std::string SOURCE_SHA256 = "da899e8572e1063cf86883405f64221a2a74a784bc0f536e53bbd23a59e8e701";
const std::string REPAIRED_SHA256 = "a28e8d1bf48ccd338ea7598fa67a91a2c717d539c412460872e4b08680f2c22c";
const std::string PS3_SHA256 = "756cbe1838b89ae987d07f51142b2730e0e1594862dbc2c402e6c09d41bc2529";
const std::string MATERIAL = "objects/characters/mp-kit/torso-outer/treasure-hunter/mp-kit-treasure-hunter-torso-out-jacket-a-00/default";
const std::string COLOR = "art/objects/characters/mp-kit/textures/torso-outer/treasure-hunter/mp-kit-treasure-hunter-torso-out-jacket-a-00-color.tga/8CAC6BC4B9B5E83C.ndb";
const std::string DECAY = "art/objects/multiplayer/loading-menu/loading-menu-cloth/texture/loading-menu-cloth-decay-color.tga/CCE61591FCB9F4F9.ndb";
const uint8_t FMT_NORMAL[4] = {0x02, 0x00, 0x08, 0x0a}, FMT_TANGENT[4] = {0x03, 0x00, 0x08, 0x0a};

using BlobP = std::shared_ptr<Blob>;
std::shared_ptr<PtrTarget> unowned(PtrTarget &t) { return std::shared_ptr<PtrTarget>(std::shared_ptr<PtrTarget>(), &t); }

void require(bool ok, const std::string &m) {
    if (!ok) throw Stop(m);
}

Bytes slice(const Pak &p, const Ref &r, uint64_t n) {
    const uint64_t a = p.pbase(r.first) + r.second;
    require(a + n <= p.d.size(), "Out-of-page data");
    return Bytes(p.d.begin() + std::ptrdiff_t(a), p.d.begin() + std::ptrdiff_t(a + n));
}

struct Mesh {
    Ref loc;
    uint32_t nv = 0, ni = 0;
    Ref st;   // the stream descriptors
    Bytes pos, uv, idx;
};
Mesh mesh(const Pak &p, const PakItem &g, int i) {
    const Ref s = bg::need(p, g.page, g.data_off() + 0x10);
    Mesh m;
    m.loc = {s.first, s.second + 0x88ull * uint64_t(i)};
    m.nv = p.pu32(s.first, m.loc.second + 0x20), m.ni = p.pu32(s.first, m.loc.second + 0x24);
    m.st = bg::need(p, s.first, m.loc.second + 0x30);
    m.pos = slice(p, bg::need(p, m.st.first, m.st.second + 0x10), uint64_t(m.nv) * 12);
    m.uv = slice(p, bg::need(p, m.st.first, m.st.second + 0x18 + 0x10), uint64_t(m.nv) * 4);
    m.idx = slice(p, bg::need(p, s.first, m.loc.second + 0x40), uint64_t(m.ni) * 2);
    return m;
}

using V3 = std::array<double, 3>;
V3 sub(const V3 &a, const V3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3 &a, const V3 &b) {   // a1*b2 - a2*b1, ... (each product rounded)
    const double x0 = a[1] * b[2], y0 = a[2] * b[1], x1 = a[2] * b[0], y1 = a[0] * b[2], x2 = a[0] * b[1], y2 = a[1] * b[0];
    return {x0 - y0, x1 - y1, x2 - y2};
}
// a sum of 3 as the reference's reduction made it: 0 + a + b + c, in order
double sum3(double a, double b, double c) {
    const double r = a + b;
    return r + c;
}
double norm3(const V3 &v) {
    const double a = v[0] * v[0], b = v[1] * v[1], c = v[2] * v[2];
    return std::sqrt(sum3(a, b, c));
}
double half(const Bytes &d, size_t i) {   // little-endian float16 -> double
    const uint16_t h = uint16_t(d[2 * i] | d[2 * i + 1] << 8);
    const int e = h >> 10 & 31, m = h & 1023;
    double v = e == 0 ? std::ldexp(m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(m + 1024, e - 25);
    return h & 0x8000 ? -v : v;
}

// the reconstructed basis of one mesh: normals and tangents as float16x4, the zero-area triangles
std::pair<Bytes, Bytes> basis(const Mesh &m, const std::vector<std::pair<uint32_t, uint32_t>> &dups, std::vector<size_t> &zero) {
    const size_t nv = m.nv, nt = m.ni / 3;
    std::vector<V3> P(nv);
    std::vector<std::array<double, 2>> UV(nv);
    for (size_t i = 0; i < nv; i++) {
        for (int k = 0; k < 3; k++) {
            float f;
            std::memcpy(&f, &m.pos[i * 12 + 4 * size_t(k)], 4);
            P[i][size_t(k)] = f;
        }
        UV[i] = {half(m.uv, 2 * i), half(m.uv, 2 * i + 1)};
    }
    std::vector<std::array<uint32_t, 3>> T(nt);
    std::vector<bool> ref(nv, false);
    for (size_t j = 0; j < nt; j++)
        for (int k = 0; k < 3; k++) {
            const uint32_t x = uint32_t(m.idx[(3 * j + size_t(k)) * 2] | m.idx[(3 * j + size_t(k)) * 2 + 1] << 8);
            require(x < nv, "Invalid triangle index/restart");
            T[j][size_t(k)] = x, ref[x] = true;
        }
    std::vector<V3> n(nv, V3{}), t(nv, V3{}), b(nv, V3{});
    std::vector<bool> used(nv, false);   // by a triangle with area
    for (size_t j = 0; j < nt; j++) {
        const auto &tr = T[j];
        const V3 e1 = sub(P[tr[1]], P[tr[0]]), e2 = sub(P[tr[2]], P[tr[0]]);
        const V3 face = cross(e1, e2);
        const double area = norm3(face);
        if (!(area > 1e-12)) {
            zero.push_back(j);
            continue;
        }
        const double d1[2] = {UV[tr[1]][0] - UV[tr[0]][0], UV[tr[1]][1] - UV[tr[0]][1]};
        const double d2[2] = {UV[tr[2]][0] - UV[tr[0]][0], UV[tr[2]][1] - UV[tr[0]][1]};
        const double p = d1[0] * d2[1], q = d1[1] * d2[0], det = p - q;
        require(std::fabs(det) > 1e-12, "Degenerate UV triangle");
        V3 tg, bt;
        for (int k = 0; k < 3; k++) {
            const double a1 = e1[size_t(k)] * d2[1], a2 = e2[size_t(k)] * d1[1];
            tg[size_t(k)] = (a1 - a2) / det;
            const double c1 = e2[size_t(k)] * d1[0], c2 = e1[size_t(k)] * d2[0];
            bt[size_t(k)] = (c1 - c2) / det;
        }
        for (int k = 0; k < 3; k++) tg[size_t(k)] = tg[size_t(k)] * area, bt[size_t(k)] = bt[size_t(k)] * area;
        for (uint32_t v : tr) {
            used[v] = true;
            for (int k = 0; k < 3; k++) n[v][size_t(k)] += face[size_t(k)], t[v][size_t(k)] += tg[size_t(k)], b[v][size_t(k)] += bt[size_t(k)];
        }
    }
    for (const auto &[dst, src] : dups) {
        require(dst < nv && src < nv && ref[dst] && !used[dst] && used[src] && std::memcmp(&m.pos[dst * 12], &m.pos[src * 12], 12) == 0 &&
                    std::memcmp(&m.uv[dst * 4], &m.uv[src * 4], 4) == 0,
                "Invalid orphan-vertex duplicate");
        n[dst] = n[src], t[dst] = t[src], b[dst] = b[src];
    }
    for (size_t v = 0; v < nv; v++)
        if (!ref[v]) n[v] = {0, 0, 1}, t[v] = {1, 0, 0}, b[v] = {0, 1, 0};
    for (size_t v = 0; v < nv; v++) {
        const double ln = norm3(n[v]);
        require(std::isfinite(ln) && ln > 1e-12, "Degenerate/cancelled basis or vertex without nondegenerate triangles");
        for (auto &x : n[v]) x /= ln;
    }
    for (size_t v = 0; v < nv; v++) {
        const double d = sum3(n[v][0] * t[v][0], n[v][1] * t[v][1], n[v][2] * t[v][2]);
        for (int k = 0; k < 3; k++) {
            const double s = n[v][size_t(k)] * d;
            t[v][size_t(k)] = t[v][size_t(k)] - s;
        }
    }
    for (size_t v = 0; v < nv; v++) {
        const double ln = norm3(t[v]);
        require(std::isfinite(ln) && ln > 1e-12, "Degenerate/cancelled basis or vertex without nondegenerate triangles");
        for (auto &x : t[v]) x /= ln;
    }
    Bytes nb, tb;
    auto put = [](Bytes &o, double x) {
        const uint16_t h = actor::f16b(x);
        o.push_back(uint8_t(h)), o.push_back(uint8_t(h >> 8));
    };
    for (size_t v = 0; v < nv; v++) {
        const V3 c = cross(n[v], t[v]);
        const double hd = sum3(c[0] * b[v][0], c[1] * b[v][1], c[2] * b[v][2]);
        require(std::isfinite(hd) && std::fabs(hd) > 1e-12, "Ambiguous tangent handedness");
        for (double x : n[v]) put(nb, x);
        put(nb, 1.0);
        for (double x : t[v]) put(tb, x);
        put(tb, hd < 0 ? -1.0 : 1.0);
    }
    return {nb, tb};
}

Bytes build_repair(const Bytes &source, const Bytes &ps3) {
    const Pak p(source, NAME + ".pak"), s(ps3, NAME + ".pak");
    const auto gs = p.items_of("GEOMETRY_1");
    require(gs.size() == 1, "Expected exactly one geometry");
    const PakItem g = gs[0];
    // the PS3 colour map, mip 1 on
    const PakTexture *c3 = nullptr;
    const auto t3 = s.textures();
    for (const PakTexture &t : t3)
        if (t.path.size() >= COLOR.size() + 9 && t.path.compare(t.path.size() - COLOR.size() - 9, COLOR.size() + 9, "texture1/" + COLOR) == 0) c3 = &t;
    require(c3 && c3->width == 512 && c3->height == 512 && c3->mips == 10, "Unexpected PS3 color format");
    const Bytes linear = s.vram_bytes(c3->vramOffset, c3->vramSize);
    require(linear.size() == tc::ps3_linear_size(512, 512, 10, 8), "Incomplete PS3 color mip chain");
    const Bytes tiled = tc::ps3_to_ps4_texels(Bytes(linear.begin() + 128 * 128 * 8, linear.end()), 256, 256, 9, 8);
    // the basis of the visible meshes
    std::vector<std::pair<int, std::pair<Bytes, Bytes>>> bases;
    std::vector<Mesh> meshes;
    for (int i : {1, 3}) {
        Mesh m = mesh(p, g, i);
        std::vector<size_t> zero;
        const auto dups = i == 1 ? std::vector<std::pair<uint32_t, uint32_t>>{{797, 326}, {798, 329}} : std::vector<std::pair<uint32_t, uint32_t>>{};
        bases.push_back({i, basis(m, dups, zero)});
        require(zero == (i == 1 ? std::vector<size_t>{1191, 1192} : std::vector<size_t>{}), "Unexpected degenerate triangle set");
        meshes.push_back(std::move(m));
    }
    // the visible material's Color1 binding
    const std::string mkey = MATERIAL.substr(0, MATERIAL.rfind('/')) + ":default";
    const Ref mr = bg::need(p, g.page, g.data_off() + 0x50);
    std::optional<Ref> mat;
    for (uint64_t i = 0; i < 2; i++) {
        const Ref nr = bg::need(p, mr.first, mr.second + i * 0x48);
        const std::string nm = p.pstr(nr.first, nr.second);
        if (nm.substr(0, nm.find(',')) == mkey) mat = Ref{mr.first, mr.second + i * 0x48};
    }
    require(mat.has_value(), "visible material");
    const Ref br = bg::need(p, mat->first, mat->second + 0x28);
    std::optional<Ref> binding;
    for (uint64_t i = 0; i < 4; i++) {
        const Ref nr = bg::need(p, br.first, br.second + i * 0x28);
        if (p.pstr(nr.first, nr.second) == "g_tNdFetchColor1Map") binding = Ref{br.first, br.second + i * 0x28};
    }
    require(binding.has_value(), "Color1 binding");

    PakWriter w;
    w.flags = uint16_t(p.h[0] >> 16);
    std::vector<RawPage *> pages;
    for (uint32_t pg = 0; pg < p.pages.size(); pg++) {
        std::vector<std::pair<uint64_t, size_t>> fx;
        for (const Pak::Fixup &f : p.fixups)
            if (f.src == pg) fx.push_back({f.off, f.dst});
        pages.push_back(&w.raw_page(Bytes(p.d.begin() + std::ptrdiff_t(p.pbase(pg)), p.d.begin() + std::ptrdiff_t(p.pbase(pg) + p.pages[pg].size)), fx));
    }
    w.login_page = std::make_pair(size_t(p.h[2]), size_t(p.h[3]));
    Page &extra = w.page();
    auto rp = [&](uint32_t pg) { return unowned(*pages[pg]); };
    auto clone = [&](const Ref &r, uint64_t size) {
        auto b = std::make_shared<Blob>();
        b->raw(slice(p, r, size));
        for (const Pak::Fixup &f : p.fixups)
            if (f.src == r.first && r.second <= f.off && f.off < r.second + size) {
                require(f.off + 8 <= r.second + size, "Partial pointer in cloned metadata");
                b->relocs.push_back({size_t(f.off - r.second), rp(f.dst), int64_t(p.pptr(f.src, f.off))});
            }
        return b;
    };
    auto redirect = [&](const Ref &r, const std::shared_ptr<PtrTarget> &t, int64_t toff = 0) {
        require(p.is_ptr(r.first, uint32_t(r.second)), "Expected existing pointer fixup");
        pages[r.first]->redirect(r.second, t, toff);
    };
    auto count = [&](const Ref &r, uint32_t n) { pages[r.first]->set_u32(r.second, n); };

    auto strings = pages[0]->append(std::make_shared<Blob>("jacket-strings"));
    const size_t shrt = strings->cstr(COLOR), bind_short = strings->cstr(COLOR.substr(0, COLOR.rfind('/'))),
                 full = strings->cstr(cook_prefix() + COLOR.substr(0, COLOR.size() - 4));
    Bytes vram(p.d.begin() + p.vram_base, p.d.end());
    const size_t voff = (vram.size() + 255) & ~size_t(255);
    w.vram = vram;
    w.vram.resize(voff, 0);
    w.vram.insert(w.vram.end(), tiled.begin(), tiled.end());
    auto db = std::make_shared<Blob>("jacket-color");
    db->u64(0).u64(voff).u32(uint32_t(tiled.size())).u32(sid(COLOR)).u32(1).u32(0x48);
    db->u32(0x010F0000).u32(9).u32(256).u32(256).u32(0).u32(3).ptr(db, 0x40);
    db->cstr(cook_prefix() + COLOR);
    auto desc = extra.item("VRAM_DESC", db);
    auto vt = std::make_shared<Blob>("vram-table");
    const auto vdt = p.vram_desc_table();
    vt->u32(uint32_t(p.textures().size() + 1)).u32(0);
    for (const auto &[pg, off] : vdt) {
        require(pg.has_value(), "VRAM desc table without a pointer");
        vt->ptr(rp(*pg), int64_t(off));
    }
    vt->ptr(desc->data, -0x20);
    w.vram_table_item = extra.item("VRAM_DESC_TABLE", vt, "", false);

    const auto tts = p.items_of("TEXTURE_TABLE");
    require(tts.size() == 1 && p.pu32(tts[0].page, tts[0].data_off()) == 4, "Unexpected texture table count");
    const PakItem &tt = tts[0];
    const Ref hp = bg::need(p, tt.page, tt.data_off() + 8), np_ = bg::need(p, tt.page, tt.data_off() + 16), dp = bg::need(p, tt.page, tt.data_off() + 24);
    struct Row {
        uint32_t h;
        std::shared_ptr<PtrTarget> nt;
        int64_t no;
        std::shared_ptr<PtrTarget> dt;
        int64_t doff;
    };
    std::vector<Row> rows;
    for (uint64_t i = 0; i < 4; i++) {
        const Ref nm = bg::need(p, np_.first, np_.second + 8 * i), dr = bg::need(p, dp.first, dp.second + 8 * i);
        rows.push_back({p.pu32(hp.first, hp.second + 4 * i), rp(nm.first), int64_t(nm.second), rp(dr.first), int64_t(dr.second)});
    }
    rows.push_back({sid(COLOR), strings, int64_t(shrt), desc->data, 0});
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.h < b.h; });
    auto hb = std::make_shared<Blob>("hashes"), nb = std::make_shared<Blob>("names"), descs = std::make_shared<Blob>("descriptors");
    for (const Row &r : rows) hb->u32(r.h), nb->ptr(r.nt, r.no), descs->ptr(r.dt, r.doff);
    const std::pair<uint64_t, BlobP> fields[3] = {{8, hb}, {16, nb}, {24, descs}};
    for (const auto &[field, b] : fields) {
        pages[0]->append(b);
        redirect({tt.page, tt.data_off() + field}, b);
    }
    count({tt.page, tt.data_off()}, 5);

    const Ref tr = bg::need(p, g.page, g.data_off() + 0x40);
    require(p.pu32(g.page, g.data_off() + 0x38) == 4, "Unexpected geometry texture count");
    std::optional<uint64_t> color_row;
    for (uint64_t i = 0; i < 4; i++) {
        const Ref r = bg::need(p, tr.first, tr.second + i * 0x28);
        if (p.pstr(r.first, r.second) == cook_prefix() + DECAY.substr(0, DECAY.size() - 4)) color_row = i;
    }
    require(color_row.has_value(), "old geometry color entry");
    auto gt = clone(tr, 4 * 0x28);
    auto row = clone({tr.first, tr.second + *color_row * 0x28}, 0x28);
    require(row->relocs.size() == 1 && row->relocs[0].pos == 0, "Unexpected geometry texture pointers");
    row->relocs = {{0, strings, int64_t(full)}};
    const uint32_t cs = sid(COLOR);
    std::memcpy(&row->b[8], &cs, 4);
    gt->embed(row, 1);
    pages[0]->append(gt);
    redirect({g.page, g.data_off() + 0x40}, gt);
    count({g.page, g.data_off() + 0x38}, 5);
    redirect({binding->first, binding->second + 8}, strings, int64_t(bind_short));
    redirect({binding->first, binding->second + 16}, strings, int64_t(full));

    for (size_t k = 0; k < bases.size(); k++) {
        const Mesh &c = meshes[k];
        const auto &[nrm, tan] = bases[k].second;
        auto st = clone(c.st, 2 * 0x18);
        const std::pair<const uint8_t *, const Bytes *> parts[2] = {{FMT_NORMAL, &nrm}, {FMT_TANGENT, &tan}};
        for (const auto &[fmt, payload] : parts) {
            auto fb = pages[0]->append(std::make_shared<Blob>());
            fb->raw(Bytes(fmt, fmt + 4));
            auto vb = pages[0]->append(std::make_shared<Blob>());
            vb->raw(*payload);
            st->u32((8u << 16) | 1).u32(uint32_t(payload->size())).ptr(fb).ptr(vb);
        }
        pages[0]->append(st);
        count({c.loc.first, c.loc.second + 0x28}, 4);
        redirect({c.loc.first, c.loc.second + 0x30}, st);
    }
    return w.build();
}

// the PS3 jacket from the disc actor archive
Bytes ps3_jacket(const std::string &disc) {
    Psarc A(upath(disc) / "build" / "main" / "actor33.psarc");
    const std::string want = "/" + NAME + ".pak";
    std::optional<size_t> hit;
    for (size_t i = 0; i < A.names.size(); i++) {
        std::string x = "/" + A.names[i];
        for (char &ch : x)
            if (ch == '\\') ch = '/';
        if (x.size() >= want.size() && x.compare(x.size() - want.size(), want.size(), want) == 0) hit = i;   // a later one wins
    }
    if (!hit) throw Stop("missing required jacket input: " + NAME + ".pak in the PS3 actor33.psarc");
    return A.read(*hit + 1);
}

}  // namespace

int outdoor_jacket(const std::vector<std::string> &argv) {
    const Args a(argv, {}, {"--project", "--ps3-disc"});
    const std::string W = a.need("--project") + "/work/";
    const fs::path SOURCE = upath(W + "overlay-r3/u3data/build/main/actor33/ps4/" + NAME + ".pak");
    const fs::path BDIR = upath(W + "backup-outdoor-jacket"), BACKUP = BDIR / upath(NAME + ".pak");
    if (!fs::is_regular_file(SOURCE)) throw Stop("missing required jacket input: " + utf8(SOURCE));
    const Bytes src = read_file(SOURCE);
    const std::string cur = sha256_hex(src);
    if (cur == REPAIRED_SHA256) {
        std::printf("%s already repaired\n", NAME.c_str());
        return 0;
    }
    if (cur != SOURCE_SHA256) throw Stop("unexpected active jacket pak SHA-256: " + cur);
    const Bytes ps3 = ps3_jacket(a.need("--ps3-disc"));
    if (sha256_hex(ps3) != PS3_SHA256) throw Stop("Unexpected ps3_source SHA-256: " + sha256_hex(ps3));
    const Bytes out = build_repair(src, ps3);
    const std::string h = sha256_hex(out);
    if (h != REPAIRED_SHA256) throw Stop("unexpected repaired jacket SHA-256: " + h);
    fs::create_directories(BDIR);
    if (fs::exists(BACKUP)) {
        if (sha256_hex(read_file(BACKUP)) != SOURCE_SHA256) throw Stop("existing Outdoor Jacket backup has an unexpected hash");
    } else
        copy2(SOURCE, BACKUP);
    write_file_replace(SOURCE, out);
    if (sha256_hex(read_file(SOURCE)) != REPAIRED_SHA256) throw Stop("installed jacket hash verification failed");
    const std::string state = "{\n  \"asset\": \"" + NAME + "\",\n  \"source_sha256\": \"" + SOURCE_SHA256 + "\",\n  \"repaired_sha256\": \"" + REPAIRED_SHA256 +
                              "\",\n  \"candidate_bytes\": " + std::to_string(out.size()) + ",\n  \"basis_method\": \"geometrically reconstructed basis\",\n  \"ps3_source_sha256\": \"" +
                              PS3_SHA256 + "\"\n}\n";
    write_file_replace(BDIR / "state.json", Bytes(state.begin(), state.end()));
    std::printf("%s installed %s\n", NAME.c_str(), REPAIRED_SHA256.c_str());
    return 0;
}

}  // namespace u3
