// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_copy.h"
#include "gow3_threads.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gow3_toggles.h"
#include "common/thread.h"

namespace Gow3Copy {
namespace {

thread_local bool in_copy_thread = false;

// gow3 20 Oct 2026 (idle CPU): WaitAll and Run waited in std::this_thread::yield() loops
// (SwitchToThread: a full-speed spin whenever no other thread was ready on that core), WaitAll
// taking the pool mutex on every pass. They now return at once when nothing is pending (one
// atomic load, no lock), spin a little with pause instructions (copies often end within
// microseconds), then sleep on done_cv, which the thread that makes the pool idle (or that
// finishes a ParallelFor's last index) signals. WaitAll still runs queued tasks itself.
// GOW3_COPY_WAIT_BLOCK=0: the yield loops.
bool BlockingWaits() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_COPY_WAIT_BLOCK");
        const bool block = !(env && env[0] == '0');
        std::printf("Copy threads: waits %s\n",
                    block ? "spin briefly, then sleep (GOW3_COPY_WAIT_BLOCK=0: yield loops)"
                          : "yield loops (GOW3_COPY_WAIT_BLOCK=0)");
        return block;
    }();
    return on;
}
/// Pause instructions before a wait sleeps: ~10 us on older Intel cores, up to ~150 us on
/// cores with a long pause (Skylake and later).
constexpr unsigned WaitSpins = 2048;

class Pool {
public:
    Pool() {
        // Copies are on the critical path (fences wait for them): normal priority, a quarter
        // of the hardware threads available to the process (Steam Deck 2).
        unsigned count = std::clamp(Gow3Threads::Available() / 4, 1u, 4u);
        if (const char* env = std::getenv("GOW3_COPY_THREADS")) {
            count = static_cast<unsigned>(std::clamp(std::atoi(env), 0, 16));
        }
        epochs.emplace_back();
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([this, i] { Loop(i); });
        }
    }
    ~Pool() {
        {
            std::scoped_lock lk{mutex};
            stop = true;
        }
        cv.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
    }

    bool Enabled() const {
        return !threads.empty();
    }

    std::size_t Count() const {
        return threads.size();
    }

    void Async(std::function<void()> task) {
        bool wake_waiters;
        {
            std::scoped_lock lk{mutex};
            const u64 epoch = epoch_base + epochs.size() - 1;
            ++epochs.back().pending;
            ++pending;
            async_tasks.push_back({std::move(task), epoch});
            PublishIdle();
            wake_waiters = idle_waiters != 0;
        }
        cv.notify_one();
        if (wake_waiters) {
            done_cv.notify_all(); // a sleeping WaitAll runs it too, as the yield loop did
        }
    }

    void AfterCopies(std::function<void()> callback) {
        {
            std::unique_lock lk{mutex};
            if (epochs.size() > 1 || epochs.back().pending != 0 || draining) {
                // Runs once every copy issued so far is done, in order with earlier ones.
                epochs.back().callbacks.push_back(std::move(callback));
                epochs.emplace_back();
                PublishIdle();
                return;
            }
        }
        callback();
    }

    /// gow3: no task pending or running and no callback left (WaitAll would return). Acquire:
    /// pairs with the release store in PublishIdle, made under the mutex after the last task
    /// and callback ran, so their writes are visible to the caller once this returns true.
    bool IdleNow() const {
        return idle.load(std::memory_order_acquire);
    }

    void WaitAll() {
        if (BlockingWaits()) {
            WaitAllBlocking();
            return;
        }
        // The waiting thread helps instead of only spinning.
        while (true) {
            Task task;
            {
                std::scoped_lock lk{mutex};
                if (pending == 0 && epochs.size() == 1 && !draining) {
                    return;
                }
                if (!async_tasks.empty()) {
                    task = std::move(async_tasks.front());
                    async_tasks.pop_front();
                }
            }
            if (task.run) {
                RunAsync(task);
            } else {
                std::this_thread::yield();
            }
        }
    }

    void Run(std::size_t count, const std::function<void(std::size_t)>& task) {
        auto job = std::make_shared<Job>();
        job->task = &task;
        job->count = count;
        {
            std::scoped_lock lk{mutex};
            current = job;
            ++generation;
        }
        cv.notify_all();
        Work(*job);
        // Workers only call the task for indices below count, all done before this returns.
        if (BlockingWaits()) {
            for (unsigned spin = 0;
                 spin < WaitSpins && job->done.load(std::memory_order_acquire) < count; ++spin) {
                __builtin_ia32_pause();
            }
            // The thread finishing the last index takes the mutex before it signals done_cv:
            // either this check (under the mutex) sees its count, or this thread is already
            // asleep when the signal comes. Acquire: the indices' writes are visible here.
            std::unique_lock lk{mutex};
            done_cv.wait(lk, [&] { return job->done.load(std::memory_order_acquire) >= count; });
            if (current == job) {
                current.reset();
            }
            return;
        }
        while (job->done.load(std::memory_order_acquire) < count) {
            std::this_thread::yield();
        }
        std::scoped_lock lk{mutex};
        if (current == job) {
            current.reset();
        }
    }

