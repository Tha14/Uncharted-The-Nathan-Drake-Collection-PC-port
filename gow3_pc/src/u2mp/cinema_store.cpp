// u2mp cinema file store: the host-only half of the theater feature. It knows how recordings are
// laid out on disk, lists and maintains the store directory, and moves the replay stream between
// the game's 128 KB buffer and a file in fixed-size chunks. It never touches game memory, so the
// unit test can link it on its own.
//
// Reimplemented from the behaviour description u2mp/docs/spec/cinema.md.
#include "cinema_store.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace u2mp::cinema {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// Little-endian field access. The host is little-endian like the files and the game, so a plain
// unaligned copy is the whole conversion.

uint32_t get32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

void put32(uint8_t *p, uint32_t v) { std::memcpy(p, &v, sizeof v); }

uint64_t get64(const uint8_t *p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

void put64(uint8_t *p, uint64_t v) { std::memcpy(p, &v, sizeof v); }

bool valid_header(const uint8_t *header) {
    if (get32(header + hdr::kMagicOff) != kMagic) return false;
    const int32_t game = int32_t(get32(header + hdr::kGame));
    if (game < -1 || game > 0x7fff) return false;
    const int32_t players = int32_t(get32(header + hdr::kPlayerCount));
    return players >= 0 && players <= hdr::kMaxPlayers;
}

bool ensure_dir(const std::string &dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);   // an existing directory or a failure is judged below
    return fs::is_directory(dir, ec);
}

namespace {

// Local time of `t`, false when the C library cannot convert it.
bool local_time(std::time_t t, std::tm &out) {
#ifdef _WIN32
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}

// A recording counts as protected when its owner cannot write it (a read-only file on Windows).
// A file whose status cannot be read is treated as writable.
bool is_protected(const fs::path &p) {
    std::error_code ec;
    const fs::file_status st = fs::status(p, ec);
    if (ec) return false;
    return (st.permissions() & fs::perms::owner_write) == fs::perms::none;
}

// Reads the first kHeaderSize bytes of `p`; false when the file cannot supply all of them.
bool read_header(const fs::path &p, std::vector<uint8_t> &header) {
    std::FILE *f = std::fopen(p.string().c_str(), "rb");
    if (!f) return false;
    header.assign(kHeaderSize, 0);
    const size_t got = std::fread(header.data(), 1, kHeaderSize, f);
    std::fclose(f);
    return got == kHeaderSize;
}

// Every valid recording in `dir`, newest first. Anything that is not a readable recording is
// left out (and so can never be picked for deletion).
std::vector<File> scan(const std::string &dir) {
    std::vector<File> found;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) return found;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path p = it->path();
        std::error_code e;
        if (!fs::is_regular_file(p, e) || e) continue;
        if (p.extension().string() != kExtension) continue;
        const uintmax_t size = fs::file_size(p, e);
        if (e || size < kHeaderSize || size > 0xffffffffu) continue;
        File f;
        if (!read_header(p, f.header) || !valid_header(f.header.data())) continue;
        f.name = p.filename().string();
        f.size = uint32_t(size);
        f.time = get64(f.header.data() + hdr::kTime);
        f.protect = is_protected(p);
        found.push_back(std::move(f));
    }
    std::sort(found.begin(), found.end(), [](const File &a, const File &b) {
        if (a.time != b.time) return a.time > b.time;
        return a.name > b.name;
    });
    return found;
}

}  // namespace

std::vector<File> list(const std::string &dir) {
    std::vector<File> files = scan(dir);
    if (files.size() > size_t(kMaxFiles)) files.resize(kMaxFiles);
    return files;
}

void fill_entry(const File &f, uint8_t *out) {
    std::memset(out, 0, kEntrySize);
    std::tm tm{};
    if (local_time(std::time_t(f.time), tm))
        std::strftime(reinterpret_cast<char *>(out + entry::kDate), 0x100, "%a, %d %b %Y %H:%M:%S", &tm);
    // The name field keeps its terminator, so a long name is cut to 0xff characters.
    const size_t n = std::min(f.name.size(), size_t(0xff));
    std::memcpy(out + entry::kName, f.name.data(), n);
    out[entry::kStat] = 0;
    put64(out + entry::kTime, f.time);
    put32(out + entry::kSize, f.size);
    put32(out + entry::kMode, f.protect ? kModeProtected : kModeWritable);
    if (f.header.size() == kHeaderSize) std::memcpy(out + entry::kHeader, f.header.data(), kHeaderSize);
}

bool make_room(const std::string &dir) {
    std::vector<File> files = scan(dir);
    while (files.size() >= size_t(kMaxFiles)) {
        // The oldest file that may be deleted is the last unprotected one in newest-first order.
        auto victim = std::find_if(files.rbegin(), files.rend(), [](const File &f) { return !f.protect; });
        if (victim == files.rend()) return false;
        std::error_code ec;
        if (!fs::remove(fs::path(dir) / victim->name, ec)) return false;
        files.erase(std::next(victim).base());
    }
    return true;
}

