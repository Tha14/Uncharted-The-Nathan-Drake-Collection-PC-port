// u2mp rewrite tests: the fake image, stubs, the record and the comparison (see harness.h).
#include "harness.h"

#include <algorithm>
#include <cstdlib>

#if defined(_WIN32)
extern "C" __declspec(dllimport) void *__stdcall VirtualAlloc(void *, size_t, unsigned long, unsigned long);
static void *exec_mem(size_t n) { return VirtualAlloc(nullptr, n, 0x3000, 0x40); }
#else
#include <sys/mman.h>
static void *exec_mem(size_t n) {
    void *m = mmap(nullptr, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return m == MAP_FAILED ? nullptr : m;
}
#endif

namespace hx {

uint8_t *mem = nullptr;
std::vector<std::string> calls;
bool recording = true;
Glue glue_old, glue_new;
int failures = 0;

namespace {
constexpr size_t kPool = 1 << 20;
uint8_t *pool = nullptr;
size_t pool_used = 0;
std::vector<std::pair<uint64_t, uint64_t>> stubs;   // (offset, length) of every stub written into the image
std::vector<std::pair<uint64_t, void *>> retargets;   // (offset, host) of every retarget_call
}

long exec_left = -1;
uint64_t exec_fail_mask = 0;
int exec_calls = 0;
std::vector<uint64_t> fail_offsets;

bool fails_at(uint64_t off) {
    for (uint64_t o : fail_offsets)
        if (o == off) return true;
    return false;
}

std::vector<std::pair<uint64_t, uint64_t>> noexec;

bool executable(uint64_t off, uint64_t len) {
    for (const auto &[o, n] : noexec)
        if (off < o + n && o < off + len) return false;
    return true;
}

void note_retarget(uint64_t off, void *host) { retargets.push_back({off, host}); }

void *retarget_host(uint64_t off) {
    for (size_t i = retargets.size(); i-- > 0;)
        if (retargets[i].first == off) return retargets[i].second;
    return nullptr;
}

void init() {
    if (mem) return;
    // one block: the pool follows the image, so near stubs are in rel32 reach of it (as near_stub promises)
    mem = static_cast<uint8_t *>(exec_mem(kImageSize + kPool));
    pool = mem ? mem + kImageSize : nullptr;
    if (!mem || !pool) {
        std::fprintf(stderr, "harness: no executable memory\n");
        std::exit(2);
    }
}

void clear() {
    init();
    std::memset(mem, 0, kImageSize);
    std::memset(pool, 0, kPool);
    pool_used = 0;
    stubs.clear();
    retargets.clear();
    exec_left = -1;
    exec_fail_mask = 0;
    exec_calls = 0;
    fail_offsets.clear();
    noexec.clear();
    calls.clear();
    recording = true;
}

void stub(uint64_t off, void *host) {
    uint8_t *p = mem + off;
    const uint64_t h = uint64_t(uintptr_t(host));
    p[0] = 0x49, p[1] = 0xbb;   // movabs r11, host
    std::memcpy(p + 2, &h, 8);
    p[10] = 0x41, p[11] = 0xff, p[12] = 0xe3;   // jmp r11
    stubs.push_back({off, 13});
}

void stub_ptr(uint64_t slot_off, uint64_t stub_off, void *host) {
    stub(stub_off, host);
    put<uint64_t>(slot_off, addr(stub_off));
}

unsigned char *exec_alloc(size_t n) {
    n = (n + 15) & ~size_t(15);
    const int call = exec_calls++;
    if (call < 64 && ((exec_fail_mask >> call) & 1)) return nullptr;
    if (exec_left == 0) return nullptr;
    if (pool_used + n > kPool) return nullptr;
    if (exec_left > 0) exec_left--;
    unsigned char *p = pool + pool_used;
    pool_used += n;
    return p;
}

void rec(const char *format, ...) {
    if (!recording) return;
    char b[2048];
    va_list a;
    va_start(a, format);
    std::vsnprintf(b, sizeof b, format, a);
    va_end(a);
    calls.push_back(b);
}

std::string where(const void *p) {
    char b[64];
    const uintptr_t v = uintptr_t(p);
    if (!p) return "null";
    if (v >= uintptr_t(mem) && v < uintptr_t(mem) + kImageSize) {
        std::snprintf(b, sizeof b, "img+0x%llx", (unsigned long long)(v - uintptr_t(mem)));
        return b;
    }
    if (v >= uintptr_t(pool) && v < uintptr_t(pool) + kPool) return "pool";
    return "host";
}

std::vector<uint8_t> snapshot() {
    std::vector<uint8_t> s(mem, mem + kImageSize);
    for (const auto &[o, n] : stubs) std::memset(s.data() + o, 0, n);   // stubs hold host addresses: not compared
    return s;
}

void restore(const std::vector<uint8_t> &s) { std::memcpy(mem, s.data(), kImageSize); }

void bind_both() {
    if (glue_old.bind_image) glue_old.bind_image();
    if (glue_new.bind_image) glue_new.bind_image();
}

void set_image_both(uint8_t *base, uint64_t size) {
    if (glue_old.set_image) glue_old.set_image(base, size);
    if (glue_new.set_image) glue_new.set_image(base, size);
}

bool same(const char *scenario, const std::vector<std::string> &a, const std::vector<std::string> &b, const std::vector<uint8_t> *img_a,
          const std::vector<uint8_t> *img_b) {
    bool ok = true;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n && ok; i++)
        if (a[i] != b[i]) {
            std::printf("FAIL %s: call %zu\n  old: %s\n  new: %s\n", scenario, i, a[i].c_str(), b[i].c_str());
            ok = false;
        }
    if (ok && a.size() != b.size()) {
        std::printf("FAIL %s: %zu calls old, %zu new; first extra: %s\n", scenario, a.size(), b.size(),
                    (a.size() > n ? a[n] : b[n]).c_str());
        ok = false;
    }
    if (img_a && img_b) {
        for (size_t i = 0; i < kImageSize; i++)
            if ((*img_a)[i] != (*img_b)[i]) {
                std::printf("FAIL %s: image differs at +0x%zx (old %02x, new %02x)\n", scenario, i, (*img_a)[i], (*img_b)[i]);
                ok = false;
                break;
            }
    }
    if (!ok) failures++;
    return ok;
}

}  // namespace hx

extern "C" unsigned char *runtime_near_image_alloc(size_t bytes, int) { return hx::exec_alloc(bytes); }
