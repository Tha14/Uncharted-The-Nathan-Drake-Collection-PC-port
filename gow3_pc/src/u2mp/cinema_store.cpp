// u2mp cinema file store (see cinema_store.h). PS3 1.10 originals: refresh 0x4631d8, make room
// 0x4635c8, load 0x463738, open 0x4638cc, delete 0x463ab8, protect 0x463ba4, pump 0x463ce8,
// finish 0x464464.
#include "cinema_store.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace u2mp::cinema {
namespace fs = std::filesystem;

uint32_t get32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
void put32(uint8_t *p, uint32_t v) { std::memcpy(p, &v, 4); }
uint64_t get64(const uint8_t *p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
void put64(uint8_t *p, uint64_t v) { std::memcpy(p, &v, 8); }

namespace {

bool local_time(std::time_t t, std::tm &out) {
#ifdef _WIN32
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}

fs::path path_of(const std::string &dir, const std::string &name) { return fs::path(dir) / name; }

bool is_protected(const fs::path &p) {
    std::error_code ec;
    const auto st = fs::status(p, ec);
    return !ec && (st.permissions() & fs::perms::owner_write) == fs::perms::none;
}

bool read_header(const fs::path &p, std::vector<uint8_t> &header) {
    header.assign(kHeaderSize, 0);
    std::FILE *f = std::fopen(p.string().c_str(), "rb");
    if (!f) return false;
    const bool ok = std::fread(header.data(), 1, kHeaderSize, f) == kHeaderSize;
    std::fclose(f);
    return ok && valid_header(header.data());
}

// Every valid recording in `dir`, newest first.
std::vector<File> list_all(const std::string &dir) {
    std::vector<File> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        if (!it->is_regular_file(ec) || p.extension() != kExtension) continue;
        File f;
        f.name = p.filename().string();
        const auto size = fs::file_size(p, ec);
        if (ec || size < kHeaderSize || size > 0xffffffffu) continue;
        f.size = uint32_t(size);
        if (!read_header(p, f.header)) continue;
        f.time = get64(f.header.data() + hdr::kTime);
        f.protect = is_protected(p);
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const File &a, const File &b) {
        return a.time != b.time ? a.time > b.time : a.name > b.name;
    });
    return out;
}

}  // namespace

bool valid_header(const uint8_t *h) {
    const int32_t game = int32_t(get32(h + hdr::kGame));
    const int32_t players = int32_t(get32(h + hdr::kPlayerCount));
    return get32(h + hdr::kMagicOff) == kMagic && game >= -1 && game < 0x8000 && players >= 0 &&
           players <= hdr::kMaxPlayers;
}

bool ensure_dir(const std::string &dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return fs::is_directory(dir, ec);
}

std::vector<File> list(const std::string &dir) {
    auto all = list_all(dir);
    if (all.size() > size_t(kMaxFiles)) all.resize(kMaxFiles);
    return all;
}

void fill_entry(const File &f, uint8_t *out) {
    std::memset(out, 0, kEntrySize);
    std::tm tm{};
    if (local_time(std::time_t(f.time), tm))
        std::strftime(reinterpret_cast<char *>(out + entry::kDate), 0x100, "%a, %d %b %Y %H:%M:%S", &tm);
    std::snprintf(reinterpret_cast<char *>(out + entry::kName), 0x100, "%s", f.name.c_str());
    out[entry::kStat] = 0;
    put64(out + entry::kTime, f.time);
    put32(out + entry::kSize, f.size);
    put32(out + entry::kMode, f.protect ? kModeProtected : kModeWritable);
    if (f.header.size() == kHeaderSize) std::memcpy(out + entry::kHeader, f.header.data(), kHeaderSize);
}

bool make_room(const std::string &dir) {
    auto files = list_all(dir);
    while (files.size() >= size_t(kMaxFiles)) {
        auto oldest = std::find_if(files.rbegin(), files.rend(), [](const File &f) { return !f.protect; });
        if (oldest == files.rend()) return false;
        std::error_code ec;
        if (!fs::remove(path_of(dir, oldest->name), ec)) return false;
        files.erase(std::next(oldest).base());
    }
    return true;
}

