// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: a sampling profiler for one thread (GOW3_PROFILE=<seconds per report>), to find where the
// GPU command thread spends its time and what grows when the frame rate falls over minutes. A
// thread suspends the target about once per millisecond, reads its instruction pointer and
// resumes it (nothing is allocated while it is suspended). Each report lists the sampled
// addresses as module+offset with their counts in GOW3_DUMP_DIR/profile_<n>.txt; offsets into
// the runtime's executable are symbolized offline (llvm-symbolizer, image base 0x140000000).
#include "gow3_profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Gow3Profiler {

void Start(unsigned long thread_id, const char* name) {
#ifdef _WIN32
    const char* env = std::getenv("GOW3_PROFILE");
    if (!env || !*env || std::atoi(env) <= 0) {
        return;
    }
    const int period = std::max(5, std::atoi(env));
    const char* dir_env = std::getenv("GOW3_DUMP_DIR");
    const std::string dir = dir_env && *dir_env ? dir_env : ".";
    std::thread([thread_id, period, dir, label = std::string(name)] {
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_QUERY_INFORMATION,
                                   FALSE, thread_id);
        if (!thread) {
            std::printf("Profiler: cannot open thread %lu\n", thread_id);
            return;
        }
        std::printf("Profiler: sampling %s (thread %lu), a report every %d s\n", label.c_str(),
                    thread_id, period);
        std::fflush(stdout);
        std::vector<unsigned long long> samples;
        samples.reserve(size_t(period) * 2000);
        auto last = std::chrono::steady_clock::now();
        for (int report = 0;;) {
            Sleep(1);
            if (SuspendThread(thread) == DWORD(-1)) {
                break;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            const BOOL ok = GetThreadContext(thread, &context);
            ResumeThread(thread);
            if (ok && samples.size() < samples.capacity()) {
                samples.push_back(context.Rip);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(period)) {
                continue;
            }
            last = now;
            std::unordered_map<unsigned long long, unsigned> counts;
            for (const auto rip : samples) {
                ++counts[rip];
            }
            std::vector<std::pair<unsigned, unsigned long long>> sorted;
            for (const auto& [rip, n] : counts) {
                sorted.emplace_back(n, rip);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            const std::string path = dir + "/profile_" + std::to_string(report++) + ".txt";
            if (FILE* f = std::fopen(path.c_str(), "w")) {
                std::fprintf(f, "# %s, %zu samples in %d s\n", label.c_str(), samples.size(), period);
                for (const auto& [n, rip] : sorted) {
                    HMODULE module = nullptr;
                    char module_name[MAX_PATH] = "?";
                    unsigned long long offset = rip;
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCSTR>(rip), &module) &&
                        module) {
                        GetModuleFileNameA(module, module_name, sizeof(module_name));
                        offset = rip - reinterpret_cast<unsigned long long>(module);
                    }
                    const char* base = std::strrchr(module_name, '\\');
                    std::fprintf(f, "%u %s+%#llx\n", n, base ? base + 1 : module_name, offset);
                }
                std::fclose(f);
                std::printf("Profiler: %zu samples written to %s\n", samples.size(), path.c_str());
                std::fflush(stdout);
            }
            samples.clear();
        }
        CloseHandle(thread);
    }).detach();
#else
    (void)thread_id;
    (void)name;
#endif
}

} // namespace Gow3Profiler
