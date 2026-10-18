// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <immintrin.h>
#include "common/shared_handle_table.h"
#include <thread>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

extern boost::asio::io_context io_context;
extern void KernelSignalRequest();

static Common::SharedHandleTable<EqueueInternal,OrbisKernelEqueue> kqueues;
static constexpr auto HrTimerSpinlockThresholdNs = 1200000u;

std::shared_ptr<EqueueInternal> GetEqueue(OrbisKernelEqueue eq) {
    return kqueues.Find(eq);
}
static void HrTimerCallback(OrbisKernelEqueue eq,const OrbisKernelEvent& kevent) {
    if (auto equeue=GetEqueue(eq)) {
        equeue->TriggerEvent(kevent.ident,OrbisKernelEvent::Filter::HrTimer,kevent.udata);
    }
}
static void TimerCallback(OrbisKernelEqueue eq,const OrbisKernelEvent& kevent) {
    if (auto equeue=GetEqueue(eq); equeue && equeue->EventExists(kevent.ident,kevent.filter)) {
        equeue->TriggerEvent(kevent.ident,OrbisKernelEvent::Filter::Timer,kevent.udata);
        if (!(kevent.flags & OrbisKernelEvent::Flags::OneShot)) {
            equeue->ScheduleEvent(kevent.ident,kevent.filter,TimerCallback);
        }
    }
}

// Events are uniquely identified by id and filter.
bool EqueueInternal::AddEvent(EqueueEvent& event) {
    // Save id and filter before event is moved into m_events.
    const u64 id = event.event.ident;
    const auto filter = event.event.filter;

    {
        std::scoped_lock lock{m_mutex};

        // Calculate timer interval
        event.time_added = std::chrono::steady_clock::now();
        if (event.event.filter == OrbisKernelEvent::Filter::Timer) {
            // Set timer interval, this is stored in milliseconds for timers.
            event.timer_interval = std::chrono::milliseconds(event.event.data);
        } else if (event.event.filter == OrbisKernelEvent::Filter::HrTimer) {
            // Retrieve inputted time, this is stored in the bintime format.
            OrbisKernelBintime* time = reinterpret_cast<OrbisKernelBintime*>(event.event.data);

            // Convert the bintime format to a timespec.
            OrbisKernelTimespec ts;
            ts.tv_sec = time->sec;
            ts.tv_nsec = (1000000000 * (time->frac >> 32)) >> 32;

            // Then use the timespec to set the timer interval.
            event.timer_interval = std::chrono::nanoseconds(ts.tv_nsec + ts.tv_sec * 1000000000);
        }

        // First, check if there's already an event with the same id and filter.
        const auto& find_it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
            return ev.event.ident == id && ev.event.filter == filter;
        });
        // If there is a duplicate event, we need to update that instead.
        if (find_it != m_events.cend()) {
            // Specifically, update user data and timer_interval.
            // Trigger status and event data should remain intact.
            auto& old_event = *find_it;
            old_event.timer_interval = event.timer_interval;
            old_event.event.udata = event.event.udata;
            return true;
        }

        // Clear input data from event.
        event.event.data = 0;

        // Remove add flag from event
        event.event.flags &= ~OrbisKernelEvent::Flags::Add;

        // Clear flag is appended to most event types internally.
        if (event.event.filter != OrbisKernelEvent::Filter::User) {
            event.event.flags |= OrbisKernelEvent::Flags::Clear;
        }

        const auto& it = std::ranges::find(m_events, event);
        if (it != m_events.cend()) {
            *it = std::move(event);
        } else {
            m_events.emplace_back(std::move(event));
        }
    }

    // Schedule callbacks for timer events
    if (filter == OrbisKernelEvent::Filter::Timer) {
        return this->ScheduleEvent(id, OrbisKernelEvent::Filter::Timer, TimerCallback);
    } else if (filter == OrbisKernelEvent::Filter::HrTimer) {
        return this->ScheduleEvent(id, OrbisKernelEvent::Filter::HrTimer, HrTimerCallback);
    }

    return true;
}

