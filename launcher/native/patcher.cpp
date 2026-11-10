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
#include <cmath>
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

// ------------------------------------------------------------------------------------------------ render resolution

// Render resolution. The engine's display settings (constructors and the 1080p/720p mode switch, all within ~0x700
// bytes) store the size in several fields: display and render size (+0x08/+0x18/+0x24/+0x38), aspect, tile grid
// (+0x40 = width/8, +0x44 = height/4, as the engine's own SetResolution computes them) and a larger size at
// +0x30/+0x34 (2560x1216 at 1080p), scaled with the rest. The render target heaps of the engine's memory table grow
// with the pixel count (see tools/probe_uncharted.py for the measurements behind each step).
struct Failure {
    int kind;   // 1 the game's code is not as expected, 2 the size is too large
    std::string message;
};

void le(std::string &d, size_t at, uint64_t v, int n) {
    for (int i = 0; i < n; ++i) d[at + i] = (char)(v >> (8 * i));
}
uint64_t rd(const std::string &d, size_t at, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= (uint64_t)(uint8_t)d[at + i] << (8 * i);
    return v;
}
std::string stored(uint8_t base, uint8_t disp, uint32_t value) {
    std::string s = {(char)0xc7, (char)base, (char)disp};
    for (int i = 0; i < 4; ++i) s += (char)(value >> (8 * i));
    return s;
}
// replaces every occurrence (without overlaps, from the left) inside [start, end); how many there were
int replace_in(std::string &d, size_t start, size_t end, const std::string &from, const std::string &to) {
    int n = 0;
    for (size_t at = d.find(from, start); at != std::string::npos && at + from.size() <= end; at = d.find(from, at)) {
        d.replace(at, from.size(), to);
        at += from.size();
        ++n;
    }
    return n;
}

