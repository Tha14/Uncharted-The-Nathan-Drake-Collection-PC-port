// dctools/ndpak.cpp: see ndpak.hpp
#include "ndpak.hpp"

#include "u3fix.hpp"

#include <algorithm>
#include <cstdio>

namespace u3 {

Pak::Pak(const fs::path &p) : d(read_file(p)), path(utf8(p)) { parse(); }
Pak::Pak(Bytes data, std::string name) : d(std::move(data)), path(std::move(name)) { parse(); }

uint16_t Pak::u16(uint64_t o) const {
    if (o + 2 > d.size()) throw Stop(fmt("%s: read past the end at %#llx", path.c_str(), (unsigned long long)o));
    return ps3 ? uint16_t(d[o] << 8 | d[o + 1]) : uint16_t(d[o] | d[o + 1] << 8);
}
uint32_t Pak::u32(uint64_t o) const {
    if (o + 4 > d.size()) throw Stop(fmt("%s: read past the end at %#llx", path.c_str(), (unsigned long long)o));
    return ps3 ? uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3] : rd32le(&d[o]);
}
uint64_t Pak::u64(uint64_t o) const {
    if (o + 8 > d.size()) throw Stop(fmt("%s: read past the end at %#llx", path.c_str(), (unsigned long long)o));
    return ps3 ? uint64_t(u32(o)) << 32 | u32(o + 4) : rd64le(&d[o]);
}
std::string Pak::cstr(uint64_t o) const {
    if (o >= d.size()) return "";
    size_t e = o;
    while (e < d.size() && d[e]) e++;
    if (e == d.size()) e = d.size() - 1;   // bytes.find() == -1: the slice ends one before the end
    return e > o ? std::string(d.begin() + std::ptrdiff_t(o), d.begin() + std::ptrdiff_t(e)) : "";
}
uint64_t Pak::pbase(uint32_t page) const {
    if (page >= pages.size()) throw Stop(fmt("%s: no page %u", path.c_str(), page));
    return pages[page].file;
}

void Pak::parse() {
    if (d.size() >= 4 && d[0] == 0 && d[1] == 0 && (d[2] << 8 | d[3]) == 0x0a79) ps3 = true;
    else if (d.size() >= 2 && (d[0] | d[1] << 8) == 0x0a79) ps3 = false;
    else throw Stop(path + ": not a 0x0a79 pak");
    ptrsize = ps3 ? 4 : 8;
    pagehdr_size = ps3 ? 0x10 : 0x14;
    for (int i = 0; i < 11; i++) h[i] = u32(uint64_t(i) * 4);
    if (u16(ps3 ? 2 : 0) != 0x0a79) throw Stop(path + ": bad magic");
    for (uint32_t i = 0; i < h[4]; i++) {
        const uint64_t o = h[5] + uint64_t(i) * 12;
        pages.push_back({u32(o), u32(o + 4), u32(o + 8)});
    }
    for (uint32_t i = 0; i < h[6]; i++) {
        const uint64_t o = h[7] + uint64_t(i) * 12;
        const FixupDir fd{i32(o), i32(o + 4), i32(o + 8)};
        fixup_dirs.push_back(fd);
        if (fd.cnt <= 0) continue;
        // the fixup tables live in the header block, addressed in 0x80000-byte units
        const int64_t base = int64_t(fd.page) * 0x80000;
        if (base + fd.off + int64_t(fd.cnt) * 8 > int64_t(h[1]))
            throw Stop(fmt("fixup directory out of the header block (%d, %s, %d)", fd.page, pyhex(uint64_t(uint32_t(fd.off))).c_str(), fd.cnt));
        for (int32_t j = 0; j < fd.cnt; j++) {
            const uint64_t fo = uint64_t(base + fd.off + int64_t(j) * 8);
            fixups.push_back({u16(fo), u16(fo + 2), u32(fo + 4)});
        }
    }
    for (const Fixup &f : fixups) ptrmap[{f.src, f.off}] = f.dst;
    vram_base = int64_t(d.size()) - int64_t(h[8]);
    for (uint32_t pi = 0; pi < pages.size(); pi++) {
        const uint64_t po = pages[pi].file;
        const uint32_t psize = ps3 ? u32(po + 4) : u32(po + 0xc);
        const uint16_t n = u16(po + pagehdr_size - 2);
        PageEntries pe{psize, {}};
        for (uint32_t j = 0; j < n; j++) {
            const uint64_t eo = po + pagehdr_size + uint64_t(j) * 16;
            const uint64_t nm = u64(eo);
            const uint32_t io = u32(eo + 8);
            pe.entries.push_back({nm, io});
            items.push_back(item(pi, io));
        }
        page_entries.push_back(pe);
    }
    login_item = item(h[2], h[3]);
    const uint64_t b = pbase(login_item.page) + login_item.data_off();
    const uint32_t n = u32(b);
    for (uint32_t i = 0; i < n; i++) login.push_back({u32(b + 8 + uint64_t(i) * 8), u32(b + 12 + uint64_t(i) * 8)});
}

