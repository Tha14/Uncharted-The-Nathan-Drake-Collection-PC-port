// Recipe step vram-gap: the paks the v7.38 fade-record fix rebuilt with a gap between the last page and the VRAM data.
//   u3fix vram-gap --project P [--apply] [--restore] [--dir D]
//
// The PS4 pak loader takes the VRAM bytes from where the last page ends (the header has only the VRAM size, no
// offset). The rebuilt paks aligned the VRAM start to 16, so the ones whose last page ended at 8 mod 16 got 8 pad bytes
// and fail to load (level error 0xc). The repair deletes the pad bytes between the last page end and (file length -
// VRAM size); no other byte changes. Scans overlay-r3 pak23/ps4 and actor33/ps4 (or --dir D, dry run only); a gap
// outside 1..15 is reported and left alone. --apply backs each pak up once to work/backup-vramgap/<rel> and keeps
// work/backup-vramgap/manifest.json (md5 before / after, gap); --restore [--apply] copies the backups back. --apply and
// --restore --apply refuse while the game runs. Replaces fix_vram_gap.py (same messages and manifest).
#include "u3fix.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace u3 {
namespace {

struct Gap {
    int64_t end, vram_base, gap;
};
// last page end, VRAM start, gap; false when it is not a PS4 pak this understands
bool analyse_header(const Bytes &h, uint64_t file_size, const Bytes *table, Gap &g) {
    if (h.size() < 0x2c || rd16le(h, 0) != 0x0a79) return false;
    // header: u16 magic, u16 flags, u32 header size, login page, login offset, page count (+0x10), page table (+0x14),
    // fixup count, fixup offset, VRAM size (+0x20), VRAM page, VRAM offset
    const uint32_t page_ct = rd32le(h, 0x10), page_ent_off = rd32le(h, 0x14), vram_size = rd32le(h, 0x20);
    if (page_ct == 0 || page_ct > 4096 || uint64_t(page_ent_off) + 12ull * page_ct > file_size) return false;
    if (!table || table->size() < 12ull * page_ct) return false;
    int64_t end = 0;
    for (uint32_t i = 0; i < page_ct; i++) {
        const uint64_t off = rd32le(*table, 12ull * i), size = rd32le(*table, 12ull * i + 4);
        if (off + size > file_size) return false;
        end = std::max<int64_t>(end, int64_t(off + size));
    }
    g.end = end;
    g.vram_base = int64_t(file_size) - int64_t(vram_size);
    g.gap = g.vram_base - end;
    return true;
}
bool analyse(const Bytes &d, Gap &g) {
    if (d.size() < 0x2c) return false;
    const uint32_t page_ct = rd32le(d, 0x10), page_ent_off = rd32le(d, 0x14);
    if (page_ct == 0 || page_ct > 4096 || uint64_t(page_ent_off) + 12ull * page_ct > d.size()) return false;
    const Bytes h(d.begin(), d.begin() + 0x2c);
    const Bytes t(d.begin() + page_ent_off, d.begin() + page_ent_off + 12ull * page_ct);
    return analyse_header(h, d.size(), &t, g);
}
bool analyse_file(const fs::path &p, Gap &g) {
    std::error_code ec;
    const uintmax_t sz = fs::file_size(p, ec);
    if (ec || sz < 0x2c) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    Bytes h(0x2c);
    f.read(reinterpret_cast<char *>(h.data()), std::streamsize(h.size()));
    if (f.gcount() != std::streamsize(h.size())) return false;
    const uint32_t page_ct = rd32le(h, 0x10), page_ent_off = rd32le(h, 0x14);
    if (page_ct == 0 || page_ct > 4096 || uint64_t(page_ent_off) + 12ull * page_ct > sz) return false;
    Bytes t(12ull * page_ct);
    f.seekg(std::streamoff(page_ent_off));
    f.read(reinterpret_cast<char *>(t.data()), std::streamsize(t.size()));
    if (f.gcount() != std::streamsize(t.size())) return false;
    return analyse_header(h, sz, &t, g);
}

json::Value *field(json::Value &obj, const std::string &k) { return obj.get(k); }

void save_manifest(const fs::path &man, const json::Value &m) {   // the script wrote it in text mode: CRLF lines
    Bytes b;
    for (char c : json_dump_indent1(m)) {
        if (c == '\n') b.push_back('\r');
        b.push_back(uint8_t(c));
    }
    write_file_replace(man, b);
}

}  // namespace