private:
    using u64 = unsigned long long;
    struct Task {
        std::function<void()> run;
        u64 epoch{};
    };
    /// Copies issued between two AfterCopies() calls, and the callbacks waiting for them.
    struct Epoch {
        std::size_t pending = 0;
        std::vector<std::function<void()>> callbacks;
    };
    struct Job {
        const std::function<void(std::size_t)>* task{};
        std::size_t count{};
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
    };

    void RunAsync(Task& task) {
        const bool was = in_copy_thread;
        in_copy_thread = true;
        task.run();
        in_copy_thread = was;
        Complete(task.epoch);
    }

    /// Retires a task and runs the callbacks of epochs with nothing left, oldest first. One
    /// thread drains at a time so the callbacks keep their order.
    void Complete(u64 epoch) {
        std::unique_lock lk{mutex};
        --epochs[epoch - epoch_base].pending;
        --pending;
        if (draining) {
            return;
        }
        draining = true;
        while (true) {
            std::vector<std::function<void()>> ready;
            while (epochs.size() > 1 && epochs.front().pending == 0) {
                for (auto& callback : epochs.front().callbacks) {
                    ready.push_back(std::move(callback));
                }
                epochs.pop_front();
                ++epoch_base;
            }
            if (ready.empty()) {
                break;
            }
            lk.unlock();
            for (auto& callback : ready) {
                callback();
            }
            lk.lock();
        }
        draining = false;
        // The pool turns idle only here (the drain also covers decrements made meanwhile by
        // threads that returned above): wake the sleeping WaitAll callers.
        PublishIdle();
        if (BlockingWaits() && IdleLocked()) {
            done_cv.notify_all();
        }
    }

    void Work(Job& job) {
        const bool was = in_copy_thread;
        in_copy_thread = true;
        for (std::size_t i; (i = job.next.fetch_add(1, std::memory_order_relaxed)) < job.count;) {
            (*job.task)(i);
            // Release: the index's writes before the count Run reads with acquire.
            if (job.done.fetch_add(1, std::memory_order_release) + 1 == job.count &&
                BlockingWaits()) {
                // The last index: wake Run (see there for why the mutex is taken first).
                { std::scoped_lock lk{mutex}; }
                done_cv.notify_all();
            }
        }
        in_copy_thread = was;
    }

    /// gow3: under the mutex. WaitAll's condition.
    bool IdleLocked() const {
        return pending == 0 && epochs.size() == 1 && !draining;
    }
    /// gow3: under the mutex, after every change of pending, epochs or draining: the flag
    /// IdleNow and WaitAll read without the lock.
    void PublishIdle() {
        idle.store(IdleLocked(), std::memory_order_release);
    }

    /// gow3 (GOW3_COPY_WAIT_BLOCK): WaitAll without the yield loop.
    void WaitAllBlocking() {
        if (IdleNow()) {
            return; // nothing pending: no lock (the acquire load orders the copies' writes)
        }
        bool spun = false;
        while (true) {
            Task task;
            {
                std::unique_lock lk{mutex};
                if (spun) {
                    // Sleeps until idle or until a queued task can be run here. The condition
                    // is checked under the mutex its writers hold (Async, AfterCopies,
                    // Complete), and they signal done_cv after changing it: no lost wake-up.
                    ++idle_waiters;
                    done_cv.wait(lk, [&] { return IdleLocked() || !async_tasks.empty(); });
                    --idle_waiters;
                }
                if (IdleLocked()) {
                    return;
                }
                if (!async_tasks.empty()) {
                    task = std::move(async_tasks.front());
                    async_tasks.pop_front();
                }
            }
            if (task.run) {
                RunAsync(task); // helps instead of only waiting
                continue;
            }
            // Only running copies left: spin on the flag a little before sleeping.
            for (unsigned spin = 0; spin < WaitSpins && !IdleNow(); ++spin) {
                __builtin_ia32_pause();
            }
            if (IdleNow()) {
                return;
            }
            spun = true;
        }
    }

    void Loop(unsigned index) {
        Common::SetCurrentThreadName(("gow3:Copy" + std::to_string(index)).c_str());
        u64 seen = 0;
        std::unique_lock lk{mutex};
        while (true) {
            cv.wait(lk, [&] {
                return stop || !async_tasks.empty() || (generation != seen && current);
            });
            if (stop) {
                return;
            }
            if (!async_tasks.empty()) {
                Task task = std::move(async_tasks.front());
                async_tasks.pop_front();
                lk.unlock();
                RunAsync(task);
                lk.lock();
                continue;
            }
            seen = generation;
            const auto job = current;
            lk.unlock();
            Work(*job);
            lk.lock();
        }
    }

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<Job> current;
    u64 generation = 0;
    bool stop = false;
    std::deque<Task> async_tasks;
    std::deque<Epoch> epochs; ///< back() is open for new copies
    u64 epoch_base = 0;       ///< epoch number of epochs.front()
    std::size_t pending = 0;
    bool draining = false;
    /// gow3 (GOW3_COPY_WAIT_BLOCK): signalled when the pool turns idle, a task is queued while
    /// a WaitAll sleeps, or a ParallelFor's last index is done.
    std::condition_variable done_cv;
    std::atomic<bool> idle{true}; ///< IdleLocked() as of the last change (PublishIdle)
    unsigned idle_waiters = 0;    ///< WaitAll callers asleep on done_cv (under the mutex)
};

