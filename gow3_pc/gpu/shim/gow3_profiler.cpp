// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: a sampling profiler for one thread (GOW3_PROFILE=<seconds per report>), to find where the
// GPU command thread spends its time and what grows when the frame rate falls over minutes. A
// thread suspends the target about once per millisecond, reads its instruction pointer and
// resumes it (nothing is allocated while it is suspended). Each report lists the sampled
// addresses as module+offset with their counts in GOW3_DUMP_DIR/profile_<n>.txt; offsets into
// the runtime's executable are symbolized offline (llvm-symbolizer, image base 0x140000000).
// profile_<n>_stacks.txt holds the call stacks (up to 16 frames, innermost first, counted per
// distinct stack), so a wait shows who waits: host frames are unwound with their unwind tables,
// guest code (no module, "?+address") by its saved frame pointers.
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

#ifdef _WIN32
namespace {

constexpr int MaxDepth = 16;

struct Stack {
    unsigned long long pc[MaxDepth];
    int depth;
};

bool Read(unsigned long long address, void* out, SIZE_T size) {
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &done) &&
           done == size;
}

/// Walks the suspended thread's stack (no allocation: the thread may hold the heap lock).
void Walk(CONTEXT context, Stack& stack) {
    const unsigned long long low = context.Rsp, high = context.Rsp + (8ull << 20);
    stack.depth = 0;
    while (stack.depth < MaxDepth && context.Rip) {
        stack.pc[stack.depth++] = context.Rip;
        DWORD64 image_base = 0;
        if (PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr)) {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                             &handler_data, &establisher, nullptr);
        } else if (image_base) { // a leaf function: the return address is on top of the stack
            unsigned long long ret = 0;
            if (!Read(context.Rsp, &ret, sizeof(ret))) {
                break;
            }
            context.Rip = ret;
            context.Rsp += 8;
        } else { // guest code: saved frame pointers, if it keeps them
            unsigned long long frame[2];
            if (context.Rbp < low || context.Rbp >= high || !Read(context.Rbp, frame, sizeof(frame))) {
                break;
            }
            context.Rip = frame[1];
            context.Rsp = context.Rbp + 16;
            context.Rbp = frame[0];
        }
        if (context.Rsp < low || context.Rsp >= high) {
            break;
        }
    }
}

} // namespace
#endif

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
        std::vector<Stack> stacks;
        stacks.reserve(samples.capacity());
        auto last = std::chrono::steady_clock::now();
        for (int report = 0;;) {
            Sleep(1);
            if (SuspendThread(thread) == DWORD(-1)) {
                break;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            const BOOL ok = GetThreadContext(thread, &context);
            if (ok && samples.size() < samples.capacity()) {
                samples.push_back(context.Rip);
                stacks.emplace_back();
                Walk(context, stacks.back());
            }
            ResumeThread(thread);
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
            std::string tag = label;
            for (char& c : tag) {
                c = c == ' ' ? '-' : c;
            }
            std::unordered_map<unsigned long long, std::string> names;
            const auto name_of = [&names](unsigned long long rip) -> const std::string& {
                auto [it, added] = names.try_emplace(rip);
                if (added) {
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
                    char text[MAX_PATH + 32];
                    std::snprintf(text, sizeof(text), "%s+%#llx", base ? base + 1 : module_name, offset);
                    it->second = text;
                }
                return it->second;
            };
            const std::string stem = dir + "/profile_" + tag + "_" + std::to_string(report++);
            const std::string path = stem + ".txt";
            if (FILE* f = std::fopen(path.c_str(), "w")) {
                std::fprintf(f, "# %s, %zu samples in %d s\n", label.c_str(), samples.size(), period);
                for (const auto& [n, rip] : sorted) {
                    std::fprintf(f, "%u %s\n", n, name_of(rip).c_str());
                }
                std::fclose(f);
                std::printf("Profiler: %zu samples written to %s\n", samples.size(), path.c_str());
                std::fflush(stdout);
            }
            std::unordered_map<std::string, unsigned> stack_counts;
            for (const Stack& stack : stacks) {
                std::string key;
                for (int i = 0; i < stack.depth; ++i) {
                    key += (i ? ";" : "") + name_of(stack.pc[i]);
                }
                ++stack_counts[key];
            }
            std::vector<std::pair<unsigned, std::string>> sorted_stacks;
            for (auto& [key, n] : stack_counts) {
                sorted_stacks.emplace_back(n, key);
            }
            std::sort(sorted_stacks.rbegin(), sorted_stacks.rend());
            if (FILE* f = std::fopen((stem + "_stacks.txt").c_str(), "w")) {
                std::fprintf(f, "# %s, %zu samples in %d s, stacks innermost first\n", label.c_str(),
                             stacks.size(), period);
                for (const auto& [n, key] : sorted_stacks) {
                    std::fprintf(f, "%u %s\n", n, key.c_str());
                }
                std::fclose(f);
            }
            samples.clear();
            stacks.clear();
        }
        CloseHandle(thread);
    }).detach();
#else
    (void)thread_id;
    (void)name;
#endif
}

void StartCurrent(const char* name) {
#ifdef _WIN32
    Start(GetCurrentThreadId(), name);
#else
    (void)name;
#endif
}

} // namespace Gow3Profiler
