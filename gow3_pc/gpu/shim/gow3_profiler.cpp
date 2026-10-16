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
#include <psapi.h>
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

/// gow3 16 Oct: the loaded modules' unwind tables (.pdata), read while the sampled thread runs.
/// RtlLookupFunctionEntry takes the function table lock: when the suspended thread held it (an
/// exception being dispatched, e.g. a guest page fault), the profiler waited for it forever with
/// the thread suspended and the game froze after the first report.
struct Module {
    unsigned long long base, end;
    const RUNTIME_FUNCTION* table;
    DWORD count;
};

std::vector<Module> LoadModules() {
    std::vector<Module> out;
    HMODULE handles[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed)) {
        return out;
    }
    for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i) {
        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info))) {
            continue;
        }
        const auto base = reinterpret_cast<unsigned long long>(info.lpBaseOfDll);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        out.push_back({base, base + info.SizeOfImage,
                       dir.Size ? reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress)
                                : nullptr,
                       DWORD(dir.Size / sizeof(RUNTIME_FUNCTION))});
    }
    std::sort(out.begin(), out.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    return out;
}

/// The unwind entry for `rip` without locks; image_base is set when rip is in a module.
const RUNTIME_FUNCTION* LookupFunction(const std::vector<Module>& modules, unsigned long long rip,
                                       DWORD64& image_base) {
    image_base = 0;
    auto it = std::upper_bound(modules.begin(), modules.end(), rip,
                               [](unsigned long long value, const Module& m) { return value < m.base; });
    if (it == modules.begin()) {
        return nullptr;
    }
    --it;
    if (rip >= it->end) {
        return nullptr;
    }
    image_base = it->base;
    const DWORD offset = DWORD(rip - it->base);
    DWORD lo = 0, hi = it->count;
    while (lo < hi) {
        const DWORD mid = (lo + hi) / 2;
        const RUNTIME_FUNCTION& f = it->table[mid];
        if (offset < f.BeginAddress) {
            hi = mid;
        } else if (offset >= f.EndAddress) {
            lo = mid + 1;
        } else {
            const RUNTIME_FUNCTION* entry = &f;
            // An indirect entry (low bit set) names the entry that holds the unwind data, as
            // RtlLookupFunctionEntry resolves it (system DLLs have them).
            if (entry->UnwindData & 1) {
                const unsigned long long target = it->base + (entry->UnwindData & ~DWORD(1));
                if (target < it->base || target + sizeof(RUNTIME_FUNCTION) > it->end) {
                    return nullptr;
                }
                entry = reinterpret_cast<const RUNTIME_FUNCTION*>(target);
            }
            // The unwind info must lie in the image and be a known version (1 or 2).
            const unsigned long long info = it->base + entry->UnwindData;
            unsigned char header = 0;
            if (info < it->base || info + 4 > it->end || !Read(info, &header, 1) ||
                (header & 7) == 0 || (header & 7) > 2) {
                return nullptr;
            }
            return entry;
        }
    }
    return nullptr;
}

