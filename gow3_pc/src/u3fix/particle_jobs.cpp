// Uncharted 3 1.02, 12 Nov 2026: wait for the particle jobs at the renderer's job check instead of running on.
//
// The particle renderer's job check (particle-renderer.cpp:719, image 0xeb9ca0, AssertGameJobsNotRunning) asserts that the
// five particle job counters it is given (an array of counter objects, value at +0x20, SetupPreupdate first) are 0. On
// PS4 those jobs are always done by then. On a slower PC a SetupPreupdate job was still running: the assertion trap is
// stepped over, the renderer marks its jobs finished and the next frame starts new ones on top (1, then 2 jobs still
// running), and the game froze seconds later (a co-op host, 2026-11-11). The check's entry now first waits for those
// counters to reach 0, at most 1 s, so the frame goes on the way it does on PS4; a job that never ends leaves the game as
// before. Found by its bytes; installed as a jump at the entry to a stub that calls the wait and replays the prologue.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace u3fix {

namespace {

std::atomic<uint64_t> g_waits{0}, g_timeouts{0};

bool busy(uint64_t **counters) {
    for (int i = 0; i < 5; i++)
        if (counters[i] && __atomic_load_n(&counters[i][4], __ATOMIC_ACQUIRE)) return true;   // value at +0x20
    return false;
}

// called from the stub with the check's argument (the guest calls with the System V convention)
__attribute__((sysv_abi)) void particle_jobs_wait(uint64_t **counters) {
    if (!counters || !busy(counters)) return;
    const auto start = std::chrono::steady_clock::now();
    unsigned spins = 0;
    bool still = true;
    unsigned waited_ms = 0;
    for (;;) {
        still = busy(counters);
        if (!still) break;
        waited_ms = static_cast<unsigned>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        if (waited_ms >= 1000) break;
        if (++spins < 200) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const uint64_t n = ++g_waits;
    if (still) {
        const uint64_t t = ++g_timeouts;
        if (t <= 5 || t % 100 == 0)
            std::printf("Uncharted 3: particle jobs still running after 1 s at the renderer's check (time %llu); the game goes on\n",
                        static_cast<unsigned long long>(t));
    } else if (n <= 5 || n % 500 == 0) {
        std::printf("Uncharted 3: particle jobs still running at the renderer's check: waited %u ms for them (time %llu)\n",
                    waited_ms, static_cast<unsigned long long>(n));
    }
}

// push rbp; mov rbp,rsp; push r15..r12; push rbx; push rax; then the five counter loads and the first test
constexpr unsigned char kSite[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x48, 0x8b, 0x07, 0x48, 0x8b,
    0x70, 0x20, 0x48, 0x8b, 0x47, 0x08, 0x4c, 0x8b, 0x68, 0x20, 0x48, 0x8b, 0x47, 0x10, 0x4c, 0x8b, 0x60, 0x20, 0x48,
    0x8b, 0x47, 0x18, 0x4c, 0x8b, 0x78, 0x20, 0x48, 0x8b, 0x47, 0x20, 0x4c, 0x8b, 0x70, 0x20, 0x48, 0x85, 0xf6};
constexpr unsigned kPrologue = 6;   // push rbp; mov rbp,rsp; push r15

}  // namespace

bool install_particle_jobs_wait(unsigned char *image, uint64_t size) {
    const uint64_t at = find_unique(image, size, kSite, sizeof(kSite));
    if (at == kNotFound) return false;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    // entry rsp = 8 mod 16: push rdi makes the call aligned; the handler (System V) keeps rbx/rbp/r12-r15 and the check's
    // only input, rdi, is restored; then the replaced prologue and a jump back
    unsigned n = 0;
    stub[n++] = 0x57;                                                         // push rdi
    const uint64_t handler = reinterpret_cast<uint64_t>(&particle_jobs_wait);
    stub[n++] = 0x48; stub[n++] = 0xb8; std::memcpy(stub + n, &handler, 8); n += 8;   // mov rax, handler
    stub[n++] = 0xff; stub[n++] = 0xd0;                                       // call rax
    stub[n++] = 0x5f;                                                         // pop rdi
    std::memcpy(stub + n, image + at, kPrologue); n += kPrologue;            // the prologue
    stub[n++] = 0xff; stub[n++] = 0x25; std::memset(stub + n, 0, 4); n += 4; // jmp [rip]
    const uint64_t back = reinterpret_cast<uint64_t>(image + at + kPrologue);
    std::memcpy(stub + n, &back, 8); n += 8;
    if (!finish_stub(stub) || !jump_to(image, at, kPrologue, stub)) return false;
    std::printf("Uncharted 3: particle job wait at the renderer's job check %#llx\n", static_cast<unsigned long long>(at));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
