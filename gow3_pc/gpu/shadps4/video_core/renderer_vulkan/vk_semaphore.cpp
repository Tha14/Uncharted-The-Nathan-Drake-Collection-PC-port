// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include <cstdio>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"

namespace Vulkan {
void DumpGpuCheckpoints(const Instance& instance);
void DumpDeviceFault(const Instance& instance);

constexpr u64 WAIT_TIMEOUT = 1000000000;

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() = default;

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

bool Semaphore::WaitFor(u64 tick, u64 timeout_ns) {
    if (IsFree(tick)) {
        return true;
    }
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };
    if (instance.GetDevice().waitSemaphores(&wait_info, timeout_ns) != vk::Result::eSuccess) {
        return false; // timeout (a lost device shows in the next plain Wait)
    }
    Refresh();
    return true;
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    unsigned seconds=0;
    for (;;) {
        const auto result=instance.GetDevice().waitSemaphores(&wait_info, WAIT_TIMEOUT);
        if (result==vk::Result::eSuccess) break;
        if (result==vk::Result::eErrorDeviceLost) {
            DumpGpuCheckpoints(instance);
            DumpDeviceFault(instance);
        }
        ASSERT_MSG(result==vk::Result::eTimeout, "GPU semaphore wait failed: {}",vk::to_string(result));
        if (++seconds==1 || seconds%10==0) {
            DumpGpuCheckpoints(instance);
            Refresh();
            std::fprintf(stderr,"GPU wait: %u seconds, target=%llu completed=%llu submitted-next=%llu\n",seconds,
                         (unsigned long long)tick,(unsigned long long)KnownGpuTick(),
                         (unsigned long long)CurrentTick());
        }
    }
    Refresh();
}

} // namespace Vulkan
