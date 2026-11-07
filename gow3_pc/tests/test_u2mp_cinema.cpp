// u2mp cinema file store test (no game needed): ninja -C out/gpu u2mp-cinema-test
// Writes a recording through the pump logic (32 KB flushes), reads it back through the playback
// logic (16 KB discards, 64 KB reads) and checks the list / make-room / protect / delete rules.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "../src/u2mp/bitstream.h"
#include "../src/u2mp/cinema_store.h"

using namespace u2mp;
using namespace u2mp::cinema;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static std::vector<uint8_t> make_header(int32_t game, uint64_t time) {
    std::vector<uint8_t> h(kHeaderSize, 0);
    put32(h.data() + hdr::kMagicOff, kMagic);
    put32(h.data() + hdr::kGame, uint32_t(game));
    put64(h.data() + hdr::kTime, time);
    put32(h.data() + hdr::kPlayerCount, 2);
    std::snprintf(reinterpret_cast<char *>(h.data() + hdr::kTitle), 64, "Deathmatch - Plaza %d", game);
    return h;
}

// The recorder's record: u32 time, u8 type, then a payload of `bits` bits.
static void put_record(Stream &s, uint32_t t, uint8_t type, uint32_t payload, unsigned bits) {
    BitStream b{s.data, s.bits, s.max_bits, 0};
    write_bits(b, t, 32);
    write_bits(b, type, 8);
    write_bits(b, payload, bits);
    s.bits = b.bits;
}

static bool get(Stream &s, Reader &r, uint32_t &v, unsigned bits) {
    r.fill(s);
    BitStream b{s.data, s.bits, s.max_bits, 0};
    if (!read_bits(b, v, bits)) return false;
    s.bits = b.bits;
    return true;
}

static std::vector<uint32_t> buffer_words() { return std::vector<uint32_t>((kBufferBytes + 64) / 4, 0); }

static void test_stream(const fs::path &dir) {
    const std::string path = (dir / "a.u2cinema").string();
    auto words = buffer_words();
    Stream s{words.data(), 0, uint32_t(kBufferBytes * 8), 0};
    Writer w;
    CHECK(w.open(path));
    // stream start, as WaitReady::Exit writes it
    BitStream b{s.data, 0, s.max_bits, 0};
    write_bits(b, kStartTag, 32);
    write_bits(b, 7, 32);
    write_bits(b, 0x3f800000, 32);
    write_bits(b, 1234, 32);
    s.bits = b.bits;
    const int kRecords = 40000;   // 40000 * 67 bits = 335 KB: many flushes
    for (int i = 0; i < kRecords; ++i) {
        put_record(s, uint32_t(1000 + i), uint8_t(i % 80), uint32_t(i * 2654435761u) >> 5, 27);
        w.flush(s);   // the pump, once per frame
        CHECK(s.bits <= (kWriteChunk + 1) * 8);
    }
    b = BitStream{s.data, s.bits, s.max_bits, 0};
    write_bits(b, 0, 32);
    s.bits = b.bits;
    const auto header = make_header(7, 1700000000);
    CHECK(w.finish(s, header.data()));

    std::vector<uint8_t> got(kHeaderSize);
    Reader r;
    auto rwords = buffer_words();
    Stream rs{rwords.data(), 0, uint32_t(kBufferBytes * 8), 0};
    CHECK(r.open(path, got.data()));
    CHECK(std::memcmp(got.data(), header.data(), kHeaderSize) == 0);
    uint32_t v = 0;
    CHECK(get(rs, r, v, 32) && v == kStartTag);
    CHECK(get(rs, r, v, 32) && v == 7);
    CHECK(get(rs, r, v, 32) && v == 0x3f800000);
    CHECK(get(rs, r, v, 32) && v == 1234);
    int bad = 0;
    for (int i = 0; i < kRecords; ++i) {
        uint32_t t = 0, type = 0, payload = 0;
        if (!get(rs, r, t, 32) || !get(rs, r, type, 8) || !get(rs, r, payload, 27) ||
            t != uint32_t(1000 + i) || type != uint32_t(i % 80) || payload != (uint32_t(i * 2654435761u) >> 5))
            ++bad;
        CHECK(rs.bytes <= kBufferBytes);
    }
    CHECK(bad == 0);
    CHECK(get(rs, r, v, 32) && v == 0);   // end marker
    CHECK(r.eof());
    r.close();
    const auto files = list(dir.string());
    CHECK(files.size() == 1 && files[0].name == "a.u2cinema" && !files[0].protect);
    std::vector<uint8_t> e(kEntrySize);
    fill_entry(files[0], e.data());
    CHECK(std::strcmp(reinterpret_cast<char *>(e.data() + entry::kName), "a.u2cinema") == 0);
    CHECK(get32(e.data() + entry::kMode) == kModeWritable);
    CHECK(get32(e.data() + entry::kHeader + hdr::kGame) == 7);
    CHECK(cinema::remove(dir.string(), "a.u2cinema"));
}

