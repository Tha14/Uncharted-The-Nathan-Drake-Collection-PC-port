// importer/validate.cpp: u2import --validate, the converters checked on files both platforms have: the PS3 original
// converted and compared with the retail PS4 file (a JSON report in <work>/validate/validation.json and state.json;
// nothing is installed).
//
//   xvag    the 4 PS-ADPCM music tracks retail PS4 kept: byte-identical (with the retail interleave)
//   light   LIGHT_TABLE_1 of the shared MP levels: byte-identical (pointers compared as offsets)
//   vis     VIS_INFO_1 of the shared MP levels: byte-identical
//   ingame  shared -ingame10 / -npc-ingame10 paks: every item compared block by block (blocks = the item cut at every
//           pointer target inside it, pointers masked, trailing 0xaa / 0 padding ignored, in any order)
//   nav     NAV_MESH_1 of the shared MP -ingame10 paks: byte-identical, matched by nav mesh name (retail leaves the last
//           word of each 0x18-byte portal record uninitialised)
//   skins   the 16 shared mp-hero skins, converted with a template other than themselves: the loader check, prototypes,
//           submeshes (index counts and materials per submesh) against retail
//   stage   shared sky / wide background paks: converted (with the stage check), item types, submesh / index / texture
//           counts against retail
//   banks   the first 40 banks both have: identical, or the same structure (sound / grain / name counts)
//   coll    COLLISION_DATA_HAVOK_BG of 11 shared MP levels: each converted item against the retail item of the closest
//           size, plus the structure check on the retail items
//   water   WATER_DATA of 5 shared MP levels: byte-identical except the material ids and the exporter's unset words
#include "actor.hpp"
#include "ps4ingame.hpp"
#include "visinfo.hpp"