bool EqueueInternal::ScheduleEvent(u64 id, s16 filter,
                                   void (*callback)(OrbisKernelEqueue, const OrbisKernelEvent&)) {
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });
    if (it == m_events.cend()) {
        return false;
    }

    const auto& event = *it;
    ASSERT(event.event.filter == OrbisKernelEvent::Filter::Timer ||
           event.event.filter == OrbisKernelEvent::Filter::HrTimer);

    if (!it->timer) {
        it->timer = std::make_unique<boost::asio::steady_timer>(io_context, event.timer_interval);
    } else {
        // If the timer already exists we are scheduling a reoccurrence after the next period.
        // Set the expiration time to the previous occurrence plus the period.
        it->timer->expires_at(it->timer->expiry() + event.timer_interval);
    }

    it->timer->async_wait(
        [this, event_data = event.event, callback](const boost::system::error_code& ec) {
            if (ec) {
                if (ec != boost::system::errc::operation_canceled) {
                    LOG_ERROR(Kernel_Event, "Timer callback error: {}", ec.message());
                } else {
                    // Timer was cancelled (removed) before it triggered
                    LOG_DEBUG(Kernel_Event, "Timer cancelled");
                }
                return;
            }
            callback(this->m_handle, event_data);
        });
    KernelSignalRequest();

    return true;
}

bool EqueueInternal::RemoveEvent(u64 id, s16 filter) {
    bool has_found = false;
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });
    if (it != m_events.cend()) {
        m_events.erase(it);
        has_found = true;
    }
    return has_found;
}

int EqueueInternal::WaitForEvents(OrbisKernelEvent* ev, int num, const OrbisKernelUseconds* timo) {
    if (timo != nullptr && *timo == 0) {
        // Effectively acts as a poll; only events that have already
        // arrived at the time of this function call can be received.
        // gow3: under the queue lock. Reading and clearing an event while TriggerEvent sets it
        // on another thread lost the second trigger (Uncharted: a level stayed unloaded until
        // an unrelated load woke the loader).
        std::scoped_lock lock{m_mutex};
        return GetTriggeredEvents(ev, num);
    }
    const auto micros = timo ? *timo : 0u;

    if (HasSmallTimer()) {
        // If a small timer is set, just wait for it to expire.
        return WaitForSmallTimer(ev, num, micros);
    }

    int count = 0;

    const auto predicate = [&] {
        count = GetTriggeredEvents(ev, num);
        return count > 0;
    };

    // gow3 18 Oct 2026 (GOW3_EQUEUE_SPIN_US=<n>): watch for a trigger for up to n us before
    // sleeping. Uncharted 3's interrupt handler threads take ~2,200 compute interrupts a frame,
    // each one a kernel wakeup (~7% of the GPU command thread on a Ryzen laptop); a waiter
    // still spinning when the next one comes takes it without being woken. No wakeup is lost:
    // the sleep below re-checks under the lock, as before.
    static const u32 spin_us = [] {
        const char* env = std::getenv("GOW3_EQUEUE_SPIN_US");
        const int value = env ? std::atoi(env) : 0;
        return value > 0 ? static_cast<u32>(std::min(value, 2000)) : 0u;
    }();
    if (spin_us) {
        const u32 limit = micros ? std::min(spin_us, micros) : spin_us;
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(limit);
        u64 seen = m_matched.load(std::memory_order_acquire);
        {
            std::scoped_lock lock{m_mutex};
            if (predicate()) {
                return count;
            }
        }
        for (u32 polls = 0;; ++polls) {
            if (const u64 now = m_matched.load(std::memory_order_acquire); now != seen) {
                seen = now;
                std::scoped_lock lock{m_mutex};
                if (predicate()) {
                    return count;
                }
            }
            if ((polls & 63) == 63 && std::chrono::steady_clock::now() >= until) {
                break;
            }
            _mm_pause();
        }
    }

    if (micros == 0) {
        // Wait indefinitely for events
        std::unique_lock lock{m_mutex};
        ++m_waiters;
        m_cond.wait(lock, predicate);
        --m_waiters;
    } else {
        // Wait up until the timeout value
        std::unique_lock lock{m_mutex};
        ++m_waiters;
        m_cond.wait_for(lock, std::chrono::microseconds(micros), predicate);
        --m_waiters;
    }

    return count;
}

