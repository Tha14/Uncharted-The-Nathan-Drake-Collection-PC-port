// importer/texts.cpp: the text banks (text1/<language>.<category>) completed with the multiplayer strings
// (u2mp/docs/TEXTS.md).
//
// A bank (big endian on PS3 and PS4): u32 count, count x {u32 hash, u32 offset} sorted by hash, then the strings
// (NUL-terminated, offsets from the start of the string area). PS3 and PS4 number the languages alike (2 English ...
// 18 Polish); PS4 adds 19 Latin American Spanish, 20 Brazilian Portuguese, 21 Greek, 22 Czech and 23 Turkish, whose
// banks hold "_" placeholders for many texts.
//
// A real PS4 string is never changed. A text the PS4 bank lacks, or has only as "_", comes from the GOTY PS3 bank of
// the language (newest archive first), then for 19 / 20 from the completed Spanish / Portuguese bank. New strings go
// after the existing ones; placeholders first (in bank order), then the missing ids (in source order). English is
// always written: the runtime shows it for what a language still lacks.
#include "u2import.hpp"

namespace u2imp {
namespace {

const int ENGLISH = 2;
const std::map<int, int> RELATED = {{19, 6}, {20, 9}};   // language -> the one it shows before English

uint32_t be(const Bytes &d, size_t o) {
    if (o + 4 > d.size()) throw std::runtime_error("struct.error: text bank truncated");
    return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}
void put_be(Bytes &o, uint32_t v) {
    for (int k = 3; k >= 0; k--) o.push_back(uint8_t(v >> (8 * k)));
}

struct Bank {
    std::vector<std::pair<uint32_t, uint32_t>> recs;   // hash, offset (bank order)
    Bytes blob;
};
Bank parse(const Bytes &d) {
    Bank b;
    const uint32_t n = be(d, 0);
    for (uint32_t i = 0; i < n; i++) b.recs.push_back({be(d, 4 + 8 * size_t(i)), be(d, 8 + 8 * size_t(i))});
    b.blob.assign(d.begin() + long(4 + 8 * size_t(n)), d.end());
    return b;
}
std::string string_at(const Bytes &blob, uint32_t off) {
    size_t e = off;
    while (e < blob.size() && blob[e]) e++;
    if (e >= blob.size()) throw std::runtime_error("ValueError: subsection not found");
    return std::string(blob.begin() + long(off), blob.begin() + long(e));
}
// hash -> string, in bank order (later duplicates replace the value, keep the place)
using Strings = std::vector<std::pair<uint32_t, std::string>>;
Strings strings(const Bytes &d) {
    const Bank b = parse(d);
    Strings s;
    std::map<uint32_t, size_t> at;
    for (const auto &[h, o] : b.recs) {
        const auto it = at.find(h);
        if (it != at.end()) s[it->second].second = string_at(b.blob, o);
        else at[h] = s.size(), s.push_back({h, string_at(b.blob, o)});
    }
    return s;
}
bool placeholder(const std::string &s) { return s == "_"; }

struct Filled {
    std::optional<Bytes> data;
    size_t added = 0, replaced = 0;
};
Filled fill(const Bytes &bank, const std::vector<Bytes> &sources) {
    const Bank b = parse(bank);
    const Strings have = strings(bank);
    std::vector<std::map<uint32_t, std::string>> src;
    std::vector<Strings> src_order;
    for (const Bytes &s : sources)
        if (!s.empty()) {
            src_order.push_back(strings(s));
            std::map<uint32_t, std::string> m(src_order.back().begin(), src_order.back().end());
            src.push_back(m);
        }
    Bytes out = b.blob;
    std::map<uint32_t, uint32_t> index;
    for (const auto &[h, o] : b.recs) index[h] = o;
    auto pick = [&](uint32_t h) -> const std::string * {
        for (const auto &s : src) {
            const auto it = s.find(h);
            if (it != s.end() && !placeholder(it->second)) return &it->second;
        }
        return nullptr;
    };
    Filled f;
    auto add = [&](uint32_t h, const std::string &v) {
        index[h] = uint32_t(out.size());
        out.insert(out.end(), v.begin(), v.end());
        out.push_back(0);
    };
    for (const auto &[h, s] : have)
        if (placeholder(s))
            if (const std::string *v = pick(h)) add(h, *v), f.replaced++;
    for (const Strings &s : src_order)
        for (const auto &[h, _] : s)
            if (!index.count(h))
                if (const std::string *v = pick(h)) add(h, *v), f.added++;
    if (!f.added && !f.replaced) return {};
    Bytes d;
    put_be(d, uint32_t(index.size()));
    for (const auto &[h, o] : index) put_be(d, h), put_be(d, o);
    d.insert(d.end(), out.begin(), out.end());
    f.data = d;
    return f;
}

}  // namespace

std::map<int, std::pair<std::optional<Bytes>, TextCounts>> texts_complete(const std::vector<std::pair<int, Bytes>> &ps4,
                                                                          const std::map<int, std::vector<Bytes>> &goty) {
    std::vector<std::pair<int, Bytes>> step1;
    std::map<int, TextCounts> counts;
    std::map<int, const Bytes *> original;
    for (const auto &[lang, bank] : ps4) {
        const auto g = goty.find(lang);
        Filled f = fill(bank, g == goty.end() ? std::vector<Bytes>{} : g->second);
        step1.push_back({lang, f.data ? *f.data : bank});
        counts[lang].goty = (long long)(f.added + f.replaced);
        original[lang] = &bank;
    }
    auto find1 = [&](int lang) -> const Bytes * {
        for (const auto &[l, b] : step1)
            if (l == lang) return &b;
        return nullptr;
    };
    Strings english;
    if (const Bytes *e = find1(ENGLISH)) english = strings(*e);
    std::map<int, std::pair<std::optional<Bytes>, TextCounts>> out;
    for (const auto &[lang, bank] : step1) {
        Bytes data = bank;
        const auto rel = RELATED.find(lang);
        if (rel != RELATED.end())
            if (const Bytes *r = find1(rel->second)) {
                Filled f = fill(data, {*r});
                if (f.data) {
                    data = *f.data;
                    counts[lang].related = (long long)(f.added + f.replaced);
                }
            }
        std::map<uint32_t, std::string> have;
        for (const auto &[h, s] : strings(data)) have[h] = s;
        long long left = 0;
        for (const auto &[h, s] : english) {
            const auto it = have.find(h);
            if (!placeholder(s) && (it == have.end() || placeholder(it->second))) left++;
        }
        counts[lang].left = left;
        if (data != *original[lang] || lang == ENGLISH) out[lang] = {data, counts[lang]};
        else out[lang] = {std::nullopt, counts[lang]};
    }
    return out;
}

}  // namespace u2imp