bool remove(const std::string &dir, const std::string &name) {
    if (name.empty()) return false;
    const fs::path p = fs::path(dir) / name;
    if (is_protected(p)) return false;
    std::error_code ec;
    return fs::remove(p, ec);
}

bool set_protected(const std::string &dir, const std::string &name, bool protect) {
    constexpr fs::perms write_bits = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
    std::error_code ec;
    fs::permissions(fs::path(dir) / name, write_bits, protect ? fs::perm_options::remove : fs::perm_options::add,
                    ec);
    return !ec;
}

std::string new_name(const std::string &dir, std::time_t t) {
    std::string stamp = "recording";
    std::tm tm{};
    char text[64];
    if (local_time(t, tm) && std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &tm) > 0) stamp = text;
    auto taken = [&](const std::string &candidate) {
        std::error_code ec;
        return fs::exists(fs::path(dir) / candidate, ec) && !ec;
    };
    std::string candidate = stamp + kExtension;
    // Numbered alternatives end at -99; that last one is used even when it exists too.
    for (int n = 2; n <= 99 && taken(candidate); ++n) candidate = stamp + "-" + std::to_string(n) + kExtension;
    return candidate;
}

// ---------------------------------------------------------------------------------------------
// Writer: streams a recording to disk while the match runs.

Writer::~Writer() { abandon(); }

bool Writer::open(const std::string &path) {
    abandon();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    path_ = path;
    failed_ = false;
    // Room for the header, which is only known when the match ends.
    const std::vector<uint8_t> placeholder(kHeaderSize, 0);
    if (std::fwrite(placeholder.data(), 1, kHeaderSize, f_) != kHeaderSize) {
        failed_ = true;
        return false;
    }
    return true;
}

void Writer::flush(Stream &s) {
    // Once the cursor is past the first 32 KB, that chunk is complete: write it out and slide
    // the rest down so the 128 KB buffer never fills during a match.
    constexpr uint32_t chunk_bits = uint32_t(kWriteChunk * 8);
    auto *bytes = reinterpret_cast<uint8_t *>(s.data);
    while (uint32_t(s.bits + 7) >= chunk_bits + 8) {
        if (f_ && std::fwrite(bytes, 1, kWriteChunk, f_) != kWriteChunk) failed_ = true;
        std::memmove(bytes, bytes + kWriteChunk, kBufferBytes - kWriteChunk);
        std::memset(bytes + (kBufferBytes - kWriteChunk), 0, kWriteChunk);
        s.bits -= chunk_bits;
    }
}

bool Writer::finish(Stream &s, const uint8_t *header) {
    if (!f_) return false;
    flush(s);
    const size_t words = size_t((uint64_t(s.bits) + 31) / 32);
    if (std::fwrite(s.data, 4, words, f_) != words) failed_ = true;
    if (std::fseek(f_, 0, SEEK_SET) != 0) failed_ = true;
    if (std::fwrite(header, 1, kHeaderSize, f_) != kHeaderSize) failed_ = true;
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

// ---------------------------------------------------------------------------------------------
// Reader: feeds a recording back into the replay stream during playback.

Reader::~Reader() { close(); }

bool Reader::open(const std::string &path, uint8_t *header_out) {
    close();
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) return false;
    eof_ = false;
    const size_t got = std::fread(header_out, 1, kHeaderSize, f_);
    if (got != kHeaderSize || !valid_header(header_out)) {
        close();
        return false;
    }
    return true;
}

void Reader::fill(Stream &s) {
    auto *bytes = reinterpret_cast<uint8_t *>(s.data);
    // Drop 16 KB the game has read past, keeping the cursor on the same data.
    constexpr uint32_t discard_bits = uint32_t(kDiscardChunk * 8);
    while (uint32_t(s.bits + 7) >= discard_bits + 8 && s.bytes >= kDiscardChunk) {
        std::memmove(bytes, bytes + kDiscardChunk, s.bytes - kDiscardChunk);
        s.bytes -= uint32_t(kDiscardChunk);
        s.bits -= discard_bits;
        if (s.bytes < kBufferBytes) std::memset(bytes + s.bytes, 0, kBufferBytes - s.bytes);
    }
    // Keep more than 64 KB ahead of the reader while the file has data.
    while (f_ && !eof_ && s.bytes <= kReadChunk) {
        const size_t got = std::fread(bytes + s.bytes, 1, kReadChunk, f_);
        s.bytes += uint32_t(got);
        if (got < kReadChunk) eof_ = true;
    }
}

void Reader::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
    eof_ = false;
}

}  // namespace u2mp::cinema
