// Recipe step london-fade: look fixes for the stage paks of overlay-r3. Replaces apply_london.py, fadefix.py and llib.py.
//  1. fade records: a PS3 background prototype with header byte 1 bit 0x04 ("fade out with distance") keeps the flag on
//     PS4 (prototype flag byte 1) but the converter left its fade record pointer (+0x48) empty, and then the PS4 engine
//     never draws it. Each such prototype gets a 16-byte record {f32 start, f32 end, 0, 0} from the PS3 header's u16
//     start / end (+8), appended to the page that holds the prototype; every other byte, fixup and the VRAM stay.
//     Prototypes are matched by index; a flag that differs from PS3 skips the pak.
//  2. sun: ldn-train-multi.pak / -lit.pak sun intensity back to 886.71 (the PS3 / retail value).
//  3. --instflags (off): PS3 instance word +8 bits 16..19 -> PS4 instance flag bytes (+0x18) set to 0.
// PS3 sources: dlc-patch18 > dlc-patch13 > goty-patch > goty-dlc > disc pak23 / bin, then DATA2's *-7.psarc maps.
// dig-coop-approach.pak is left alone unless --include-failing.
//   u3fix london-fade --project P --ps3-disc D --ps3-update U --ps3-data2 D2 [--maps ldn|all] [--instflags]
//                     [--include-failing] [--out DIR] [--apply] [--restore]
// Dry run by default; --out DIR also writes the rebuilt paks there; --apply backs up to work/backup-london once
// (manifest.json with the md5 before / after) and writes overlay-r3; --restore --apply puts the backups back.
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "ps4writer.hpp"
#include "psarc.hpp"
#include "u3fix.hpp"

#include <cstring>

