// mkzip.cpp - makes the release zip: mkzip <out.zip> <folder>
// Every file and folder under <folder> goes in, named "<folder name>/<path>" with forward slashes (UTF-8, flag 0x800
// when a name is not plain ASCII), files deflated with zlib (level 6), folders as stored "name/" entries. A plain zip
// with sizes in the local headers and a central directory, no zip64: what the launcher's updater
// (launcher/native/updater.cpp) and Windows' own zip support read. Built and run by tools/package_windows.sh.
#include <windows.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Entry {
    std::string name;
    uint32_t crc = 0, csize = 0, usize = 0, offset = 0, attr = 0;
    uint16_t method = 0, time = 0, date = 0, flags = 0;
};

void put16(std::string &o, uint32_t v) { o += char(v & 0xff), o += char((v >> 8) & 0xff); }
void put32(std::string &o, uint32_t v) { put16(o, v & 0xffff), put16(o, v >> 16); }

void dos_time(const fs::path &p, uint16_t &t, uint16_t &d) {   // the file's local modification time (1980-01-01 at least)
    WIN32_FILE_ATTRIBUTE_DATA a{};
    FILETIME local{};
    WORD dd = 0x21, dt = 0;
    if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) && FileTimeToLocalFileTime(&a.ftLastWriteTime, &local))
        FileTimeToDosDateTime(&local, &dd, &dt);
    t = dt;
    d = dd;
}

bool fail(const std::string &why) {
    std::fprintf(stderr, "mkzip: %s\n", why.c_str());
    return false;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: mkzip <out.zip> <folder>\n");
        return 2;
    }
    const fs::path out = argv[1], root = fs::absolute(fs::path(argv[2]));
    if (!fs::is_directory(root)) return fail("not a folder: " + fs::path(argv[2]).u8string()), 1;
    const std::string top = root.filename().u8string();
    std::vector<fs::path> items;
    for (const auto &e : fs::recursive_directory_iterator(root)) items.push_back(e.path());
    std::sort(items.begin(), items.end());
    std::ofstream z(out, std::ios::binary);
    if (!z) return fail("cannot write " + out.u8string()), 1;
    std::vector<Entry> entries;
    uint64_t pos = 0;
    auto write = [&](const std::string &s) {
        z.write(s.data(), std::streamsize(s.size()));
        pos += s.size();
    };
    auto add = [&](const fs::path &p, bool dir) -> bool {
        Entry e;
        std::string rel = fs::relative(p, root).generic_u8string();
        e.name = top + "/" + rel + (dir ? "/" : "");
        for (unsigned char c : e.name)
            if (c >= 0x80) e.flags = 0x800;
        dos_time(p, e.time, e.date);
        e.attr = dir ? (0x41edu << 16) | 0x10 : 0x81a4u << 16;   // drwxr-xr-x + MS-DOS folder / -rw-r--r--
        std::string data;
        if (!dir) {
            std::ifstream f(p, std::ios::binary);
            if (!f) return fail("cannot read " + p.u8string());
            std::string raw((std::istreambuf_iterator<char>(f)), {});
            e.usize = uint32_t(raw.size());
            e.crc = uint32_t(crc32(0, reinterpret_cast<const Bytef *>(raw.data()), uInt(raw.size())));
            z_stream s{};
            if (deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return fail("zlib");
            data.resize(deflateBound(&s, uLong(raw.size())));
            s.next_in = reinterpret_cast<Bytef *>(raw.data());
            s.avail_in = uInt(raw.size());
            s.next_out = reinterpret_cast<Bytef *>(data.data());
            s.avail_out = uInt(data.size());
            const int r = deflate(&s, Z_FINISH);
            deflateEnd(&s);
            if (r != Z_STREAM_END) return fail("zlib could not deflate " + p.u8string());
            data.resize(s.total_out);
            e.method = 8;
            if (data.size() >= raw.size()) data.swap(raw), e.method = 0;   // stored when deflate does not help
            e.csize = uint32_t(data.size());
        }
        if (pos > 0xffffffffull || data.size() > 0xffffffffull) return fail("the zip would need zip64");
        e.offset = uint32_t(pos);
        std::string h;
        put32(h, 0x04034b50), put16(h, 20), put16(h, e.flags), put16(h, e.method), put16(h, e.time), put16(h, e.date);
        put32(h, e.crc), put32(h, e.csize), put32(h, e.usize), put16(h, uint16_t(e.name.size())), put16(h, 0);
        write(h + e.name);
        write(data);
        entries.push_back(e);
        return true;
    };
    for (const fs::path &p : items)
        if (!add(p, fs::is_directory(p))) return 1;
    const uint64_t cd = pos;
    for (const Entry &e : entries) {
        std::string h;
        put32(h, 0x02014b50), put16(h, 0x0314), put16(h, 20), put16(h, e.flags), put16(h, e.method), put16(h, e.time);
        put16(h, e.date), put32(h, e.crc), put32(h, e.csize), put32(h, e.usize), put16(h, uint16_t(e.name.size()));
        put16(h, 0), put16(h, 0), put16(h, 0), put16(h, 0), put32(h, e.attr), put32(h, e.offset);
        write(h + e.name);
    }
    if (entries.size() > 0xffff || pos > 0xffffffffull) return fail("the zip would need zip64"), 1;
    std::string end;
    put32(end, 0x06054b50), put16(end, 0), put16(end, 0), put16(end, uint16_t(entries.size()));
    put16(end, uint16_t(entries.size())), put32(end, uint32_t(pos - cd)), put32(end, uint32_t(cd)), put16(end, 0);
    write(end);
    z.close();
    if (!z) return fail("cannot write " + out.u8string()), 1;
    std::printf("%s: %zu entries, %llu bytes\n", out.u8string().c_str(), entries.size(), (unsigned long long)pos);
    return 0;
}