long long resolution(std::string &data, int width, int height, bool move, std::string &log) {
    const long long mib = 1 << 20;
    std::vector<size_t> anchor = find_all(data, parse_pattern("c7 ?? 08 80070000 c7 ?? 0c 38040000"), 1);
    if (anchor.empty()) throw Failure{1, "Resolution patch: display settings code not found"};
    size_t first = anchor[0];
    size_t start = first >= 0x40 ? first - 0x40 : 0, end = std::min(data.size(), first + 0x800);
    struct Store { int disp; uint32_t old_value, new_value; };
    std::vector<Store> stores = {
        {0x08, 1920, (uint32_t)width}, {0x18, 1920, (uint32_t)width}, {0x24, 1920, (uint32_t)width},
        {0x38, 1920, (uint32_t)width}, {0x0c, 1080, (uint32_t)height}, {0x1c, 1080, (uint32_t)height},
        {0x28, 1080, (uint32_t)height}, {0x3c, 1080, (uint32_t)height}, {0x40, 240, (uint32_t)(width / 8)},
        {0x44, 270, (uint32_t)(height / 4)}, {0x30, 2560, (uint32_t)(2560LL * width / 1920)},
        {0x34, 1216, (uint32_t)(1216LL * height / 1080)}};
    // Diagnostics: UNCHARTED_RES_KEEP=<hex field offsets> leaves those fields at 1080p.
    char keep_text[512] = "";
    GetEnvironmentVariableA("UNCHARTED_RES_KEEP", keep_text, sizeof keep_text);
    std::vector<long> keep;
    for (char *t = keep_text; *t;) {
        while (*t == ',' || *t == ' ' || *t == '\t') ++t;
        if (!*t) break;
        char *e = nullptr;
        keep.push_back(std::strtol(t, &e, 16));
        if (e == t) throw Failure{1, "Resolution patch: bad UNCHARTED_RES_KEEP"};
        t = e;
    }
    int changed = 0;
    for (uint8_t base : {0x47, 0x40})   // [rdi+disp] and [rax+disp]
        for (const Store &s : stores) {
            bool kept = false;
            for (long k : keep) kept = kept || k == s.disp;
            if (!kept) changed += replace_in(data, start, end, stored(base, (uint8_t)s.disp, s.old_value),
                                             stored(base, (uint8_t)s.disp, s.new_value));
        }
    std::string packed_old = "\x48\xb8", packed_new = "\x48\xb8";
    for (int i = 0; i < 4; ++i) packed_old += (char)(1920u >> (8 * i));
    for (int i = 0; i < 4; ++i) packed_old += (char)(1080u >> (8 * i));
    for (int i = 0; i < 4; ++i) packed_new += (char)((uint32_t)width >> (8 * i));
    for (int i = 0; i < 4; ++i) packed_new += (char)((uint32_t)height >> (8 * i));
    changed += replace_in(data, start, end, packed_old, packed_new);
    // The screen wipe of Uncharted 2 and 3 is drawn from the render size: it takes the PS4's 1920x1080 instead.
    std::vector<size_t> wipe = find_all(data, parse_pattern("48 8d 05 ???????? 8b 48 08 8b 40 0c c4 e1 fa 2a c0"), (size_t)-1);
    if (wipe.size() == 1) {
        std::string patch;
        bytes_from_hex("b980070000b8380400000f1f00", patch);
        data.replace(wipe[0], patch.size(), patch);
        log += "Patched: screen wipe in the PS4's 1920x1080 (image offset " + hex_offset((long long)wipe[0]) + ")\n";
    } else if (!wipe.empty()) {
        log += "Screen wipe patch NOT applied: " + std::to_string(wipe.size()) + " matches in this game version\n";
    }
    const double factor = std::max(1.0, (double)((long long)width * height) / (double)(1920 * 1080));
    auto table_entry = [&](uint32_t id, uint32_t parent, uint64_t align, uint64_t min_size) {
        std::string key(8, '\0');
        le(key, 0, id, 4);
        le(key, 4, parent, 4);
        std::vector<std::pair<size_t, long long>> found;
        for (size_t at = data.find(key); at != std::string::npos; at = data.find(key, at + 1)) {
            if (at + 32 > data.size()) throw Failure{1, "Resolution patch: memory table entry cut short"};
            uint64_t size = rd(data, at + 8, 8), entry_align = rd(data, at + 16, 8), pad = rd(data, at + 24, 8);
            if (entry_align == align && pad == 0 && size >= min_size) found.push_back({at, (long long)size});
        }
        if (found.size() != 1) {
            char m[160];
            std::snprintf(m, sizeof m, "Resolution patch: memory table entry %#x found %d times", id, (int)found.size());
            throw Failure{1, m};
        }
        return found[0];
    };
    auto onion = table_entry(2, 2, 0x100000, 0x40000000);
    auto garlic = table_entry(3, 2, 0x100000, 0x80000000ULL);
    auto heap = table_entry(0x85, 6, 0x80, 0x10000000);   // Render Target Heap
    auto rt = table_entry(0x84, 6, 0x80, 0x4000000);      // Render Target
    const long long step = 64 * mib, area_limit = 4096 * mib, headroom = 128 * mib;
    auto ceil_div = [](long long a, long long b) { return a >= 0 ? (a + b - 1) / b : -((-a) / b); };   // Python's -(-a // b)
    auto size_text = std::to_string(width) + "x" + std::to_string(height);
    long long heap_new = 0, rt_new = 0, onion_new = 0, garlic_new = 0;
    int heap_context = 6;
    bool fitted = false;
    for (double margin : {1.1, 1.05, 1.0}) {
        try {
            auto grown = [&](long long old) {
                if (!(factor > 1)) return old;
                long long by = (long long)((double)old * (factor * margin - 1));
                return old + ceil_div(by, step) * step;
            };
            long long h = grown(heap.second), r = grown(rt.second);
            long long o = onion.second, g = garlic.second + (h - heap.second) + (r - rt.second);
            int context = 6;
            if (g >= area_limit) {
                if (!move) throw Failure{2, size_text + ": the render targets don't fit the GPU area"};
                h = ceil_div(h, step) * step;
                o = onion.second + h;
                long long rest = garlic.second - heap.second - rt.second;
                g = ceil_div(rest + r, step) * step;
                r = g - rest;
                context = 5;
            }
            if (o + headroom >= area_limit || g >= area_limit)
                throw Failure{2, size_text + " needs more memory than the game's areas hold"};
            heap_new = h; rt_new = r; onion_new = o; garlic_new = g; heap_context = context;
            fitted = true;
            break;
        } catch (const Failure &f) {
            if (f.kind != 2 || margin == 1.0) throw;
        }
    }
    if (!fitted) throw Failure{1, "Resolution patch: no layout"};
    long long extra = (onion_new - onion.second) + (garlic_new - garlic.second);
    le(data, heap.first, 0x85, 4);
    le(data, heap.first + 4, (uint64_t)heap_context, 4);
    le(data, heap.first + 8, (uint64_t)heap_new, 8);
    le(data, rt.first + 8, (uint64_t)rt_new, 8);
    le(data, onion.first + 8, (uint64_t)onion_new, 8);
    le(data, garlic.first + 8, (uint64_t)garlic_new, 8);
    // The memory table check (ValidateContext) expects the direct memory blocks to add up to 4608 MiB.
    std::vector<size_t> total = find_all(data, parse_pattern("488d75 ?? 48ba 0000002001000000"), (size_t)-1);
    if (total.size() != 1)
        throw Failure{1, "Resolution patch: memory total check found " + std::to_string(total.size()) + " times"};
    le(data, total[0] + 6, (uint64_t)(0x120000000LL + extra), 8);
    log += "Patched: render resolution " + size_text + " (" + std::to_string(changed) + " stores; render target heaps " +
           std::to_string(heap_new / mib) + " + " + std::to_string(rt_new / mib) + " MiB, the first in the " +
           (heap_context == 5 ? "CPU" : "GPU") + " area)\n";
    return extra / mib;
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

// Patches the render resolution into the linked image in data (in place, only when it worked). extra_mib: the memory
// the areas grew by. out: the log lines. 0 patched; 1 the game's code is not as expected, 2 the size is too large (the
// last line says why); negative: out too small (data unchanged; call again with that size).
UN_API int un_resolution_patch(unsigned char *data, unsigned long long size, int width, int height, int move,
                               long long *extra_mib, char *out, int cap) {
    std::string work((const char *)data, (size_t)size), log;
    try {
        long long extra = resolution(work, width, height, move != 0, log);
        if ((int)log.size() + 1 > cap) return -(int)(log.size() + 1);
        if (work.size() != size) return give(log + "Resolution patch: the image changed size", out, cap) < 0 ? -1 : 1;
        std::memcpy(data, work.data(), work.size());
        if (extra_mib) *extra_mib = extra;
        return give(log, out, cap);
    } catch (const Failure &f) {
        int status = give(log + f.message, out, cap);
        return status < 0 ? status : f.kind;
    }
}

// The render size of the linked image at `image` for the size chosen (width x height): the first that fits of the size
// itself and smaller ones. Drake's Fortune keeps its render targets in the GPU area (move off): above ~1620p it renders
// at the largest 16:9 size that fits there, and the output is upscaled. Ultrawide sizes fall back to smaller sizes of
// the same shape (width/16 and height/4 whole: Uncharted 3 asserts widths in steps of 16 in its post processing).
// multiplayer: Uncharted 3's multiplayer image, which keeps the PS4's render size if the patch does not fit it.
// out: "<width> <height> <extra MiB>", then the log lines. 0 ok; 1 the game's code is not as expected (the last line
// says why; the image is unchanged); negative: out too small (nothing written; call again with that size).
UN_API int un_render_resolution(const wchar_t *image, const char *executable, int width, int height, int multiplayer,
                                char *out, int cap) {
    std::string exe = executable ? executable : "", data, log;
    if (width == 1920 && height == 1080) return give("1920 1080 0\n", out, cap);
    if (!read_file(image, data)) return give("cannot read the image", out, cap) < 0 ? -64 : 1;
    const bool move = exe != "eboot.bin";
    std::vector<std::pair<int, int>> candidates = {{width, height}};
    if (std::fabs((double)width / height - 16.0 / 9.0) > 0.01) {
        for (double scale : {0.9, 0.8, 0.75, 2.0 / 3.0, 0.6, 0.5})
            candidates.push_back({(int)(long long)(width * scale) / 16 * 16, (int)(long long)(height * scale) / 4 * 4});
    } else {
        const std::pair<int, int> fallbacks[] = {{3200, 1800}, {2880, 1620}, {2560, 1440}, {1920, 1080}};
        for (auto &s : fallbacks)
            if ((long long)s.first * s.second < (long long)width * height) candidates.push_back(s);
    }
    std::string chosen = data;
    int render_w = 1920, render_h = 1080;
    long long extra_mib = 0;
    for (auto &candidate : candidates) {
        std::string patched = data;
        try {
            extra_mib = resolution(patched, candidate.first, candidate.second, move, log);
        } catch (const Failure &f) {
            if (f.kind == 2) {
                log += "Render resolution " + f.message + "\n";
                continue;
            }
            if (!multiplayer) {
                int status = give(log + f.message, out, cap);
                return status < 0 ? status : 1;
            }
            log += "Uncharted 3 multiplayer: render resolution not changed (" + f.message + ")\n";
            break;
        }
        if (candidate.first != width || candidate.second != height)
            log += "Drake's Fortune renders at " + std::to_string(candidate.first) + "x" + std::to_string(candidate.second) +
                   " instead of " + std::to_string(width) + "x" + std::to_string(height) + "\n";
        chosen = patched;
        render_w = candidate.first;
        render_h = candidate.second;
        break;
    }
    std::string text = std::to_string(render_w) + " " + std::to_string(render_h) + " " + std::to_string(extra_mib) + "\n" + log;
    if ((int)text.size() + 1 > cap) return -(int)(text.size() + 1);
    if (!write_file(image, chosen)) return give(log + "cannot write the image", out, cap) < 0 ? -64 : 1;
    return give(text, out, cap);
}

}  // extern "C"