namespace u3 {
namespace {


struct Proto4 {
    uint32_t pg;
    uint64_t off;
    Bytes flags;
    ORef fade;
};
std::vector<Proto4> ps4_protos(const Pak &q) {
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) return {};
    const uint32_t pg = gs[0].page;
    const uint64_t b = gs[0].data_off();
    const uint32_t n = q.pu32(pg, b + 0x18);
    std::vector<Proto4> out;
    if (!n) return out;
    const Ref pp = bg::need(q, pg, b + 0x20);
    for (uint32_t i = 0; i < n; i++) {
        const Ref r = bg::need(q, pp.first, pp.second + 8ull * i);
        Proto4 x{r.first, r.second, Bytes(16, 0), bg::res(q, r.first, r.second + 0x48)};
        if (const ORef fl = bg::res(q, r.first, r.second + 0x20)) {
            const uint64_t a = q.pbase(fl->first) + fl->second;
            x.flags.assign(q.d.begin() + std::ptrdiff_t(std::min<uint64_t>(a, q.d.size())), q.d.begin() + std::ptrdiff_t(std::min<uint64_t>(a + 16, q.d.size())));
        }
        out.push_back(std::move(x));
    }
    return out;
}
bool flagged(const Proto4 &p) { return p.flags.size() > 1 && p.flags[1]; }

struct Fade3 {
    bool on;
    uint16_t start, end;
};
std::vector<Fade3> ps3_fades(const Pak &p) {
    std::vector<Fade3> out;
    for (const bg::Proto &c : bg::collection(p)) {
        const uint64_t b = p.pbase(c.hdr.first) + c.hdr.second;
        out.push_back({(bg::u8(p, b + 1) & 4) != 0, bg::be16(p, b + 8), bg::be16(p, b + 10)});
    }
    return out;
}

struct Report {
    size_t flagged = 0, fixed = 0, inst = 0;
    bool mism = false;
    std::string mism_text;
};

void require(bool ok, const std::string &m) {
    if (!ok) throw Stop("AssertionError: " + m);
}

// the rebuilt pak (or nothing to change)
std::optional<Bytes> fade_fix(const Pak &q, const Pak &p, bool inst_flags, Report &rep) {
    const std::vector<Proto4> pr = ps4_protos(q);
    std::vector<size_t> need;
    for (size_t i = 0; i < pr.size(); i++) {
        rep.flagged += flagged(pr[i]);
        if (flagged(pr[i]) && !pr[i].fade) need.push_back(i);
    }
    const std::vector<Fade3> f3 = ps3_fades(p);
    std::vector<std::string> mism;
    for (size_t i : need)
        if (i >= f3.size() || !f3[i].on) mism.push_back(std::to_string(i));
    if (mism.empty())
        for (size_t i = 0; i < pr.size(); i++)
            if (i < f3.size() && flagged(pr[i]) != f3[i].on) {
                mism.push_back("('flag', " + std::to_string(i) + ")");
                break;
            }
    if (!mism.empty()) {
        rep.mism = true;
        for (size_t k = 0; k < mism.size() && k < 5; k++) rep.mism_text += (k ? ", " : "[") + mism[k];
        rep.mism_text += "]";
        return std::nullopt;
    }
    if (need.empty() && !inst_flags) return std::nullopt;
    PakWriter w;
    std::vector<RawPage *> pages;
    for (uint32_t pi = 0; pi < q.pages.size(); pi++) {
        std::vector<std::pair<uint64_t, size_t>> fx;
        for (const Pak::Fixup &f : q.fixups)
            if (f.src == pi) fx.push_back({f.off, f.dst});
        std::sort(fx.begin(), fx.end());
        pages.push_back(&w.raw_page(Bytes(q.d.begin() + std::ptrdiff_t(q.pbase(pi)), q.d.begin() + std::ptrdiff_t(q.pbase(pi) + q.pages[pi].size)), fx));
    }
    for (size_t i : need) {
        auto fb = std::make_shared<Blob>("fade" + std::to_string(i));
        const float v[4] = {float(f3[i].start), float(f3[i].end), 0.0f, 0.0f};
        Bytes b(16);
        std::memcpy(b.data(), v, 16);
        fb->raw(b);
        pages[pr[i].pg]->append(fb);
        pages[pr[i].pg]->redirect(pr[i].off + 0x48, fb);
        rep.fixed++;
    }
    if (inst_flags) {   // PS3 instance word +8 bit 16+k -> PS4 instance flag byte k (+0x18) = 0; instances in collection order
        const PakItem g = q.items_of("GEOMETRY_1").at(0);
        const uint32_t n = q.pu32(g.page, g.data_off() + 0x28);
        const Ref ip = bg::need(q, g.page, g.data_off() + 0x30);
        std::vector<uint32_t> x8s;
        for (const bg::Proto &c : bg::collection(p))
            for (const ORef &ia : c.insts) x8s.push_back(p.pu32(bg::need(ia).first, bg::need(ia).second + 8));
        for (size_t i = 0; i < std::min<size_t>(n, x8s.size()); i++) {
            const Ref r = bg::need(q, ip.first, ip.second + 8 * i);
            const uint64_t base = q.pbase(r.first) + r.second;
            if (rd32le(q.d, base + 4) != (x8s[i] & 0xffff)) throw Stop("ValueError: instance order differs from PS3 at " + std::to_string(i));
            const uint32_t hi = x8s[i] >> 16 & 0xf;
            if (!hi) continue;
            const uint32_t cur = rd32le(q.d, base + 0x18);
            uint32_t nv = cur;
            for (int k = 0; k < 4; k++)
                if (hi >> k & 1) nv &= ~(0xffu << (8 * k));
            if (nv != cur) pages[r.first]->set_u32(r.second + 0x18, nv), rep.inst++;
        }
    }
    if (need.empty() && !rep.inst) return std::nullopt;
    w.login_page = std::make_pair(size_t(q.h[2]), size_t(q.h[3]));
    w.vram_table_loc = std::make_pair(size_t(q.h[9]), size_t(q.h[10]));
    w.vram = Bytes(q.d.begin() + q.vram_base, q.d.end());
    Bytes out = w.build();
    // the checks of the reference tool: same pages, items and VRAM, one more fixup per record, the records as meant,
    // every other word of the original pages unchanged (but the page size, the fade pointers, instance flag words)
    const Pak nq(out, "rebuilt");
    require(nq.pages.size() == q.pages.size(), "page count");
    require(nq.fixups.size() == q.fixups.size() + need.size(), "fixup count");
    require(nq.items.size() == q.items.size(), "item list changed");
    for (size_t k = 0; k < q.items.size(); k++)
        require(nq.items[k].type == q.items[k].type && nq.items[k].name == q.items[k].name && nq.items[k].page == q.items[k].page, "item list changed");
    require(Bytes(nq.d.begin() + nq.vram_base, nq.d.end()) == Bytes(q.d.begin() + q.vram_base, q.d.end()), "vram changed");
    const std::vector<Proto4> npr = ps4_protos(nq);
    for (size_t i : need) {
        require(npr.at(i).fade.has_value(), "fade record");
        float v[4];
        std::memcpy(v, &nq.d.at(nq.pbase(npr[i].fade->first) + npr[i].fade->second), 16);
        require(v[0] == float(f3[i].start) && v[1] == float(f3[i].end) && v[2] == 0 && v[3] == 0, "fade record values");
    }
    std::map<uint32_t, std::set<uint64_t>> iflag;
    {
        const PakItem g = nq.items_of("GEOMETRY_1").at(0);
        const uint32_t ni = nq.pu32(g.page, g.data_off() + 0x28);
        if (ni) {
            const Ref ip = bg::need(nq, g.page, g.data_off() + 0x30);
            for (uint32_t i = 0; i < ni; i++) {
                const Ref r = bg::need(nq, ip.first, ip.second + 8ull * i);
                iflag[r.first].insert(r.second + 0x18);
            }
        }
    }
    for (uint32_t pi = 0; pi < q.pages.size(); pi++) {
        const uint64_t ps = q.pages[pi].size;
        require(nq.pages[pi].size >= ps, "page shrank");
        std::set<uint64_t> allowed = iflag[pi];
        allowed.insert(0xc);
        for (size_t i : need)
            if (pr[i].pg == pi) allowed.insert(pr[i].off + 0x48);
        const uint8_t *o = &q.d[q.pbase(pi)], *nn = &nq.d[nq.pbase(pi)];
        for (uint64_t k = 0; k + 3 < ps; k += 4)
            if (std::memcmp(o + k, nn + k, 4) != 0 && !allowed.count(k) && !(k >= 4 && allowed.count(k - 4)))
                throw Stop(u3::fmt("AssertionError: unexpected change page %u off %#llx", pi, (unsigned long long)k));
    }
    return out;
}

// the PS3 archives, in priority order
struct Ps3Paks {
    std::vector<std::pair<std::string, std::unique_ptr<Psarc>>> arcs;
    Ps3Paks(const std::string &disc, const std::string &update, const std::string &data2) {
        for (const auto &[tag, f] : std::vector<std::pair<std::string, std::string>>{{"p18", update + "/dlc-patch18.psarc"},
                                                                                       {"p13", update + "/dlc-patch13.psarc"},
                                                                                       {"gpatch", disc + "/goty-patch.psarc"},
                                                                                       {"gdlc", disc + "/goty-dlc.psarc"},
                                                                                       {"pak23", disc + "/build/main/pak23.psarc"},
                                                                                       {"bin", disc + "/build/main/bin.psarc"}})
            arcs.push_back({tag, std::make_unique<Psarc>(upath(f))});
        for (const std::string &d : {data2 + "/build/main/pak23/", data2 + "/build/main/"}) {   // server-delivered maps last
            if (!fs::is_directory(upath(d))) continue;
            std::vector<std::string> xs;
            for (const auto &e : fs::directory_iterator(upath(d))) xs.push_back(utf8(e.path().filename()));
            std::sort(xs.begin(), xs.end());
            for (const std::string &x : xs) {
                if (x.size() < 8 || x.compare(x.size() - 8, 8, "-7.psarc") != 0 || x.find("speech") != std::string::npos) continue;
                std::unique_ptr<Psarc> a;
                try {
                    a = std::make_unique<Psarc>(upath(d + x));
                } catch (const std::exception &) {
                    continue;
                }
                const std::string tag = "d2:" + x.substr(0, x.size() - 8);
                auto it = std::find_if(arcs.begin(), arcs.end(), [&](const auto &e) { return e.first == tag; });
                if (it != arcs.end()) it->second = std::move(a);   // a dict keeps the first key's place
                else arcs.push_back({tag, std::move(a)});
            }
        }
    }
    // the first archive that has it: (tag, bytes)
    std::optional<std::pair<std::string, Bytes>> find(const std::string &name) {
        for (auto &[tag, a] : arcs)
            for (size_t i = 0; i < a->names.size(); i++) {
                const std::string &n = a->names[i];
                if (n == name || (n.size() > name.size() && n.compare(n.size() - name.size() - 1, name.size() + 1, "/" + name) == 0)) return std::make_pair(tag, a->read(i + 1));
            }
        return std::nullopt;
    }
};

}  // namespace