bool EqueueInternal::TriggerEvent(u64 ident, s16 filter, void* trigger_data) {
    bool has_found = false;
    bool waiting = false;
    bool newly = false; // an event went from idle to pending (a waiter has not been woken for it)
    {
        std::scoped_lock lock{m_mutex};
        waiting = m_waiters > 0;
        for (auto& event : m_events) {
            if (event.event.ident == ident && event.event.filter == filter) {
                newly |= !event.IsTriggered();
                if (filter == OrbisKernelEvent::Filter::VideoOut) {
                    event.TriggerDisplay(trigger_data);
                } else if (filter == OrbisKernelEvent::Filter::User) {
                    event.TriggerUser(trigger_data);
                } else if (filter == OrbisKernelEvent::Filter::Timer ||
                           filter == OrbisKernelEvent::Filter::HrTimer) {
                    event.TriggerTimer();
                } else {
                    event.Trigger(trigger_data);
                }
                has_found = true;
            }
        }
        if (has_found) {
            m_matched.fetch_add(1, std::memory_order_release);
        }
    }
    // gow3: every waiter re-checks; with several threads waiting on one queue, a single
    // wakeup could go to one that does not take the event. 17 Oct 2026: only when an event
    // became pending and a thread waits (counted under the lock, which a waiter holds from its
    // last check until it sleeps): Uncharted 3's compute interrupts cost the GPU command thread
    // ~8% in wakeup calls at 4K. GOW3_EQUEUE_STATS=1: triggers and wakeups per 5 s.
    static const bool stats = [] {
        const char* env = std::getenv("GOW3_EQUEUE_STATS");
        return env && env[0] == '1';
    }();
    if (stats) {
        static std::atomic<u64> triggers{0}, found{0}, woken{0};
        static std::atomic<s64> last{0};
        ++triggers;
        found += has_found;
        woken += newly && waiting;
        const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        s64 expected = last.load();
        if (now - expected >= 5 && last.compare_exchange_strong(expected, now)) {
            std::printf("Equeue triggers (5 s): %llu, %llu matched an event, %llu woke a waiter\n",
                        (unsigned long long)triggers.exchange(0), (unsigned long long)found.exchange(0),
                        (unsigned long long)woken.exchange(0));
            std::fflush(stdout);
        }
    }
    // A pending event already woke the waiters (or one finds it before sleeping): coalesced, as
    // the kernel does. Uncharted 3: ~2,200 compute interrupts a frame in crowds.
    static const bool always = [] {
        const char* env = std::getenv("GOW3_EQUEUE_ALWAYS_NOTIFY");
        return env && env[0] == '1';
    }();
    if (always || (newly && waiting)) {
        m_cond.notify_all();
    }
    return has_found;
}

int EqueueInternal::GetTriggeredEvents(OrbisKernelEvent* ev, int num) {
    int count = 0;
    for (auto it = m_events.begin(); it != m_events.end();) {
        if (it->IsTriggered()) {
            ev[count++] = it->event;
            if (it->event.flags & OrbisKernelEvent::Flags::Clear) {
                it->Clear();
            }
            if (it->event.flags & OrbisKernelEvent::Flags::OneShot) {
                it = m_events.erase(it);
            } else {
                ++it;
            }

            if (count == num) {
                break;
            }
        } else {
            ++it;
        }
    }

    return count;
}

bool EqueueInternal::AddSmallTimer(EqueueEvent& ev) {
    // Retrieve inputted time, this is stored in the bintime format
    OrbisKernelBintime* time = reinterpret_cast<OrbisKernelBintime*>(ev.event.data);
    OrbisKernelTimespec ts;
    ts.tv_sec = time->sec;
    ts.tv_nsec = ((1000000000 * (time->frac >> 32)) >> 32);

    // Create the small timer
    SmallTimer st;
    st.event = ev.event;
    st.added = std::chrono::steady_clock::now();
    st.interval = std::chrono::nanoseconds(ts.tv_nsec + ts.tv_sec * 1000000000);
    {
        std::scoped_lock lock{m_mutex};
        m_small_timers[st.event.ident] = std::move(st);
    }
    return true;
}

