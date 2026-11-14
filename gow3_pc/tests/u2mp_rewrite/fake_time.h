// u2mp rewrite tests: a time() the test controls, for sources that stamp files or records with the Unix time
// (progression.cpp: the store save time and the players' last-seen time). Force-included into the old and the new
// sources (flags_<test>.sh: -include fake_time.h); the standard headers come first, then `time` names
// hx_fake_time, which returns hx::fake_time_s. The test includes this header with HX_NO_TIME_MACRO defined.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace hx {
inline long long fake_time_s = 1700000000;   // what the sources' time() returns
}

inline std::time_t hx_fake_time(std::time_t *t) {
    const std::time_t v = std::time_t(::hx::fake_time_s);
    if (t) *t = v;
    return v;
}
namespace std {
using ::hx_fake_time;
}

#ifndef HX_NO_TIME_MACRO
#define time hx_fake_time
#endif
