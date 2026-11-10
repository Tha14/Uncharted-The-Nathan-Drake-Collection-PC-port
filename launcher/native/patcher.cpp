// patcher.cpp - the game starter's code patches (tools/probe_uncharted.py), part of uncharted_native.dll: byte patterns
// ("??" any byte) found in the linked game image, and the patches applied there.
//
// A patch lists variants; the first variant whose pattern occurs exactly once (counted without overlaps, from the
// start) is applied. A variant changes one place (pattern, offset, original, replacement: other bytes there stop the
// start) or several, all or none (pattern, [(offset, original, replacement), ...]: other bytes skip the variant).

#include "native.h"
#include "json.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

#include <string>
#include <vector>

namespace {

using json::Value;

struct Pattern {
    std::vector<int> bytes;   // 0..255, -1 any byte
    bool ok = true;
};

int hexdigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Pattern parse_pattern(const std::string &text) {
    Pattern p;
    std::string t;
    for (char c : text) if (c != ' ') t += c;
    if (t.size() % 2) p.ok = false;
    for (size_t i = 0; i + 1 < t.size(); i += 2) {
        if (t[i] == '?' && t[i + 1] == '?') { p.bytes.push_back(-1); continue; }
        int a = hexdigit(t[i]), b = hexdigit(t[i + 1]);
        if (a < 0 || b < 0) { p.ok = false; break; }
        p.bytes.push_back(a * 16 + b);
    }
    return p;
}

bool bytes_from_hex(const std::string &h, std::string &out) {
    out.clear();
    if (h.size() % 2) return false;
    for (size_t i = 0; i < h.size(); i += 2) {
        int a = hexdigit(h[i]), b = hexdigit(h[i + 1]);
        if (a < 0 || b < 0) return false;
        out += (char)(a * 16 + b);
    }
    return true;
}

bool matches_at(const std::string &data, size_t at, const Pattern &p) {
    if (at + p.bytes.size() > data.size()) return false;
    for (size_t k = 0; k < p.bytes.size(); ++k)
        if (p.bytes[k] >= 0 && (uint8_t)data[at + k] != p.bytes[k]) return false;
    return true;
}

// match starts, without overlaps, at most `limit` of them
std::vector<size_t> find_all(const std::string &data, const Pattern &p, size_t limit) {
    std::vector<size_t> found;
    if (p.bytes.empty()) {   // an empty pattern matches between every byte (Python's finditer)
        for (size_t i = 0; i <= data.size() && found.size() < limit; ++i) found.push_back(i);
        return found;
    }
    size_t anchor = 0;   // the first fixed byte, searched with memchr
    while (anchor < p.bytes.size() && p.bytes[anchor] < 0) ++anchor;
    size_t at = 0;
    while (found.size() < limit && at + p.bytes.size() <= data.size()) {
        if (anchor < p.bytes.size()) {
            const void *hit = std::memchr(data.data() + at + anchor, p.bytes[anchor], data.size() - at - anchor);
            if (!hit) break;
            size_t start = (size_t)((const char *)hit - data.data()) - anchor;
            if (start + p.bytes.size() > data.size()) break;
            if (matches_at(data, start, p)) { found.push_back(start); at = start + p.bytes.size(); }
            else at = start + 1;
        } else {
            found.push_back(at);
            at += p.bytes.size();
        }
    }
    return found;
}

std::string hex_offset(long long v) {
    char b[32];
    std::snprintf(b, sizeof b, v < 0 ? "-0x%llx" : "0x%llx", (unsigned long long)(v < 0 ? -v : v));
    return b;
}

bool range_equals(const std::string &data, long long at, const std::string &original) {
    if (at < 0 || (size_t)at > data.size()) return original.empty() && at >= 0;
    return data.compare((size_t)at, original.size(), original) == 0 && (size_t)at + original.size() <= data.size();
}

void put(std::string &data, long long at, const std::string &replacement) {
    if ((size_t)at + replacement.size() > data.size()) data.resize((size_t)at + replacement.size());
    std::memcpy(&data[(size_t)at], replacement.data(), replacement.size());
}

// Applies the patches to data; log: the lines the game starter prints. false: a patch's place held other bytes
// (log ends with the error, data must not be written).
bool apply(std::string &data, const Value &patches, std::string &log, std::string &error) {
    if (patches.kind != Value::Array) { error = "bad patch list"; return false; }
    for (const Value &patch : patches.items) {
        if (patch.kind != Value::Array || patch.items.size() != 2 || patch.items[1].kind != Value::Array) {
            error = "bad patch";
            return false;
        }
        const std::string &description = patch.items[0].s;
        bool applied = false;
        for (const Value &variant : patch.items[1].items) {
            if (variant.kind != Value::Array || variant.items.empty()) continue;
            Pattern p = parse_pattern(variant.items[0].s);
            if (!p.ok) { error = "Patch '" + description + "': bad pattern"; return false; }
            std::vector<size_t> found = find_all(data, p, 2);
            if (found.size() != 1) continue;
            if (variant.items.size() == 2) {   // several changes: all of them, or none if any bytes differ
                struct Edit { long long at; std::string original, replacement; };
                std::vector<Edit> edits;
                for (const Value &e : variant.items[1].items) {
                    Edit x;
                    x.at = (long long)found[0] + e.items[0].i;
                    bytes_from_hex(e.items[1].s, x.original);
                    bytes_from_hex(e.items[2].s, x.replacement);
                    edits.push_back(x);
                }
                bool same = true;
                for (const Edit &x : edits) same = same && range_equals(data, x.at, x.original);
                if (!same) continue;
                for (const Edit &x : edits) put(data, x.at, x.replacement);
                log += "Patched: " + description + " (image offset " + hex_offset((long long)found[0]) + ", " +
                       std::to_string(edits.size()) + " changes)\n";
                applied = true;
                break;
            }
            long long at = (long long)found[0] + variant.items[1].i;
            std::string original, replacement;
            bytes_from_hex(variant.items[2].s, original);
            bytes_from_hex(variant.items[3].s, replacement);
            if (!range_equals(data, at, original)) {
                error = "Patch '" + description + "': unexpected bytes at " + hex_offset(at);
                return false;
            }
            put(data, at, replacement);
            log += "Patched: " + description + " (image offset " + hex_offset(at) + ")\n";
            applied = true;
            break;
        }
        if (!applied) log += "Patch '" + description + "' NOT applied: no variant matches this game version\n";
    }
    return true;
}

bool read_file(const wchar_t *path, std::string &data) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) != 0;
    if (ok) {
        data.assign((size_t)size.QuadPart, '\0');
        size_t done = 0;
        while (ok && done < data.size()) {
            DWORD got = 0;
            ok = ReadFile(h, &data[done], (DWORD)std::min<size_t>(data.size() - done, 1 << 30), &got, nullptr) && got;
            done += got;
        }
    }
    CloseHandle(h);
    return ok;
}

