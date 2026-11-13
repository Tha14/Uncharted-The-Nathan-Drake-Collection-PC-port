// Recipe step map-colors-lightmap: clear the "own lightmap UVs" flag (instance submesh +0x20 bit 0x100) of instance
// submeshes that hold triangles whose lightmap UVs (stream slot 5) are all zero. Replaces apply_mapcolor.py and its lib.py.
// Cause: the stage converter merges up to 20 PS3 blocks per submesh and sets the flag if ANY block has lightmap UVs;
// blocks without them then sample lightmap texel (0, 0) instead of their vertex light (Airstrip: a black floor). Same
// size, 4 bytes per instance submesh; those fall back to vertex light, and their few UV-carrying triangles lose the
// lightmap detail (reported as lost_lm_m2, the world-space area, next to zero_m2).
//   u3fix map-colors --project P [--apply | --revert] [--root DIR] [--backup DIR] [--only a.pak,b.pak]
// Dry run lists every change. --apply backs each pak up to work/backup-mapcolor (replacing a backup of another build:
// the file still has edits pending, so it is the pre-fix state) and writes backup-mapcolor/manifest.json (an earlier
// entry is kept while it still describes the overlay file). --revert restores the backups. Level paks of at least
// 200000 bytes, no ingame12.
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "u3fix.hpp"

