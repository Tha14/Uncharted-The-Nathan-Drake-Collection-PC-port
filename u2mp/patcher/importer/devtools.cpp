// importer/devtools.cpp: the converter commands for maintainers (u2import.exe; the import never runs them).
//
//   u2import --dc learn   <ps3 base> <ps4 game> <schema.json> [net-patch9.bin]   the DC layout schema from every PS3
//                         dc1/<x>.bin with a PS4 dc1/ps4/<x>.bin (and net-patch9 against net.bin, partially)
//   u2import --dc test    <ps3 base> <ps4 game> <schema.json> [net-patch9.bin]   every pair converted and compared (the
//                         schema is learned first when the file is missing)
//   u2import --dc convert <schema.json> <ps3 module.bin> <out.bin>
//   u2import --dc info    <module.bin>                                         header and symbols of a U2 module
//   u2import --hk-tables --ps3-elf <EBOOT.elf> --ps4 <game folder> --out <u2hkclasses.json> [--objdump <path>]
//   u2import --twin-build <ps3 mp skin.pak> <ps3 twin.pak> <ps4 twin.pak> <out.pak>
//   u2import --psarc list <file.psarc> | --psarc extractall <file.psarc> <folder>
#include "actor.hpp"
#include "dcconv.hpp"

#include <cstdio>