int EqueueInternal::WaitForSmallTimer(OrbisKernelEvent* ev, int num, u32 micros) {
    ASSERT(num >= 1);

    auto curr_clock = std::chrono::steady_clock::now();
    const auto wait_end_us = (micros == 0) ? std::chrono::steady_clock::time_point::max()
                                           : curr_clock + std::chrono::microseconds{micros};
    int count = 0;
    do {
        curr_clock = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock{m_mutex};
            for (auto it = m_small_timers.begin(); it != m_small_timers.end() && count < num;) {
                const SmallTimer& st = it->second;

                if (curr_clock - st.added >= st.interval) {
                    ev[count++] = st.event;
                    it = m_small_timers.erase(it);
                } else {
                    ++it;
                }
            }

            // gow3: events triggered while a short timer is pending are delivered too, as
            // the kernel does; they used to wait until the queue had no small timer.
            if (count < num) {
                count += GetTriggeredEvents(ev + count, num - count);
            }
            if (count > 0)
                return count;
        }
        std::this_thread::yield();
    } while (curr_clock < wait_end_us);

    return 0;
}

bool EqueueInternal::EventExists(u64 id, s16 filter) {
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });

    return it != m_events.cend();
}

static s32 NextEqueueHandle() { // gow3
    static std::atomic<s32> next{0x40000};
    return next++;
}

s32 PS4_SYSV_ABI posix_kqueue() {
    // Reserve a file handle for the kqueue
    // gow3: equeue handles come from their own range, not the file table.
    s32 kqueue_handle = NextEqueueHandle();

    // Plenty of equeue logic uses names to identify queues.
    // Create a unique name for the queue we create.
    char name[32];
    memset(name, 0, sizeof(name));
    snprintf(name, sizeof(name), "kqueue%i", kqueue_handle);

    // Create the queue
    kqueues.Insert(kqueue_handle,std::make_shared<EqueueInternal>(kqueue_handle,name));
    LOG_INFO(Kernel_Event, "kqueue created with name {}", name);

    // Return handle.
    return kqueue_handle;
}

// Helper method to detect supported filters.
// We don't want to allow adding events we don't handle properly.
bool SupportedEqueueFilter(OrbisKernelEvent::Filter filter) {
    return filter == OrbisKernelEvent::Filter::GraphicsCore ||
           filter == OrbisKernelEvent::Filter::HrTimer ||
           filter == OrbisKernelEvent::Filter::Timer || filter == OrbisKernelEvent::Filter::User ||
           filter == OrbisKernelEvent::Filter::VideoOut;
}

s32 PS4_SYSV_ABI posix_kevent(s32 handle, OrbisKernelEvent* changelist, u64 nchanges,
                              OrbisKernelEvent* eventlist, u64 nevents,
                              OrbisKernelTimespec* timeout) {
    LOG_INFO(Kernel_Event, "called, eq = {}, nchanges = {}, nevents = {}", handle, nchanges,
             nevents);

    // Get the equeue
    auto equeue=GetEqueue(handle);
    if (!equeue) {
        *__Error() = POSIX_EBADF;
        return ORBIS_FAIL;
    }

    // First step is to apply all changes in changelist.
    for (u64 i = 0; i < nchanges; i++) {
        auto event = changelist[i];
        if (!SupportedEqueueFilter(event.filter)) {
            LOG_ERROR(Kernel_Event, "Unsupported event filter {}",
                      magic_enum::enum_name(event.filter));
            continue;
        }

        // Check the event flags to determine the appropriate action
        if (event.flags & OrbisKernelEvent::Flags::Add) {
            // The caller is requesting to add an event.
            EqueueEvent internal_event{};
            internal_event.event = event;
            if (!equeue->AddEvent(internal_event)) {
                // Failed to add event, return error.
                *__Error() = POSIX_ENOMEM;
                return ORBIS_FAIL;
            }
        }

        if (event.flags & OrbisKernelEvent::Flags::Delete) {
            // The caller is requesting to remove an event.
            if (!equeue->RemoveEvent(event.ident, event.filter)) {
                // Failed to remove event, return error.
                *__Error() = POSIX_ENOENT;
                return ORBIS_FAIL;
            }
        }

        if (event.filter == OrbisKernelEvent::Filter::User && event.fflags == 0x1000000) {
            // For user events, this fflags value indicates we need to trigger the event.
            if (!equeue->TriggerEvent(event.ident, OrbisKernelEvent::Filter::User, event.udata)) {
                *__Error() = POSIX_ENOENT;
                return ORBIS_FAIL;
            }
        } else if (event.fflags != 0) {
            // The title is using filter-specific flags. Right now, these are unhandled.
            LOG_ERROR(Kernel_Event, "Unhandled fflags {:#x} for event filter {}", event.fflags,
                      magic_enum::enum_name(event.filter));
            continue;
        }
    }

    // Now we need to wait on the event list.
    s32 count = 0;
    if (nevents > 0) {
        if (timeout != nullptr) {
            OrbisKernelUseconds micros = (timeout->tv_sec * 1000000) + (timeout->tv_nsec / 1000);
            count = equeue->WaitForEvents(eventlist, nevents, &micros);
        } else {
            count = equeue->WaitForEvents(eventlist, nevents, nullptr);
        }
    }
    return count;
}