namespace u2imp {

using namespace u3;

namespace {

const std::vector<std::string> XVAG_PAIRS = {"146_JungleExplore_LOOP1", "148_JungleExplore_ENDTAG1", "149_JungleExplore_LOOP2", "150_JungleExplore_ENDTAG2"};
const std::vector<std::string> LEVEL_PAIRS = {"village-multi-wide", "gompa-multi-wide", "sham-multi-wide", "dig-multi", "stage-multi"};
const std::vector<std::string> INGAME_PAIRS = {"sky-village-multi-ingame10", "gompa-multi-npc-ingame10", "village-multi-npc-ingame10",
                                               "stage-multi-ingame10",       "treasure-multi-ingame10",  "dig-multi-ingame10"};
const std::vector<std::string> NAV_PAIRS = {"gompa-multi-ingame10", "village-multi-ingame10", "shambhala-multi-ingame10", "warzone-multi-ingame10"};
const std::vector<std::string> STAGE_PAIRS = {"sky-village-multi", "village-multi-wide", "gompa-multi-wide", "sham-multi-wide"};
const std::vector<std::string> SKINS = {"chloe-variation", "divesuit", "eddie", "elena", "elena-divesuit", "merc", "navarro", "pirate", "red", "roman",
                                        "skeleton-1-glow", "skeleton-1-platinum", "sullivan-blue", "sullivan-red", "sullivan-winter", "udf"};
const std::vector<std::string> COLL_PAIRS = {"dig-multi",      "stage-multi",   "sham-multi-wide", "village-multi",     "treasure-multi",     "gompa-multi",
                                             "warzone-multi",  "shambhala-multi", "ice-cave-multi", "train-wreck-multi", "sniper-towers-multi"};
const std::vector<std::string> WATER_PAIRS = {"village-multi", "treasure-multi", "gompa-multi", "warzone-multi", "shambhala-multi"};
const uint32_t PS4_WATER_OBJ = 0x88;

bool isfile(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
long long fsize(const fs::path &p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? -1 : (long long)n;
}

// a Counter: counts in the order the keys first occurred
struct Tally {
    std::vector<std::pair<std::string, long long>> v;
    long long &operator[](const std::string &k) {
        for (auto &[n, c] : v)
            if (n == k) return c;
        v.emplace_back(k, 0);
        return v.back().second;
    }
    void update(const Tally &o) {
        for (const auto &[k, c] : o.v) (*this)[k] += c;
    }
    Value json() const {
        Value o = Value::object();
        for (const auto &[k, c] : v) o.set(k, num(c));
        return o;
    }
};

Value arr(const std::vector<std::string> &v) {
    Value a;
    a.kind = Value::Array;
    for (const std::string &s : v) a.items.push_back(str(s));
    return a;
}

// (exception type, message) as Python names them
std::pair<std::string, std::string> exc(const std::exception &e) {
    const std::string w = e.what();
    if (dynamic_cast<const Unsupported *>(&e)) return {"Unsupported", w};
    if (dynamic_cast<const NavMeshUnsupported *>(&e)) return {"NavMeshUnsupported", w};
    const size_t c = w.find(": ");
    if (c != std::string::npos && c > 0 && std::all_of(w.begin(), w.begin() + std::ptrdiff_t(c), [](char ch) { return std::isalnum((unsigned char)ch) || ch == '_'; }) &&
        (ends(w.substr(0, c), "Error") || ends(w.substr(0, c), "Exception")))
        return {w.substr(0, c), w.substr(c + 2)};
    return {"RuntimeError", w};
}

std::string le64(uint64_t v) {
    std::string o(8, '\0');
    for (int k = 0; k < 8; k++) o[size_t(k)] = char(v >> (8 * k));
    return o;
}
void put_le64(Bytes &b, size_t pos, int64_t v) {
    if (v < 0) throw std::runtime_error("OverflowError: can't convert negative int to unsigned");
    if (pos + 8 > b.size()) b.resize(pos + 8);
    for (int k = 0; k < 8; k++) b[pos + size_t(k)] = uint8_t(uint64_t(v) >> (8 * k));
}

// the item's bytes with every pointer slot replaced by its target offset relative to the item (another page: 0xfeed0000 + page)
Bytes item_bytes(const Pak &p, const PakItem &it) {
    const uint64_t b = p.pbase(it.page) + it.data_off();
    Bytes out(p.d.begin() + std::ptrdiff_t(b), p.d.begin() + std::ptrdiff_t(std::min<uint64_t>(b + it.size, p.d.size())));
    for (uint64_t o = 0; o + 7 < it.size; o += 8)
        if (const auto r = p.resolve(it.page, uint32_t(it.data_off() + o)))
            put_le64(out, o, r->first == it.page ? int64_t(r->second) - int64_t(it.data_off()) : int64_t(0xfeed0000ull + r->first));
    return out;
}

// a converted blob's bytes with every pointer as its target's offset in the blob
Bytes blob_bytes(const std::shared_ptr<Blob> &blob) {
    if (!blob) throw std::runtime_error("AttributeError: 'NoneType' object has no attribute 'b'");
    Bytes b = blob->b;
    for (const Blob::Reloc &r : blob->relocs) put_le64(b, r.pos, r.target->root().second + r.off);
    return b;
}

std::string repr_dict(const Tally &t) {   // repr() of dict(Counter)
    std::string o = "{";
    for (size_t i = 0; i < t.v.size(); i++) o += (i ? ", " : "") + py_repr(t.v[i].first) + ": " + std::to_string(t.v[i].second);
    return o + "}";
}

struct Run {
    const PS3 &ps3;
    const PS4 &ps4;
    Context ctx;   // work = <work>/validate
    fs::path work;
    std::string ffmpeg;

    std::optional<fs::path> ps3_file(const std::string &key) {
        const auto c = ps3.candidates(key);
        if (c.empty()) return std::nullopt;
        std::string flat = key;
        std::replace(flat.begin(), flat.end(), '/', '_');
        const fs::path p = work / "ps3" / P(flat);
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (!fs::exists(p, ec)) write_bytes(p, c[0]->read());
        return p;
    }

    Value xvag() {
        Value out = Value::object();
        for (const std::string &n : XVAG_PAIRS) {
            const auto src = ps3.candidates("sound1/music/" + n + ".xvag");
            const fs::path ref = ps4.main / "sound1" / "music" / P(n + ".xvag");
            if (src.empty() || !isfile(ref)) {
                out.set(n, str("pair missing"));
                continue;
            }
            const Bytes r = read_bytes(ref);
            if (r.size() < 0x3c) throw std::runtime_error("struct.error: unpack_from requires a buffer of at least 60 bytes");
            const uint32_t il = rd32le(r, 0x38);
            out.set(n, str(xvag_convert(src[0]->read(), il) == r ? "identical" : "DIFFERENT"));
        }
        return out;
    }

    Value tables(bool light) {
        Value out = Value::object();
        const std::string t = light ? "LIGHT_TABLE_1" : "VIS_INFO_1";
        for (const std::string &n : LEVEL_PAIRS) {
            const auto a = ps3_file("pak22/" + n + ".pak");
            const fs::path ref = ps4.pak_dir / P(n + ".pak");
            if (!a || !isfile(ref)) {
                out.set(n, str("pair missing"));
                continue;
            }
            const Pak p(*a), q(ref);
            if (p.items_of(t).empty()) continue;
            const auto qi = q.items_of(t);
            if (qi.empty()) throw std::runtime_error("IndexError: list index out of range");
            std::shared_ptr<Blob> blob;
            if (light)
                blob = u2_light_table(p);
            else {
                const auto bl = vis_blobs(p);
                if (bl.size() != 1) {
                    out.set(n, str("split VIS item (not compared)"));
                    continue;
                }
                blob = bl[0];
            }
            const Bytes mine = blob_bytes(blob), theirs = item_bytes(q, qi[0]);
            out.set(n, str(mine == theirs ? fmt("identical (%zu bytes)", mine.size()) : fmt("DIFFERENT (%zu vs %zu bytes)", mine.size(), theirs.size())));
        }
        return out;
    }

    // the item cut at every pointer target inside it; pointers masked, trailing 0xaa then 0 bytes dropped
    static std::vector<Bytes> blocks(const Pak &p, const PakItem &it) {
        const uint64_t lo = it.data_off(), hi = lo + it.size, b = p.pbase(it.page);
        std::set<uint64_t> cuts = {lo, hi};
        for (const auto &[src, tp] : p.ptrmap)
            if (src.first == it.page && lo <= src.second && src.second < hi) {
                const uint64_t t = p.pptr(src.first, src.second);
                if (tp == it.page && lo <= t && t < hi) cuts.insert(t);
            }
        std::vector<Bytes> out;
        for (auto c = cuts.begin(); std::next(c) != cuts.end(); ++c) {
            const uint64_t a = *c, e = *std::next(c);
            const uint64_t s = std::min<uint64_t>(b + a, p.d.size()), f = std::min<uint64_t>(b + e, p.d.size());
            Bytes bs(p.d.begin() + std::ptrdiff_t(s), p.d.begin() + std::ptrdiff_t(f));
            for (uint64_t o = a; o < e; o += 8)
                if (p.is_ptr(it.page, uint32_t(o))) {
                    const size_t at = size_t(o - a), end = std::min(at + 8, bs.size());
                    bs.erase(bs.begin() + std::ptrdiff_t(at), bs.begin() + std::ptrdiff_t(end));
                    bs.insert(bs.begin() + std::ptrdiff_t(at), 8, uint8_t('P'));
                }
            while (!bs.empty() && bs.back() == 0xaa) bs.pop_back();
            while (!bs.empty() && bs.back() == 0) bs.pop_back();
            out.push_back(std::move(bs));
        }
        return out;
    }
    // the comparison lines per item type (the detail lines left out)
    static std::vector<std::string> cmpitems(const fs::path &a, const fs::path &b) {
        const Pak pa(a), pb(b);
        std::set<std::string> types;
        for (const PakItem &i : pa.items) types.insert(i.type);
        for (const PakItem &i : pb.items) types.insert(i.type);
        std::vector<std::string> lines;
        for (const std::string &t : types) {
            const auto ia = pa.items_of(t), ib = pb.items_of(t);
            if (ia.size() != ib.size()) {
                lines.push_back(fmt("%s count %zu %zu", t.c_str(), ia.size(), ib.size()));
                continue;
            }
            for (size_t k = 0; k < ia.size(); k++) {
                std::map<Bytes, long long> ba, bb;
                for (Bytes &x : blocks(pa, ia[k])) ba[x]++;
                for (Bytes &x : blocks(pb, ib[k])) bb[x]++;
                if (ba == bb) {
                    lines.push_back(fmt("%s MATCH %zu", t.c_str(), ba.size()));
                    continue;
                }
                long long only_a = 0, only_b = 0, total = 0;
                for (const auto &[x, n] : ba) {
                    total += n;
                    const auto f = bb.find(x);
                    only_a += std::max(0LL, n - (f == bb.end() ? 0 : f->second));
                }
                for (const auto &[x, n] : bb) {
                    const auto f = ba.find(x);
                    only_b += std::max(0LL, n - (f == ba.end() ? 0 : f->second));
                }
                lines.push_back(fmt("%s DIFF %lld only-converted %lld only-retail of %lld", t.c_str(), only_a, only_b, total));
            }
        }
        return lines;
    }

    Value ingame() {
        Value out = Value::object();
        Context c = ctx;
        c.drop_navmesh = true;
        for (const std::string &n : INGAME_PAIRS) {
            const auto a = ps3_file("pak22/" + n + ".pak");
            const fs::path ref = ps4.pak_dir / P(n + ".pak");
            if (!a || !isfile(ref)) {
                out.set(n, str("pair missing"));
                continue;
            }
            const fs::path dst = work / "out" / P(n + ".pak");
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            std::vector<std::string> notes;
            try {
                notes = convert_level(c, *a, dst);
                notes.erase(notes.begin());   // the kind
            } catch (const std::exception &e) {
                const auto [ty, msg] = exc(e);
                out.set(n, str("FAILED " + ty + ": " + msg));
                continue;
            }
            out.set(n, obj({{"notes", arr(notes)}, {"items", arr(cmpitems(dst, ref))}}));
        }
        return out;
    }

    Value nav() {
        Value out = Value::object();
        Tally tot;
        for (const std::string &n : NAV_PAIRS) {
            const auto a = ps3_file("pak22/" + n + ".pak");
            const fs::path ref = ps4.pak_dir / P(n + ".pak");
            if (!a || !isfile(ref)) {
                out.set(n, str("pair missing"));
                continue;
            }
            const Pak p(*a), q(ref);
            std::map<std::optional<std::string>, PakItem> retail;
            for (const PakItem &it : q.items_of("NAV_MESH_1")) {
                const auto r = q.resolve(it.page, it.data_off() + 0x20);
                retail[r ? std::optional<std::string>(q.pstr(r->first, r->second)) : std::nullopt] = it;
            }
            Tally res;
            for (const PakItem &it : p.items_of("NAV_MESH_1")) {
                const auto r = p.resolve(it.page, it.data_off() + 0x1c);
                const auto ib = retail.find(r ? std::optional<std::string>(p.pstr(r->first, r->second)) : std::nullopt);
                if (ib == retail.end()) {
                    res["no retail match"]++;
                    continue;
                }
                Bytes m, t;
                try {
                    m = blob_bytes(nav_mesh_u2(p, it));
                    t = item_bytes(q, ib->second);
                } catch (const NavMeshUnsupported &e) {
                    res[std::string("unsupported: ") + e.what()]++;
                    continue;
                }
                if (m == t) {
                    res["identical"]++;
                    continue;
                }
                const uint64_t po = m.size() >= 0x60 ? rd64le(m, 0x58) : 0;
                if (po && m.size() == t.size())
                    for (uint64_t k = po + 0x14; k < m.size(); k += 0x18)
                        for (uint64_t j = k; j < std::min<uint64_t>(k + 4, m.size()); j++) m[j] = t[j] = 0;
                res[m == t ? "identical except portal padding" : "DIFFERENT"]++;
            }
            out.set(n, res.json());
            tot.update(res);
        }
        out.set("_total", tot.json());
        return out;
    }

    // (prototype count, per submesh (index count, material name before ',', vertex count))
    static std::pair<uint64_t, std::vector<std::tuple<uint32_t, std::string, uint32_t>>> submeshes(const Pak &p) {
        const auto gs = p.items_of("GEOMETRY_1");
        if (gs.empty()) throw std::runtime_error("IndexError: list index out of range");
        const uint32_t pg = gs[0].page;
        const uint64_t b = gs[0].data_off(), gb = p.pbase(pg) + b;
        const uint64_t nsub = p.u64(gb + 8), nproto = p.u64(gb + 0x18);
        const ORef sr = bg::res(p, pg, b + 0x10);
        std::vector<std::tuple<uint32_t, std::string, uint32_t>> subs;
        for (uint64_t i = 0; i < nsub; i++) {
            const uint64_t o = bg::need(sr).second + 0x88 * i, sb = p.pbase(sr->first) + o;
            const uint32_t nv = p.u32(sb + 0x20), ni = p.u32(sb + 0x24);
            const Ref m = bg::need(p, sr->first, o + 0x48);
            std::string mat = p.pstr(m.first, m.second);
            mat = mat.substr(0, mat.find(','));
            subs.emplace_back(ni, mat, nv);
        }
        return {nproto, subs};
    }

    Value skins() {
        Value out = Value::object();
        Tally tot;
        for (const std::string &s : SKINS) {
            const std::string name = "mp-hero-" + s;
            const auto a = ps3_file("actor27/" + name + ".pak");
            const fs::path ref = ps4.actor_dir / P(name + ".pak");
            if (!a || !isfile(ref)) {
                out.set(name, str("pair missing"));
                continue;
            }
            const fs::path dst = work / "out" / P(name + ".pak");
            try {
                std::error_code ec;
                fs::create_directories(dst.parent_path(), ec);
                const fs::path tmpl = skin_template(*a, ps4.actor_dir, name);
                convert_actor_pak(*a, tmpl, dst);
                const auto errs = actor_check(dst);
                const auto [np_a, sa] = submeshes(Pak(dst));
                const auto [np_b, sb] = submeshes(Pak(ref));
                std::map<std::pair<uint32_t, std::string>, long long> ca, cb;
                long long va = 0, vb = 0;
                for (const auto &[ni, m, nv] : sa) ca[{ni, m}]++, va += nv;
                for (const auto &[ni, m, nv] : sb) cb[{ni, m}]++, vb += nv;
                long long same = 0;
                for (const auto &[k, n] : ca)
                    if (const auto f = cb.find(k); f != cb.end()) same += std::min(n, f->second);
                out.set(name, obj({{"template", str(S(tmpl.filename()))},
                                   {"check", errs.empty() ? str("ok") : arr(errs)},
                                   {"prototypes", str(fmt("%llu/%llu", (unsigned long long)np_a, (unsigned long long)np_b))},
                                   {"submeshes_matching", str(fmt("%lld of %zu", same, sb.size()))},
                                   {"submeshes", str(fmt("%zu/%zu", sa.size(), sb.size()))},
                                   {"vertices", str(fmt("%lld/%lld", va, vb))},
                                   {"size", str(fmt("%lld/%lld", (long long)fsize(dst), (long long)fsize(ref)))}}));
                tot["skins"] += 1;
                tot["sub_match"] += same;
                tot["sub_retail"] += (long long)sb.size();
                tot["check_ok"] += errs.empty();
            } catch (const std::exception &e) {
                const auto [ty, msg] = exc(e);
                out.set(name, str("FAILED " + ty + ": " + msg.substr(0, 200)));
            }
        }
        out.set("_total", tot.json());
        return out;
    }

    Value stage() {
        Value out = Value::object();
        for (const std::string &n : STAGE_PAIRS) {
            const auto a = ps3_file("pak22/" + n + ".pak");
            const fs::path ref = ps4.pak_dir / P(n + ".pak");
            if (!a || !isfile(ref)) {
                out.set(n, str("pair missing"));
                continue;
            }
            const fs::path dst = work / "out" / P(n + ".pak");
            try {
                std::error_code ec;
                fs::create_directories(dst.parent_path(), ec);
                std::vector<std::string> notes = convert_level(ctx, *a, dst);
                notes.erase(notes.begin());   // the kind
                const Pak p(dst), q(ref);
                Tally ta, tb;
                for (const PakItem &i : p.items)
                    if (i.type != "RAW_DATA") ta[i.type]++;
                for (const PakItem &i : q.items)
                    if (i.type != "RAW_DATA") tb[i.type]++;
                std::set<std::string> sa_, sb_;
                for (const auto &[k, c] : ta.v) sa_.insert(k);
                for (const auto &[k, c] : tb.v) sb_.insert(k);
                const auto [na, sa] = submeshes(p);
                const auto [nb, sb] = submeshes(q);
                long long ia = 0, ib = 0;
                for (const auto &x : sa) ia += std::get<0>(x);
                for (const auto &x : sb) ib += std::get<0>(x);
                (void)na, (void)nb;
                out.set(n, obj({{"notes", arr(notes)},
                                {"item_types", str(sa_ == sb_ ? std::string("same") : repr_dict(ta) + " vs " + repr_dict(tb))},
                                {"submeshes", str(fmt("%zu/%zu", sa.size(), sb.size()))},
                                {"indices", str(fmt("%lld/%lld", ia, ib))},
                                {"textures", str(fmt("%lld/%lld", ta["VRAM_DESC"], tb["VRAM_DESC"]))},
                                {"size", str(fmt("%lld/%lld", (long long)fsize(dst), (long long)fsize(ref)))}}));
            } catch (const std::exception &e) {
                const auto [ty, msg] = exc(e);
                out.set(n, str("FAILED " + ty + ": " + msg.substr(0, 300)));
            }
        }
        return out;
    }

    Value banks(size_t limit = 40) {
        Tally out;
        std::vector<std::string> ex;
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(ps4.bank_dir, ec))
            if (ends(S(e.path().filename()), ".bnk")) names.push_back(S(e.path().filename()));
        std::sort(names.begin(), names.end());
        if (names.size() > limit) names.resize(limit);
        for (const std::string &f : names) {
            const auto c = ps3.candidates("sound1/banks/" + f);
            if (c.empty()) continue;
            Bytes o;
            try {
                std::string note;
                o = bank_convert(c[0]->read(), ffmpeg, note);
            } catch (const std::exception &e) {
                if (std::string(e.what()).find("ffmpeg was not found") != std::string::npos) {
                    out["needs ffmpeg"]++;
                    continue;
                }
                out["failed"]++;
                ex.push_back(f + ": " + exc(e).second);
                continue;
            }
            const Bytes r = read_bytes(ps4.bank_dir / P(f));
            if (o == r) {
                out["identical"]++;
                continue;
            }
            auto head = [](const Bytes &d) {
                const uint32_t at = rd32le(d, 8);
                std::vector<uint32_t> h;
                for (int k = 0; k < 16; k++) h.push_back(rd32le(d, at + 4ull * k));
                return h;
            };
            const auto ho = head(o), hr = head(r);
            if (ho[0] == hr[0] && ho[1] == hr[1] && ho[5] == hr[5] && ho[6] == hr[6])
                out["same structure (sound/grain/name counts), different samples"]++;
            else {
                out["different structure"]++;
                auto hx = [](const std::vector<uint32_t> &h) {
                    std::vector<std::string> v;
                    for (int k = 0; k < 8; k++) v.push_back(fmt("0x%x", h[size_t(k)]));
                    return py_list(v);
                };
                ex.push_back(f + ": " + hx(ho) + " vs " + hx(hr));
            }
        }
        if (ex.size() > 5) ex.resize(5);
        return obj({{"counts", out.json()}, {"examples", arr(ex)}});
    }

    Value coll() {
        Value out = Value::object();
        Tally tot;
        for (const std::string &n : COLL_PAIRS) {
            const auto a = ps3_file("pak22/" + n + ".pak");
            const fs::path ref = ps4.pak_dir / P(n + ".pak");
            if (!a || !isfile(ref)) {
                out.set(n, str("pair missing"));
                continue;
            }
            const Pak p(*a), q(ref);
            const auto qi = q.items_of("COLLISION_DATA_HAVOK_BG");
            Tally res;
            for (const auto &blob : u2_collision(p)) {
                const Bytes m = blob_bytes(blob);
                if (qi.empty()) throw std::runtime_error("ValueError: min() arg is an empty sequence");
                const PakItem *best = &qi[0];
                for (const PakItem &x : qi)
                    if (std::llabs((long long)x.size - (long long)m.size()) < std::llabs((long long)best->size - (long long)m.size())) best = &x;
                res[m == item_bytes(q, *best) ? "identical" : "DIFFERENT"]++;
            }
            long long probs = 0;
            for (const PakItem &x : qi) probs += (long long)u2_collision_problems(q, x).size();
            res["retail check problems"] = probs;
            out.set(n, res.json());
            tot.update(res);
        }
        out.set("_total", tot.json());
        return out;
    }

    Value water() {
        Value out = Value::object();
        for (const std::string &n : WATER_PAIRS) {
            const auto a = ps3_file("pak22/" + n + ".pak");
            const fs::path ref = ps4.pak_dir / P(n + ".pak");
            if (!a || !isfile(ref)) {
                out.set(n, str("pair missing"));
                continue;
            }
            const Pak p(*a), q(ref);
            Bytes m = blob_bytes(u2_water(p));
            const auto qi = q.items_of("WATER_DATA");
            if (qi.empty()) throw std::runtime_error("IndexError: list index out of range");
            Bytes t = item_bytes(q, qi[0]);
            const uint32_t nobj = rd32le(t, 4);
            if (m.size() == t.size())
                for (uint32_t k = 0; k < nobj; k++)
                    for (uint32_t x : {4u, 0x78u, 0x7cu, 0x80u, 0x84u}) {
                        const size_t o = 0x10 + size_t(PS4_WATER_OBJ) * k + x;
                        for (size_t j = o; j < std::min(o + 4, m.size()); j++) m[j] = t[j];
                    }
            out.set(n, str(m == t ? fmt("identical (%u objects)", nobj) : fmt("DIFFERENT (%zu vs %zu bytes)", m.size(), t.size())));
        }
        return out;
    }
};

}  // namespace

Value validate(const PS3 &ps3, const PS4 &ps4, const Context &ctx, const fs::path &work, const std::function<void(const std::string &)> &lg) {
    std::error_code ec;
    fs::create_directories(work, ec);
    Run r{ps3, ps4, ctx, work, find_ffmpeg("")};
    r.ctx.work = work;
    const std::vector<std::pair<std::string, std::function<Value()>>> checks = {
        {"xvag", [&] { return r.xvag(); }},     {"light", [&] { return r.tables(true); }}, {"vis", [&] { return r.tables(false); }},
        {"ingame", [&] { return r.ingame(); }}, {"nav", [&] { return r.nav(); }},          {"skins", [&] { return r.skins(); }},
        {"stage", [&] { return r.stage(); }},   {"banks", [&] { return r.banks(); }},      {"coll", [&] { return r.coll(); }},
        {"water", [&] { return r.water(); }}};
    Value rep = Value::object();
    for (const auto &[k, fn] : checks) {
        lg("validate: " + k + " ...");
        Value v;
        try {
            v = fn();
        } catch (const std::exception &e) {
            const auto [ty, msg] = exc(e);
            v = str("FAILED " + ty + ": " + msg.substr(0, 600));
        }
        lg("validate: " + k + ": " + dumps(v).substr(0, 600));
        rep.set(k, v);
    }
    write_text(work / "validation.json", dump_indent1(rep));
    return rep;
}

}  // namespace u2imp