int vram_gap(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--restore"}, {"--project", "--dir"});
    bool apply = a.has("--apply");
    const std::string p = win_path(a.need("--project"));
    const std::string ov = p + "\\work\\overlay-r3\\u3data\\build\\main", bk = p + "\\work\\backup-vramgap", man = bk + "\\manifest.json";
    if (a.has("--restore")) {
        if (apply && game_running()) throw Stop("game running: stop it first");
        if (!fs::exists(upath(man))) throw Stop("no backups in " + bk);
        const json::Value m = read_json(upath(man));
        for (const auto &[rel, e] : m.fields) {
            const std::string src = bk + "\\" + rel;
            if (!fs::exists(upath(src))) {
                std::printf("no backup %s\n", rel.c_str());
                continue;
            }
            const json::Value *before = e.get("md5_before");
            if (!before || md5_hex(read_file(upath(src))) != before->s) throw Stop("backup md5 differs: " + rel);
            std::printf("restore %s\n", rel.c_str());
            if (apply) copy2(upath(src), upath(ov + "\\" + rel));
        }
        std::printf("%s\n", apply ? "restored" : "dry run: nothing written (use --restore --apply)");
        return 0;
    }
    struct Dir {
        std::string path, rel;   // rel: the folder relative to the overlay (for the backup and the manifest)
    };
    std::vector<Dir> dirs;
    if (a.val.count("--dir")) {
        dirs.push_back({a.get("--dir", ""), ""});
        apply = false;
    } else {
        dirs = {{ov + "\\pak23\\ps4", "pak23\\ps4"}, {ov + "\\actor33\\ps4", "actor33\\ps4"}};
    }
    if (apply && game_running()) throw Stop("game running: stop it first");
    json::Value m = fs::exists(upath(man)) ? read_json(upath(man)) : json::Value::object();
    int found = 0, bad = 0, scanned = 0;
    for (const Dir &dd : dirs) {
        if (!fs::is_directory(upath(dd.path))) continue;
        std::vector<std::string> names;
        for (const auto &e : fs::directory_iterator(upath(dd.path))) names.push_back(utf8(e.path().filename()));
        std::sort(names.begin(), names.end());
        for (const std::string &f : names) {
            if (f.size() < 4 || f.compare(f.size() - 4, 4, ".pak") != 0) continue;
            const std::string path = dd.path + "\\" + f;
            Gap g;
            scanned++;
            if (!analyse_file(upath(path), g) || g.gap == 0) continue;
            if (g.gap < 0 || g.gap > 15) {
                bad++;
                std::printf("%-45s gap %lld: not a padding gap, left alone\n", f.c_str(), (long long)g.gap);
                continue;
            }
            found++;
            if (!apply) {
                const uintmax_t sz = fs::file_size(upath(path));
                std::printf("%-45s last page ends %s, VRAM at %s: %lld pad bytes -> removed (%llu -> %lld bytes)\n", f.c_str(), pyhex(uint64_t(g.end)).c_str(),
                            pyhex(uint64_t(g.vram_base)).c_str(), (long long)g.gap, (unsigned long long)sz, (long long)(int64_t(sz) - g.gap));
                continue;
            }
            const Bytes d = read_file(upath(path));
            Gap real;
            if (!analyse(d, real) || real.end != g.end || real.vram_base != g.vram_base) throw Stop("changed during scan: " + f);
            g = real;
            std::printf("%-45s last page ends %s, VRAM at %s: %lld pad bytes -> removed (%zu -> %lld bytes)\n", f.c_str(), pyhex(uint64_t(g.end)).c_str(),
                        pyhex(uint64_t(g.vram_base)).c_str(), (long long)g.gap, d.size(), (long long)(int64_t(d.size()) - g.gap));
            Bytes out(d.begin(), d.begin() + g.end);
            out.insert(out.end(), d.begin() + g.vram_base, d.end());
            Gap check;
            if (!analyse(out, check) || check.gap != 0) throw Stop("AssertionError: repair check failed");
            const std::string rel = dd.rel + "\\" + f, dst = bk + "\\" + rel;
            fs::create_directories(upath(dst).parent_path());
            if (!fs::exists(upath(dst))) {
                write_file_replace(upath(dst), d);
                fs::last_write_time(upath(dst), fs::last_write_time(upath(path)));
                json::Value e = json::Value::object();
                e.set("md5_before", json::Value::text(md5_hex(d)));
                m.set(rel, e);
            } else {
                const json::Value *e = m.get(rel), *before = e ? e->get("md5_before") : nullptr;
                if (!before || before->kind != json::Value::String || md5_hex(read_file(upath(dst))) != before->s)
                    throw Stop("existing backup of " + rel + " does not match the manifest");
            }
            write_file_replace(upath(path), out);
            json::Value *e = field(m, rel);
            e->set("md5_after", json::Value::text(md5_hex(out)));
            e->set("gap", json::Value::integer(g.gap));
            save_manifest(upath(man), m);
        }
    }
    std::printf("scanned %d paks, %d with a VRAM gap%s\n", scanned, found, bad ? fmt(", %d odd (left alone)", bad).c_str() : "");
    if (!apply) std::printf("dry run: nothing written (use --apply)\n");
    return 0;
}

}  // namespace u3