int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue* eq, const char* name) {
    if (eq == nullptr) {
        LOG_ERROR(Kernel_Event, "Event queue is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (name == nullptr) {
        LOG_ERROR(Kernel_Event, "Event queue name is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // Maximum is 32 including null terminator
    static constexpr u64 MaxEventQueueNameSize = 32;
    if (std::strlen(name) > MaxEventQueueNameSize) {
        LOG_ERROR(Kernel_Event, "Event queue name exceeds 32 bytes!");
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }

    LOG_INFO(Kernel_Event, "name = {}", name);

    // Reserve a file handle for the kqueue
    // gow3: equeue handles come from their own range, not the file table.
    OrbisKernelEqueue kqueue_handle = NextEqueueHandle();

    // Create the equeue
    kqueues.Insert(kqueue_handle,std::make_shared<EqueueInternal>(kqueue_handle,name));
    *eq = kqueue_handle;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue eq) {
    return kqueues.Erase(eq) ? ORBIS_OK : ORBIS_KERNEL_ERROR_EBADF;
}

int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue eq, OrbisKernelEvent* ev, int num, int* out,
                                     OrbisKernelUseconds* timo) {
    HLE_TRACE;
    auto equeue=GetEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }


    TRACE_HINT(equeue->GetName());
    LOG_TRACE(Kernel_Event, "equeue = {} num = {}", equeue->GetName(), num);

    if (ev == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    if (num < 1) {
        *out = 0;
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    *out = equeue->WaitForEvents(ev, num, timo);

    if (*out == 0) {
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAddHRTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelTimespec* ts,
                                          void* udata) {
    auto equeue=GetEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    const auto total_ns = ts->tv_sec * 1000000000 + ts->tv_nsec;

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::HrTimer;
    event.event.flags = OrbisKernelEvent::Flags::Add | OrbisKernelEvent::Flags::OneShot;
    event.event.fflags = 0;
    // Data is stored as the address of a OrbisKernelBintime struct.
    OrbisKernelBintime time{ts->tv_sec, ts->tv_nsec * 0x44b82fa09};
    event.event.data = reinterpret_cast<u64>(&time);
    event.event.udata = udata;

    // HR timers cannot be implemented within the existing event queue architecture due to the
    // slowness of the notification mechanism. For instance, a 100us timer will lose its precision
    // as the trigger time drifts by +50-700%, depending on the host PC and workload. To address
    // this issue, we use a spinlock for small waits (which can be adjusted using
    // `HrTimerSpinlockThresholdUs`) and fall back to boost asio timers if the time to tick is
    // large. Even for large delays, we truncate a small portion to complete the wait
    // using the spinlock, prioritizing precision.
    if (total_ns < HrTimerSpinlockThresholdNs) {
        return equeue->AddSmallTimer(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
    }

    if (!equeue->AddEvent(event)) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteHRTimerEvent(OrbisKernelEqueue eq, int id) {
    auto equeue=GetEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    if (equeue->HasSmallTimer()) {
        return equeue->RemoveSmallTimer(id) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOENT;
    } else {
        return equeue->RemoveEvent(id, OrbisKernelEvent::Filter::HrTimer)
                   ? ORBIS_OK
                   : ORBIS_KERNEL_ERROR_ENOENT;
    }
}

int PS4_SYSV_ABI sceKernelAddTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelUseconds usec,
                                        void* udata) {
    auto equeue=GetEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = static_cast<u64>(id);
    event.event.filter = OrbisKernelEvent::Filter::Timer;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    event.event.fflags = 0;
    event.event.data = usec / 1000;
    event.event.udata = udata;

    if (!equeue->AddEvent(event)) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteTimerEvent(OrbisKernelEqueue eq, int id) {
    auto equeue=GetEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    return equeue->RemoveEvent(id, OrbisKernelEvent::Filter::Timer)
               ? ORBIS_OK
               : ORBIS_KERNEL_ERROR_ENOENT;
}

int PS4_SYSV_ABI sceKernelAddUserEvent(OrbisKernelEqueue eq, int id) {
    auto equeue=GetEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::User;
    event.event.udata = 0;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    event.event.fflags = 0;
    event.event.data = 0;

    return equeue->AddEvent(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
}

int PS4_SYSV_ABI sceKernelAddUserEventEdge(OrbisKernelEqueue eq, int id) {
    auto equeue=GetEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::User;
    event.event.udata = 0;
    event.event.flags = OrbisKernelEvent::Flags::Add | OrbisKernelEvent::Flags::Clear;
    event.event.fflags = 0;
    event.event.data = 0;

    return equeue->AddEvent(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
}

void* PS4_SYSV_ABI sceKernelGetEventUserData(const OrbisKernelEvent* ev) {
    ASSERT(ev);
    return ev->udata;
}

u64 PS4_SYSV_ABI sceKernelGetEventId(const OrbisKernelEvent* ev) {
    return ev->ident;
}

int PS4_SYSV_ABI sceKernelTriggerUserEvent(OrbisKernelEqueue eq, int id, void* udata) {
    auto equeue=GetEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    if (!equeue->TriggerEvent(id, OrbisKernelEvent::Filter::User, udata)) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteUserEvent(OrbisKernelEqueue eq, int id) {
    auto equeue=GetEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    if (!equeue->RemoveEvent(id, OrbisKernelEvent::Filter::User)) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelGetEventFilter(const OrbisKernelEvent* ev) {
    return ev->filter;
}

u64 PS4_SYSV_ABI sceKernelGetEventData(const OrbisKernelEvent* ev) {
    return ev->data;
}

void RegisterEventQueue(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("nh2IFMgKTv8", "libScePosix", 1, "libkernel", posix_kqueue);
    LIB_FUNCTION("RW-GEfpnsqg", "libScePosix", 1, "libkernel", posix_kevent);
    LIB_FUNCTION("D0OdFMjp46I", "libkernel", 1, "libkernel", sceKernelCreateEqueue);
    LIB_FUNCTION("jpFjmgAC5AE", "libkernel", 1, "libkernel", sceKernelDeleteEqueue);
    LIB_FUNCTION("fzyMKs9kim0", "libkernel", 1, "libkernel", sceKernelWaitEqueue);
    LIB_FUNCTION("vz+pg2zdopI", "libkernel", 1, "libkernel", sceKernelGetEventUserData);
    LIB_FUNCTION("4R6-OvI2cEA", "libkernel", 1, "libkernel", sceKernelAddUserEvent);
    LIB_FUNCTION("WDszmSbWuDk", "libkernel", 1, "libkernel", sceKernelAddUserEventEdge);
    LIB_FUNCTION("R74tt43xP6k", "libkernel", 1, "libkernel", sceKernelAddHRTimerEvent);
    LIB_FUNCTION("J+LF6LwObXU", "libkernel", 1, "libkernel", sceKernelDeleteHRTimerEvent);
    LIB_FUNCTION("57ZK+ODEXWY", "libkernel", 1, "libkernel", sceKernelAddTimerEvent);
    LIB_FUNCTION("YWQFUyXIVdU", "libkernel", 1, "libkernel", sceKernelDeleteTimerEvent);
    LIB_FUNCTION("F6e0kwo4cnk", "libkernel", 1, "libkernel", sceKernelTriggerUserEvent);
    LIB_FUNCTION("LJDwdSNTnDg", "libkernel", 1, "libkernel", sceKernelDeleteUserEvent);
    LIB_FUNCTION("mJ7aghmgvfc", "libkernel", 1, "libkernel", sceKernelGetEventId);
    LIB_FUNCTION("23CPPI1tyBY", "libkernel", 1, "libkernel", sceKernelGetEventFilter);
    LIB_FUNCTION("kwGyyjohI50", "libkernel", 1, "libkernel", sceKernelGetEventData);
}

} // namespace Libraries::Kernel