bool write_file(const wchar_t *path, const std::string &data) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    bool ok = true;
    while (ok && done < data.size()) {
        DWORD put_n = 0;
        ok = WriteFile(h, data.data() + done, (DWORD)std::min<size_t>(data.size() - done, 1 << 30), &put_n, nullptr) && put_n;
        done += put_n;
    }
    CloseHandle(h);
    return ok;
}

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

}  // namespace

extern "C" {

// patches_json: [[description, [variant, ...]], ...]; a variant is [pattern, offset, original hex, replacement hex] or
// [pattern, [[offset, original hex, replacement hex], ...]]. out: the log lines. 0 applied and written; 1 not written
// (the last line says why); negative: out too small (nothing written; call again with that size).
UN_API int un_apply_patches(const wchar_t *image, const char *patches_json, char *out, int cap) {
    Value patches;
    std::string data, log, error;
    if (!json::parse(patches_json ? patches_json : "", patches)) error = "bad patch list";
    else if (!read_file(image, data)) error = "cannot read the image";
    else if (apply(data, patches, log, error)) {
        if ((int)log.size() + 1 > cap) return -(int)(log.size() + 1);
        if (!write_file(image, data)) { error = "cannot write the image"; }
        else return give(log, out, cap);
    }
    int status = give(log + error, out, cap);
    return status < 0 ? status : 1;
}

// out: the start offsets of the pattern's matches in the file (decimal, one per line, without overlaps, at most
// `limit`)
UN_API int un_pattern_find(const wchar_t *file, const char *pattern, int limit, char *out, int cap) {
    std::string data, text;
    Pattern p = parse_pattern(pattern ? pattern : "");
    if (!p.ok || !read_file(file, data)) return 1;
    for (size_t at : find_all(data, p, limit > 0 ? (size_t)limit : (size_t)-1)) text += std::to_string(at) + "\n";
    return give(text, out, cap);
}

}  // extern "C"