int london_fade(const std::vector<std::string> &argv) {
    const Args a(argv, {"--instflags", "--include-failing", "--apply", "--restore"}, {"--project", "--ps3-disc", "--ps3-update", "--ps3-data2", "--maps", "--out"});
    const std::string W = a.need("--project") + "/work/", OV = W + "overlay-r3/u3data/build/main/pak23/ps4/", BK = W + "backup-london/";
    const std::string maps = a.get("--maps", "all"), out = a.get("--out", "");
    const bool apply = a.has("--apply"), inst = a.has("--instflags");
    if (a.has("--restore")) {
        if (apply && game_running()) throw Stop("game running: stop it first");
        if (!fs::exists(upath(BK + "manifest.json"))) throw Stop("no backups in " + BK);
        const json::Value man = read_json(upath(BK + "manifest.json"));
        for (const auto &[f, m] : man.fields) {
            if (!fs::exists(upath(BK + f))) {
                std::printf("no backup %s\n", f.c_str());
                continue;
            }
            std::string before;
            for (const auto &[k, v] : m.fields)
                if (k == "md5_before") before = v.s;
            if (md5_hex(read_file(upath(BK + f))) != before) throw Stop("backup md5 differs: " + f);
            std::printf("restore %s\n", f.c_str());
            if (apply) copy2(upath(BK + f), upath(OV + f));
        }
        std::printf("%s\n", apply ? "restored" : "dry run: nothing written (use --restore --apply)");
        return 0;
    }
    if (apply && game_running()) throw Stop("game running: stop it first");
    if (!out.empty()) fs::create_directories(upath(out));
    json::Value manifest = fs::exists(upath(BK + "manifest.json")) ? read_json(upath(BK + "manifest.json")) : json::Value::object();
    auto entry = [&](const std::string &f) -> json::Value * {
        for (auto &[k, v] : manifest.fields)
            if (k == f) return &v;
        return nullptr;
    };
    auto field = [](json::Value &o, const std::string &k) -> json::Value & {
        for (auto &[x, v] : o.fields)
            if (x == k) return v;
        o.fields.push_back({k, json::Value()});
        return o.fields.back().second;
    };
    std::vector<std::string> targets;
    for (const auto &e : fs::directory_iterator(upath(OV))) targets.push_back(utf8(e.path().filename()));
    std::sort(targets.begin(), targets.end());
    auto ends = [](const std::string &s, const std::string &x) { return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0; };
    std::unique_ptr<Ps3Paks> ps3;
    const std::map<std::string, bool> SUN = {{"ldn-train-multi.pak", true}, {"ldn-train-multi-lit.pak", true}};
    const float SUN_PS3 = 886.71f;
    for (const std::string &f : targets) {
        if (!ends(f, ".pak") || ends(f, "-ingame12.pak") || ends(f, "-killvis.pak")) continue;
        if (maps == "ldn" && !(f.rfind("ldn-", 0) == 0 || f.rfind("coop-ldn", 0) == 0)) continue;
        if (f == "dig-coop-approach.pak" && !a.has("--include-failing")) {
            std::printf("%-45s skipped: fails crashcheck already (use --include-failing)\n", f.c_str());
            continue;
        }
        const Bytes raw = read_file(upath(OV + f));
        std::unique_ptr<Pak> q;
        try {
            q = std::make_unique<Pak>(raw, f);
        } catch (const std::exception &e) {
            std::printf("%-45s unreadable %s\n", f.c_str(), e.what());
            continue;
        }
        size_t nneed = 0;
        for (const Proto4 &x : ps4_protos(*q)) nneed += flagged(x) && !x.fade;
        Bytes nw = raw;
        std::vector<std::string> notes;
        if (nneed || inst) {
            if (!ps3) ps3 = std::make_unique<Ps3Paks>(a.need("--ps3-disc"), a.need("--ps3-update"), a.need("--ps3-data2"));
            auto src = ps3->find(f);
            if (!src) {
                std::printf("%-45s %zu fade-flagged prototypes without record, NO PS3 SOURCE - skipped\n", f.c_str(), nneed);
                continue;
            }
            Report rep;
            std::optional<Bytes> data;
            try {
                data = fade_fix(*q, Pak(src->second, f), inst, rep);
            } catch (const std::exception &e) {
                std::printf("%-45s FAILED %s\n", f.c_str(), e.what());
                continue;
            }
            if (rep.mism) {
                std::printf("%-45s prototype mismatch against PS3 %s (%s) - skipped\n", f.c_str(), src->first.c_str(), rep.mism_text.c_str());
                continue;
            }
            if (data) {
                nw = *data;
                notes.push_back(u3::fmt("fade records added %zu/%zu (PS3 %s)", rep.fixed, rep.flagged, src->first.c_str()));
                if (inst && rep.inst) notes.push_back(u3::fmt("instance flag words changed %zu", rep.inst));
            }
        }
        if (SUN.count(f)) {   // the stored float32 bits, not a tolerance: the converted pak holds 886.7106
            const Pak sq(nw, f);
            const auto lts = sq.items_of("LIGHT_TABLE_1");
            if (lts.empty()) throw Stop("IndexError: no LIGHT_TABLE_1");
            const Ref sun = bg::need(sq, lts[0].page, lts[0].data_off() + 0x48);
            const uint64_t at = sq.pbase(sun.first) + sun.second + 0x38;
            if (std::memcmp(&nw.at(at), &SUN_PS3, 4) != 0) {
                float old;
                std::memcpy(&old, &nw[at], 4);
                std::memcpy(&nw[at], &SUN_PS3, 4);
                notes.push_back(u3::fmt("sun %.4g -> %.2f", double(old), double(SUN_PS3)));
            }
        }
        if (nw == raw) {
            std::printf("%-45s ok (nothing to do)\n", f.c_str());
            continue;
        }
        std::string nj;
        for (size_t k = 0; k < notes.size(); k++) nj += (k ? "; " : "") + notes[k];
        std::printf("%-45s %s  (%zu -> %zu bytes)\n", f.c_str(), nj.c_str(), raw.size(), nw.size());
        std::fflush(stdout);
        if (!out.empty()) write_file_replace(upath(out) / upath(f), nw);
        if (apply) {
            fs::create_directories(upath(BK));
            if (!fs::exists(upath(BK + f))) {
                copy2(upath(OV + f), upath(BK + f));
                json::Value e = json::Value::object();
                e.fields.push_back({"md5_before", json::Value::text(md5_hex(raw))});
                if (json::Value *x = entry(f)) *x = e;
                else manifest.fields.push_back({f, e});
            } else {
                json::Value *x = entry(f);
                std::string before;
                if (x)
                    for (const auto &[k, v] : x->fields)
                        if (k == "md5_before") before = v.s;
                if (!x || md5_hex(read_file(upath(BK + f))) != before) throw Stop("existing backup of " + f + " does not match the manifest");
            }
            write_file_replace(upath(OV + f), nw);
            json::Value &e = *entry(f);
            field(e, "md5_after") = json::Value::text(md5_hex(nw));
            json::Value nl;
            nl.kind = json::Value::Array;
            for (const auto &n : notes) nl.items.push_back(json::Value::text(n));
            field(e, "notes") = nl;
            std::string js;   // json.dump to a text-mode file on Windows: CRLF line ends
            for (char c : json_dump_indent1(manifest)) js += c == 10 ? std::string{char(13), char(10)} : std::string(1, c);
            write_file_replace(upath(BK + "manifest.json"), Bytes(js.begin(), js.end()));
        }
    }
    if (out.empty() && !apply) std::printf("dry run: nothing written (use --out DIR or --apply)\n");
    return 0;
}

}  // namespace u3
