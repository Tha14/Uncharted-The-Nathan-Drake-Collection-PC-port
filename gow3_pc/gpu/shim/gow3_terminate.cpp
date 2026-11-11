// gow3_terminate.cpp: when C++ ends the process through std::terminate, leave a trace in boot.log first.
//
// std::terminate ends in abort(), which Windows turns into a fail-fast (exit code 0xC0000409). A fail-fast skips
// every exception handler, so neither the runtime's fault handler nor the u2mp crash log sees it, and boot.log ended
// with libc++abi's bare "terminating" (11 Nov 2026, Uncharted 2 multiplayer menu, 3 minutes in). This handler writes
// what is known at that moment: the thread, the exception in flight (if any: none usually means a std::thread that was
// still running got destroyed or reassigned, or a noexcept function threw) and the return addresses on the stack,
// as module+offset (the guest image's base is in boot.log's "Guest image base" line). Then it aborts as before.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <typeinfo>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {

#ifdef _WIN32
void describe(void *address, char *out, size_t size) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) && module) {
        GetModuleBaseNameA(GetCurrentProcess(), module, name, sizeof name);
        std::snprintf(out, size, "%s+0x%llx", name,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                      reinterpret_cast<uintptr_t>(module)));
        return;
    }
    std::snprintf(out, size, "0x%llx (no module: guest code or generated code)",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
}
#endif

[[noreturn]] void on_terminate() {
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) std::abort();   // a second thread terminating meanwhile: just end

#ifdef _WIN32
    std::fprintf(stderr, "Terminate: std::terminate on thread %lu", static_cast<unsigned long>(GetCurrentThreadId()));
#else
    std::fprintf(stderr, "Terminate: std::terminate");
#endif
    if (std::exception_ptr pending = std::current_exception()) {
        try {
            std::rethrow_exception(pending);
        } catch (const std::exception &e) {
            std::fprintf(stderr, ", exception %s: %s", typeid(e).name(), e.what());
        } catch (...) {
            std::fprintf(stderr, ", an exception that is not a std::exception");
        }
    } else {
        std::fprintf(stderr, ", no exception in flight (a running std::thread destroyed or reassigned, or a throw "
                             "out of a noexcept function)");
    }
    std::fputc('\n', stderr);
#ifdef _WIN32
    void *frames[48];
    const USHORT count = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        char where[MAX_PATH + 48];
        describe(frames[i], where, sizeof where);
        std::fprintf(stderr, "  #%u %s\n", static_cast<unsigned>(i), where);
    }
#endif
    std::fflush(stderr);
    std::fflush(stdout);
    std::abort();
}

}  // namespace

// Called first thing by gow3gpu_init, so it holds for every thread the runtime starts afterwards.
extern "C" void gow3_install_terminate_trace() {
    std::set_terminate(&on_terminate);
}
