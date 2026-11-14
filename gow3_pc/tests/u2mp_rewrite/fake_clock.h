// u2mp rewrite tests: a steady_clock the test controls, for sources that read a host clock (scoreboard dt,
// matchend 10 s timer). Force-included into the old and the new sources (flags_<test>.sh: -include fake_clock.h);
// the standard headers come first, then std::chrono::steady_clock names hx_fake_clock, whose now() is
// hx::fake_clock_ns. The test includes this header too and sets the value.
#pragma once
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hx {
inline long long fake_clock_ns = 0;   // what the sources' steady_clock::now() returns, in ns
}

namespace std::chrono {
struct hx_fake_clock {
    using duration = nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<hx_fake_clock>;
    static constexpr bool is_steady = true;
    static time_point now() noexcept { return time_point(duration(::hx::fake_clock_ns)); }
};
}  // namespace std::chrono

#ifndef HX_NO_CLOCK_MACRO
#define steady_clock hx_fake_clock
#endif
