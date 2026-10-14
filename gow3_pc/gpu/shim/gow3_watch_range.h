// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

// GOW3_WATCH_RANGE=<lo>:<hi>[,<lo>:<hi>...] (hex guest addresses): logs GPU-side writes into the
// ranges (shader storage buffers, DMA copies and fills, WRITE_DATA, render targets) with the
// shader hash, at most 400 lines. Diagnostics for data that the GPU produces and the CPU reads
// or overwrites.
namespace Gow3WatchRange {

struct Span {
    std::uint64_t lo = 0, hi = 0;
};

inline const std::vector<Span>& Ranges() {
    static const auto ranges = [] {
        std::vector<Span> out;
        const char* env = std::getenv("GOW3_WATCH_RANGE");
        while (env && *env) {
            char* end = nullptr;
            Span span;
            span.lo = std::strtoull(env, &end, 16);
            if (!end || *end != ':') {
                break;
            }
            span.hi = std::strtoull(end + 1, &end, 16);
            if (span.hi > span.lo) {
                out.push_back(span);
            }
            env = end && *end == ',' ? end + 1 : nullptr;
        }
        return out;
    }();
    return ranges;
}

/// The first range (callers that only need one).
inline bool Range(std::uint64_t& lo, std::uint64_t& hi) {
    const auto& ranges = Ranges();
    lo = ranges.empty() ? 0 : ranges[0].lo;
    hi = ranges.empty() ? 0 : ranges[0].hi;
    return hi > lo;
}

inline void Note(const char* what, std::uint64_t hash, std::uint32_t slot, std::uint64_t address,
                 std::uint64_t size) {
    bool hit = false;
    for (const auto& span : Ranges()) {
        if (address < span.hi && address + size > span.lo) {
            hit = true;
            break;
        }
    }
    if (!hit) {
        return;
    }
    static std::atomic<int> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) < 400) {
        std::printf("Watch range: %s shader %016llx slot %u at %#llx size %llu\n", what,
                    static_cast<unsigned long long>(hash), slot,
                    static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(size));
        std::fflush(stdout);
    }
}

} // namespace Gow3WatchRange
