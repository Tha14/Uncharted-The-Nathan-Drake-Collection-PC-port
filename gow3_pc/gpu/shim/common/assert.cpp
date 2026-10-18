// gow3: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"

// gow3 18 Oct 2026: set on compute precompile workers (vk_pipeline_cache.cpp), whose input is a
// guess ahead of the GPU thread: an assertion there throws, the job is dropped, and the GPU thread
// compiles the real dispatch itself. Everywhere else an assertion stops the port as before.
thread_local bool gow3_assert_throws = false;

void assert_fail_impl() {
    if (gow3_assert_throws) {
        throw std::runtime_error("GPU library assertion");
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