#include <cmath>
#include <cstring>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace u3 {
namespace {

// numpy's float64 sum of a contiguous array: blocks of 8 partial sums up to 128 values, halves above
double pairwise(const double *a, size_t n) {
    if (n < 8) {
        double r = 0.;
        for (size_t i = 0; i < n; i++) r += a[i];
        return r;
    }
    if (n <= 128) {
        double r[8];
        for (int j = 0; j < 8; j++) r[j] = a[j];
        size_t i = 8;
        for (; i < n - n % 8; i += 8)
            for (int j = 0; j < 8; j++) r[j] += a[i + size_t(j)];
        double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; i++) res += a[i];
        return res;
    }
    size_t n2 = n / 2;
    n2 -= n2 % 8;
    return pairwise(a, n2) + pairwise(a + n2, n - n2);
}
// numpy's add.reduce of a float64 array: 0 plus the pairwise sum of all values
double np_sum(const std::vector<double> &v) {
    return 0.0 + pairwise(v.data(), v.size());
}
double half(uint16_t h) {
    const int e = h >> 10 & 31, m = h & 1023;
    const double v = e == 0 ? std::ldexp(m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(m + 1024, e - 25);
    return h & 0x8000 ? -v : v;
}

struct Sub {
    std::string name, mat;
    uint32_t nv = 0, ni = 0, nstreams = 0;
    uint64_t ib = 0;
    Ref st;
};
std::map<std::string, Sub> subs(const Pak &q) {   // by name, a later one wins
    std::map<std::string, Sub> out;
    const PakItem g = q.items_of("GEOMETRY_1").at(0);
    const uint64_t b = q.pbase(g.page) + g.data_off();
    const uint32_t ns = rd32le(q.d, b + 8);
    const Ref tr = bg::need(q, g.page, g.data_off() + 0x10);
    for (uint32_t i = 0; i < ns; i++) {
        const uint64_t o = tr.second + 0x88ull * i, pb = q.pbase(tr.first) + o;
        Sub s;
        if (const ORef nr = bg::res(q, tr.first, o + 8)) s.name = q.pstr(nr->first, nr->second);
        if (q.is_ptr(tr.first, uint32_t(o + 0x48))) {
            const ORef mr = bg::res(q, tr.first, o + 0x48);
            if (mr) s.mat = q.pstr(mr->first, mr->second);
        }
        s.nv = rd32le(q.d, pb + 0x20), s.ni = rd32le(q.d, pb + 0x24), s.nstreams = rd32le(q.d, pb + 0x28);
        s.st = bg::need(q, tr.first, o + 0x30);
        const Ref ia = bg::need(q, tr.first, o + 0x40);
        s.ib = q.pbase(ia.first) + ia.second;
        out[s.name] = s;
    }
    return out;
}
// the stream of a slot in a stream table: (rows, components, file offset, type)
struct Stream {
    size_t rows = 0;
    int comps = 0, ty = 0;
    uint64_t base = 0;
};
std::map<int, Stream> stream_table(const Pak &q, const Ref &st, uint32_t n) {
    std::map<int, Stream> out;
    for (uint32_t k = 0; k < n; k++) {
        const uint64_t so = st.second + 0x18ull * k;
        const Ref f = bg::need(q, st.first, so + 8);
        const uint64_t fb = q.pbase(f.first) + f.second;
        const int slot = q.d.at(fb), sz = q.d.at(fb + 2), ty = q.d.at(fb + 3);
        const Ref da = bg::need(q, st.first, so + 16);
        const uint32_t L = q.pu32(st.first, so + 4);
        Stream s;
        s.ty = ty, s.base = q.pbase(da.first) + da.second;
        if (ty == 6) s.comps = 3;
        else if (ty == 0x22) s.comps = 2;
        else if (ty == 0x0a) s.comps = 4;
        else throw Stop(u3::fmt("KeyError: %d", ty));
        if (!sz) throw Stop("ZeroDivisionError: stream element size 0");
        s.rows = L / uint32_t(sz);
        out[slot] = s;
    }
    return out;
}

struct Edit {
    uint64_t off;
    uint32_t flags;
    size_t inst;
    std::string sub, mat;
    double zero_m2, lost_lm_m2;
};

std::vector<Edit> plan(const Pak &q) {
    std::vector<Edit> out;
    if (q.items_of("GEOMETRY_1").empty()) return out;
    const auto sb = subs(q);
    const PakItem g = q.items_of("GEOMETRY_1").at(0);
    const uint64_t b = q.pbase(g.page) + g.data_off();
    const uint32_t ni = rd32le(q.d, b + 0x28);
    if (!ni) return out;
    const Ref pp = bg::need(q, g.page, g.data_off() + 0x30);
    for (uint32_t i = 0; i < ni; i++) {
        const Ref t = bg::need(q, pp.first, pp.second + 8ull * i);
        const uint64_t a = q.pbase(t.first) + t.second;
        double M[16];
        for (int k = 0; k < 16; k++) {
            float f;
            std::memcpy(&f, &q.d.at(a + 0x80 + 4ull * uint64_t(k)), 4);
            M[k] = f;
        }
        const uint32_t ns = rd32le(q.d, a + 0x20);
        if (!ns) continue;
        const Ref r = bg::need(q, t.first, t.second + 0x30);
        for (uint32_t k = 0; k < ns; k++) {
            const uint64_t o = r.second + 0x50ull * k, xb = q.pbase(r.first) + o;
            const Ref nr = bg::need(q, r.first, o);
            const std::string name = q.pstr(nr.first, nr.second);
            const uint32_t nst = rd32le(q.d, xb + 0x10), flags = rd32le(q.d, xb + 0x20);
            std::map<int, Stream> st;
            if (const ORef sr = bg::res(q, r.first, o + 0x18)) st = stream_table(q, *sr, nst);
            if (!(flags & 0x100) || !st.count(5)) continue;
            auto si = sb.find(name);
            if (si == sb.end() || st[5].rows < si->second.nv) continue;
            const Sub &s = si->second;
            const Stream &lm = st[5];
            auto lmzero = [&](uint32_t v) {   // max over the components == 0
                double mx = -INFINITY;
                bool nan = false;
                for (int c = 0; c < lm.comps; c++) {
                    const uint64_t p = lm.base + (uint64_t(v) * uint64_t(lm.comps) + uint64_t(c)) * 2;
                    const double x = half(uint16_t(q.d.at(p) | q.d.at(p + 1) << 8));
                    nan = nan || std::isnan(x);
                    mx = std::max(mx, x);
                }
                return !nan && mx == 0;
            };
            const uint32_t ntri = s.ni / 3;
            std::vector<std::array<uint32_t, 3>> ix(ntri);
            std::vector<bool> tz(ntri);
            bool any = false;
            for (uint32_t j = 0; j < ntri; j++) {
                bool z = true;
                for (int c = 0; c < 3; c++) {
                    const uint64_t p = s.ib + (3ull * j + uint64_t(c)) * 2;
                    ix[j][size_t(c)] = uint32_t(q.d.at(p) | q.d.at(p + 1) << 8);
                    z = z && lmzero(ix[j][size_t(c)]);
                }
                tz[j] = z, any = any || z;
            }
            if (!any) continue;
            const auto ss = stream_table(q, s.st, s.nstreams);
            if (!ss.count(0)) throw Stop("KeyError: 0");
            const Stream &ps = ss.at(0);
            auto world = [&](uint32_t v) {   // [x y z] @ M[:3,:3] + M[3,:3], each output a chain of fused multiply-adds
                std::array<double, 3> w{};
                double p[3];
                for (int c = 0; c < 3; c++) {
                    float f;
                    std::memcpy(&f, &q.d.at(ps.base + (uint64_t(v) * 3 + uint64_t(c)) * 4), 4);
                    p[c] = f;
                }
                for (int j = 0; j < 3; j++) {
                    double acc = p[0] * M[j];
                    acc = std::fma(p[1], M[4 + j], acc);
                    acc = std::fma(p[2], M[8 + j], acc);
                    w[size_t(j)] = acc + M[12 + j];
                }
                return w;
            };
            std::vector<double> zs, ls;
            for (uint32_t j = 0; j < ntri; j++) {
                const auto t0 = world(ix[j][0]), t1 = world(ix[j][1]), t2 = world(ix[j][2]);
                double e1[3], e2[3];
                for (int c = 0; c < 3; c++) e1[c] = t1[size_t(c)] - t0[size_t(c)], e2[c] = t2[size_t(c)] - t0[size_t(c)];
                const double x0 = e1[1] * e2[2], y0 = e1[2] * e2[1], x1 = e1[2] * e2[0], y1 = e1[0] * e2[2], x2 = e1[0] * e2[1], y2 = e1[1] * e2[0];
                const double c0 = x0 - y0, c1 = x1 - y1, c2 = x2 - y2;
                const double s0 = c0 * c0, s1 = c1 * c1, s2 = c2 * c2;
                const double s01 = s0 + s1;
                const double ar = std::sqrt(s01 + s2) / 2;   // the norm: ((x^2 + y^2) + z^2)
                (tz[j] ? zs : ls).push_back(ar);
            }
            const uint64_t off = q.pbase(r.first) + r.second + 0x50ull * k + 0x20;
            if (rd32le(q.d, off) != flags) throw Stop("AssertionError: instance submesh flags");
            out.push_back({off, flags, i, name, s.mat.substr(0, s.mat.find(',')), np_sum(zs), np_sum(ls)});
        }
    }
    return out;
}

json::Value edit_json(const Edit &e) {
    json::Value v = json::Value::object();
    auto num = [](double x) {
        json::Value r;
        r.kind = json::Value::Float;
        r.f = x;
        return r;
    };
    v.fields.push_back({"off", json::Value::integer((long long)e.off)});
    v.fields.push_back({"flags", json::Value::integer(e.flags)});
    v.fields.push_back({"inst", json::Value::integer((long long)e.inst)});
    v.fields.push_back({"sub", json::Value::text(e.sub)});
    v.fields.push_back({"mat", json::Value::text(e.mat)});
    v.fields.push_back({"zero_m2", num(e.zero_m2)});
    v.fields.push_back({"lost_lm_m2", num(e.lost_lm_m2)});
    return v;
}

}  // namespace

