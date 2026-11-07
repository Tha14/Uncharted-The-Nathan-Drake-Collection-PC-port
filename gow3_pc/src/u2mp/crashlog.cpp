// u2mp crash log (diagnostics): an exception nothing handled is about to end the process. The runtime's
// own handler deals with guest faults it expects (GPU page protection...); anything that reaches
// here would otherwise vanish without a trace. Writes the fault, the registers and the return
// addresses on the stack that fall in the guest image or our executable, then lets it end as before.
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>

#include <cstdio>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

void where(char *out, size_t size, uint64_t address) {
    const uint64_t base = uint64_t(uintptr_t(image.base));
    if (address >= base && address < base + image.size) {
        std::snprintf(out, size, "guest 0x%llx", (unsigned long long)(address - base));
        return;
    }
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(uintptr_t(address)), &module) && module) {
        GetModuleBaseNameA(GetCurrentProcess(), module, name, sizeof name);
        std::snprintf(out, size, "%s+0x%llx", name, (unsigned long long)(address - uint64_t(uintptr_t(module))));
        return;
    }
    std::snprintf(out, size, "0x%llx", (unsigned long long)address);
}

LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
volatile LONG reported = 0;

void report(EXCEPTION_POINTERS *info);

LONG WINAPI last_chance(EXCEPTION_POINTERS *info) {
    report(info);
    return previous ? previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

// First-chance, called after the runtime's own vectored handler (which resolves the GPU page faults
// it expects and returns CONTINUE_EXECUTION for them): what reaches us is not one of those. Logs the
// first few access violations / fatal codes, then lets the search continue unchanged.
LONG CALLBACK first_chance(EXCEPTION_POINTERS *info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                       code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                       code == 0xc0000409 /* fail fast */ || code == EXCEPTION_PRIV_INSTRUCTION;
    if (fatal && InterlockedIncrement(&reported) <= 8) report(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

void report(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *e = info->ExceptionRecord;
    const CONTEXT *c = info->ContextRecord;
    char at[160];
    where(at, sizeof at, uint64_t(uintptr_t(e->ExceptionAddress)));
    std::fprintf(stdout, "u2mp: CRASH exception 0x%08lx at %s", e->ExceptionCode, at);
    if (e->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e->NumberParameters >= 2)
        std::fprintf(stdout, " (%s 0x%llx)", e->ExceptionInformation[0] ? "write" : "read",
                     (unsigned long long)e->ExceptionInformation[1]);
    std::fprintf(stdout, " thread %lu\n", GetCurrentThreadId());
    std::fprintf(stdout, "u2mp:   rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx rsp=%llx\n",
                 (unsigned long long)c->Rax, (unsigned long long)c->Rbx, (unsigned long long)c->Rcx,
                 (unsigned long long)c->Rdx, (unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
                 (unsigned long long)c->Rbp, (unsigned long long)c->Rsp);
    std::fprintf(stdout, "u2mp:   r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
                 (unsigned long long)c->R8, (unsigned long long)c->R9, (unsigned long long)c->R10,
                 (unsigned long long)c->R11, (unsigned long long)c->R12, (unsigned long long)c->R13,
                 (unsigned long long)c->R14, (unsigned long long)c->R15);
    // return addresses on the stack (guest image or a loaded module), up to 24
    const uint64_t base = uint64_t(uintptr_t(image.base));
    int shown = 0;
    MEMORY_BASIC_INFORMATION mbi;
    const uint64_t *sp = reinterpret_cast<const uint64_t *>(uintptr_t(c->Rsp));
    if (VirtualQuery(sp, &mbi, sizeof mbi)) {
        const uint64_t *end = reinterpret_cast<const uint64_t *>(uintptr_t(mbi.BaseAddress) + mbi.RegionSize);
        for (const uint64_t *p = sp; p < end && p < sp + 4096 && shown < 24; ++p) {
            const uint64_t v = *p;
            const bool guest = v >= base && v < base + image.size;
            HMODULE m = nullptr;
            const bool module = !guest && v > 0x10000 &&
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(uintptr_t(v)), &m) && m;
            if (!guest && !module) continue;
            where(at, sizeof at, v);
            std::fprintf(stdout, "u2mp:   stack +0x%llx: %s\n", (unsigned long long)((p - sp) * 8), at);
            ++shown;
        }
    }
    std::fflush(stdout);
}

}  // namespace

void install_crashlog() {
    previous = SetUnhandledExceptionFilter(&last_chance);
    AddVectoredExceptionHandler(0, &first_chance);   // 0: after the runtime's handler
    log("crash log armed");
}

}  // namespace u2mp
#else
namespace u2mp { void install_crashlog() {} }
#endif
