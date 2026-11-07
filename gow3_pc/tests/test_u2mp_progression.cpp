// u2mp progression store test (no game needed): ninja -C out/gpu u2mp-progression-test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/u2mp/progression_store.h"

using namespace u2mp::progression;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static Store sample() {
    Store s;
    s.publish_count = 7;
    s.has_blob = true;
    for (size_t i = 0; i < kBlobSize; ++i) s.blob[i] = uint8_t(i * 7 + 3);
    s.blob[0] = 10, s.blob[1] = s.blob[2] = s.blob[3] = 0;
    s.local_name = "Drake";
    PlayerStats p;
    p.name = "Chloe";
    p.kills = 12, p.deaths = 3, p.wins = 2, p.losses = 1, p.seconds = 900, p.coop_points = 0, p.money = 4000;
    p.matches = 3, p.rank = 5, p.last_seen = 1700000000;
    s.recent.push_back(p);
    return s;
}

static void test_roundtrip() {
    const Store s = sample();
    const std::vector<uint8_t> bytes = encode(s, 1234);
    Store d;
    CHECK(decode(bytes.data(), bytes.size(), d));
    CHECK(d.publish_count == 7 && d.has_blob && d.blob == s.blob && d.local_name == "Drake");
    CHECK(d.recent.size() == 1 && d.recent[0].name == "Chloe" && d.recent[0].kills == 12 &&
          d.recent[0].money == 4000 && d.recent[0].rank == 5 && d.recent[0].last_seen == 1700000000);
    // any flipped byte is rejected (CRC), as are short files and a wrong magic
    for (size_t i : {size_t(0), size_t(9), size_t(0x40), bytes.size() / 2, bytes.size() - 1}) {
        std::vector<uint8_t> bad = bytes;
        bad[i] ^= 0x40;
        Store x;
        CHECK(!decode(bad.data(), bad.size(), x));
    }
    Store x;
    CHECK(!decode(bytes.data(), bytes.size() - 1, x));
    CHECK(!decode(bytes.data(), 16, x));
}

static void test_files() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "u2mp-progression-test";
    fs::remove_all(dir);
    const std::string path = (dir / "sub" / "progression.bin").string();
    Store s = sample();
    Store none;
    CHECK(!load(path, none));
    CHECK(save(path, s, 1));          // creates the directories
    CHECK(fs::exists(path) && !fs::exists(path + ".tmp"));
    s.publish_count = 8;
    CHECK(save(path, s, 2));          // previous file becomes .bak
    CHECK(fs::exists(path + ".bak"));
    s.publish_count = 9;
    CHECK(save(path, s, 3));          // .bak replaced again
    Store l;
    CHECK(load(path, l) && l.publish_count == 9);
    // a damaged main file falls back to the previous one
    {
        FILE *f = std::fopen(path.c_str(), "r+b");
        CHECK(f);
        if (f) {
            std::fseek(f, 0x50, SEEK_SET);
            std::fputc(0xee, f);
            std::fclose(f);
        }
    }
    Store b;
    CHECK(load(path, b) && b.publish_count == 8);
    fs::remove_all(dir);
}

static void test_matches() {
    Store s;
    std::vector<MatchPlayer> m(3);
    m[0].name = "Drake", m[0].local = true, m[0].kills = 5;
    m[1].name = "Chloe", m[1].kills = 7, m[1].deaths = 2, m[1].result = 1, m[1].money = 1500, m[1].seconds = 600;
    m[2].name = "Flynn", m[2].kills = 1, m[2].deaths = 9, m[2].result = -1, m[2].rank = 3;
    note_match(s, m, 100);
    note_match(s, m, 200);
    CHECK(s.local_name == "Drake");
    CHECK(s.recent.size() == 2);
    const PlayerStats *chloe = nullptr;
    for (const PlayerStats &p : s.recent)
        if (p.name == "Chloe") chloe = &p;
    CHECK(chloe && chloe->kills == 14 && chloe->wins == 2 && chloe->losses == 0 && chloe->matches == 2 &&
          chloe->money == 3000 && chloe->seconds == 1200 && chloe->last_seen == 200);
    // capped at kMaxRecent, most recent kept
    for (int i = 0; i < 100; ++i) {
        std::vector<MatchPlayer> one(1);
        one[0].name = "P" + std::to_string(i);
        one[0].kills = 1;
        note_match(s, one, 1000 + uint64_t(i));
    }
    CHECK(s.recent.size() == kMaxRecent);
    CHECK(s.recent.front().name == "P99");
    // the local player is never listed as a remote one
    std::vector<MatchPlayer> again(1);
    again[0].name = "Drake", again[0].local = true;
    s.recent.push_back(PlayerStats{"Drake"});
    note_match(s, again, 5000);
    for (const PlayerStats &p : s.recent) CHECK(p.name != "Drake");
}

static void test_boards() {
    Store s;
    for (int i = 0; i < 30; ++i) {
        PlayerStats p;
        p.name = "P" + std::to_string(i);
        p.kills = uint32_t(i);   // P0 has no kills: not on the kills board
        p.money = uint32_t(1000 * i);
        s.recent.push_back(p);
    }
    LocalStats me;
    me.name = "Drake";
    me.kills = 15, me.money = 999999, me.deaths = 4, me.losses = 2, me.seconds = 100, me.coop_seconds = 50, me.rank = 9;
    const std::vector<Entry> kills = board(s, me, 0);
    CHECK(kills.size() == 30);   // 29 remote with kills + local
    for (size_t i = 1; i < kills.size(); ++i) CHECK(kills[i - 1].score >= kills[i].score);
    // P15 and Drake tie at 15: same rank, name order
    size_t at = 0;
    for (size_t i = 0; i < kills.size(); ++i)
        if (kills[i].local) at = i;
    CHECK(kills[at].score == 15 && kills[at].badge == 9 && kills[at].seconds == 150 && kills[at].deaths == 4);
    CHECK(kills[at].name == "Drake" && kills[at + 1].name == "P15" && kills[at + 1].rank == kills[at].rank);
    CHECK(kills[0].rank == 1 && kills[0].name == "P29");
    const std::vector<Entry> cash = board(s, me, 1);
    CHECK(cash[0].local && cash[0].rank == 1);
    const std::vector<Entry> coop = board(s, me, 3);
    CHECK(coop.size() == 1 && coop[0].local && coop[0].seconds == 50);
    CHECK(board(s, me, 4).empty() && board(s, me, -1).empty());

    // pages of 11; -1 = the local player's page
    const Page mine = page(kills, -1);
    CHECK(mine.total == 30 && mine.page == int(at / kRowsPerPage));
    bool found = false;
    for (const Entry &e : mine.rows) found = found || e.local;
    CHECK(found);
    const Page last = page(kills, 99);
    CHECK(last.page == 2 && last.rows.size() == 8);
    const Page first = page(kills, 0);
    CHECK(first.page == 0 && first.rows.size() == 11 && first.rows[0].name == "P29");
    CHECK(page({}, -1).total == 0);
}

int main() {
    // CRC-32 check value
    const char *check = "123456789";
    CHECK(crc32(reinterpret_cast<const uint8_t *>(check), 9) == 0xcbf43926u);
    test_roundtrip();
    test_files();
    test_matches();
    test_boards();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("u2mp progression store: all tests passed\n");
    return 0;
}