int map_colors(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--revert"}, {"--project", "--root", "--backup", "--only"});
    const std::string R = a.need("--project") + "/";
    const std::string root = a.get("--root", R + "work/overlay-r3/u3data/build/main/pak23/ps4/"), backup = a.get("--backup", R + "work/backup-mapcolor");
    const fs::path MAN = upath(backup) / "manifest.json";
    if (a.has("--revert")) {
        const json::Value m = read_json(MAN);
        for (const auto &[f, e] : m.fields) {
            const Bytes b = read_file(upath(backup) / upath(f));
            std::string before;
            for (const auto &[k, v] : e.fields)
                if (k == "md5_before") before = v.s;
            if (md5_hex(b) != before) throw Stop("AssertionError: " + f);
            write_file_replace(upath(root) / upath(f), b);
            std::printf("restored %s\n", f.c_str());
        }
        return 0;
    }
    std::vector<std::string> fs_;
    for (const auto &e : fs::directory_iterator(upath(root))) {
        const std::string n = utf8(e.path().filename());
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".pak") == 0 && !(n.size() > 13 && n.compare(n.size() - 13, 13, "-ingame12.pak") == 0) &&
            fs::file_size(e.path()) >= 200000)
            fs_.push_back(n);
    }
    std::sort(fs_.begin(), fs_.end());
    if (!a.get("--only", "").empty()) {
        std::set<std::string> only;
        const std::string o = a.get("--only", "");
        for (size_t s = 0, k; s <= o.size(); s = k + 1) {
            k = o.find(',', s);
            if (k == std::string::npos) k = o.size();
            only.insert(o.substr(s, k - s));
        }
        std::vector<std::string> keep;
        for (const auto &n : fs_)
            if (only.count(n)) keep.push_back(n);
        fs_ = keep;
    }
    json::Value man = json::Value::object();
    for (const std::string &n : fs_) {
        const fs::path f = upath(root) / upath(n);
        Bytes d = read_file(f);
        const std::vector<Edit> L = plan(Pak(d, n));
        if (L.empty()) continue;
        double zs = 0, ls = 0;   // Python's sum() of the per-edit values, in order
        for (const Edit &e : L) zs += e.zero_m2;
        for (const Edit &e : L) ls += e.lost_lm_m2;
        std::printf("%s: %zu instsubs, zero-UV %.0f m2, lightmap detail lost %.0f m2\n", n.c_str(), L.size(), zs, ls);
        for (const Edit &e : L)
            std::printf("   inst %zu %s @%s %s->%s %s zero %.0f lost %.0f\n", e.inst, e.sub.c_str(), pyhex(e.off).c_str(), pyhex(e.flags).c_str(), pyhex(e.flags & ~0x100u).c_str(),
                        e.mat.c_str(), e.zero_m2, e.lost_lm_m2);
        if (!a.has("--apply")) continue;
        fs::create_directories(upath(backup));
        const fs::path bk = upath(backup) / upath(n);
        if (!fs::exists(bk) || md5_hex(read_file(bk)) != md5_hex(d)) fs::copy_file(f, bk, fs::copy_options::overwrite_existing);
        json::Value e = json::Value::object();
        e.fields.push_back({"md5_before", json::Value::text(md5_hex(read_file(bk)))});
        json::Value ed;
        ed.kind = json::Value::Array;
        for (const Edit &x : L) ed.items.push_back(edit_json(x));
        e.fields.push_back({"edits", ed});
        for (const Edit &x : L) wr32le(&d[x.off], x.flags & ~0x100u);
        write_file_replace(f, d);
        e.fields.push_back({"md5_after", json::Value::text(md5_hex(d))});
        man.fields.push_back({n, e});
    }
    if (a.has("--apply")) {
        if (fs::exists(MAN)) {   // an earlier entry stays while it still describes the overlay file
            const json::Value old = read_json(MAN);
            for (const auto &[k, v] : old.fields) {
                bool have = false;
                for (const auto &x : man.fields) have = have || x.first == k;
                if (have || !fs::exists(upath(root) / upath(k))) continue;
                std::string after;
                for (const auto &[kk, vv] : v.fields)
                    if (kk == "md5_after") after = vv.s;
                if (md5_hex(read_file(upath(root) / upath(k))) == after) man.fields.push_back({k, v});
            }
        }
        std::string js;   // json.dump to a text-mode file on Windows: CRLF line ends
        for (char c : json_dump_indent1(man)) js += c == 10 ? std::string{char(13), char(10)} : std::string(1, c);
        fs::create_directories(upath(backup));
        write_file_replace(MAN, Bytes(js.begin(), js.end()));
        std::printf("applied; manifest %s\n", utf8(MAN).c_str());
    } else
        std::printf("DRY RUN: nothing written. Add --apply to write.\n");
    return 0;
}

}  // namespace u3
