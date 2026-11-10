// prepare.cpp - the game's preparation (gow3_pc/scripts/prepare.py), part of uncharted_native.dll: an already plaintext
// PS4 SELF (the player's own executable) read into the memory image the runtime loads (boot.bin: its loadable segments,
// base-relative relocations and the symbols it imports), with eboot.elf, entry.bin and the analysis.json report.
//
// No decryption, nothing patched in the source files. All offsets are checked before use: a value out of bounds stops
// the preparation ("prepare failed: ...").

#include "native.h"
#include "json.h"
#include "self_reader.h"

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

using namespace selfread;

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
    make_dirs(out);
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