namespace u2imp {

std::vector<std::tuple<std::string, Bytes, Bytes>> dc_pairs(const PS3 &ps3, const fs::path &dcdir);   // netpatch.cpp
Bytes dc_convert(const std::string &schema, const Bytes &ps3, u3::dcconv::Counts *rep);
std::pair<size_t, size_t> dc_check(const Bytes &ps4);
void havok_tables(const std::string &ps3_elf, const std::string &ps4_game, const fs::path &out, const std::string &objdump);   // havok.cpp

namespace {

void out(const std::string &s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
}

// repr() of a JSON value as Python prints the dict / list it was
std::string pyrepr(const Value &v) {
    switch (v.kind) {
    case Value::Null: return "None";
    case Value::Bool: return v.b ? "True" : "False";
    case Value::Int: return std::to_string(v.i);
    case Value::Float: return json::real(v.f);
    case Value::String: return py_repr(utf8_latin1(v.s));
    case Value::Array: {
        std::string o = "[";
        for (size_t i = 0; i < v.items.size(); i++) o += (i ? ", " : "") + pyrepr(v.items[i]);
        return o + "]";
    }
    case Value::Object: {
        std::string o = "{";
        for (size_t i = 0; i < v.fields.size(); i++) o += (i ? ", " : "") + py_repr(utf8_latin1(v.fields[i].first)) + ": " + pyrepr(v.fields[i].second);
        return o + "}";
    }
    }
    return "";
}

std::string nonzero(const u3::dcconv::Counts &rep) {   // {k: v for k, v in rep.items() if v}
    std::string o = "{";
    for (const std::string &k : rep.order)
        if (rep.n.at(k)) o += (o.size() > 1 ? ", " : "") + py_repr(k) + ": " + std::to_string(rep.n.at(k));
    return o + "}";
}

uint32_t rd(const Bytes &d, size_t o, bool be) {
    if (o + 4 > d.size()) throw std::runtime_error("struct.error: unpack_from requires a buffer of at least 4 bytes");
    return be ? uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3]
              : uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24;
}

int dc_info(const fs::path &path) {
    const Bytes d = read_bytes(path);
    const std::string magic = d.size() >= 4 ? std::string(d.begin(), d.begin() + 4) : "";
    const bool be = magic == "DC00";
    if (!be && magic != "00CD") throw std::runtime_error("ValueError: not a DC module");
    auto word = [&](size_t o) { return be ? uint64_t(rd(d, o, true)) : uint64_t(rd(d, o, false)) | uint64_t(rd(d, o + 4, false)) << 32; };
    const uint64_t reloc = word(8), root = word(0x10);
    if (!(0x14 <= root && root < reloc && reloc + 4 <= d.size()))
        throw std::runtime_error(fmt("ValueError: bad U2 DC header (root %#llx reloc %#llx size %#zx)", (unsigned long long)root, (unsigned long long)reloc, d.size()));
    const uint32_t root_hdr = rd(d, size_t(root - 4), be), word0 = rd(d, size_t(root), be), n = rd(d, size_t(root + 4), be);
    const uint64_t es = be ? 8 : 16;
    std::vector<std::pair<uint32_t, uint64_t>> syms;
    for (uint32_t i = 0; i < n; i++) {
        const size_t o = size_t(root + 8 + i * es);
        syms.push_back({rd(d, o, be), be ? rd(d, o + 4, true) : word(o + 8)});
    }
    const uint64_t data_start = root + 8 + n * es;
    const uint32_t cnt = rd(d, size_t(reloc), be);
    std::set<uint64_t> slots;
    if (be) {
        if (reloc + 4 + 4ull * cnt > d.size()) throw std::runtime_error("ValueError: relocation list past the end of the module");
        for (uint32_t i = 0; i < cnt; i++) slots.insert(rd(d, size_t(reloc + 4 + 4ull * i), true));
    } else
        for (uint32_t bi = 0; bi < cnt && reloc + 4 + bi < d.size(); bi++)
            for (int k = 0; k < 8; k++)
                if (d[size_t(reloc + 4 + bi)] >> k & 1) slots.insert((uint64_t(bi) * 8 + uint64_t(k)) * 8);
    out(fmt("%s reloc %#llx root %#llx type %#x word0 %u symbols %zu data %#llx slots %zu", be ? "PS3" : "PS4", (unsigned long long)reloc, (unsigned long long)root,
            root_hdr, word0, syms.size(), (unsigned long long)data_start, slots.size()));
    for (const auto &[s, v] : syms) out(fmt("  %08x -> %#llx", s, (unsigned long long)v));
    return 0;
}

int dc_learn_test(bool test, const std::vector<std::string> &v) {
    if (v.size() < 5) throw std::runtime_error("usage: u2import --dc learn|test <ps3 base> <ps4 game> <schema.json> [net-patch9.bin]");
    const PS3 ps3(v[2], "");
    const PS4 ps4(v[3]);
    const fs::path dcdir = ps4.main / "dc1" / "ps4", schema_path = P(v[4]);
    const auto pl = dc_pairs(ps3, dcdir);
    std::error_code ec;
    std::string schema;
    if (!test || !fs::is_regular_file(schema_path, ec)) {
        std::vector<std::tuple<std::string, Bytes, Bytes>> extra;
        if (v.size() > 5 && fs::is_regular_file(dcdir / "net.bin", ec)) extra.push_back({"net-patch9.bin~net.bin", read_bytes(P(v[5])), read_bytes(dcdir / "net.bin")});
        schema = u3::dcconv::learn_u2(pl, extra, [](const std::string &s) { out(s); });
        write_text(schema_path, schema);
        json::Value sv;
        json::parse(schema, sv);
        const Value *keys = sv.get("keys"), *stats = sv.get("stats");
        out(fmt("learned %zu keys from %zu pairs ", keys ? keys->fields.size() : 0, pl.size()) + (stats ? pyrepr(*stats) : "None"));
        if (!test) return 0;
    } else
        schema = read_text(schema_path);
    std::vector<std::pair<std::string, long long>> tot;
    auto bump = [&](const std::string &k) {
        for (auto &[n, c] : tot)
            if (n == k) return void(c++);
        tot.push_back({k, 1});
    };
    for (const auto &[name, d3, want] : pl) {
        Bytes got;
        u3::dcconv::Counts rep;
        try {
            got = dc_convert(schema, d3, &rep);
        } catch (const std::exception &e) {
            bump("error");
            out("ERROR " + name + " " + e.what());
            continue;
        }
        if (got == want) {
            bump("identical");
            continue;
        }
        bump("different");
        const size_t n = std::min(got.size(), want.size());
        size_t fd = 0;
        while (fd < n && got[fd] == want[fd]) fd++;
        out(fmt("DIFF %-36s %7zu %7zu first@%zx ", name.c_str(), got.size(), want.size(), fd) + nonzero(rep));
    }
    std::string r = "{";
    long long ident = 0;
    for (const auto &[k, c] : tot) {
        r += (r.size() > 1 ? ", " : "") + py_repr(k) + ": " + std::to_string(c);
        if (k == "identical") ident = c;
    }
    out(r + "} " + fmt("match rate %.1f%%", 100.0 * double(ident) / double(std::max<size_t>(1, pl.size()))));
    return 0;
}

}  // namespace

