// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdio>
#include "common/assert.h"
#include "common/types.h"
#include "core/address_space.h"

namespace VideoCore {

struct PageState {
    u8 num_write_watchers;
    u8 num_read_watchers;

    Core::MemoryPermission WritePerm() const noexcept {
        return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                       : Core::MemoryPermission::None;
    }

    Core::MemoryPermission ReadPerm() const noexcept {
        return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                      : Core::MemoryPermission::None;
    }

    Core::MemoryPermission Perms() const noexcept {
        // A pending readback must block writes too, until its GPU version reaches RAM.
        return num_read_watchers != 0 ? Core::MemoryPermission::None
                                     : ReadPerm() | WritePerm();
    }

    template <s32 delta, bool is_read>
    u8 AddDelta() {
        if constexpr (is_read) {
            if constexpr (delta == 1) {
                return ++num_read_watchers;
            } else if (delta == -1) {
                // gow3 18 Oct 2026: a landing readback's read watch can outlive the page's
                // state (reset when the range was unmapped or remapped meanwhile): the page is
                // unwatched already, which is what the decrement wants. Was a fatal assert (U3,
                // Radeon 780M, once in ~4 minutes of play).
                if (num_read_watchers == 0) {
                    static std::atomic<u32> logged{0};
                    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                        std::printf("Page watchers: read watch removed from an unwatched page "
                                    "(ignored)\n");
                    }
                    return 0;
                }
                return --num_read_watchers;
            } else {
                return num_read_watchers;
            }
        } else {
            if constexpr (delta == 1) {
                return ++num_write_watchers;
            } else if (delta == -1) {
                ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                return --num_write_watchers;
            } else {
                return num_write_watchers;
            }
        }
    }
};

} // namespace VideoCore