Pool& GetPool() {
    static Pool pool;
    return pool;
}

struct Batch {
    std::vector<Item> items;
    unsigned long long bytes = 0;
};
thread_local Batch batch;

} // namespace

bool Enabled() {
    return GetPool().Enabled() && !Gow3Toggle::Disabled(Gow3Toggle::ParallelCopies);
}

bool Idle() {
    return BlockingWaits() && batch.items.empty() && GetPool().IdleNow();
}

void Async(std::function<void()> task) {
    if (!Enabled()) {
        task();
        return;
    }
    GetPool().Async(std::move(task));
}

void FlushBatch() {
    if (batch.items.empty()) {
        return;
    }
    auto items = std::make_shared<std::vector<Item>>(std::move(batch.items));
    batch.items = {};
    batch.items.reserve(256);
    batch.bytes = 0;
    Async([items] {
        for (const auto& item : *items) {
            item.run(item);
        }
    });
}

void QueueCopy(const Item& item) {
    if (!Enabled()) {
        item.run(item);
        return;
    }
    // Smaller batches (32 items / 64 KiB, done before the next fence) measured slower.
    batch.items.push_back(item);
    batch.bytes += item.size;
    if (batch.bytes >= 512 * 1024 || batch.items.size() >= 256) {
        FlushBatch();
    }
}

void AfterCopies(std::function<void()> callback) {
    FlushBatch();
    GetPool().AfterCopies(std::move(callback));
}

void WaitAsync() {
    // The caller's own batch runs here: handing it over only to wait for it costs a wakeup.
    // gow3 17 Oct: GOW3_COPY_SPLIT_KB=n splits batches of n KiB or more with the copy threads.
    // Off by default: Uncharted 3 at 4K measured no gain (62.6 vs 63.9 FPS); the copies there
    // were mesh data copied on every bind (BufferCache::UseArenaForStatic removed them).
    static const unsigned long long split_bytes = [] {
        const char* env = std::getenv("GOW3_COPY_SPLIT_KB");
        return env ? std::strtoull(env, nullptr, 10) * 1024 : 0ull;
    }();
    if (!batch.items.empty()) {
        if (split_bytes && batch.bytes >= split_bytes && batch.items.size() >= 4 && Enabled()) {
            auto items = std::make_shared<std::vector<Item>>(std::move(batch.items));
            const unsigned long long total = batch.bytes;
            batch.items = {};
            batch.items.reserve(256);
            batch.bytes = 0;
            // Parts of about equal size: the copy threads take all but the first, which runs here.
            const std::size_t parts = std::min<std::size_t>(GetPool().Count() + 1, items->size());
            const unsigned long long share = total / parts + 1;
            std::size_t begin = 0;
            std::size_t first_end = 0;
            unsigned long long bytes = 0;
            for (std::size_t i = 0, part = 0; i < items->size(); ++i) {
                bytes += (*items)[i].size;
                const bool last = i + 1 == items->size();
                if (bytes >= share || last) {
                    if (part == 0) {
                        first_end = i + 1;
                    } else {
                        GetPool().Async([items, begin, end = i + 1] {
                            for (std::size_t k = begin; k < end; ++k) {
                                (*items)[k].run((*items)[k]);
                            }
                        });
                    }
                    ++part;
                    begin = i + 1;
                    bytes = 0;
                }
            }
            for (std::size_t k = 0; k < first_end; ++k) {
                (*items)[k].run((*items)[k]);
            }
        } else {
            for (const auto& item : batch.items) {
                item.run(item);
            }
            batch.items.clear();
            batch.bytes = 0;
        }
    }
    GetPool().WaitAll();
}

void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& task) {
    if (count <= 1 || in_copy_thread || !Enabled()) {
        for (std::size_t i = 0; i < count; ++i) {
            task(i);
        }
        return;
    }
    GetPool().Run(count, task);
}

} // namespace Gow3Copy
