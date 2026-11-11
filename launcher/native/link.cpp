// link.cpp - joins modules into one image (native version of gow3_pc/scripts/link_modules.py), built into
// uncharted_native.dll. Starting from the prepared eboot (boot.bin, format G3PROBE2), it appends system modules that
// ship with the game (unless told otherwise: libc.prx and then libSceFios2.prx) and writes the result the runtime
// loads, boot-linked.bin (format G3PROBE5), along with a link.json report.
//
// Layout: each module starts on the next 64 KiB boundary after the one before it. All modules feed a single shared
// import table; the C runtime gets the first chance to supply any function (its host contracts), and whatever it
// doesn't supply is bound to the module's own native export. An export matches an import when the NID, the library
// identity and the module identity are all equal. libSceLibcInternal is not part of the game's files, but libc.prx
// exports the same functions under the same NIDs, so imports from the internal library are bound to those.
// Thread-local loads of the form `mov rax, fs:[0]` (initial-exec model) are rewritten to read gs: instead, because
// fs belongs to the host.

#include "native.h"
#include "json.h"
#include "self_reader.h"

#include <cstdint>
#include <cstring>

#include <algorithm>
#include <array>

#include <map>
#include <string>
#include <vector>

namespace {

using json::Value;
using namespace selfread;

// Spells a module or library ID the way PS4 symbol names carry it: Base64 digits with the highest one leading.
std::string encode_id(uint64_t value) {
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result(1, alphabet[value & 63]);
    value >>= 6;
    while (value) {
        result.insert(result.begin(), alphabet[value & 63]);
        value >>= 6;
    }
    return result;
}

// What a "NID#library#module" symbol name stands for: the NID plus the library and module, each with its version.
// Names without that shape leave ok false.
struct Identity {
    bool ok = false;
    std::string nid, library, module;
    unsigned library_version = 0, module_version = 0;
    std::string key() const {
        if (!ok) return std::string(1, '\0');
        return nid + '\x1f' + library + '\x1f' + std::to_string(library_version) + '\x1f' + module + '\x1f' +
               std::to_string(module_version);
    }
};

struct Symbol {
    std::string name;
    unsigned type, binding, section;
    uint64_t value, size;
    Identity identity;
};
struct Relocation {
    uint64_t target, kind, symbol;
    int64_t addend;
};

struct Module {
    std::string elf;
    std::vector<Ph> ph;
    std::map<uint64_t, uint64_t> tags;
    std::vector<Symbol> symbols;
    std::vector<Relocation> relocs;
    std::map<std::string, std::pair<std::string, unsigned>> libraries, modules;
    std::string sha256;