bool remove(const std::string &dir, const std::string &name) {
    const fs::path p = path_of(dir, name);
    if (name.empty() || is_protected(p)) return false;
    std::error_code ec;
    return fs::remove(p, ec);
}

bool set_protected(const std::string &dir, const std::string &name, bool protect) {
    std::error_code ec;
    const auto write = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
    fs::permissions(path_of(dir, name), write, protect ? fs::perm_options::remove : fs::perm_options::add, ec);
    return !ec;
}

std::string new_name(const std::string &dir, std::time_t t) {
    char stamp[32] = "recording";
    std::tm tm{};
    if (local_time(t, tm)) std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    std::string name = std::string(stamp) + kExtension;
    std::error_code ec;
    for (int i = 2; fs::exists(path_of(dir, name), ec) && i < 100; ++i)
        name = std::string(stamp) + "-" + std::to_string(i) + kExtension;
    return name;
}

// ---- Writer ---------------------------------------------------------------------------------
Writer::~Writer() { abandon(); }

bool Writer::open(const std::string &path) {
    abandon();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    path_ = path;
    failed_ = false;
    static const uint8_t zero[kHeaderSize] = {};
    failed_ = std::fwrite(zero, 1, kHeaderSize, f_) != kHeaderSize;
    return !failed_;
}

void Writer::flush(Stream &s) {
    auto *bytes = reinterpret_cast<uint8_t *>(s.data);
    while (s.bits + 7 >= (kWriteChunk + 1) * 8) {   // the PS4 pump's test: bits + 7 >= 0x40008
        if (f_ && std::fwrite(bytes, 1, kWriteChunk, f_) != kWriteChunk) failed_ = true;
        std::memmove(bytes, bytes + kWriteChunk, kBufferBytes - kWriteChunk);
        std::memset(bytes + kBufferBytes - kWriteChunk, 0, kWriteChunk);
        s.bits -= uint32_t(kWriteChunk * 8);
    }
}

bool Writer::finish(Stream &s, const uint8_t *header) {
    if (!f_) return false;
    flush(s);
    const size_t words = (size_t(s.bits) + 31) / 32;
    if (std::fwrite(s.data, 4, words, f_) != words) failed_ = true;
    if (std::fseek(f_, 0, SEEK_SET) != 0 || std::fwrite(header, 1, kHeaderSize, f_) != kHeaderSize) failed_ = true;
    if (std::fclose(f_) != 0) failed_ = true;
    f_ = nullptr;
    if (failed_) {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    return !failed_;
}

void Writer::abandon() {
    if (!f_) return;
    std::fclose(f_);
    f_ = nullptr;
    std::error_code ec;
    fs::remove(path_, ec);
}

// ---- Reader ---------------------------------------------------------------------------------
Reader::~Reader() { close(); }

bool Reader::open(const std::string &path, uint8_t *header_out) {
    close();
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) return false;
    eof_ = false;
    if (std::fread(header_out, 1, kHeaderSize, f_) != kHeaderSize || !valid_header(header_out)) {
        close();
        return false;
    }
    return true;
}

void Reader::fill(Stream &s) {
    auto *bytes = reinterpret_cast<uint8_t *>(s.data);
    // The PS4 pump's test: bits + 7 >= 0x20008 (the cursor is past the first 16 KB).
    while (s.bits + 7 >= (kDiscardChunk + 1) * 8 && s.bytes >= kDiscardChunk) {
        std::memmove(bytes, bytes + kDiscardChunk, s.bytes - kDiscardChunk);
        s.bytes -= uint32_t(kDiscardChunk);
        s.bits -= uint32_t(kDiscardChunk * 8);
        std::memset(bytes + s.bytes, 0, kBufferBytes - s.bytes);
    }
    while (f_ && !eof_ && s.bytes <= kBufferBytes - kReadChunk) {
        const size_t n = std::fread(bytes + s.bytes, 1, kReadChunk, f_);
        s.bytes += uint32_t(n);
        if (n < kReadChunk) eof_ = true;
    }
}

void Reader::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
    eof_ = false;
}

}  // namespace u2mp::cinema
