// u2mp cinema (theater) file store: the part of PS3 1.10 cinema.cpp that has no game state in it.
//
// Pure host code (no guest addresses), so the unit test links it alone. The guest side, which
// feeds the game's replay stream to and from these files, is cinema.cpp. File format and the PS3
// functions this follows: u2mp/docs/CINEMA.md.
//
// File (<user dir>/u2mp/cinema/<yyyymmdd-hhmmss>.u2cinema), all little-endian:
//   0x000  header, kHeaderSize bytes: the game's cinema header (cinema object +0x1c8), magic first
//   0x4b0  the replay stream: the game's BitStream words (u32, bit n in word n>>5 under mask
//          0x80000000 >> (n & 31)), whole words, as the game wrote them:
//            u32 5 (start tag), u32 net game index, u32 clock float bits, u32 clock start,
//            { u32 time (frame clock, never 0), u8 net event type, <event Write payload> }...,
//            u32 0 (end)
// Same layout as the PS3 file (header 1200 bytes + stream), but PS3 words were big-endian.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace u2mp::cinema {

constexpr uint32_t kMagic = 0x04b00005;          // PS3 cinema+152: (header size << 16) | version 5
constexpr size_t kHeaderSize = 0x4b0;            // 1200
constexpr int kMaxFiles = 20;                    // PS3 refresh/make-room limit
constexpr size_t kEntrySize = 0x6c8;             // one list entry in the game (PS3 1736)
constexpr uint32_t kModeWritable = 0x180, kModeProtected = 0x100;   // PS3 cellFsChmod modes
constexpr size_t kBufferBytes = 0x20000;         // replay stream buffer (PS3 NetInit: 128 KB)
constexpr size_t kWriteChunk = 0x8000;           // written when the cursor passes 32 KB
constexpr size_t kReadChunk = 0x10000;           // read while fewer than 64 KB are buffered
constexpr size_t kDiscardChunk = 0x4000;         // dropped once the cursor passes 16 KB
constexpr uint32_t kStartTag = 5;
constexpr const char *kExtension = ".u2cinema";

// Header fields (offsets in the 1200-byte header; PS4 build, verified against the replay code).
namespace hdr {
constexpr size_t kMagicOff = 0x00;       // u32 kMagic
constexpr size_t kGame = 0x04;           // s32 net game index (ngm+0x1ca0, s16)
constexpr size_t kClockF = 0x08;         // f32 (0x113a034)
constexpr size_t kClockStart = 0x0c;     // u32 (0x113a038)
constexpr size_t kDuration = 0x10;       // s32 seconds (list shows m:ss)
constexpr size_t kTime = 0x18;           // u64 time_t of the recording
constexpr size_t kOverrides = 0x20;      // 20 bytes: ngm+0x21b4..0x21c8 (SetNetGame overrides)
constexpr size_t kOverridesSize = 0x14;
constexpr size_t kOwner = 0x34;          // char[32]
constexpr size_t kTitle = 0x54;          // char[64] "%s - %s %s"
constexpr size_t kDescription = 0x94;    // "Description"
constexpr size_t kPlayerCount = 0x114;   // s32
constexpr size_t kPlayers = 0x118;       // kMaxPlayers x 0x5c: name[0x40], then 7 words
constexpr size_t kPlayerSize = 0x5c;
constexpr int kMaxPlayers = 10;
static_assert(kPlayers + kMaxPlayers * kPlayerSize == kHeaderSize, "header layout");
}  // namespace hdr

// The game's list entry (kEntrySize bytes), as the PS3 refresh filled it.
namespace entry {
constexpr size_t kDate = 0x000;          // char[0x100] formatted local time
constexpr size_t kName = 0x100;          // char[0x100] file name (no directory)
constexpr size_t kStat = 0x200;          // u8 stat result (0)
constexpr size_t kTime = 0x208;          // u64 sort key
constexpr size_t kSize = 0x210;          // u32 file size
constexpr size_t kMode = 0x214;          // u32 kModeWritable or kModeProtected
constexpr size_t kHeader = 0x218;        // header copy
static_assert(kHeader + kHeaderSize <= kEntrySize, "entry layout");
}  // namespace entry

uint32_t get32(const uint8_t *p);
void put32(uint8_t *p, uint32_t v);
uint64_t get64(const uint8_t *p);
void put64(uint8_t *p, uint64_t v);

// A header the replay code can use: magic, game index in s16 range, at most 10 players.
bool valid_header(const uint8_t *header);

struct File {
    std::string name;            // file name in the store directory
    uint64_t time = 0;           // header time (newest first)
    uint32_t size = 0;
    bool protect = false;        // read-only file = PS3 mode 0x100
    std::vector<uint8_t> header; // kHeaderSize bytes
};

// The PS3 refresh (0x4631d8): valid recordings in `dir`, newest first, at most kMaxFiles.
// Unlike the PS3 code it never deletes a file it cannot read; it only leaves it out.
std::vector<File> list(const std::string &dir);
// Fills one game list entry (kEntrySize bytes) from a file record.
void fill_entry(const File &f, uint8_t *out);
// The PS3 make-room step (0x4635c8): while kMaxFiles recordings exist, delete the oldest
// unprotected one. False when all of them are protected (the PS3 code then did not record).
bool make_room(const std::string &dir);
// PS3 0x463ab8 / 0x463ba4. Protected files are not deleted.
bool remove(const std::string &dir, const std::string &name);
bool set_protected(const std::string &dir, const std::string &name, bool protect);
// "yyyymmdd-hhmmss.u2cinema" for `t` (local time); a numeric suffix when the name is taken.
std::string new_name(const std::string &dir, std::time_t t);
bool ensure_dir(const std::string &dir);

// The game's replay stream object (PS4 0x113a258): a BitStream plus the count of valid bytes.
struct Stream {
    uint32_t *data;
    uint32_t bits;       // cursor
    uint32_t max_bits;
    uint32_t bytes;      // bytes of data loaded (reading)
};
static_assert(sizeof(Stream) == 0x18, "stream layout");

// Recording side of the PS3 pump (0x463ce8) and finish (0x464464).
class Writer {
public:
    ~Writer();
    bool open(const std::string &path);          // writes a zero header placeholder
    bool is_open() const { return f_ != nullptr; }
    // Writes 32 KB chunks while the cursor is past 32 KB and shifts the rest down.
    void flush(Stream &s);
    // Writes the remaining whole words, then the header at offset 0, and closes.
    bool finish(Stream &s, const uint8_t *header);
    void abandon();                               // close and delete the partial file
private:
    std::FILE *f_ = nullptr;
    std::string path_;
    bool failed_ = false;
};

// Playback side of the PS3 pump.
class Reader {
public:
    ~Reader();
    bool open(const std::string &path, uint8_t *header_out);   // reads and checks the header
    bool is_open() const { return f_ != nullptr; }
    // Drops consumed 16 KB chunks, then reads 64 KB chunks while at most 64 KB are buffered.
    void fill(Stream &s);
    bool eof() const { return eof_; }
    void close();
private:
    std::FILE *f_ = nullptr;
    bool eof_ = false;
};

}  // namespace u2mp::cinema