    uint64_t tag(uint64_t t) const {
        auto it = tags.find(t);
        if (it == tags.end()) throw Failure{"dynamic tag " + hex0x(t) + " missing"};
        return it->second;
    }
    Identity identity(const std::string &name) const {
        Identity id;
        size_t a = name.find('#');
        if (a == std::string::npos) return id;
        size_t b = name.find('#', a + 1);
        if (b == std::string::npos || name.find('#', b + 1) != std::string::npos) return id;
        auto lib = libraries.find(name.substr(a + 1, b - a - 1));
        auto mod = modules.find(name.substr(b + 1));
        if (lib == libraries.end() || mod == modules.end()) throw Failure{"unknown library or module ID in " + name};
        id.ok = true;
        id.nid = name.substr(0, a);
        id.library = lib->second.first;
        id.library_version = lib->second.second;
        id.module = mod->second.first;
        id.module_version = mod->second.second;
        return id;
    }
};

// The "NID#library" name an import is stored under. The host's tables are keyed this way because the short letter IDs
// inside each eboot are not the same from game to game.
std::string canonical(const Identity &id, const std::string &fallback) { return id.ok ? id.nid + "#" + id.library : fallback; }

const Ph *find_ph(const std::vector<Ph> &ph, uint64_t type) {
    for (const Ph &p : ph) if (p.type == type) return &p;
    return nullptr;
}

Module load_module(const std::wstring &path) {
    std::string source;
    if (!read_file(path, source)) throw Failure{"[Errno 2] No such file or directory: '" + shown(path) + "'"};
    Self self = parse_self(source);
    Module m;
    m.elf = std::move(self.elf);
    m.ph = self.ph;
    const Ph *dp = find_ph(m.ph, 2);
    if (!dp) throw Failure{"no dynamic segment"};
    std::vector<std::pair<uint64_t, uint64_t>> dynamic;
    for (uint64_t pos = dp->offset; pos < dp->offset + dp->filesz; pos += 16) {
        uint64_t tag = u(m.elf, (long long)pos, 8), value = u(m.elf, (long long)pos + 8, 8);
        if (!tag) break;
        dynamic.push_back({tag, value});
    }
    for (auto &d : dynamic) m.tags[d.first] = d.second;
    const Ph *lp = find_ph(m.ph, 0x61000000);
    if (!lp) throw Failure{"no dynamic data segment"};
    std::string blob = span(m.elf, (long long)lp->offset, (long long)lp->filesz);
    std::string strings = span(blob, (long long)m.tag(0x61000035), (long long)m.tag(0x61000037));
    for (auto &d : dynamic) {
        uint64_t tag = d.first, value = d.second;
        if (tag != 0x61000013 && tag != 0x61000015 && tag != 0x6100000d && tag != 0x6100000f) continue;
        auto &table = (tag == 0x61000013 || tag == 0x61000015) ? m.libraries : m.modules;
        std::string key = encode_id(value >> 48);
        std::pair<std::string, unsigned> identity{cstring(strings, (long long)(value & 0xffffffff)),
                                                  (unsigned)((value >> 32) & 65535)};
        auto it = table.find(key);
        if (it != table.end() && it->second != identity)
            throw Failure{"one module or library ID is given two different meanings"};
        table[key] = identity;
    }
    std::string symtab = span(blob, (long long)m.tag(0x61000039), (long long)m.tag(0x6100003f));
    for (size_t pos = 0; pos < symtab.size(); pos += 24) {
        need(symtab, (long long)pos, 24);
        Symbol s;
        uint64_t n = u(symtab, (long long)pos, 4), info = u(symtab, (long long)pos + 4, 1);
        s.section = (unsigned)u(symtab, (long long)pos + 6, 2);
        s.value = u(symtab, (long long)pos + 8, 8);
        s.size = u(symtab, (long long)pos + 16, 8);
        s.type = (unsigned)(info & 15);
        s.binding = (unsigned)(info >> 4);
        s.name = cstring(strings, (long long)n);
        if (!s.name.empty()) s.identity = m.identity(s.name);
        m.symbols.push_back(s);
    }
    const uint64_t tables[2][2] = {{0x61000029, 0x6100002d}, {0x6100002f, 0x61000031}};
    for (auto &t : tables) {
        std::string table = span(blob, (long long)m.tag(t[0]), (long long)m.tag(t[1]));
        for (size_t pos = 0; pos < table.size(); pos += 24) {
            need(table, (long long)pos, 24);
            uint64_t info = u(table, (long long)pos + 8, 8);
            m.relocs.push_back({u(table, (long long)pos, 8), info & 0xffffffff, info >> 32,
                                (int64_t)u(table, (long long)pos + 16, 8)});
        }
    }
    m.sha256 = sha256_hex(source);
    return m;
}

void put64(std::string &image, uint64_t at, uint64_t value) {
    need(image, (long long)at, 8);
    for (int i = 0; i < 8; ++i) image[(size_t)at + i] = (char)(value >> (8 * i));
}

// Turns each initial-exec `mov rax, fs:[0]` in the code segments into the same load from gs:, by swapping the 0x64
// prefix for 0x65. The host needs fs for itself (on Linux glibc used it). Returns how many were changed.
int patch_fs_loads(std::string &image, const std::vector<Ph> &ph, uint64_t base) {
    static const std::string fs_load("\x64\x48\x8b\x04\x25\x00\x00\x00\x00", 9);
    int patched = 0;
    for (const Ph &p : ph) {
        if (p.type != 1 || !(p.flags & 1)) continue;
        uint64_t start = base + p.vaddr, end = std::min<uint64_t>(base + p.vaddr + p.filesz, image.size());
        size_t at = start <= image.size() ? image.find(fs_load, (size_t)start) : std::string::npos;
        while (at != std::string::npos && at + fs_load.size() <= end) {
            image[at] = 0x65;
            ++patched;
            at = image.find(fs_load, at + fs_load.size());
        }
    }
    return patched;
}

void add64(std::string &o, uint64_t v) {
    for (int i = 0; i < 8; ++i) o += (char)(v >> (8 * i));
}

struct Export {
    uint64_t address, kind;
};
struct TableEntry {
    std::string file, sha256;
    uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module, tls_relocations, exports;
};
struct Relocated {
    uint64_t target, kind;
    int64_t value, addend;
};

std::string link(const std::wstring &game, const std::wstring &out, const std::vector<std::string> &module_names,
                 const std::string &executable) {
    Module main = load_module(join(game, std::wstring(executable.begin(), executable.end())));
    std::string raw;
    if (!read_file(join(out, L"boot.bin"), raw))
        throw Failure{"[Errno 2] No such file or directory: '" + shown(join(out, L"boot.bin")) + "'"};
    need(raw, 0, 56);
    if (raw.compare(0, 8, "G3PROBE2") != 0)
        throw Failure{"boot.bin is not a G3PROBE2 image straight from the preparation"};
    const uint64_t size = u(raw, 8, 8), entry = u(raw, 16, 8), ns = u(raw, 24, 8), nr = u(raw, 32, 8), ni = u(raw, 40, 8),
                   flags = u(raw, 48, 8);
    long long pos = 56;
    std::vector<std::array<uint64_t, 3>> segments;
    for (uint64_t i = 0; i < ns; ++i, pos += 24) segments.push_back({u(raw, pos, 8), u(raw, pos + 8, 8), u(raw, pos + 16, 8)});
    std::vector<std::string> names;
    for (uint64_t i = 0; i < ni; ++i, pos += 128) {
        std::string field = span(raw, pos, 128);
        names.push_back(field.substr(0, field.find('\0')));
    }
    std::vector<Relocated> relocs;
    for (uint64_t i = 0; i < nr; ++i, pos += 32)
        relocs.push_back({u(raw, pos, 8), u(raw, pos + 8, 8), (int64_t)u(raw, pos + 16, 8), (int64_t)u(raw, pos + 24, 8)});
    std::string image = span(raw, pos, (long long)size);
    if (pos + (long long)size != (long long)raw.size()) throw Failure{"boot.bin has extra bytes after its image"};
    raw.clear();
    raw.shrink_to_fit();

    // Import slots by identity. Identities keep the position where they first appeared; when one shows up again, the
    // newer slot number replaces the older one (the same thing a Python dict does on reassignment).
    std::vector<std::pair<std::string, size_t>> order;
    std::vector<Identity> order_identity;
    std::map<std::string, size_t> where;
    auto remember = [&](const Identity &id, size_t index) {
        const std::string key = id.key();
        auto it = where.find(key);
        if (it != where.end()) {
            order[it->second].second = index;
            return;
        }
        where[key] = order.size();
        order.push_back({key, index});
        order_identity.push_back(id);
    };
    for (size_t i = 0; i < names.size(); ++i) {
        Identity id = main.identity(names[i]);
        remember(id, i);
        names[i] = canonical(id, names[i]);
    }
    auto imported = [&](const Symbol &symbol) -> size_t {
        auto it = where.find(symbol.identity.key());
        if (it != where.end()) return order[it->second].second;
        if (!symbol.identity.ok) throw Failure{"an import without a name"};
        remember(symbol.identity, names.size());
        names.push_back(canonical(symbol.identity, ""));
        return names.size() - 1;
    };

    std::map<std::string, Export> exports;
    std::map<std::pair<std::string, std::string>, std::vector<Export>> by_nid;
    std::vector<TableEntry> table;
    uint64_t base = (size + 65535) & ~65535ull;
    int fs_patched = patch_fs_loads(image, main.ph, 0);
    uint64_t tls_module = 2;
    for (const std::string &filename : module_names) {
        const std::wstring file_w(filename.begin(), filename.end());
        Module m = load_module(join(join(game, L"sce_module"), file_w));
        std::vector<Ph> loads;
        for (const Ph &p : m.ph) if (p.type == 1 || p.type == 0x61000010) loads.push_back(p);
        if (loads.empty()) throw Failure{"max() arg is an empty sequence"};
        uint64_t modsize = 0;
        for (const Ph &p : loads) modsize = std::max(modsize, p.vaddr + p.memsz);
        if (base + modsize > 512ull * 1024 * 1024) throw Failure{"the linked image would be larger than 512 MiB"};
        if (base + modsize < image.size()) throw Failure{"negative count"};
        image.resize((size_t)(base + modsize), '\0');
        auto mapped = [&](uint64_t address, uint64_t length) {
            for (const Ph &p : loads)
                if (p.vaddr <= address && address + length <= p.vaddr + p.memsz) return true;
            return false;
        };
        for (const Ph &p : loads) {
            if (p.filesz > p.memsz) throw Failure{filename + ": a segment's file size is larger than its memory size"};
            std::string bytes = span(m.elf, (long long)p.offset, (long long)p.filesz);
            std::memcpy(&image[(size_t)(base + p.vaddr)], bytes.data(), bytes.size());
            segments.push_back({base + p.vaddr, p.memsz, p.flags});
        }
        const Ph *tls = find_ph(m.ph, 7);
        uint64_t module_id = 0;
        if (tls) {
            if (tls->filesz > tls->memsz || !mapped(tls->vaddr, tls->memsz))
                throw Failure{filename + ": thread-local segment laid out in a way the linker can't handle"};
            module_id = tls_module++;
        }
        uint64_t tls_relocations = 0;
        for (const Relocation &r : m.relocs) {
            if (!mapped(r.target, 8))
                throw Failure{filename + ": a relocation points outside the module at " + hex0x(r.target)};
            const uint64_t target = r.target + base;
            if (r.kind == 8) {
                relocs.push_back({target, 0, (int64_t)base + r.addend, 0});
            } else if (r.kind == 16) {
                if (r.symbol >= m.symbols.size()) throw Failure{"list index out of range"};
                const Symbol &symbol = m.symbols[(size_t)r.symbol];
                if (!symbol.name.empty() || symbol.type != 3 || r.addend || !module_id)
                    throw Failure{filename + ": thread-local relocation of a kind the linker can't handle"};
                put64(image, target, module_id);
                ++tls_relocations;
            } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
                if (r.symbol >= m.symbols.size()) throw Failure{"list index out of range"};
                const Symbol &symbol = m.symbols[(size_t)r.symbol];
                if (symbol.section) {
                    if (symbol.section == 0xfff1)
                        put64(image, target, symbol.value + (uint64_t)r.addend);
                    else
                        relocs.push_back({target, 0, (int64_t)(base + symbol.value) + r.addend, 0});
                } else if (!symbol.identity.ok && symbol.binding == 2) {
                    put64(image, target, 0);   // weak symbol nobody defines, e.g. module_start/module_stop
                } else {
                    const uint64_t rk = symbol.type == 1 ? 2 : 1;
                    if ((rk == 1 && r.addend) || !(0 <= r.addend && r.addend < 4096))
                        throw Failure{filename + ": an import carries an addend the linker can't express"};
                    relocs.push_back({target, rk, (int64_t)imported(symbol), r.addend});
                }
            } else {
                throw Failure{filename + ": relocation type not handled: " + std::to_string(r.kind)};
            }
        }
        uint64_t count = 0;
        for (const Symbol &s : m.symbols) {
            if (!(s.section && (s.binding == 1 || s.binding == 2) && s.identity.ok && (s.type == 1 || s.type == 2))) continue;
            if (!mapped(s.value, std::max<uint64_t>(1, s.size)))
                throw Failure{filename + ": an export lies outside the loaded segments"};
            const std::string key = s.identity.key();
            if (exports.count(key)) throw Failure{filename + ": the same export is defined more than once"};
            Export e{base + s.value, s.type == 1 ? 2u : 1u};
            exports[key] = e;
            by_nid[{s.identity.nid, s.identity.library}].push_back(e);
            ++count;
        }
        fs_patched += patch_fs_loads(image, m.ph, base);
        auto init = m.tags.find(12);
        table.push_back({filename, m.sha256, base, modsize, base + (init != m.tags.end() ? init->second : 0),
                         tls ? base + tls->vaddr : 0, tls ? tls->memsz : 0, tls ? tls->filesz : 0, module_id, tls_relocations,
                         count});
        base = (base + modsize + 65535) & ~65535ull;
    }

