// self_reader.h - shared by prepare.cpp and link.cpp (uncharted_native.dll): a plaintext PS4 SELF read into its ELF,
// bounds-checked reads (an offset out of range throws Failure), file reading and writing, and SHA hashes.
#pragma once

#include "native.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <string>
#include <vector>

namespace selfread {

struct Failure { std::string message; };

inline int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

inline std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}

// a path as Python's repr shows it in an error (backslashes doubled)
inline std::string shown(const std::wstring &path) {
    std::string out;
    for (char c : utf8(path)) out += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return out;
}

inline std::string hex0x(uint64_t v) {   // Python's hex()
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

inline std::string hexbytes(const std::string &d) {
    static const char *x = "0123456789abcdef";
    std::string o;
    for (unsigned char c : d) { o += x[c >> 4]; o += x[c & 15]; }
    return o;
}

inline std::string grouped(unsigned long long v) {   // Python's {:,}
    std::string s = std::to_string(v), o;
    int n = 0;
    for (size_t i = s.size(); i-- > 0;) {
        o.insert(o.begin(), s[i]);
        if (++n % 3 == 0 && i) o.insert(o.begin(), ',');
    }
    return o;
}

inline bool read_file(const std::wstring &path, std::string &data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
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

inline void write_file(const std::wstring &path, const std::string &data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw Failure{"cannot write " + utf8(path)};
    size_t done = 0;
    bool ok = true;
    while (ok && done < data.size()) {
        DWORD put = 0;
        ok = WriteFile(h, data.data() + done, (DWORD)std::min<size_t>(data.size() - done, 1 << 30), &put, nullptr) && put;
        done += put;
    }
    CloseHandle(h);
    if (!ok) throw Failure{"cannot write " + utf8(path)};
}

// The folder and any missing parents (Python: mkdir(parents=True, exist_ok=True)).
inline void make_dirs(const std::wstring &path) {
    for (size_t at = 0; (at = path.find_first_of(L"\\/", at + 1)) != std::wstring::npos;) {
        const std::wstring part = path.substr(0, at);
        if (!part.empty() && part.back() != L':') CreateDirectoryW(part.c_str(), nullptr);
    }
    CreateDirectoryW(path.c_str(), nullptr);
}

inline std::string digest(const wchar_t *algorithm, const std::string &data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out(size, '\0');
    if (BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        for (size_t at = 0; at < data.size(); at += 1 << 30)
            BCryptHashData(hash, (PUCHAR)data.data() + at, (ULONG)std::min<size_t>(data.size() - at, 1 << 30), 0);
        BCryptFinishHash(hash, (PUCHAR)&out[0], (ULONG)size, 0);
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}
inline std::string sha256_hex(const std::string &data) { return hexbytes(digest(BCRYPT_SHA256_ALGORITHM, data, 32)); }

// ------------------------------------------------------------------------------------------------ bounds-checked reads

inline void need(const std::string &d, long long offset, long long size) {
    if (offset < 0 || size < 0 || offset + size > (long long)d.size()) {
        char b[96];
        std::snprintf(b, sizeof b, "out of bounds: %s+%s / %s", hex0x((uint64_t)offset).c_str(), hex0x((uint64_t)size).c_str(),
                      hex0x(d.size()).c_str());
        throw Failure{b};
    }
}
inline std::string span(const std::string &d, long long offset, long long size) {
    need(d, offset, size);
    return d.substr((size_t)offset, (size_t)size);
}
inline uint64_t u(const std::string &d, long long at, int n) {
    need(d, at, n);
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= (uint64_t)(uint8_t)d[(size_t)at + i] << (8 * i);
    return v;
}
// the NUL-terminated ASCII text at `off` (Python: data[off:data.index(0, off)].decode('ascii'))
inline std::string cstring(const std::string &d, long long off) {
    need(d, off, 1);
    size_t end = d.find('\0', (size_t)off);
    if (end == std::string::npos) throw Failure{"subsection not found"};
    std::string s = d.substr((size_t)off, end - (size_t)off);
    for (unsigned char c : s)
        if (c >= 0x80) throw Failure{"'ascii' codec can't decode a symbol name"};
    return s;
}

// ------------------------------------------------------------------------------------------------ SELF

struct Header {
    uint64_t entry = 0, phoff = 0;
    unsigned phentsize = 0, phnum = 0, machine = 0;
};
struct Ph {
    uint64_t type, flags, offset, vaddr, paddr, filesz, memsz, align;
};
struct Segment {
    uint64_t flags, offset, size;
};
struct Self {
    std::string elf;
    Header header;
    std::vector<Ph> ph;
    std::vector<Segment> segments;
    std::vector<int> missing;
};

// The plaintext ELF inside a PS4 SELF (its segments placed at their file offsets); a segment that is encrypted,
// compressed or blocked stops it.
inline Self parse_self(const std::string &data) {
    if (span(data, 0, 4) != std::string("O\x15=\x1d", 4)) throw Failure{"expected PS4 SELF"};
    unsigned count = (unsigned)u(data, 24, 2);
    long long base = 32 + (long long)count * 32;
    need(data, base, 64);
    Self s;
    std::string ident = data.substr((size_t)base, 16);
    s.header.machine = (unsigned)u(data, base + 18, 2);
    s.header.entry = u(data, base + 24, 8);
    s.header.phoff = u(data, base + 32, 8);
    s.header.phentsize = (unsigned)u(data, base + 54, 2);
    s.header.phnum = (unsigned)u(data, base + 56, 2);
    if (ident.compare(0, 7, std::string("\x7f" "ELF\x02\x01\x01", 7)) != 0 || s.header.machine != 62)
        throw Failure{"expected little-endian x86-64 ELF"};
    if (s.header.phentsize != 56 || !(0 < s.header.phnum && s.header.phnum < 256)) throw Failure{"unsupported program headers"};
    for (unsigned i = 0; i < s.header.phnum; ++i) {
        long long at = base + (long long)s.header.phoff + i * 56;
        need(data, at, 56);
        s.ph.push_back({u(data, at, 4), u(data, at + 4, 4), u(data, at + 8, 8), u(data, at + 16, 8), u(data, at + 24, 8),
                        u(data, at + 32, 8), u(data, at + 40, 8), u(data, at + 48, 8)});
    }
    uint64_t end = 0;
    for (const Ph &p : s.ph) end = std::max(end, p.offset + p.filesz);
    if (end > 512ull * 1024 * 1024) throw Failure{"probe image exceeds 512 MiB limit"};
    s.elf.assign((size_t)end, '\0');
    uint64_t header_end = s.header.phoff + s.header.phnum * 56ull;
    std::string head = span(data, base, (long long)header_end);
    if (head.size() > s.elf.size()) s.elf.resize(head.size());
    std::memcpy(&s.elf[0], head.data(), head.size());
    std::vector<std::pair<uint64_t, uint64_t>> covered = {{0, header_end}};
    for (unsigned i = 0; i < count; ++i) {
        uint64_t flags = u(data, 32 + i * 32, 8), off = u(data, 32 + i * 32 + 8, 8), size = u(data, 32 + i * 32 + 16, 8),
                 mem = u(data, 32 + i * 32 + 24, 8);
        s.segments.push_back({flags, off, size});
        if (!(flags & 0x800)) continue;
        if (flags & 10) throw Failure{"encrypted/compressed SELF segment is unsupported"};
        uint64_t index = (flags >> 20) & 4095;
        if (index >= s.ph.size()) throw Failure{"invalid segment index"};
        const Ph &p = s.ph[(size_t)index];
        if (size != p.filesz || size != mem) throw Failure{"unsupported blocked segment layout"};
        std::string bytes = span(data, (long long)off, (long long)size);
        std::memcpy(&s.elf[(size_t)p.offset], bytes.data(), bytes.size());
        covered.push_back({p.offset, p.offset + size});
    }
    for (size_t i = 0; i < s.ph.size(); ++i) {
        const Ph &p = s.ph[i];
        if (!p.filesz) continue;
        bool in = false;
        for (auto &c : covered) in = in || (c.first <= p.offset && p.offset + p.filesz <= c.second);
        if (in) continue;
        s.missing.push_back((int)i);
        if (p.type == 1 || p.type == 2 || p.type == 0x61000000 || p.type == 0x61000010)
            throw Failure{"required segment " + std::to_string(i) + " is unavailable"};
    }
    return s;
}

}  // namespace selfread
