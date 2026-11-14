// u2mp rewrite tests, group "progression": the scenario state the stand-ins of deps_progression.cpp answer from.
// One copy (namespace hx::pg, defined in test_progression.cpp), shared by the old and the new build of the stand-ins.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "u2_online_abi.h"

namespace hx::pg {

struct State {
    // online.cpp
    bool linked = false;
    std::deque<bool> request_results;   // board_request answers, front first; request_default once empty
    bool request_default = true;
    int board_state = 0;
    int rows = 0;
    uint32_t total = 0;
    std::vector<U2oScore> scores;       // board_row(i): &scores[i] when i < size and null_row[i] is not set
    std::vector<bool> null_row;
    std::string my_npid;
    // netheap.cpp: allocations come from the fake image (so both builds get the same addresses)
    uint64_t heap_next = 0x1600000, heap_end = 0x1640000;
    uint64_t heap_fail_mask = 0;   // call i (0-based) returns NULL when bit i is set
    int heap_calls = 0;
    // C runtime
    bool user_dir_null = false;
    std::string user_dir = "ud";
    int translate_result = 0;
    std::string translate_out = "net-patch9.bin";
};
extern State st;

}  // namespace hx::pg