    std::vector<std::array<uint64_t, 3>> bindings;
    std::vector<std::string> unresolved;
    for (size_t k = 0; k < order.size(); ++k) {
        const Identity &id = order_identity[k];
        const size_t index = order[k].second;
        const Export *found = nullptr;
        auto e = exports.find(order[k].first);
        if (id.ok && e != exports.end()) found = &e->second;
        if (!found && id.ok && id.library == "libSceLibcInternal") {
            auto c = by_nid.find({id.nid, "libc"});
            if (c != by_nid.end() && c->second.size() == 1) found = &c->second[0];
        }
        if (found)
            bindings.push_back({(uint64_t)index, found->address, found->kind});
        else
            unresolved.push_back(names[index]);
    }

    const Ph *main_tls = find_ph(main.ph, 7);
    uint64_t tls_values[4] = {0, 0, 0, 0};
    if (main_tls) {
        tls_values[0] = main_tls->vaddr;
        tls_values[1] = main_tls->filesz;
        tls_values[2] = main_tls->memsz;
        tls_values[3] = main_tls->align;
        if (main_tls->filesz > main_tls->memsz || main_tls->memsz > 1024 * 1024 || main_tls->vaddr + main_tls->filesz > size)
            throw Failure{"the eboot's thread-local segment is laid out in a way the linker can't handle"};
    }
    const Ph *procparam = find_ph(main.ph, 0x61000001);
    if (!procparam) throw Failure{"no process parameter segment"};