/// Walks the suspended thread's stack (no allocation and no locks: the thread may hold the heap
/// or the function table lock).
/// One x64 unwind step from the function's unwind codes, every stack read checked (gow3 16 Oct:
/// RtlVirtualUnwind reads the stack directly; a frame register holding garbage in a sampled
/// frame took the game down with a read of address 0xa0). Epilogues are not detected: a sample
/// taken in one may unwind wrongly and ends early. Returns false when the walk has to stop.
bool UnwindStep(const RUNTIME_FUNCTION* function, DWORD64 image_base, CONTEXT& context) {
    // Register numbers of the unwind codes: rax rcx rdx rbx rsp rbp rsi rdi r8-r15.
    unsigned long long* regs[16] = {&context.Rax, &context.Rcx, &context.Rdx, &context.Rbx,
                                    &context.Rsp, &context.Rbp, &context.Rsi, &context.Rdi,
                                    &context.R8,  &context.R9,  &context.R10, &context.R11,
                                    &context.R12, &context.R13, &context.R14, &context.R15};
    const unsigned long long offset = context.Rip - image_base - function->BeginAddress;
    bool primary = true;
    for (int chain = 0; chain < 8; ++chain, primary = false) {
        const unsigned long long info = image_base + function->UnwindData;
        unsigned char header[4];
        if (!Read(info, header, sizeof(header))) {
            return false;
        }
        const unsigned version = header[0] & 7, flags = header[0] >> 3, count = header[2];
        const unsigned frame_reg = header[3] & 15, frame_offset = header[3] >> 4;
        if (version == 0 || version > 2) {
            return false;
        }
        unsigned short codes[256];
        const unsigned slots = (count + 1) & ~1u; // padded to an even count before a chain entry
        if (slots && !Read(info + 4, codes, slots * sizeof(unsigned short))) {
            return false;
        }
        for (unsigned i = 0; i < count;) {
            const unsigned code_offset = codes[i] & 0xff, op = (codes[i] >> 8) & 15, op_info = codes[i] >> 12;
            unsigned used = 1;
            unsigned extra = 0;
            switch (op) {
            case 1: used = op_info == 0 ? 2 : 3; break;
            case 4: case 8: used = 2; break;
            case 5: case 9: used = 3; break;
            case 6: used = version == 2 ? 1 : 2; break; // v2: epilogue descriptor
            default: break;
            }
            if (i + used > count) {
                return false;
            }
            // Prologue operations not yet executed at this offset (primary entry only).
            const bool executed = !primary || op == 6 || offset >= code_offset;
            if (executed) {
                unsigned long long& rsp = context.Rsp;
                switch (op) {
                case 0: // UWOP_PUSH_NONVOL
                    if (!Read(rsp, regs[op_info], 8)) {
                        return false;
                    }
                    rsp += 8;
                    break;
                case 1: // UWOP_ALLOC_LARGE
                    extra = op_info == 0 ? codes[i + 1] * 8u : (codes[i + 1] | (unsigned(codes[i + 2]) << 16));
                    rsp += extra;
                    break;
                case 2: // UWOP_ALLOC_SMALL
                    rsp += op_info * 8 + 8;
                    break;
                case 3: // UWOP_SET_FPREG
                    rsp = *regs[frame_reg] - frame_offset * 16ull;
                    break;
                case 4: // UWOP_SAVE_NONVOL
                    if (!Read(rsp + codes[i + 1] * 8ull, regs[op_info], 8)) {
                        return false;
                    }
                    break;
                case 5: // UWOP_SAVE_NONVOL_FAR
                    if (!Read(rsp + (codes[i + 1] | (unsigned(codes[i + 2]) << 16)), regs[op_info], 8)) {
                        return false;
                    }
                    break;
                case 10: { // UWOP_PUSH_MACHFRAME: the interrupted frame (rip, cs, eflags, rsp, ss)
                    const unsigned long long frame = rsp + (op_info ? 8 : 0);
                    unsigned long long rip = 0, old_rsp = 0;
                    if (!Read(frame, &rip, 8) || !Read(frame + 24, &old_rsp, 8)) {
                        return false;
                    }
                    context.Rip = rip;
                    rsp = old_rsp;
                    return true;
                }
                default: // xmm saves, epilogue descriptors: no general register changes
                    break;
                }
            }
            i += used;
        }
        if (!(flags & UNW_FLAG_CHAININFO)) {
            break;
        }
        // The chained entry follows the (padded) codes.
        static thread_local RUNTIME_FUNCTION chained;
        if (!Read(info + 4 + slots * sizeof(unsigned short), &chained, sizeof(chained))) {
            return false;
        }
        function = &chained;
    }
    unsigned long long ret = 0;
    if (!Read(context.Rsp, &ret, sizeof(ret))) {
        return false;
    }
    context.Rip = ret;
    context.Rsp += 8;
    return true;
}

void Walk(const std::vector<Module>& modules, CONTEXT context, Stack& stack) {
    const unsigned long long low = context.Rsp, high = context.Rsp + (8ull << 20);
    stack.depth = 0;
    while (stack.depth < MaxDepth && context.Rip) {
        stack.pc[stack.depth++] = context.Rip;
        DWORD64 image_base = 0;
        if (const RUNTIME_FUNCTION* function = LookupFunction(modules, context.Rip, image_base)) {
            if (!UnwindStep(function, image_base, context)) {
                break;
            }
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
        std::vector<Module> modules = LoadModules();
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
                Walk(modules, context, stacks.back());
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
            modules = LoadModules(); // modules loaded since (the thread runs now)
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
