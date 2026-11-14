// u2mp rewrite tests, group "session": the scenario state the stand-ins of deps_session.cpp answer from, and the
// namespace-neutral entry table (Api) each build of deps_session.cpp fills for test_session.cpp.
// One copy of the state (namespace hx::ss, defined in test_session.cpp), shared by the old and the new build.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hx::ss {

struct Addr {
    uint32_t ip = 0;     // network byte order (as net::Address)
    uint16_t port = 0;
};

// one scripted transport event, delivered by the next Transport::pump
struct Incoming {
    bool peer = false;            // true: on_peer(slot, joined); false: on_message(slot, data)
    uint8_t slot = 0;
    bool joined = false;
    std::vector<uint8_t> data;
};

struct State {
    // net::Transport
    bool host_ok = true, join_ok = true, host_link_ok = true, join_link_ok = true;
    uint8_t local_slot = 0;                    // what local_slot() reports after join / pump
    std::vector<Incoming> incoming;            // drained by the next pump
    std::map<int, Addr> peer_addr;             // slot -> address (peer_address true)
    bool parse_ok = true;                      // net::Address::parse (false: fails)
    // gamenet
    bool gamenet_install = true;
    double clock = 1.0;                        // gamenet::clock()
    // online
    bool link = false;                         // online::start() returns a link
    bool online_active = false, create_room = false, host_change = false, host_lost_taken = false;
    uint64_t match_room = 0;
    std::string my_npid;
    std::map<uint32_t, std::string> npid_by_ip;
    std::function<void()> on_host_lost_cb;     // called inside online::on_host_lost (calls back into session)
    // matchmaking / frontend6
    bool mm_installed = false, leader_starting = false;
    // voice
    std::set<uint32_t> talking, muted;
    // other modules
    bool extra_net = false;                    // extra_net_package() -> "net-u2mp"
    int32_t playlist_type = -1;
    std::set<int> dlc_off;
    uint32_t fingerprint = 0;
    bool netheap_null = false;
    uint64_t netheap_off = 0x17f0000;
    int translate_result = 0;
    std::string file_path;                     // what runtime_file_translate writes
    // guest functions (test_session.cpp)
    std::map<uint32_t, uint64_t> lookup;       // data lookup id -> image offset (absent / 0: NULL)
    std::map<std::string, uint64_t> request;   // RequestPackage name -> package offset (absent: NULL)
    bool packages_loaded = true, loader_busy = false;
    int present_players = 3;
    uint32_t team_colour = 0xff123456;
    bool event_null = false;
    uint8_t event_refs = 1;                    // reference count a created event starts with
};
extern State st;

// the public entry points of one build of session.cpp (old or new), with namespace-neutral types
struct Api {
    const char *tag = "";
    void (*install)(int &) = nullptr;
    bool (*member_at)(int, uint32_t &, std::string &) = nullptr;
    bool (*member_name)(uint32_t, std::string &) = nullptr;
    uint32_t (*local_id)() = nullptr;
    bool (*is_host)() = nullptr;
    bool (*kick)(uint32_t) = nullptr;
    std::string (*local_name)() = nullptr;
    int (*rank)() = nullptr;
    int (*member_count)() = nullptr;
    void (*send_message)(int, const std::vector<uint8_t> &, int) = nullptr;
    uint32_t (*net_ticks)() = nullptr;
    void (*create_match)() = nullptr;
    uint32_t (*member_rank)(uint32_t) = nullptr;
    void (*online_reopen)(const Addr *owner) = nullptr;   // with the fake link
    void (*online_match_ready)(uint64_t) = nullptr;
    void (*set_local_name)(const std::string &) = nullptr;
    bool (*member_address)(uint32_t, Addr &) = nullptr;
    void (*online_migrated)(bool, bool) = nullptr;
    void (*online_host_gone)() = nullptr;
    bool (*match_migration_ok)() = nullptr;
    uint32_t (*net_id_of)(const std::string &) = nullptr;
    void (*online_rehost)(const Addr *owner, uint32_t) = nullptr;   // with the fake link
    const uint8_t *(*package_by_id)(uint32_t) = nullptr;
};

inline std::string hex(const uint8_t *d, size_t n) {
    static const char k[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        if (i) s += ' ';
        s += k[d[i] >> 4];
        s += k[d[i] & 15];
    }
    return s;
}

}  // namespace hx::ss
