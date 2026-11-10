// prepare.cpp - the game's preparation (gow3_pc/scripts/prepare.py), part of uncharted_native.dll: an already plaintext
// PS4 SELF (the player's own executable) read into the memory image the runtime loads (boot.bin: its loadable segments,
// base-relative relocations and the symbols it imports), with eboot.elf, entry.bin and the analysis.json report.
//
// No decryption, nothing patched in the source files. All offsets are checked before use: a value out of bounds stops
// the preparation ("prepare failed: ...").

#include "native.h"
#include "json.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

using json::Value;

struct Failure { std::string message; };

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}

// a path as Python's repr shows it in an error (backslashes doubled)
std::string shown(const std::wstring &path) {
    std::string out;
    for (char c : utf8(path)) out += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return out;
}

std::string hex0x(uint64_t v) {   // Python's hex()
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

std::string hexbytes(const std::string &d) {
    static const char *x = "0123456789abcdef";
    std::string o;
    for (unsigned char c : d) { o += x[c >> 4]; o += x[c & 15]; }
    return o;
}

std::string grouped(unsigned long long v) {   // Python's {:,}
    std::string s = std::to_string(v), o;
    int n = 0;
    for (size_t i = s.size(); i-- > 0;) {
        o.insert(o.begin(), s[i]);
        if (++n % 3 == 0 && i) o.insert(o.begin(), ',');
    }
    return o;
}

bool read_file(const std::wstring &path, std::string &data) {
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

void write_file(const std::wstring &path, const std::string &data) {
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

std::string digest(const wchar_t *algorithm, const std::string &data, size_t size) {
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
std::string sha256_hex(const std::string &data) { return hexbytes(digest(BCRYPT_SHA256_ALGORITHM, data, 32)); }

// ------------------------------------------------------------------------------------------------ bounds-checked reads

void need(const std::string &d, long long offset, long long size) {
    if (offset < 0 || size < 0 || offset + size > (long long)d.size()) {
        char b[96];
        std::snprintf(b, sizeof b, "out of bounds: %s+%s / %s", hex0x((uint64_t)offset).c_str(), hex0x((uint64_t)size).c_str(),
                      hex0x(d.size()).c_str());
        throw Failure{b};
    }
}
std::string span(const std::string &d, long long offset, long long size) {
    need(d, offset, size);
    return d.substr((size_t)offset, (size_t)size);
}
uint64_t u(const std::string &d, long long at, int n) {
    need(d, at, n);
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= (uint64_t)(uint8_t)d[(size_t)at + i] << (8 * i);
    return v;
}
// the NUL-terminated ASCII text at `off` (Python: data[off:data.index(0, off)].decode('ascii'))
std::string cstring(const std::string &d, long long off) {
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
Self parse_self(const std::string &data) {
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

// A symbol's NID as the PS4 names imports: SHA-1 of the name and a fixed salt, the first 8 bytes reversed, Base64
// without padding and with '-' for '/'.
std::string nid(const std::string &name) {
    std::string salt = std::string("\x51\x8d\x64\xa6\x35\xde\xd8\xc1\xe6\xb0\x39\xb1\xc3\xe5\x52\x30", 16);
    std::string h = digest(BCRYPT_SHA1_ALGORITHM, name + salt, 20).substr(0, 8);
    std::reverse(h.begin(), h.end());
    static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < h.size(); i += 3) {
        uint32_t v = (uint8_t)h[i] << 16 | (i + 1 < h.size() ? (uint8_t)h[i + 1] << 8 : 0) | (i + 2 < h.size() ? (uint8_t)h[i + 2] : 0);
        size_t chars = std::min<size_t>(4, (h.size() - i) * 8 / 6 + 1);
        for (size_t k = 0; k < chars; ++k) out += b64[(v >> (18 - 6 * k)) & 63];
    }
    std::replace(out.begin(), out.end(), '/', '-');
    return out;
}

const Ph &first_of(const std::vector<Ph> &ph, uint64_t type) {
    for (const Ph &p : ph) if (p.type == type) return p;
    throw Failure{"segment missing"};
}

// Proves that this dump's libc _init_env is exactly RET (never assumed), and notes ios_base::Init.
Value inspect_libc(const std::wstring &path) {
    std::string source;
    if (!read_file(path, source)) throw Failure{"[Errno 2] No such file or directory: '" + shown(path) + "'"};
    Self s = parse_self(source);
    const Ph &dynamic = first_of(s.ph, 2);
    std::map<uint64_t, uint64_t> tags;
    for (uint64_t pos = dynamic.offset; pos < dynamic.offset + dynamic.filesz; pos += 16)
        tags[u(s.elf, (long long)pos, 8)] = u(s.elf, (long long)pos + 8, 8);
    auto tag = [&](uint64_t t) {
        auto it = tags.find(t);
        if (it == tags.end()) throw Failure{std::to_string(t)};
        return (long long)it->second;
    };
    const Ph &lib = first_of(s.ph, 0x61000000);
    std::string blob = span(s.elf, (long long)lib.offset, (long long)lib.filesz);
    std::string strings = span(blob, tag(0x61000035), tag(0x61000037));
    std::string syms = span(blob, tag(0x61000039), tag(0x6100003f));
    Value evidence = Value::object();
    evidence.set("sha256", Value::text(sha256_hex(source)));
    evidence.set("init_env_is_ret", Value::boolean(false));
    const std::string init_env = nid("_init_env"), ios = nid("_ZNSt8ios_base4InitC1Ev");
    for (long long pos = 0; pos < (long long)syms.size(); pos += 24) {
        need(syms, pos, 24);
        uint32_t name = (uint32_t)u(syms, pos, 4);
        unsigned info = (unsigned)u(syms, pos + 4, 1), section = (unsigned)u(syms, pos + 6, 2);
        uint64_t value = u(syms, pos + 8, 8), size = u(syms, pos + 16, 8);
        std::string symbol = cstring(strings, name);
        std::string key = symbol.substr(0, symbol.find('#'));
        if ((key != init_env && key != ios) || !section || (info & 15) != 2) continue;
        const Ph *code_ph = nullptr;
        for (const Ph &p : s.ph)
            if (p.type == 1 && (p.flags & 1) && p.vaddr <= value && value < p.vaddr + p.filesz) { code_ph = &p; break; }
        if (!code_ph) throw Failure{"no code segment for " + symbol};
        std::string code = span(s.elf, (long long)(code_ph->offset + value - code_ph->vaddr), (long long)size);
        if (key == init_env) {
            evidence.set("symbol", Value::text(symbol));
            evidence.set("address", Value::text(hex0x(value)));
            evidence.set("size", Value::integer((long long)size));
            evidence.set("bytes", Value::text(hexbytes(code)));
            evidence.set("init_env_is_ret", Value::boolean(code == "\xc3"));
        } else {
            Value i = Value::object();
            i.set("symbol", Value::text(symbol));
            i.set("address", Value::text(hex0x(value)));
            i.set("size", Value::integer((long long)size));
            i.set("code_sha256", Value::text(sha256_hex(code)));
            i.set("name", Value::text("_ZNSt8ios_base4InitC1Ev"));
            evidence.set("ios_base_init", i);
        }
    }
    return evidence;
}

// the files under a folder, depth first in name order (their first path part: the top-level folder)
void walk(const std::wstring &folder, const std::wstring &top, std::vector<std::pair<std::wstring, uint64_t>> &files) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(folder, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> dirs;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(name);
        else files.push_back({top.empty() ? name : top, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const std::wstring &d : dirs) walk(join(folder, d), top.empty() ? d : top, files);
}

std::string prepare(const std::wstring &game, const std::wstring &out, const std::string &executable) {
    std::string source;
    std::wstring exe_path = join(game, std::wstring(executable.begin(), executable.end()));
    if (!read_file(exe_path, source)) throw Failure{"[Errno 2] No such file or directory: '" + shown(exe_path) + "'"};
    Self s = parse_self(source);
    std::vector<const Ph *> loads;
    for (const Ph &p : s.ph) if (p.type == 1 || p.type == 0x61000010) loads.push_back(&p);
    auto mapped = [&](uint64_t addr, uint64_t size) {
        for (const Ph *p : loads) if (p->vaddr <= addr && addr + size <= p->vaddr + p->memsz) return true;
        return false;
    };
    uint64_t size = 0;
    for (const Ph *p : loads) size = std::max(size, p->vaddr + p->memsz);
    if (loads.empty()) throw Failure{"max() arg is an empty sequence"};
    if (size > 512ull * 1024 * 1024 || !mapped(s.header.entry, 1)) throw Failure{"invalid memory image"};
    std::string image((size_t)size, '\0');
    for (const Ph *p : loads) {
        if (p->filesz > p->memsz) throw Failure{"segment file size exceeds memory size"};
        std::string bytes = span(s.elf, (long long)p->offset, (long long)p->filesz);
        std::memcpy(&image[(size_t)p->vaddr], bytes.data(), bytes.size());
    }
    const Ph &dp = first_of(s.ph, 2);
    std::vector<std::pair<uint64_t, uint64_t>> dyn;
    for (uint64_t pos = dp.offset; pos < dp.offset + dp.filesz; pos += 16) {
        uint64_t t = u(s.elf, (long long)pos, 8), v = u(s.elf, (long long)pos + 8, 8);
        if (t == 0) break;
        dyn.push_back({t, v});
    }
    std::map<uint64_t, uint64_t> tags;
    for (auto &d : dyn) tags[d.first] = d.second;
    auto tag = [&](uint64_t t) {
        auto it = tags.find(t);
        if (it == tags.end()) throw Failure{std::to_string(t)};
        return (long long)it->second;
    };
    const Ph &lib = first_of(s.ph, 0x61000000);
    std::string blob = span(s.elf, (long long)lib.offset, (long long)lib.filesz);
    std::string strings = span(blob, tag(0x61000035), tag(0x61000037));
    std::string syms = span(blob, tag(0x61000039), tag(0x6100003f));
    if (syms.size() % 24) throw Failure{"bad symbol table size"};
    struct Symbol { std::string name; unsigned type, section; uint64_t value, size; };
    std::vector<Symbol> symbols;
    for (long long pos = 0; pos < (long long)syms.size(); pos += 24) {
        uint32_t name = (uint32_t)u(syms, pos, 4);
        unsigned info = (unsigned)u(syms, pos + 4, 1), shndx = (unsigned)u(syms, pos + 6, 2);
        symbols.push_back({cstring(strings, name), info & 15, shndx, u(syms, pos + 8, 8), u(syms, pos + 16, 8)});
    }
    struct Reloc { uint64_t target, kind; int64_t value, addend; };
    std::vector<Reloc> relocs;
    std::vector<std::pair<uint64_t, long long>> counts;   // relocation kind -> how many, in first-seen order
    std::vector<std::pair<uint64_t, uint64_t>> imports;   // symbol -> import index, in first-seen order
    for (auto tables : {std::make_pair(0x61000029ull, 0x6100002dull), std::make_pair(0x6100002full, 0x61000031ull)}) {
        std::string table = span(blob, tag(tables.first), tag(tables.second));
        if (table.size() % 24) throw Failure{"bad relocation table size"};
        for (long long pos = 0; pos < (long long)table.size(); pos += 24) {
            uint64_t target = u(table, pos, 8), info = u(table, pos + 8, 8);
            int64_t addend = (int64_t)u(table, pos + 16, 8);
            uint64_t kind = info & 0xffffffff, sym = info >> 32;
            bool seen = false;
            for (auto &c : counts) if (c.first == kind) { ++c.second; seen = true; }
            if (!seen) counts.push_back({kind, 1});
            if (!mapped(target, 8)) throw Failure{"relocation outside mapped segments: " + hex0x(target)};
            if (kind == 8) {
                relocs.push_back({target, 0, addend, 0});   // base-relative
            } else if (kind == 1 || kind == 6 || kind == 7) {
                if (sym >= symbols.size()) throw Failure{"list index out of range"};
                const Symbol &symbol = symbols[(size_t)sym];
                if (symbol.section) {
                    if (symbol.section == 0xfff1) {
                        uint64_t v = symbol.value + (uint64_t)addend;
                        for (int i = 0; i < 8; ++i) image[(size_t)target + i] = (char)(v >> (8 * i));
                    } else {
                        relocs.push_back({target, 0, (int64_t)(symbol.value + (uint64_t)addend), 0});
                    }
                } else {
                    uint64_t index = imports.size();
                    bool found = false;
                    for (auto &i : imports) if (i.first == sym) { index = i.second; found = true; }
                    if (!found) imports.push_back({sym, index});
                    uint64_t import_kind = symbol.type == 1 ? 2 : 1;
                    if ((import_kind == 1 && addend) || !(0 <= addend && addend < 4096)) throw Failure{"unsupported import addend"};
                    relocs.push_back({target, import_kind, (int64_t)index, addend});
                }
            } else {
                throw Failure{"unsupported relocation " + std::to_string(kind)};
            }
        }
    }
    std::vector<std::string> names;
    for (auto &i : imports) names.push_back(symbols[(size_t)i.first].name);
    static const char *candidates[] = {
        "_init_env", "atexit", "exit", "_exit", "__cxa_atexit", "__cxa_finalize", "__cxa_guard_acquire",
        "__cxa_guard_release", "__cxa_guard_abort", "__stack_chk_guard", "__stack_chk_fail", "malloc", "free", "memcpy",
        "memmove", "memset", "memcmp", "strlen", "printf", "_ZNSt8ios_base4InitC1Ev", "sceKernelLoadStartModule",
        "sceVideoOutOpen", "sceKernelCreateSema", "sceKernelWaitSema", "sceKernelPollSema", "sceKernelSignalSema",
        "sceKernelCancelSema", "sceKernelDeleteSema", "sceAppContentInitialize", "sceAppContentAppParamGetInt",
        "sceAppContentGetAddcontInfoList", "pthread_cond_init", "sceSysmoduleLoadModule", "pthread_mutexattr_init",
        "gettimeofday", "scePthreadMutexattrInit", "scePthreadMutexattrSettype", "scePthreadMutexattrDestroy",
        "scePthreadMutexInit", "scePthreadMutexLock", "scePthreadMutexTrylock", "scePthreadMutexUnlock",
        "scePthreadMutexDestroy", "scePthreadRwlockInit", "scePthreadRwlockDestroy", "scePthreadRwlockRdlock",
        "scePthreadRwlockWrlock", "scePthreadRwlockTryrdlock", "scePthreadRwlockTrywrlock", "scePthreadRwlockUnlock",
        "scePthreadRwlockTimedrdlock", "scePthreadRwlockTimedwrlock", "sceKernelGetDirectMemorySize",
        "sceKernelAllocateDirectMemory", "sceKernelMapDirectMemory", "sceKernelReleaseDirectMemory", "sceKernelMunmap"};
    std::map<std::string, std::string> known;
    for (const char *c : candidates) known[nid(c)] = c;
    Value libc_evidence = inspect_libc(join(game, L"sce_module\\libc.prx"));
    const bool init_env_is_ret = libc_evidence.get("init_env_is_ret")->b;
    CreateDirectoryW(out.c_str(), nullptr);
    // boot.bin: a deliberately small format, consumed by the runtime's loader; no host struct packing
    std::string boot = "G3PROBE2";
    auto q = [&](uint64_t v) { for (int i = 0; i < 8; ++i) boot += (char)(v >> (8 * i)); };
    q(size); q(s.header.entry); q(loads.size()); q(relocs.size()); q(names.size()); q(init_env_is_ret ? 1 : 0);
    for (const Ph *p : loads) { q(p->vaddr); q(p->memsz); q(p->flags); }
    for (const std::string &name : names) {
        if (name.size() >= 128) throw Failure{"import name too long"};
        boot += name;
        boot += std::string(128 - name.size(), '\0');
    }
    for (const Reloc &r : relocs) { q(r.target); q(r.kind); q((uint64_t)r.value); q((uint64_t)r.addend); }
    boot += image;
    write_file(join(out, L"boot.bin"), boot);
    write_file(join(out, L"eboot.elf"), s.elf);
    write_file(join(out, L"entry.bin"), image.substr(0, 1024));
    // the game's resources (dvdroot_ps4), counted by top-level folder
    std::vector<std::pair<std::wstring, uint64_t>> files;
    walk(join(game, L"dvdroot_ps4"), L"", files);
    Value resources = Value::object();
    unsigned long long total_bytes = 0;
    for (auto &f : files) {
        std::string key = utf8(f.first);
        const Value *v = resources.get(key);
        resources.set(key, Value::integer(v ? v->i + 1 : 1));
        total_bytes += f.second;
    }
    // the report
    char sfo_buf[1 << 16];
    if (un_system("sfo", game.c_str(), sfo_buf, sizeof sfo_buf) != 0)   // the folder's sce_sys\param.sfo (system.cpp)
        throw Failure{"bad SFO signature"};
    Value sfo;
    json::parse(sfo_buf, sfo);
    Value report = Value::object();
    report.set("source_sha256", Value::text(sha256_hex(source)));
    report.set("source_bytes", Value::integer((long long)source.size()));
    report.set("sfo", sfo);
    report.set("entry", Value::text(hex0x(s.header.entry)));
    report.set("image_bytes", Value::integer((long long)size));
    Value phs;
    phs.kind = Value::Array;
    for (const Ph &p : s.ph) {
        Value o = Value::object();
        const char *keys[] = {"type", "flags", "offset", "vaddr", "paddr", "filesz", "memsz", "align"};
        uint64_t vals[] = {p.type, p.flags, p.offset, p.vaddr, p.paddr, p.filesz, p.memsz, p.align};
        for (int i = 0; i < 8; ++i) o.set(keys[i], Value::integer((long long)vals[i]));
        phs.items.push_back(o);
    }
    report.set("program_headers", phs);
    Value segs;
    segs.kind = Value::Array;
    for (const Segment &g : s.segments) {
        Value o = Value::object();
        o.set("flags", Value::text(hex0x(g.flags)));
        o.set("offset", Value::integer((long long)g.offset));
        o.set("size", Value::integer((long long)g.size));
        segs.items.push_back(o);
    }
    report.set("self_segments", segs);
    Value missing;
    missing.kind = Value::Array;
    for (int m : s.missing) missing.items.push_back(Value::integer(m));
    report.set("unavailable_metadata_headers", missing);
    Value needed;
    needed.kind = Value::Array;
    for (auto &d : dyn) if (d.first == 1) needed.items.push_back(Value::text(cstring(strings, (long long)d.second)));
    report.set("needed", needed);
    Value counts_v = Value::object();
    long long total_relocs = 0;
    for (auto &c : counts) { counts_v.set(std::to_string(c.first), Value::integer(c.second)); total_relocs += c.second; }
    report.set("relocation_counts", counts_v);
    report.set("import_count", Value::integer((long long)names.size()));
    Value names_v, hints = Value::object();
    names_v.kind = Value::Array;
    for (const std::string &n : names) {
        names_v.items.push_back(Value::text(n));
        auto it = known.find(n.substr(0, n.find('#')));
        if (it != known.end()) hints.set(n, Value::text(it->second));
    }
    report.set("imports", names_v);
    report.set("import_name_hints", hints);
    report.set("libc_evidence", libc_evidence);
    std::vector<std::string> modules;
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(join(game, L"sce_module\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) throw Failure{"[WinError 3] The system cannot find the path specified"};
        do {
            std::wstring n = fd.cFileName;
            if (n != L"." && n != L"..") modules.push_back(utf8(n));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(modules.begin(), modules.end());   // UTF-8 byte order is code point order (Python's sorted)
    }
    Value modules_v;
    modules_v.kind = Value::Array;
    for (auto &m : modules) modules_v.items.push_back(Value::text(m));
    report.set("bundled_modules", modules_v);
    report.set("resources", resources);
    report.set("resource_bytes", Value::integer((long long)total_bytes));
    report.set("status", Value::text("Prepared only; execution and Vulkan are tested separately."));
    std::string text = json::dump(report, false) + "\n", crlf;
    for (char c : text) { if (c == '\n') crlf += "\r\n"; else crlf += c; }   // Python's write_text on Windows
    write_file(join(out, L"analysis.json"), crlf);
    // what prepare.py prints
    const Value *title = sfo.get("TITLE");
    std::string missing_text = "[";
    for (size_t i = 0; i < s.missing.size(); ++i) missing_text += (i ? ", " : "") + std::to_string(s.missing[i]);
    missing_text += "]";
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(out.c_str(), MAX_PATH * 4, full, nullptr);
    std::wstring resolved = n && n < MAX_PATH * 4 ? std::wstring(full, n) : out;
    while (resolved.size() > 3 && resolved.back() == L'\\') resolved.pop_back();
    return (title ? (title->kind == Value::String ? title->s : std::to_string(title->i)) : std::string("None")) +
           " | entry=" + hex0x(s.header.entry) + " | image=" + grouped(size) + " bytes\n" + std::to_string(names.size()) +
           " imported symbols; " + grouped((unsigned long long)total_relocs) + " relocations; " +
           std::to_string(needed.items.size()) + " required modules\n" + "Unavailable non-loadable metadata headers: " +
           missing_text + "; not a byte-exact ELF reconstruction\n" + "Output: " + utf8(resolved) + "\n" +
           "libc _init_env verified RET: " + (init_env_is_ret ? "True" : "False") + "\n";
}

}  // namespace

extern "C" {

// prepare.py's work for the game folder: boot.bin, eboot.elf, entry.bin and analysis.json in `out`. out: the lines
// prepare.py prints. 0 ok; 1 failed (out: why, as "prepare failed: <why>" ends it).
UN_API int un_prepare(const wchar_t *game, const wchar_t *out_dir, const char *executable, char *out, int cap) {
    try {
        return give(prepare(game ? game : L"", out_dir ? out_dir : L"", executable ? executable : "eboot.bin"), out, cap);
    } catch (const Failure &f) {
        int status = give(f.message, out, cap);
        return status < 0 ? status : 1;
    }
}

}  // extern "C"