    std::string o;
    o.reserve(image.size() + names.size() * 128 + relocs.size() * 32 + 4096);
    o += "G3PROBE5";
    for (uint64_t v : {(uint64_t)image.size(), entry, (uint64_t)segments.size(), (uint64_t)relocs.size(),
                       (uint64_t)names.size(), flags})
        add64(o, v);
    add64(o, procparam->vaddr);
    for (uint64_t v : tls_values) add64(o, v);
    add64(o, table.size());
    for (const TableEntry &t : table)
        for (uint64_t v : {t.base, t.size, t.init, t.tls_address, t.tls_memsz, t.tls_filesz, t.tls_module}) add64(o, v);
    add64(o, bindings.size());
    for (auto &b : bindings) for (uint64_t v : b) add64(o, v);
    for (auto &s : segments) for (uint64_t v : s) add64(o, v);
    for (const std::string &name : names) {
        if (name.size() >= 128) throw Failure{"an import name is 128 bytes or longer"};
        o += name;
        o.append(128 - name.size(), '\0');
    }
    for (const Relocated &r : relocs) {
        add64(o, r.target);
        add64(o, r.kind);
        add64(o, (uint64_t)r.value);
        add64(o, (uint64_t)r.addend);
    }
    o += image;
    image.clear();
    write_file(join(out, L"boot-linked.bin"), o);
    o.clear();