// a maintainer command: its exit code, or nullopt when v is not one
std::optional<int> dev_command(const std::vector<std::string> &v) {
    if (v.empty()) return std::nullopt;
    if (v[0] == "--dc") {
        const std::string cmd = v.size() > 1 ? v[1] : "";
        if (cmd == "info" && v.size() > 2) return dc_info(P(v[2]));
        if (cmd == "learn" || cmd == "test") return dc_learn_test(cmd == "test", v);
        if (cmd == "convert" && v.size() > 4) {
            u3::dcconv::Counts rep;
            const Bytes o = dc_convert(read_text(P(v[2])), read_bytes(P(v[3])), &rep);
            const auto [nsym, nptr] = dc_check(o);
            out(fmt("{'symbols': %zu, 'pointers': %zu, 'size': %zu} ", nsym, nptr, o.size()) + nonzero(rep));
            write_bytes(P(v[4]), o);
            return 0;
        }
        std::fputs("usage: u2import --dc learn|test <ps3 base> <ps4 game> <schema.json> [net-patch9.bin] | convert <schema.json> <in> <out> | info <module>\n", stderr);
        return 2;
    }
    if (v[0] == "--hk-tables") {
        std::map<std::string, std::string> o;
        for (size_t i = 1; i + 1 < v.size(); i += 2) o[v[i]] = v[i + 1];
        if (o["--ps3-elf"].empty() || o["--ps4"].empty() || o["--out"].empty()) {
            std::fputs("usage: u2import --hk-tables --ps3-elf <EBOOT.elf> --ps4 <game folder> --out <file.json> [--objdump <path>]\n", stderr);
            return 2;
        }
        havok_tables(o["--ps3-elf"], o["--ps4"], P(o["--out"]), o["--objdump"]);
        return 0;
    }
    if (v[0] == "--twin-build") {
        if (v.size() < 5) {
            std::fputs("usage: u2import --twin-build <ps3 mp skin.pak> <ps3 twin.pak> <ps4 twin.pak> <out.pak>\n", stderr);
            return 2;
        }
        const auto r = twin_build(P(v[1]), P(v[2]), P(v[3]), P(v[4]));
        if (const auto *why = std::get_if<std::string>(&r)) {
            std::fputs(("NoTwin: " + *why + "\n").c_str(), stderr);
            return 1;
        }
        out(pyrepr(std::get<TwinStats>(r).json()));
        return 0;
    }
    if (v[0] == "--psarc") {
        if (v.size() > 2 && v[1] == "list") {
            u3::Psarc p(P(v[2]));
            for (size_t i = 0; i < p.names.size(); i++) out(fmt("%zu %llu ", i + 1, (unsigned long long)p.length(i + 1)) + p.names[i]);
            return 0;
        }
        if (v.size() > 3 && v[1] == "extractall") {
            u3::Psarc p(P(v[2]));
            for (size_t i = 0; i < p.names.size(); i++) {
                std::string n = p.names[i];
                while (!n.empty() && n[0] == '/') n.erase(0, 1);
                const fs::path dst = P(v[3]) / P(n);
                std::error_code ec;
                fs::create_directories(dst.parent_path(), ec);
                write_bytes(dst, p.read(i + 1));
            }
            out(fmt("extracted %zu", p.names.size()));
            return 0;
        }
        std::fputs("usage: u2import --psarc list <file> | --psarc extractall <file> <folder>\n", stderr);
        return 2;
    }
    return std::nullopt;
}

}  // namespace u2imp
