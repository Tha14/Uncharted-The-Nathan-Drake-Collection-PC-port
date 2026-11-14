// u2mp rewrite tests, group "menus" (menus.cpp, menuimages.cpp): the scenario state the stand-ins of
// deps_menus.cpp answer from. One copy (namespace hx::mn, defined in test_menus.cpp), shared by the old and the
// new build of the stand-ins.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hx::mn {

struct State {
    // session.cpp
    std::string local_name;
    int members = 0;
    std::vector<int> member_ok;            // member_at(r) true when r < size and member_ok[r]
    std::vector<uint32_t> member_id;
    std::vector<std::string> member_name;  // may hold embedded NULs
    bool host = true;
    // online.cpp
    bool online = false;
    int friends = 0;
    std::vector<int> friend_on;
    std::vector<std::string> friend_rows;
    int join_rows = -1;
    std::vector<std::string> join_row_texts;
    std::string join_title;
    // voice.cpp
    float volume = 0.0f;
    // progression.cpp
    std::vector<std::string> recent;
    // dlc.cpp: net game indexes dlc_net_game_available() refuses
    std::vector<int32_t> dlc_off;
    // session.cpp package_by_id: image offset of the package answered for any id (0 = NULL)
    uint64_t package = 0;
};
extern State s;

}  // namespace hx::mn