static void write_empty(const fs::path &dir, const std::string &name, uint64_t time) {
    auto words = buffer_words();
    Stream s{words.data(), 0, uint32_t(kBufferBytes * 8), 0};
    Writer w;
    CHECK(w.open((dir / name).string()));
    const auto h = make_header(1, time);
    CHECK(w.finish(s, h.data()));
}

static void test_store(const fs::path &dir) {
    for (int i = 0; i < kMaxFiles; ++i) write_empty(dir, "r" + std::to_string(100 + i) + kExtension, 1000 + i);
    // a file that is not a recording is ignored, never deleted
    if (std::FILE *f = std::fopen((dir / "junk.u2cinema").string().c_str(), "wb")) {
        std::fputs("not a recording", f);
        std::fclose(f);
    }
    auto files = list(dir.string());
    CHECK(files.size() == size_t(kMaxFiles));
    CHECK(files.front().time == 1000 + kMaxFiles - 1 && files.back().time == 1000);
    CHECK(set_protected(dir.string(), "r100.u2cinema", true));
    CHECK(!cinema::remove(dir.string(), "r100.u2cinema"));
    CHECK(make_room(dir.string()));   // deletes r101 (oldest unprotected)
    files = list(dir.string());
    CHECK(files.size() == size_t(kMaxFiles - 1));
    CHECK(fs::exists(dir / "r100.u2cinema") && !fs::exists(dir / "r101.u2cinema"));
    CHECK(fs::exists(dir / "junk.u2cinema"));
    CHECK(files.back().name == "r100.u2cinema" && files.back().protect);
    write_empty(dir, "r999.u2cinema", 5000);
    for (const auto &f : list(dir.string())) set_protected(dir.string(), f.name, true);
    CHECK(!make_room(dir.string()));   // all protected: no room
    for (const auto &f : list(dir.string())) set_protected(dir.string(), f.name, false);
    CHECK(make_room(dir.string()));
    CHECK(list(dir.string()).size() == size_t(kMaxFiles - 1));
    const std::string n = new_name(dir.string(), std::time_t(1700000000));
    CHECK(n.size() > std::strlen(kExtension) && n.find(kExtension) != std::string::npos);
    auto h = make_header(1, 1);
    put32(h.data(), 0x12345678);
    CHECK(!valid_header(h.data()));
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("u2mp-cinema-test-" + std::to_string(std::rand()));
    fs::remove_all(dir);
    CHECK(ensure_dir(dir.string()));
    test_stream(dir);
    test_store(dir);
    for (const auto &e : fs::directory_iterator(dir))
        fs::permissions(e.path(), fs::perms::owner_write, fs::perm_options::add);
    fs::remove_all(dir);
    std::printf("%s\n", failures ? "u2mp cinema test FAILED" : "u2mp cinema test passed");
    return failures ? 1 : 0;
}