PakItem Pak::item(uint32_t page, uint32_t off, bool listed) const {
    const uint64_t b = pbase(page) + off;
    PakItem it;
    it.page = page;
    it.off = off;
    it.name = pstr(page, u64(b));
    it.type = pstr(page, u64(b + 8));
    it.size = u32(b + 16);
    it.listed = listed;
    return it;
}

PakItem Pak::item_at(uint32_t page, uint32_t off) const {
    for (const PakItem &it : items)
        if (it.page == page && it.off == off) return it;
    return item(page, off, false);
}

std::vector<PakItem> Pak::items_of(const std::string &type) const {
    std::vector<PakItem> v;
    for (const PakItem &it : items)
        if (it.type == type) v.push_back(it);
    return v;
}

std::optional<std::pair<uint32_t, uint64_t>> Pak::resolve(uint32_t page, uint32_t off) const {
    auto it = ptrmap.find({page, off});
    if (it == ptrmap.end()) return std::nullopt;
    return std::make_pair(it->second, pptr(page, off));
}

std::vector<std::pair<std::optional<uint32_t>, uint64_t>> Pak::vram_desc_table() const {
    std::vector<std::pair<std::optional<uint32_t>, uint64_t>> out;
    if (h[8] == 0 && !ps3 && h[9] == 0 && h[10] == 0) return out;
    const uint32_t pg = h[9], off = h[10];
    const PakItem it = item_at(pg, off);
    const uint32_t b = it.data_off(), n = pu32(pg, b);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t o = ps3 ? b + 4 + i * 4 : b + 8 + i * 8;
        if (auto r = resolve(pg, o)) out.push_back({r->first, r->second});
        else out.push_back({std::nullopt, ps3 ? pu32(pg, o) : pptr(pg, o)});
    }
    return out;
}

PakTexture Pak::vram_desc(uint32_t page, uint64_t off) const {
    const uint64_t b = pbase(page) + off;
    PakTexture r;
    if (ps3) {
        r.runtime = u32(b); r.vramOffset = u32(b + 4); r.vramSize = u32(b + 8); r.hash = u32(b + 0xc); r.type = u32(b + 0x10);
        r.gcmRemapCtl = u32(b + 0x14); r.gcmFormatCtl = u32(b + 0x18); r.mips = u32(b + 0x1c); r.width = u32(b + 0x20);
        r.height = u32(b + 0x24); r.depth = u32(b + 0x28); r.unk2c = u32(b + 0x2c);
        r.path = pstr(page, u32(b + 0x30));
    } else {
        r.runtime = u64(b); r.vramOffset = u64(b + 8); r.vramSize = u32(b + 0x10); r.hash = u32(b + 0x14); r.type = u32(b + 0x18);
        r.format = u32(b + 0x1c); r.unk20 = u32(b + 0x20); r.mips = u32(b + 0x24); r.width = u32(b + 0x28); r.height = u32(b + 0x2c);
        r.unk30 = u32(b + 0x30); r.unk34 = u32(b + 0x34);
        r.path = pstr(page, u64(b + 0x38));
    }
    return r;
}

std::vector<PakTexture> Pak::textures() const {
    std::vector<PakTexture> out;
    for (const auto &[pg, off] : vram_desc_table()) {
        if (!pg) continue;
        PakTexture t = vram_desc(*pg, off + 0x20);
        t.page = *pg;
        t.off = uint32_t(off);
        out.push_back(t);
    }
    return out;
}

Bytes Pak::vram_bytes(uint64_t off, uint64_t size) const {
    const int64_t a = std::clamp<int64_t>(vram_base + int64_t(off), 0, int64_t(d.size()));
    const int64_t e = std::clamp<int64_t>(vram_base + int64_t(off + size), a, int64_t(d.size()));
    return Bytes(d.begin() + a, d.begin() + e);
}

std::vector<std::string> Pak::validate() const {
    std::vector<std::string> errs;
    if (h[6] != 8) errs.push_back("numFixupTables != 8");
    uint64_t end = h[1];
    for (size_t i = 0; i < pages.size(); i++) {
        if (pages[i].file < end) errs.push_back(fmt("page %zu overlaps previous data", i));
        if (page_entries[i].size != pages[i].size)
            errs.push_back(fmt("page %zu header size %s != entry %s", i, pyhex(page_entries[i].size).c_str(), pyhex(pages[i].size).c_str()));
        end = uint64_t(pages[i].file) + pages[i].size;
    }
    if (int64_t(end) > vram_base) errs.push_back("pages overlap vram data");
    for (const Fixup &f : fixups) {
        const std::string t = fmt("(%u, %u, %u)", f.src, f.dst, f.off);
        if (f.src >= pages.size() || f.dst >= pages.size()) { errs.push_back("fixup page out of range " + t); continue; }
        if (uint64_t(f.off) + ptrsize > pages[f.src].size) { errs.push_back("fixup offset out of page " + t); continue; }
        const uint64_t v = pptr(f.src, f.off);
        if (v > pages[f.dst].size) errs.push_back("pointer value out of dst page " + t + " -> " + pyhex(v));
    }
    for (const auto &[pg, off] : login)
        if (pg >= pages.size() || off >= pages[pg].size) errs.push_back(fmt("login entry out of range (%u, %u)", pg, off));
    for (const PakTexture &t : textures())
        if (t.vramOffset + t.vramSize > h[8]) errs.push_back("texture " + t.path + " vram range out of bounds");
    return errs;
}

