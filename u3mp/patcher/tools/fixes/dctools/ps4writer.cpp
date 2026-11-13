// dctools/ps4writer.cpp: see ps4writer.hpp
#include "ps4writer.hpp"

#include "u3fix.hpp"

namespace u3 {
namespace {

constexpr uint8_t PAD = 0xaa;
size_t align(size_t n, size_t a) { return (n + a - 1) / a * a; }
const void *key(const PtrTarget *t) { return t; }
void put16(Bytes &o, size_t at, uint32_t v) { o[at] = uint8_t(v); o[at + 1] = uint8_t(v >> 8); }
void put32(Bytes &o, size_t at, uint32_t v) { wr32le(&o[at], v); }
void put64(Bytes &o, size_t at, uint64_t v) { wr64le(&o[at], v); }
void app32(Bytes &o, uint32_t v) { for (int i = 0; i < 4; i++) o.push_back(uint8_t(v >> (8 * i))); }
void app16(Bytes &o, uint32_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }

}  // namespace

Blob &Blob::u8(uint32_t v) { b.push_back(uint8_t(v)); return *this; }
Blob &Blob::u16(uint32_t v) { app16(b, v); return *this; }
Blob &Blob::u32(uint64_t v) { app32(b, uint32_t(v)); return *this; }
Blob &Blob::i32(int32_t v) { app32(b, uint32_t(v)); return *this; }
Blob &Blob::u64(uint64_t v) { app32(b, uint32_t(v)); app32(b, uint32_t(v >> 32)); return *this; }
Blob &Blob::raw(const Bytes &d) { b.insert(b.end(), d.begin(), d.end()); return *this; }
Blob &Blob::raw(const std::string &d) { b.insert(b.end(), d.begin(), d.end()); return *this; }
Blob &Blob::pad(size_t a, uint8_t fill) {
    while (b.size() % a) b.push_back(fill);
    return *this;
}
Blob &Blob::ptr(const std::shared_ptr<PtrTarget> &target, int64_t off) {
    if (target) relocs.push_back({b.size(), target, off});
    return u64(0);
}
size_t Blob::cstr(const std::string &s) {
    const size_t o = b.size();
    raw(s);
    b.push_back(0);
    return o;
}
size_t Blob::embed(const std::shared_ptr<Blob> &child, size_t a, uint8_t fill) {
    pad(a, fill);
    child->parent = this;
    child->off_in_parent = b.size();
    raw(child->b);
    for (const Reloc &r : child->relocs) relocs.push_back({child->off_in_parent + r.pos, r.target, r.off});
    child->relocs.clear();
    return child->off_in_parent;
}
std::pair<const void *, int64_t> Blob::root() const {
    int64_t o = 0;
    const Blob *n = this;
    while (n->parent) {
        o += int64_t(n->off_in_parent);
        n = n->parent;
    }
    return {key(n), o};
}

size_t StrPool::get(const std::string &s) {
    auto it = map.find(s);
    if (it != map.end()) return it->second;
    return map[s] = blob->cstr(s);
}

std::shared_ptr<PakWItem> Page::item(const std::string &type, std::shared_ptr<Blob> data, const std::string &name, bool listed) {
    auto it = std::make_shared<PakWItem>();
    it->type = type;
    it->name = name;
    it->data = data ? std::move(data) : std::make_shared<Blob>(type);
    it->listed = listed;
    it->page = this;
    parts.push_back(it);
    return it;
}
std::shared_ptr<RawSpan> Page::span(std::shared_ptr<RawSpan> s) {
    s->page = this;
    parts.push_back(s);
    return s;
}

RawPage::RawPage(PakWriter *w, size_t index, Bytes d, std::vector<std::pair<uint64_t, size_t>> fixups)
    : Page(w, index), data(std::move(d)), src_fixups(std::move(fixups)) {
    name = "rawpage" + std::to_string(index);
}
void RawPage::set_u32(uint64_t off, uint32_t v) { put32(data, off, v); }
void RawPage::set_u64(uint64_t off, uint64_t v) { put64(data, off, v); }

Page &PakWriter::page() {
    pages.push_back(std::make_unique<Page>(this, pages.size()));
    return *pages.back();
}
RawPage &PakWriter::raw_page(Bytes data, std::vector<std::pair<uint64_t, size_t>> fixups) {
    auto p = std::make_unique<RawPage>(this, pages.size(), std::move(data), std::move(fixups));
    RawPage &r = *p;
    pages.push_back(std::move(p));
    return r;
}

void PakWriter::layout(Page &page) {
    if (auto *rp = dynamic_cast<RawPage *>(&page)) {
        std::set<uint64_t> red;
        for (const auto &r : rp->redirects) red.insert(r.off);
        rp->fixups.clear();
        for (const auto &[o, d] : rp->src_fixups)
            if (!red.count(o)) rp->fixups.push_back({rp->index, d, o});
        rp->blob_abs.clear();
        rp->blob_abs[key(static_cast<const PtrTarget *>(rp))] = {rp, 0};
        rp->bytes = rp->data;
        for (const auto &b : rp->appended) {
            while (rp->bytes.size() % 16) rp->bytes.push_back(PAD);
            rp->blob_abs[key(b.get())] = {rp, rp->bytes.size()};
            rp->bytes.insert(rp->bytes.end(), b->b.begin(), b->b.end());
        }
        if (!rp->appended.empty()) put32(rp->bytes, 0xc, uint32_t(rp->bytes.size()));   // ResPageHeader.m_pageSize = the page table size
        rp->size = rp->bytes.size();
        return;
    }
    size_t nent = 0;
    for (const auto &x : page.parts) {
        if (auto *it = std::get_if<std::shared_ptr<PakWItem>>(&x)) nent += (*it)->listed ? 1 : 0;
        else nent += std::get<std::shared_ptr<RawSpan>>(x)->entries.size();
    }
    Bytes out(0x14, 0);   // link, package index, size (patched), ?, entry count
    put16(out, 0x12, uint32_t(nent));
    out.resize(out.size() + 16 * std::max<size_t>(nent, 1), 0);   // entries patched below (retail keeps one zeroed slot when there are none)
    std::vector<std::pair<uint64_t, uint64_t>> entries;
    page.fixups.clear();
    page.blob_abs.clear();
    for (const auto &x : page.parts) {
        if (auto *pit = std::get_if<std::shared_ptr<PakWItem>>(&x)) {
            PakWItem &it = **pit;
            while (out.size() % 16) out.push_back(PAD);
            if (it.phase)
                while ((out.size() + 0x20) % it.phase->first != it.phase->second) out.push_back(PAD);
            it.off = out.size();
            const size_t hdr = out.size();
            out.resize(out.size() + 0x20, 0);
            page.blob_abs[key(it.data.get())] = {&page, out.size()};
            out.insert(out.end(), it.data->b.begin(), it.data->b.end());
            size_t type_pos = 0, name_pos = 0;
            if (it.listed) {
                type_pos = out.size();
                out.insert(out.end(), it.type.begin(), it.type.end());
                out.push_back(0);
                name_pos = out.size();
                out.insert(out.end(), it.name.begin(), it.name.end());
                out.push_back(0);
            }
            put64(out, hdr, name_pos);
            put64(out, hdr + 8, type_pos);
            put32(out, hdr + 16, uint32_t(it.data->b.size()));
            put32(out, hdr + 20, 0x12345);
            put32(out, hdr + 24, 0xaaaaaaaa);
            put32(out, hdr + 28, 0);
            if (it.listed) entries.push_back({name_pos, hdr});
        } else {
            RawSpan &s = *std::get<std::shared_ptr<RawSpan>>(x);
            if (s.modulo > (uint64_t(1) << 32) && out.size() > s.phase)
                throw Stop(fmt("RawSpan pinned at %s but page %zu is already at %s (%zu entries)", pyhex(s.phase).c_str(), page.index, pyhex(out.size()).c_str(), nent));
            while (out.size() % 16 || out.size() % s.modulo != s.phase) out.push_back(PAD);
            const size_t base = out.size();
            s.off = base;
            Bytes d = s.data;
            for (const auto &[ipos, t] : s.headers) {
                put64(d, ipos, rd64le(&d[ipos]) + base);
                put64(d, ipos + 8, rd64le(&d[ipos + 8]) + base);
            }
            for (const auto &[pos, tgt] : s.internal) {
                put64(d, pos, tgt + base);
                page.fixups.push_back({page.index, page.index, base + pos});
            }
            out.insert(out.end(), d.begin(), d.end());
            for (const auto &[ipos, npos] : s.entries) entries.push_back({base + npos, base + ipos});
        }
    }
    if (entries.size() != nent) throw Stop(fmt("AssertionError: (%zu, %zu)", entries.size(), nent));
    for (size_t i = 0; i < entries.size(); i++) {
        put64(out, 0x14 + 16 * i, entries[i].first);
        put32(out, 0x14 + 16 * i + 8, uint32_t(entries[i].second));
        put32(out, 0x14 + 16 * i + 12, 0x12345);
    }
    put32(out, 0xc, uint32_t(out.size()));
    page.bytes = std::move(out);
    page.size = page.bytes.size();
}

std::map<const void *, std::pair<Page *, size_t>> PakWriter::all_abs() const {
    std::map<const void *, std::pair<Page *, size_t>> m;
    for (const auto &p : pages)
        for (const auto &[k, v] : p->blob_abs) m[k] = v;
    return m;
}

Bytes PakWriter::build() {
    for (auto &p : pages) layout(*p);
    if (login_item) {
        for (size_t i = 0; i < login_targets.size(); i++) {
            const PakWItem &t = *login_targets[i];
            const size_t tp = t.span_ref ? t.span_ref->page->index : t.page->index, to = t.span_ref ? t.span_ref->off + t.span_pos : t.off;
            put32(login_item->page->bytes, login_item->data_off() + 8 + 8 * i, uint32_t(tp));
            put32(login_item->page->bytes, login_item->data_off() + 12 + 8 * i, uint32_t(to));
        }
        login_page = std::make_pair(login_item->page->index, login_item->off);
        if (vram_table_item) vram_table_loc = std::make_pair(vram_table_item->page->index, vram_table_item->off);
        else vram_table_loc.reset();
    } else if (!login_page) {
        throw Stop("AssertionError: no login table");
    } else if (vram_table_item) {   // raw retail pages, the VRAM desc table an item on a new page
        vram_table_loc = std::make_pair(vram_table_item->page->index, vram_table_item->off);
    }
    if (vram_table_blob) {
        const auto [vpg, voff] = all_abs().at(key(vram_table_blob.get()));
        vram_table_loc = std::make_pair(vpg->index, voff);
    }
    const auto abs = all_abs();
    auto locate = [&](const std::shared_ptr<PtrTarget> &t) {
        const auto [r, roff] = t->root();
        auto it = abs.find(r);
        if (it == abs.end()) throw Stop("pointer target blob '" + t->name + "' not placed");
        return std::make_tuple(it->second.first, it->second.second, roff);
    };
    std::vector<Page::Fix> fix;
    for (const auto &p : pages) fix.insert(fix.end(), p->fixups.begin(), p->fixups.end());
    for (const auto &p : pages) {
        if (auto *rp = dynamic_cast<RawPage *>(p.get())) {
            for (const auto &r : rp->redirects) {
                const auto [tp, tbase, roff] = locate(r.target);
                put64(rp->bytes, r.off, uint64_t(int64_t(tbase) + roff + r.toff));
                fix.push_back({rp->index, tp->index, r.off});
            }
            for (const auto &b : rp->appended) {
                const size_t dbase = rp->blob_abs.at(key(b.get())).second;
                for (const auto &rl : b->relocs) {
                    const auto [tp, tbase, roff] = locate(rl.target);
                    put64(rp->bytes, dbase + rl.pos, uint64_t(int64_t(tbase) + roff + rl.off));
                    fix.push_back({rp->index, tp->index, dbase + rl.pos});
                }
            }
            continue;
        }
        for (const auto &x : p->parts) {
            auto *pit = std::get_if<std::shared_ptr<PakWItem>>(&x);
            if (!pit) continue;
            const Blob &d = *(*pit)->data;
            const size_t dbase = p->blob_abs.at(key(&d)).second;
            for (const auto &rl : d.relocs) {
                const auto [tp, tbase, roff] = locate(rl.target);
                put64(p->bytes, dbase + rl.pos, uint64_t(int64_t(tbase) + roff + rl.off));
                fix.push_back({p->index, tp->index, dbase + rl.pos});
            }
        }
    }
    fixups = fix;
    const size_t page_ct = pages.size(), fixdir_off = 0x2c + 12 * page_ct, fix_off = align(fixdir_off + 8 * 12, 16), hdr_size = fix_off + 8 * fix.size();
    std::vector<size_t> page_offs;
    size_t o = align(hdr_size, 16);
    for (const auto &p : pages) {
        page_offs.push_back(o);
        o = align(o + p->size, 16);
    }
    // VRAM data follows the last page directly (the loader reads it from the end of the last page)
    const size_t vram_base = pages.empty() ? o : page_offs.back() + pages.back()->size;
    const auto [lp, lo] = *login_page;
    const auto [vp, vo] = vram_table_loc ? *vram_table_loc : std::make_pair(lp, size_t(0));
    Bytes out;
    app16(out, 0x0a79);
    app16(out, flags);
    for (uint64_t v : {uint64_t(hdr_size), uint64_t(lp), uint64_t(lo), uint64_t(page_ct), uint64_t(0x2c), uint64_t(8), uint64_t(fixdir_off), uint64_t(vram.size()), uint64_t(vp), uint64_t(vo)})
        app32(out, uint32_t(v));
    for (size_t i = 0; i < pages.size(); i++) {
        app32(out, uint32_t(page_offs[i]));
        app32(out, uint32_t(pages[i]->size));
        app32(out, 0);
    }
    app32(out, 0);
    app32(out, uint32_t(fix_off));
    app32(out, uint32_t(fix.size()));
    out.resize(out.size() + 12 * 7, 0);
    while (out.size() < fix_off) out.push_back(PAD);
    for (const auto &f : fix) {
        app16(out, uint32_t(f.src));
        app16(out, uint32_t(f.dst));
        app32(out, uint32_t(f.off));
    }
    if (out.size() != hdr_size) throw Stop("AssertionError: header size");
    for (size_t i = 0; i < pages.size(); i++) {
        while (out.size() < page_offs[i]) out.push_back(PAD);
        out.insert(out.end(), pages[i]->bytes.begin(), pages[i]->bytes.end());
    }
    while (out.size() < vram_base) out.push_back(PAD);
    out.insert(out.end(), vram.begin(), vram.end());
    return out;
}

std::shared_ptr<PakWItem> make_login_and_vram_page(PakWriter &w, Page &page, const std::vector<std::shared_ptr<PakWItem>> &login_items,
                                                   const std::vector<std::shared_ptr<PakWItem>> &vram_items) {
    auto lb = std::make_shared<Blob>("login");
    lb->u32(login_items.size()).u32(0);
    lb->b.resize(lb->b.size() + 8 * login_items.size(), 0);   // (page, offset) pairs, patched in build()
    auto login = page.item("PAK_LOGIN_TABLE", lb, "", false);
    w.login_item = login;
    w.login_targets = login_items;
    if (!vram_items.empty()) {
        auto vb = std::make_shared<Blob>("vramtable");
        vb->u32(vram_items.size()).u32(0);
        for (const auto &it : vram_items) vb->ptr(it->data, -0x20);   // the VRAM_DESC item header
        w.vram_table_item = page.item("VRAM_DESC_TABLE", vb, "", false);
    }
    return login;
}

}  // namespace u3
