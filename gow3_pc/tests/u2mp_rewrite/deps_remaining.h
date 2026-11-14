// u2mp rewrite tests, group "remaining": the scenario state the stand-ins of deps_remaining.cpp answer from.
// One copy (namespace hx::fk), shared by the old and the new build of the stand-ins.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hx::fk {

struct Member {
    int row;
    uint32_t id;
    std::string name;
};
struct Friend {
    const char *row;       // friend_row text (may be NULL)
    std::string name;
    int kind;              // online::kRow*, or anything else
    bool online, joinable;
    std::string profile;
};

struct State {
    // session.cpp / frontend.cpp
    std::map<uint32_t, uint64_t> packages;   // package id -> image offset of the package
    std::vector<Member> members;
    uint32_t local_id = 0;
    bool is_host = false;
    bool kick_result = true;
    std::string local_name = "LocalMe";
    // online.cpp
    bool active = false, linked = false, promote_result = false, open_join_result = false;
    std::vector<Friend> friends;
    int invite_count = 0;
    bool friend_add_result = true;
    int cinema_received = 0;
    // voice.cpp
    bool voice_enabled = true, toggle_result = true;
    // progression.cpp
    std::vector<std::string> recent;
    std::string summary;
    // runtime text input
    int begin_result = 1;
    std::vector<int> poll_results;   // consumed front first; 0 when empty
    std::string poll_text;
    // texts.cpp: the trampoline handed to text_find_original
    void *tramp = nullptr;
};
extern State st;

}  // namespace hx::fk