    Value report = Value::object(), modules_v;
    modules_v.kind = Value::Array;
    for (const TableEntry &t : table) {
        Value m = Value::object();
        m.set("file", Value::text(t.file));
        m.set("base", Value::text(hex0x(t.base)));
        m.set("size", Value::integer((long long)t.size));
        m.set("init", Value::text(hex0x(t.init)));
        m.set("tls_address", Value::text(hex0x(t.tls_address)));
        m.set("tls_memsz", Value::integer((long long)t.tls_memsz));
        m.set("tls_filesz", Value::integer((long long)t.tls_filesz));
        m.set("tls_module", Value::integer((long long)t.tls_module));
        m.set("tls_relocations", Value::integer((long long)t.tls_relocations));
        m.set("exports", Value::integer((long long)t.exports));
        m.set("sha256", Value::text(t.sha256));
        modules_v.items.push_back(m);
    }
    report.set("modules", modules_v);
    report.set("bindings", Value::integer((long long)bindings.size()));
    report.set("imports", Value::integer((long long)names.size()));
    report.set("fs_loads_patched", Value::integer(fs_patched));
    Value tls_v = Value::object();
    const char *tls_keys[4] = {"vaddr", "filesz", "memsz", "align"};
    for (int i = 0; i < 4; ++i) tls_v.set(tls_keys[i], Value::integer((long long)tls_values[i]));
    report.set("main_tls", tls_v);
    Value unresolved_v;
    unresolved_v.kind = Value::Array;
    for (const std::string &n : unresolved) unresolved_v.items.push_back(Value::text(n));
    report.set("unresolved_imports", unresolved_v);
    std::string text = json::dump(report) + "\n", crlf;
    for (char c : text) { if (c == '\n') crlf += "\r\n"; else crlf += c; }   // CRLF line ends, as the Python tool wrote
    write_file(join(out, L"link.json"), crlf);