namespace {
std::string latin1_utf8(const std::string &s) {   // printed as Python prints a latin-1 str
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) o += char(c);
        else { o += char(0xc0 | c >> 6); o += char(0x80 | (c & 0x3f)); }
    }
    return o;
}
std::string padl(const std::string &s, size_t w) { return s.size() >= w ? s : std::string(w - s.size(), ' ') + s; }
}  // namespace

std::string pak_summary(const Pak &p, bool items, bool textures) {
    std::string o;
    o += latin1_utf8(p.path) + "  [" + (p.ps3 ? "ps3" : "ps4") + "]  size=" + pyhex(p.d.size()) + "\n";
    static const char *const K[] = {"magic", "flags", "hdrSize", "loginPage", "loginOff", "pageCt", "pageEntryOff", "numFixupTables",
                                    "fixupTableOff", "vramSize", "vramDescPage", "vramDescOff"};
    const uint32_t v[12] = {p.h[0] & 0xffff, p.h[0] >> 16, p.h[1], p.h[2], p.h[3], p.h[4], p.h[5], p.h[6], p.h[7], p.h[8], p.h[9], p.h[10]};
    o += "  header:";
    for (int i = 0; i < 12; i++) o += std::string(" ") + K[i] + "=" + pyhex(v[i]);
    o += "\n";
    for (size_t i = 0; i < p.pages.size(); i++)
        o += fmt("  page %zu: file %s size %s  entries=%zu\n", i, pyhex(p.pages[i].file).c_str(), pyhex(p.pages[i].size).c_str(), p.page_entries[i].entries.size());
    o += fmt("  fixups: %zu  vram base %s size %s\n", p.fixups.size(), (p.vram_base < 0 ? "-" + pyhex(uint64_t(-p.vram_base)) : pyhex(uint64_t(p.vram_base))).c_str(), pyhex(p.h[8]).c_str());
    std::string lt;
    for (const auto &[pg, off] : p.login) lt += (lt.empty() ? "" : ", ") + latin1_utf8(p.item_at(pg, off).type) + fmt("@p%u+", pg) + pyhex(off);
    o += fmt("  login table (%zu): ", p.login.size()) + lt + "\n";
    std::map<std::string, int> c;
    for (const PakItem &it : p.items) c[it.type]++;
    std::string ty;
    for (const auto &[k, n] : c) ty += (ty.empty() ? "" : ", ") + latin1_utf8(k) + " x" + std::to_string(n);
    o += "  item types: " + ty + "\n";
    if (items)
        for (const PakItem &it : p.items) {
            std::string t = latin1_utf8(it.type);
            if (t.size() < 22) t += std::string(22 - t.size(), ' ');
            o += fmt("    p%u+%06x ", it.page, it.off) + t + " size " + padl(pyhex(it.size), 7) + "  " + latin1_utf8(it.name) + "\n";
        }
    const auto errs = p.validate();
    std::string e;
    for (size_t i = 0; i < errs.size() && i < 10; i++) e += (i ? "; " : "") + errs[i];
    o += "  validate: " + (errs.empty() ? std::string("OK") : latin1_utf8(e)) + "\n";
    if (textures)
        for (const PakTexture &t : p.textures()) {
            const std::string f = p.ps3 ? fmt("gcm=%08x/%08x", t.gcmRemapCtl, t.gcmFormatCtl) : "fmt=" + pyhex(t.format);
            o += fmt("    tex %08x %4ux%-4u mips %2u vram ", t.hash, t.width, t.height, t.mips) + padl(pyhex(t.vramOffset), 9) + " +" + padl(pyhex(t.vramSize), 8) + " " + f + " " +
                 latin1_utf8(t.path) + "\n";
        }
    return o;
}

}  // namespace u3

namespace u3 {
int pakinfo(const std::vector<std::string> &argv) {
    bool tex = false, items = true;
    std::vector<std::string> paks;
    for (const std::string &a : argv) {
        if (a == "--textures") tex = true;
        else if (a == "--noitems") items = false;
        else paks.push_back(a);
    }
    for (const std::string &f : paks) {
        Pak p(upath(f));
        p.path = f;
        std::fputs(pak_summary(p, items, tex).c_str(), stdout);
    }
    return 0;
}
}  // namespace u3