    std::string summary;
    for (size_t i = 0; i < table.size(); ++i) summary += (i ? ", " : "") + table[i].file + "@" + hex0x(table[i].base);
    return "Modules linked: " + summary + "; " + std::to_string(bindings.size()) +
           " imports bound to module exports, " + std::to_string(unresolved.size()) + " for the host runtime to supply, fs->gs loads rewritten: " +
           std::to_string(fs_patched) + "\n";
}

}  // namespace

extern "C" {

// Builds boot-linked.bin and link.json in out_dir, from the boot.bin already there plus the game folder's executable
// and the modules it ships. `modules` lists file names inside sce_module, one per line; null or empty picks libc.prx
// and libSceFios2.prx. Returns 0 with a one-line summary in `out`, or 1 with the reason it failed.
UN_API int un_link_modules(const wchar_t *game, const wchar_t *out_dir, const char *executable, const char *modules,
                           char *out, int cap) {
    try {
        std::vector<std::string> names;
        std::string list = modules ? modules : "";
        size_t at = 0;
        while (at < list.size()) {
            size_t end = list.find('\n', at);
            if (end == std::string::npos) end = list.size();
            if (end > at) names.push_back(list.substr(at, end - at));
            at = end + 1;
        }
        if (!modules || !*modules) names = {"libc.prx", "libSceFios2.prx"};
        return give(link(game ? game : L"", out_dir ? out_dir : L"", names, executable ? executable : "eboot.bin"), out, cap);
    } catch (const Failure &f) {
        int status = give(f.message, out, cap);
        return status < 0 ? status : 1;
    } catch (const std::bad_alloc &) {
        int status = give("out of memory", out, cap);
        return status < 0 ? status : 1;
    }
}

}  // extern "C"
