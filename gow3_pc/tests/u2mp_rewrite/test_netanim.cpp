// u2mp rewrite test "netanim": netanim.cpp and natives.cpp, old (git) against new (spec u2mp/docs/spec/netanim.md).
//
//   bash gow3_pc/tests/u2mp_rewrite/run.sh netanim netanim.cpp natives.cpp
//
// Every scenario runs twice from a cleared image (old code, then new code) and compares the record and the image.
// The stream call-outs are driven through the patched game sites with a register thunk (hx_run_site): all general
// registers and xmm0-15 get scenario values, the stack pointer points into a host buffer with a marked red zone,
// the thunk jumps to the site, and the resume addresses (0x638000 / 0x66c250) jump to hx_capture, which records
// the registers and flags the stub left. Generator slot 4 runs against a fake snapshot reader that calls the host
// visitor's slots from a script; every guest call is recorded with its float arguments as bits.
#include "harness.h"
#include "u2mp_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace u2mp_old {
void install_netanim(int &installed);
void install_natives(int &installed);
}  // namespace u2mp_old

#if defined(_WIN32)
extern "C" __declspec(dllimport) void *__stdcall VirtualAlloc(void *, size_t, unsigned long, unsigned long);
static void set_env(const char *name, const char *value) { _putenv_s(name, value ? value : ""); }
#else
static void *VirtualAlloc(void *, size_t, unsigned long, unsigned long) { return nullptr; }
static void set_env(const char *name, const char *value) {
    if (value) setenv(name, value, 1);
    else unsetenv(name);
}
#endif

using namespace hx;

// ---------------------------------------------------------------------------------------------------------------
// register thunk
extern "C" {
struct Regs {
    uint64_t g[16];   // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    uint8_t x[16][16];
    uint64_t flags;
};
Regs hx_in, hx_out;
void *hx_target;
uint64_t hx_host_rsp;
__attribute__((sysv_abi)) void hx_run_site();
void hx_capture();
}

asm(R"(
.text
.globl hx_run_site
hx_run_site:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, hx_host_rsp(%rip)
    movdqu hx_in+128(%rip), %xmm0
    movdqu hx_in+144(%rip), %xmm1
    movdqu hx_in+160(%rip), %xmm2
    movdqu hx_in+176(%rip), %xmm3
    movdqu hx_in+192(%rip), %xmm4
    movdqu hx_in+208(%rip), %xmm5
    movdqu hx_in+224(%rip), %xmm6
    movdqu hx_in+240(%rip), %xmm7
    movdqu hx_in+256(%rip), %xmm8
    movdqu hx_in+272(%rip), %xmm9
    movdqu hx_in+288(%rip), %xmm10
    movdqu hx_in+304(%rip), %xmm11
    movdqu hx_in+320(%rip), %xmm12
    movdqu hx_in+336(%rip), %xmm13
    movdqu hx_in+352(%rip), %xmm14
    movdqu hx_in+368(%rip), %xmm15
    mov hx_in+0(%rip), %rax
    mov hx_in+8(%rip), %rcx
    mov hx_in+16(%rip), %rdx
    mov hx_in+24(%rip), %rbx
    mov hx_in+40(%rip), %rbp
    mov hx_in+48(%rip), %rsi
    mov hx_in+56(%rip), %rdi
    mov hx_in+64(%rip), %r8
    mov hx_in+72(%rip), %r9
    mov hx_in+80(%rip), %r10
    mov hx_in+88(%rip), %r11
    mov hx_in+96(%rip), %r12
    mov hx_in+104(%rip), %r13
    mov hx_in+112(%rip), %r14
    mov hx_in+120(%rip), %r15
    mov hx_in+32(%rip), %rsp
    jmp *hx_target(%rip)
.globl hx_capture
hx_capture:
    mov %rax, hx_out+0(%rip)
    mov %rcx, hx_out+8(%rip)
    mov %rdx, hx_out+16(%rip)
    mov %rbx, hx_out+24(%rip)
    mov %rsp, hx_out+32(%rip)
    mov %rbp, hx_out+40(%rip)
    mov %rsi, hx_out+48(%rip)
    mov %rdi, hx_out+56(%rip)
    mov %r8, hx_out+64(%rip)
    mov %r9, hx_out+72(%rip)
    mov %r10, hx_out+80(%rip)
    mov %r11, hx_out+88(%rip)
    mov %r12, hx_out+96(%rip)
    mov %r13, hx_out+104(%rip)
    mov %r14, hx_out+112(%rip)
    mov %r15, hx_out+120(%rip)
    movdqu %xmm0, hx_out+128(%rip)
    movdqu %xmm1, hx_out+144(%rip)
    movdqu %xmm2, hx_out+160(%rip)
    movdqu %xmm3, hx_out+176(%rip)
    movdqu %xmm4, hx_out+192(%rip)
    movdqu %xmm5, hx_out+208(%rip)
    movdqu %xmm6, hx_out+224(%rip)
    movdqu %xmm7, hx_out+240(%rip)
    movdqu %xmm8, hx_out+256(%rip)
    movdqu %xmm9, hx_out+272(%rip)
    movdqu %xmm10, hx_out+288(%rip)
    movdqu %xmm11, hx_out+304(%rip)
    movdqu %xmm12, hx_out+320(%rip)
    movdqu %xmm13, hx_out+336(%rip)
    movdqu %xmm14, hx_out+352(%rip)
    movdqu %xmm15, hx_out+368(%rip)
    mov hx_host_rsp(%rip), %rsp
    pushfq
    pop %rax
    mov %rax, hx_out+384(%rip)
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
)");

static const char *kRegNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
alignas(64) static uint8_t g_gstack[0x8000];

// jump at `off` to hx_capture (jmp [rip+0]; no register touched)
static void capture_at(uint64_t off) {
    uint8_t *p = mem + off;
    p[0] = 0xff, p[1] = 0x25, p[2] = p[3] = p[4] = p[5] = 0;
    const uint64_t h = uint64_t(uintptr_t(&hx_capture));
    std::memcpy(p + 6, &h, 8);
}

// runs the game code at image `site` with `in` (rsp is set from `misalign`), records what the stub left
static void run_site(uint64_t site, Regs in, unsigned misalign) {
    for (size_t i = 0; i < sizeof g_gstack; i++) g_gstack[i] = uint8_t(0xa5 ^ i);
    const uint64_t rsp = uint64_t(uintptr_t(g_gstack + 0x6000)) - misalign;
    in.g[4] = rsp;
    hx_in = in;
    hx_target = mem + site;
    hx_run_site();
    std::string s = "regs";
    char b[128];
    for (int i = 0; i < 16; i++)
        if (hx_out.g[i] != in.g[i]) {
            std::snprintf(b, sizeof b, " %s:%llx->%llx", kRegNames[i], (unsigned long long)in.g[i], (unsigned long long)hx_out.g[i]);
            s += b;
        }
    for (int i = 0; i < 16; i++)
        if (std::memcmp(hx_out.x[i], in.x[i], 16) != 0) {
            std::snprintf(b, sizeof b, " xmm%d changed", i);
            s += b;
        }
    std::snprintf(b, sizeof b, " flags(OSZAP)=%03llx", (unsigned long long)(hx_out.flags & 0x8d4));
    s += b;
    bool red_ok = true;
    const uint8_t *r = reinterpret_cast<const uint8_t *>(uintptr_t(rsp));
    for (int i = -0x80; i < 0x100; i++) {
        const size_t idx = size_t(r + i - g_gstack);
        if (r[i] != uint8_t(0xa5 ^ idx)) red_ok = false;
    }
    s += red_ok ? " stack-ok" : " STACK-CLOBBERED";
    rec("%s", s.c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// scenarios
static bool g_old = true;
static int g_scen = 0, g_pass = 0, g_cases = 0;
static std::vector<std::string> g_failed;

// info: a scenario outside the game's contract (e.g. an invalid ABI value); a difference is reported, not counted
static void scenario(const std::string &name, const std::function<void()> &run, bool info = false) {
    std::vector<std::string> c[2];
    std::vector<uint8_t> im[2];
    for (int k = 0; k < 2; k++) {
        g_old = k == 0;
        clear();
        bind_both();
        run();
        c[k] = calls;
        im[k] = snapshot();
        bind_both();
    }
    const bool ok = same(name.c_str(), c[0], c[1], &im[0], &im[1]);
    if (!ok) {   // context: the records before the first difference
        size_t d = 0;
        while (d < c[0].size() && d < c[1].size() && c[0][d] == c[1][d]) d++;
        for (size_t i = d > 8 ? d - 8 : 0; i < d; i++) std::printf("  before: %s\n", c[0][i].c_str());
    }
    if (const char *show = std::getenv("HX_SHOW"); show && name.find(show) != std::string::npos)
        for (const auto &l : c[0]) std::printf("  | %s\n", l.c_str());
    if (info) {
        if (!ok) hx::failures--;
        std::printf("%s %s (%zu records)\n", ok ? "INFO-SAME" : "INFO-DIFF", name.c_str(), c[0].size());
        return;
    }
    ++g_scen;
    if (ok) ++g_pass;
    else g_failed.push_back(name);
    std::printf("%s %s (%zu records)\n", ok ? "PASS" : "FAIL", name.c_str(), c[0].size());
    std::fflush(stdout);
}

static void put_bytes(uint8_t *base, uint64_t off, std::initializer_list<uint8_t> b) {
    size_t i = 0;
    for (uint8_t x : b) base[off + i++] = x;
}
static std::string fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    char b[16];
    std::snprintf(b, sizeof b, "%08x", u);
    return b;
}
static float bitsf(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// ---------------------------------------------------------------------------------------------------------------
// netanim install
struct Check {
    uint64_t off;
    std::initializer_list<uint8_t> bytes;
};
static const Check kNetanimChecks[] = {
    // stream (0..5)
    {0x6364b5, {0x41, 0xff, 0xc2, 0x41, 0xff, 0xc6, 0xe9, 0x40, 0x1b, 0x00, 0x00}},
    {0x6363f6, {0x44, 0x3b, 0x5c, 0x24, 0x54, 0x0f, 0x84, 0xb4}},
    {0x63646c, {0x73, 0x47}},
    {0x66c035, {0x4c, 0x89, 0xb5, 0x78, 0xfe, 0xff, 0xff, 0x48, 0x8b, 0x85, 0x80, 0xfe, 0xff, 0xff,
                0xff, 0xc0, 0x48, 0x89, 0x85, 0x80, 0xfe, 0xff, 0xff, 0xe9, 0xff, 0x01, 0x00, 0x00}},
    {0x66bffb, {0x48, 0x85, 0xc0, 0x75, 0x35}},
    {0x66b430, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x88,
                0x01, 0x00, 0x00}},
    // slot (6..9)
    {0x5f48d0, {0xc3, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x391590, {0x48, 0x8b, 0x7b, 0x40, 0x48, 0x8b, 0x07, 0x48, 0x8b, 0x40, 0x20}},
    {0x4bcfd0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18,
                0x4d, 0x89, 0xce}},
    {0x66b270, {0xc5, 0xf8, 0x57, 0xc0, 0xc5, 0xf8, 0x29, 0x47, 0x10, 0xc5, 0xf8, 0x28, 0x05, 0xbf, 0xa1, 0x7c, 0x00,
                0xc5, 0xf8, 0x29, 0x47, 0x20, 0xc6, 0x47, 0x30, 0x00, 0xc3}},
};

// writes the 1.02 bytes; check `broken` gets byte `at` flipped
static void netanim_bytes(uint8_t *base, int broken = -1, size_t at = 0) {
    for (int i = 0; i < 10; i++) {
        put_bytes(base, kNetanimChecks[i].off, kNetanimChecks[i].bytes);
        if (i == broken) base[kNetanimChecks[i].off + at] ^= 0x01;
    }
}

static int install_netanim() {
    int n = 0;
    if (g_old) u2mp_old::install_netanim(n);
    else u2mp::install_netanim(n);
    rec("netanim installed %d, near_stub requests %d", n, exec_calls);
    return n;
}

// the jump written at a site, by meaning: target relative to the first stub request, and the filler
static void describe_site(uint8_t *base, uint64_t site, unsigned len, uint8_t *pool0) {
    const uint8_t *p = base + site;
    std::string s;
    char b[96];
    if (p[0] == 0xe9) {
        int32_t rel;
        std::memcpy(&rel, p + 1, 4);
        const int64_t target = int64_t(uintptr_t(p + 5)) + rel;
        std::snprintf(b, sizeof b, "site 0x%llx: jmp pool%+lld", (unsigned long long)site,
                      (long long)(target - int64_t(uintptr_t(pool0))));
        s = b;
        for (unsigned i = 5; i < len; i++) {
            std::snprintf(b, sizeof b, " %02x", p[i]);
            s += b;
        }
    } else {
        std::snprintf(b, sizeof b, "site 0x%llx: not patched (%02x)", (unsigned long long)site, p[0]);
        s = b;
    }
    rec("%s", s.c_str());
}

static uint8_t *pool_next() {   // the address the next near_stub request gets (does not count as a request)
    uint8_t *p = exec_alloc(0);
    exec_calls = 0;
    return p;
}

static void install_scenarios() {
    // N1 env switch
    const char *envs[] = {"0", "00", "1", nullptr, ""};
    for (const char *e : envs) {
        scenario(std::string("N1 env ") + (e ? (std::string("\"") + e + "\"") : "unset"), [&] {
            set_env("UNCHARTED_U2MP_NETANIM", e);
            netanim_bytes(mem);
            uint8_t *p0 = pool_next();
            install_netanim();
            describe_site(mem, 0x6364b5, 11, p0);
            describe_site(mem, 0x66c035, 28, p0);
            set_env("UNCHARTED_U2MP_NETANIM", nullptr);
        });
    }
    // N2 / N3: each check broken in turn (first and last byte)
    for (int i = 0; i < 10; i++)
        for (int which = 0; which < 2; which++) {
            const size_t at = which ? kNetanimChecks[i].bytes.size() - 1 : 0;
            char name[96];
            std::snprintf(name, sizeof name, "%s check %d (0x%llx) byte %zu broken", i < 6 ? "N2 stream" : "N3 slot", i,
                          (unsigned long long)kNetanimChecks[i].off, at);
            scenario(name, [&] {
                netanim_bytes(mem, i, at);
                uint8_t *p0 = pool_next();
                install_netanim();
                describe_site(mem, 0x6364b5, 11, p0);
                describe_site(mem, 0x66c035, 28, p0);
            });
        }
    // N3b: check whose bytes lie at the very end of a too small image
    scenario("N3 image ends inside the slot 4 bytes", [&] {
        netanim_bytes(mem);
        set_image_both(mem, 0x66c035 + 28);   // stream checks fit, all slot checks fit too (0x66b270+27 < size)
        uint8_t *p0 = pool_next();
        install_netanim();
        describe_site(mem, 0x6364b5, 11, p0);
        set_image_both(mem, 0x66c035 + 27);   // reader site no longer fits
        netanim_bytes(mem);
        install_netanim();
    });
    // N4 near_stub NULL
    for (uint64_t mask : {1ull, 2ull, 3ull}) {
        char name[64];
        std::snprintf(name, sizeof name, "N4 near_stub fails mask %llu", (unsigned long long)mask);
        scenario(name, [&] {
            netanim_bytes(mem);
            uint8_t *p0 = pool_next();
            exec_fail_mask = mask;
            install_netanim();
            describe_site(mem, 0x6364b5, 11, p0);
            describe_site(mem, 0x66c035, 28, p0);
        });
    }
    // N5 everything good
    scenario("N5 full install", [&] {
        netanim_bytes(mem);
        uint8_t *p0 = pool_next();
        install_netanim();
        describe_site(mem, 0x6364b5, 11, p0);
        describe_site(mem, 0x66c035, 28, p0);
        rec("slot4 replaced: %d", calls.size() > 0);
    });
    // replace_function failing for slot 4
    scenario("N5 replace_function fails", [&] {
        netanim_bytes(mem);
        fail_offsets.push_back(0x5f48d0);
        install_netanim();
    });
    // N6 jump range (far image): writer out of range; reader out of range with the writer in range
    static uint8_t *far = nullptr;
    static uint8_t *far_base = nullptr;
    static int64_t far_lo = 0;
    {
        clear();
        bind_both();
        uint8_t *p0 = pool_next();
        // B such that writer delta = -2^31 + 0x10000 (reader out) and B2 = B + 0x20000 (writer delta < -2^31)
        const int64_t b1 = int64_t(uintptr_t(p0)) - 0x6364ba + 0x80000000ll - 0x10000;
        far_lo = b1 & ~int64_t(0xffff);
        far = static_cast<uint8_t *>(VirtualAlloc(reinterpret_cast<void *>(far_lo), 0x6c0000, 0x3000, 0x04));
        far_base = far ? reinterpret_cast<uint8_t *>(b1) : nullptr;
    }
    if (!far) {
        std::printf("SKIP N6 jump range (no memory at the far address)\n");
    } else {
        for (int which = 0; which < 3; which++) {
            const char *names[] = {"N6 reader stub out of jump range", "N6 writer stub out of jump range",
                                   "N6 writer stub exactly at -2^31"};
            scenario(names[which], [&] {
                uint8_t *p0 = pool_next();
                uint8_t *b = which == 0 ? far_base : which == 1 ? far_base + 0x20000 : nullptr;
                if (which == 2) b = reinterpret_cast<uint8_t *>(int64_t(uintptr_t(p0)) - 0x6364ba + 0x80000000ll);
                std::memset(far, 0, 0x6c0000);
                netanim_bytes(b);
                set_image_both(b, 0x680000);
                install_netanim();
                describe_site(b, 0x6364b5, 11, p0);
                describe_site(b, 0x66c035, 28, p0);
                rec("far bytes writer %02x reader %02x", b[0x6364b5], b[0x66c035]);
            });
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// stream call-outs
constexpr uint64_t CMD = 0x1400003, ANIM = 0x1410000, INFO = 0x1420000, STREAM = 0x1430000, WORDS = 0x1440000,
                   FRAME = 0x1450000, VIS = 0x1460000, VIS_VT = 0x1460100, VIS_SLOT3 = 0x1460200, RANIM = 0x1470005;

U2MP_GUEST void fake_visit3(const uint8_t *v, const uint8_t *anim, uint32_t counter, float phase) {
    rec("visitor slot3 v=%s anim=%s counter=%08x phase=%s", where(v).c_str(), where(anim).c_str(), counter,
        fbits(phase).c_str());
}

static void stream_install() {
    netanim_bytes(mem);
    install_netanim();
    capture_at(0x638000);
    capture_at(0x66c250);
    stub_ptr(VIS_VT + 0x18, VIS_SLOT3, reinterpret_cast<void *>(&fake_visit3));
    put<uint64_t>(VIS, addr(VIS_VT));
}

static Regs random_regs(std::mt19937_64 &r) {
    Regs g{};
    for (auto &x : g.g) x = r();
    for (auto &x : g.x)
        for (auto &y : x) y = uint8_t(r());
    return g;
}

struct WCase {
    int anim_mode = 0;   // 0 anim+info, 1 anim null, 2 info null
    uint16_t frames = 30;
    float frame = 0;
    uint32_t pos = 0, limit = 1024;
    uint32_t w0 = 0, w1 = 0;   // words at pos/32 and pos/32+1
    uint64_t r10 = 5, r14 = 9;
    bool far_words = false;   // pos near 2^32: words pointer biased so pos/32 lands in the image
};

static void writer_case(const WCase &c, std::mt19937_64 &r, unsigned misalign) {
    put<uint64_t>(CMD + 4, c.anim_mode == 1 ? 0 : addr(ANIM));
    put<float>(CMD + 0x10, c.frame);
    put<uint64_t>(ANIM + 0x28, c.anim_mode == 2 ? 0 : addr(INFO));
    put<uint16_t>(INFO + 0x22, c.frames);
    const uint64_t idx = c.pos / 32;
    const uint64_t words = c.far_words ? addr(WORDS) - idx * 4 : addr(WORDS);
    put<uint64_t>(STREAM, words);
    put<uint32_t>(STREAM + 8, c.pos);
    put<uint32_t>(STREAM + 0xc, c.limit);
    const uint64_t woff = c.far_words ? WORDS : WORDS + idx * 4;
    put<uint32_t>(woff, c.w0);
    put<uint32_t>(woff + 4, c.w1);
    put<uint32_t>(woff + 8, 0x5a5a5a5a);
    Regs in = random_regs(r);
    in.g[12] = addr(CMD);
    in.g[15] = addr(STREAM);
    in.g[10] = c.r10;
    in.g[14] = c.r14;
    run_site(0x6364b5, in, misalign);
    rec("writer frames=%u frame=%s pos=%u limit=%u -> pos=%u words %08x %08x %08x", c.frames, fbits(c.frame).c_str(),
        c.pos, c.limit, get<uint32_t>(STREAM + 8), get<uint32_t>(woff), get<uint32_t>(woff + 4), get<uint32_t>(woff + 8));
    ++g_cases;
}

struct RCase {
    int anim_mode = 0;   // 0 anim+info, 1 anim null, 2 info null
    bool visitor = true;
    uint16_t frames = 30;
    uint32_t pos = 0, limit = 1024;
    uint32_t w0 = 0, w1 = 0;
    uint64_t counter = 0x1234567800000007ull;
    uint64_t r14 = 0x1111222233334444ull;
    bool far_words = false;
};

static void reader_case(const RCase &c, std::mt19937_64 &r, unsigned misalign) {
    put<uint64_t>(RANIM + 0x28, c.anim_mode == 2 ? 0 : addr(INFO));
    put<uint16_t>(INFO + 0x22, c.frames);
    const uint64_t idx = c.pos / 32;
    const uint64_t words = c.far_words ? addr(WORDS) - idx * 4 : addr(WORDS);
    const uint64_t woff = c.far_words ? WORDS : WORDS + idx * 4;
    put<uint32_t>(woff, c.w0);
    put<uint32_t>(woff + 4, c.w1);
    put<uint64_t>(FRAME - 0x150, words);
    put<uint32_t>(FRAME - 0x148, c.pos);
    put<uint32_t>(FRAME - 0x144, c.limit);
    put<uint64_t>(FRAME - 0x190, c.visitor ? addr(VIS) : 0);
    put<uint64_t>(FRAME - 0x180, c.counter);
    put<uint64_t>(FRAME - 0x188, 0xdddddddddddddddd);
    Regs in = random_regs(r);
    in.g[0] = c.anim_mode == 1 ? 0 : addr(RANIM);
    in.g[5] = addr(FRAME);
    in.g[14] = c.r14;
    run_site(0x66c035, in, misalign);
    rec("reader frames=%u pos=%u limit=%u w=%08x %08x -> pos=%u counter=%016llx r14slot=%016llx", c.frames, c.pos, c.limit,
        c.w0, c.w1, get<uint32_t>(FRAME - 0x148), (unsigned long long)get<uint64_t>(FRAME - 0x180),
        (unsigned long long)get<uint64_t>(FRAME - 0x188));
    ++g_cases;
}

static float special_float(std::mt19937_64 &r, float frames) {
    static const uint32_t bits[] = {0x00000000, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fa00001,
                                    0x7f7fffff, 0xff7fffff, 0x00000001, 0x4f000000, 0xcf000000, 0x4effffff, 0x3f800000,
                                    0xbf800000, 0x3effffff, 0x5f000000};
    switch (r() % 6) {
    case 0: return bitsf(bits[r() % (sizeof bits / 4)]);
    case 1: return bitsf(uint32_t(r()));
    case 2: return frames - 1.0f;
    case 3: return float(int(r() % 200) - 50) * 0.37f;
    default: return float(double(r() % 100000) / 100000.0 * double(frames + 2.0f) - 1.0);
    }
}

static uint16_t random_frames(std::mt19937_64 &r) {
    static const uint16_t f[] = {0, 1, 2, 7, 8, 9, 30, 63, 64, 65, 200, 1000, 65535};
    return r() % 4 == 0 ? uint16_t(r()) : f[r() % (sizeof f / 2)];
}

static unsigned width_of(int mode, uint16_t frames) {
    if (mode != 0) return 8;
    return frames <= 8 ? 4 : frames > 64 ? 16 : 8;
}

static void stream_scenarios() {
    // W4 / R3: ten overflows across writer and reader: 8 log lines (first: the cap is process wide)
    scenario("W4/R3 overflow cap: 10 overflows, 8 lines", [&] {
        stream_install();
        std::mt19937_64 r(11);
        for (int i = 0; i < 10; i++) {
            if (i % 2 == 0) {
                WCase c;
                c.frames = 30, c.frame = 3.0f, c.pos = 100, c.limit = 100 + 7;   // n = 8
                writer_case(c, r, 0);
            } else {
                RCase c;
                c.frames = 100, c.pos = 40, c.limit = 40 + 15;   // n = 16
                reader_case(c, r, 8);
            }
        }
    });
    // W1 / W2 / W3 / W4: explicit writer cases
    scenario("W1-W4 writer explicit", [&] {
        stream_install();
        std::mt19937_64 r(12);
        const uint16_t frames[] = {8, 9, 64, 65, 0, 1};
        for (uint16_t fr : frames) {
            const float vals[] = {0.0f, float(fr) / 2.0f, float(fr) - 1.0f, float(fr) + 5.0f, -3.0f, NAN, -NAN, INFINITY,
                                  -INFINITY, 3e38f, -3e38f, -0.0f, 1e-40f};
            for (float v : vals) {
                WCase c;
                c.frames = fr, c.frame = v, c.pos = 37, c.limit = 4096, c.w0 = 0xffffffff, c.w1 = 0xffffffff;
                writer_case(c, r, 0);
            }
        }
        for (int mode = 1; mode <= 2; mode++) {   // W2: 8 zero bits
            WCase c;
            c.anim_mode = mode, c.pos = 3, c.w0 = 0xffffffff, c.w1 = 0x12345678;
            writer_case(c, r, 8);
        }
        const uint32_t offs[] = {0, 1, 28, 31, 32, 63, 64 + 24, 64 + 25};
        for (uint16_t fr : {uint16_t(8), uint16_t(30), uint16_t(100)})
            for (uint32_t o : offs)
                for (uint32_t pre : {0u, 0xffffffffu, 0xa5a5a5a5u}) {   // W3: OR-without-clear at offset 0
                    WCase c;
                    c.frames = fr, c.frame = float(fr - 1) * 0.6f, c.pos = o, c.limit = 1000, c.w0 = pre, c.w1 = ~pre;
                    writer_case(c, r, o & 8);
                }
        for (uint32_t extra : {0u, 1u}) {   // W4: pos + n == limit (written) / == limit + 1
            WCase c;
            c.frames = 100, c.frame = 50.0f, c.pos = 200, c.limit = 216 - extra, c.w0 = 0x0f0f0f0f;
            writer_case(c, r, 0);
        }
        {   // no 32-bit wrap in the room check
            WCase c;
            c.frames = 30, c.frame = 1.0f, c.pos = 0xfffffffc, c.limit = 0xffffffff, c.far_words = true;
            writer_case(c, r, 0);
            c.pos = 0xfffffff0, c.far_words = true;   // fits: written at word 0x7ffffff
            writer_case(c, r, 0);
        }
        {   // inc r10d / r14d wrap and upper halves
            WCase c;
            c.r10 = 0xffffffffffffffffull, c.r14 = 0x00000001ffffffffull;
            writer_case(c, r, 0);
            c.r10 = 0x7fffffff, c.r14 = 0x8000000000000000ull;
            writer_case(c, r, 8);
        }
    });
    scenario("R1-R4 reader explicit", [&] {
        stream_install();
        std::mt19937_64 r(13);
        const uint32_t offs[] = {0, 1, 16, 24, 28, 31, 32, 63};
        for (uint16_t fr : {uint16_t(8), uint16_t(9), uint16_t(64), uint16_t(65), uint16_t(0), uint16_t(1)})
            for (uint32_t o : offs)
                for (uint32_t w : {0u, 0xffffffffu, 0x80000001u, 0x12345678u}) {
                    RCase c;
                    c.frames = fr, c.pos = o, c.limit = 1000, c.w0 = w, c.w1 = ~w ^ 0x01020304;
                    reader_case(c, r, o & 8);
                }
        for (int mode = 1; mode <= 2; mode++) {
            RCase c;
            c.anim_mode = mode, c.pos = 5, c.w0 = 0x0ff00000;
            reader_case(c, r, 0);
        }
        {   // R2 visitor NULL
            RCase c;
            c.visitor = false, c.pos = 9, c.w0 = 0x00ff0000;
            reader_case(c, r, 0);
        }
        for (uint32_t extra : {0u, 1u}) {   // room
            RCase c;
            c.frames = 100, c.pos = 200, c.limit = 216 - extra, c.w0 = 0xffff0000;
            reader_case(c, r, 0);
        }
        {
            RCase c;
            c.frames = 30, c.pos = 0xfffffffc, c.limit = 0xffffffff, c.far_words = true;
            reader_case(c, r, 0);
            c.pos = 0xfffffff0;
            reader_case(c, r, 0);
        }
        for (uint64_t cnt : {0ull, 0xffffffffull, 0xffffffff7fffffffull, 0x00000001fffffffeull}) {   // R4
            RCase c;
            c.counter = cnt, c.r14 = cnt ^ 0x5555555555555555ull, c.pos = 2, c.w0 = 0x30000000;
            reader_case(c, r, 8);
        }
    });
    // random batches
    for (int batch = 0; batch < 8; batch++) {
        scenario("W random batch " + std::to_string(batch) + " (300 cases)", [&] {
            stream_install();
            std::mt19937_64 r(1000 + batch);
            for (int i = 0; i < 300; i++) {
                WCase c;
                c.anim_mode = r() % 20 == 0 ? 1 + int(r() % 2) : 0;
                c.frames = random_frames(r);
                c.frame = special_float(r, float(c.frames));
                const unsigned n = width_of(c.anim_mode, c.frames);
                c.pos = uint32_t(r() % 2048);
                if (r() % 3 == 0) c.pos = (c.pos & ~31u) | uint32_t(32 - r() % 17);
                if (r() % 8 == 0) c.pos &= ~31u;
                const int choice = int(r() % 10);
                c.limit = choice == 0 ? c.pos + n - 1 : choice == 1 ? c.pos + n : uint32_t(c.pos + n + r() % 100);
                c.w0 = uint32_t(r()), c.w1 = uint32_t(r());
                if (r() % 4 == 0) c.w0 = r() % 2 ? 0xffffffff : 0;
                c.r10 = r(), c.r14 = r();
                writer_case(c, r, unsigned(r() % 2) * 8);
            }
        });
        scenario("R random batch " + std::to_string(batch) + " (300 cases)", [&] {
            stream_install();
            std::mt19937_64 r(2000 + batch);
            for (int i = 0; i < 300; i++) {
                RCase c;
                c.anim_mode = r() % 20 == 0 ? 1 + int(r() % 2) : 0;
                c.visitor = r() % 20 != 0;
                c.frames = random_frames(r);
                const unsigned n = width_of(c.anim_mode, c.frames);
                c.pos = uint32_t(r() % 2048);
                if (r() % 3 == 0) c.pos = (c.pos & ~31u) | uint32_t(32 - r() % 17);
                const int choice = int(r() % 10);
                c.limit = choice == 0 ? c.pos + n - 1 : choice == 1 ? c.pos + n : uint32_t(c.pos + n + r() % 100);
                c.w0 = uint32_t(r()), c.w1 = uint32_t(r());
                c.counter = r(), c.r14 = r();
                reader_case(c, r, unsigned(r() % 2) * 8);
            }
        });
    }
    // round trip: what the writer writes, the reader reads (both through the stubs)
    scenario("W+R round trip (200 cases)", [&] {
        stream_install();
        std::mt19937_64 r(77);
        for (int i = 0; i < 200; i++) {
            WCase w;
            w.frames = random_frames(r);
            w.frame = special_float(r, float(w.frames));
            w.pos = uint32_t(r() % 512), w.limit = 4096, w.w0 = uint32_t(r()), w.w1 = uint32_t(r());
            writer_case(w, r, 0);
            RCase c;
            c.frames = w.frames, c.pos = w.pos, c.limit = 4096;
            c.w0 = get<uint32_t>(WORDS + (w.pos / 32) * 4), c.w1 = get<uint32_t>(WORDS + (w.pos / 32) * 4 + 4);
            reader_case(c, r, 8);
        }
    });
}

// ---------------------------------------------------------------------------------------------------------------
// generator slot 4
constexpr uint64_t ANIMS = 0x1500000, TABLES = 0x1580000, EVENTS = 0x1590000, GENS = 0x1600000;
constexpr uint64_t S_READ = 0x15a0000, S_FIRE = 0x15a0100, S_RESET = 0x15a0200;

struct Act {
    int slot;          // 0..6
    int anim;          // slot 3: anim index, -1 = null
    uint32_t counter;  // slot 3 counter / slot 4 argument
    float phase;
};
static Act V(int anim, float phase, uint32_t counter = 0) { return {3, anim, counter, phase}; }
static Act Rst() { return {0, 0, 0, 0}; }
static Act Mk(uint32_t x = 0) { return {4, 0, x, 0}; }
static Act Emp(int slot) { return {slot, 0, 0, 0}; }

static std::vector<Act> g_script[2];
static const void *g_first_vt = nullptr;

static std::string vdump(const uint8_t *v) {
    std::string s;
    char b[96];
    for (int i = 8; i < 0x38; i++) {
        std::snprintf(b, sizeof b, "%02x", v[i]);
        s += b;
    }
    std::snprintf(b, sizeof b, " last=%08x list", *reinterpret_cast<const uint32_t *>(v + 0x40));
    s += b;
    const uint8_t *L;
    std::memcpy(&L, v + 0x38, 8);
    if (!L) return s + " null";
    uint32_t n;
    std::memcpy(&n, L + 384, 4);
    std::snprintf(b, sizeof b, " n=%u:", n);
    s += b;
    for (uint32_t i = 0; i < n && i < 16; i++) {
        const uint8_t *e = L + i * 24;
        uint32_t key, ph;
        const uint8_t *an;
        std::memcpy(&key, e, 4);
        std::memcpy(&an, e + 8, 8);
        std::memcpy(&ph, e + 16, 4);
        std::snprintf(b, sizeof b, " {%x %s %08x %u %u}", key, where(an).c_str(), ph, e[20], e[21]);
        s += b;
    }
    return s;
}

U2MP_GUEST void fake_read(uint8_t *v, const uint8_t *snap, const uint8_t *anims, uint32_t a, uint32_t b, uint32_t w) {
    const void *vt;
    std::memcpy(&vt, v, 8);
    if (!g_first_vt) g_first_vt = vt;
    rec("read v%%16=%u snap=%s anims=%s a=%u b=%u w=%08x vt=%s | %s", unsigned(uintptr_t(v) & 15), where(snap).c_str(),
        where(anims).c_str(), a, b, w, vt == g_first_vt ? "same" : "OTHER", vdump(v).c_str());
    for (const Act &x : g_script[a & 1]) {
        void *const *t = static_cast<void *const *>(vt);
        switch (x.slot) {
        case 0: reinterpret_cast<void(U2MP_GUEST *)(void *)>(t[0])(v); break;
        case 3:
            reinterpret_cast<void(U2MP_GUEST *)(void *, const void *, uint32_t, float)>(t[3])(
                v, x.anim < 0 ? nullptr : mem + ANIMS + uint64_t(x.anim) * 0x200, x.counter, x.phase);
            break;
        case 4: reinterpret_cast<void(U2MP_GUEST *)(void *, uint32_t)>(t[4])(v, x.counter); break;
        default: reinterpret_cast<void(U2MP_GUEST *)(void *, uint64_t, uint64_t)>(t[x.slot])(v, 0x77, 0x88); break;
        }
        rec("  slot%d anim=%d phase=%s -> %s", x.slot, x.anim, fbits(x.phase).c_str(), vdump(v).c_str());
    }
}

U2MP_GUEST void fake_fire(const uint8_t *table, uint32_t fwd, uint32_t loop, uint32_t z, uint32_t one, void *ev, float from,
                          float to, float dur) {
    rec("fire table=%s fwd=%u loop=%u %u %u ev=%s from=%s to=%s dur=%s", where(table).c_str(), fwd, loop, z, one,
        where(ev).c_str(), fbits(from).c_str(), fbits(to).c_str(), fbits(dur).c_str());
}

U2MP_GUEST void fake_vreset(uint8_t *v) {
    rec("visitor base reset v%%16=%u", unsigned(uintptr_t(v) & 15));
    std::memset(v + 0x10, 0, 16);
    for (int i = 0; i < 16; i++) v[0x20 + i] = uint8_t(0x40 + i);
    v[0x30] = 0;
}

struct AnimDef {
    uint32_t key = 0x100;
    uint32_t flags = 0;
    uint16_t frames = 31;
    float rate = 1.0f / 30.0f;
    bool no_info = false, no_table = false;
    uint32_t tcount = 2;
};
static void make_anim(int i, const AnimDef &d) {
    const uint64_t a = ANIMS + uint64_t(i) * 0x200, info = a + 0x100, table = TABLES + uint64_t(i) * 0x20;
    put<uint32_t>(a + 0x24, d.flags);
    put<uint64_t>(a + 0x28, d.no_info ? 0 : addr(info));
    put<uint64_t>(a + 0x48, d.no_table ? 0 : addr(table));
    put<uint16_t>(info + 0x22, d.frames);
    put<float>(info + 0x2c, d.rate);
    put<uint32_t>(table, d.key);
    put<uint32_t>(table + 4, d.tcount);
}

struct Frame {
    uint32_t count = 3;
    std::vector<float> t = {0.3f, 0.2f, 0.1f};
    float now = 0.25f;
    std::vector<Act> newer, older;   // scripts of the first read (snapshot i, a=0) and the second (i+1, a=1)
    bool ev = true;
    uint32_t word = 0xabcd0001;
};

static void slot_install() {
    netanim_bytes(mem);
    install_netanim();
    stub(0x66b430, reinterpret_cast<void *>(&fake_read));
    stub(0x4bcfd0, reinterpret_cast<void *>(&fake_fire));
    stub(0x66b270, reinterpret_cast<void *>(&fake_vreset));
}

static uint64_t g_gen_next = 0;
static uint64_t new_gen() {   // a generator address no earlier scenario used (lists live for the whole process)
    const uint64_t g = GENS + (g_gen_next++ % 0x180) * 0x1000;
    return g;
}

static void run_frame(uint64_t G, const Frame &f) {
    put<uint32_t>(G + 0x24, f.word);
    put<uint32_t>(G + 0x68, f.count);
    for (size_t i = 0; i < f.t.size(); i++) put<float>(G + 0x70 + i * 0x90 + 4, f.t[i]);
    put<float>(G + 0x960, f.now);
    g_script[0] = f.newer;
    g_script[1] = f.older;
    g_first_vt = nullptr;
    rec("slot4 count=%u now=%s ev=%d", f.count, fbits(f.now).c_str(), int(f.ev));
    reinterpret_cast<void(U2MP_GUEST *)(const uint8_t *, void *)>(mem + 0x5f48d0)(mem + G, f.ev ? mem + EVENTS : nullptr);
    ++g_cases;
}

// a scenario: anims, then frames on one fresh generator (the generator address is chosen once for both runs)
static void slot_scenario(const std::string &name, const std::vector<AnimDef> &anims, const std::vector<Frame> &frames) {
    const uint64_t G = new_gen();
    scenario(name, [&] {
        slot_install();
        for (size_t i = 0; i < anims.size(); i++) make_anim(int(i), anims[i]);
        for (const Frame &f : frames) run_frame(G, f);
    });
}

static Frame fr(std::vector<Act> newer, std::vector<Act> older, bool ev = true) {
    Frame f;
    f.newer = std::move(newer);
    f.older = std::move(older);
    f.ev = ev;
    return f;
}

static void slot_scenarios() {
    AnimDef A, B, C, Lp;
    A.key = 0xa1, B.key = 0xb2, C.key = 0xc3, Lp.key = 0xd4, Lp.flags = 1;
    // S9 first: the first 3 calls of the process log (both generators)
    {
        const uint64_t G1 = new_gen(), G2 = new_gen();
        scenario("S9 two generators, call log 3 lines then silent", [&] {
            slot_install();
            make_anim(0, A);
            make_anim(1, Lp);
            Frame f1 = fr({V(0, 0.4f)}, {V(0, 0.2f)});
            Frame f2 = fr({V(1, 0.6f)}, {V(1, 0.5f)});
            run_frame(G1, f1);   // rebuild on G1
            run_frame(G2, f2);   // rebuild on G2
            run_frame(G1, fr({V(0, 0.4f)}, {V(0, 0.3f)}));   // case A on G1 (fires)
            run_frame(G2, fr({V(1, 0.6f)}, {V(1, 0.55f)}));   // case A on G2
            run_frame(G1, fr({V(0, 0.5f)}, {V(0, 0.45f)}));
        });
    }
    // S1
    {
        Frame c0 = fr({V(0, 0.4f)}, {V(0, 0.2f)});
        std::vector<Frame> fs;
        for (uint32_t n : {0u, 1u, 2u}) {
            Frame f = c0;
            f.count = n;
            fs.push_back(f);
        }
        fs.push_back(c0);                                   // rebuild
        fs.push_back(fr({V(0, 0.4f)}, {V(0, 0.3f)}));     // advance (fires)
        Frame drop = c0;
        drop.count = 2;
        fs.push_back(drop);                                 // cleared
        fs.push_back(fr({V(0, 0.45f)}, {V(0, 0.35f)}));   // case B again, no fire
        fs.push_back(fr({V(0, 0.5f)}, {V(0, 0.4f)}));     // case A, fires
        slot_scenario("S1 count 0/1/2, drop below 3 clears", {A}, fs);
    }
    // S2 pair search
    {
        std::vector<Frame> fs;
        auto add = [&](uint32_t count, std::vector<float> t, float now) {
            Frame f = fr({V(0, 0.4f)}, {V(0, 0.2f)});
            f.count = count, f.t = t, f.now = now;
            fs.push_back(f);
            Frame g = fr({V(0, 0.5f)}, {V(0, 0.3f)});   // case A with dt from this pair
            g.count = count, g.t = t, g.now = now;
            fs.push_back(g);
            Frame h = fr({V(1, 0.5f)}, {V(2, 0.3f)});   // reset the list (case C)
            h.count = 3;
            fs.push_back(h);
        };
        add(3, {0.3f, 0.2f, 0.1f}, 0.35f);              // t0 < time at i=0
        add(3, {0.3f, 0.2f, 0.1f}, 0.25f);              // t1 < time, frac
        add(4, {0.4f, 0.3f, 0.2f, 0.1f}, 0.17f);        // pair 2
        add(4, {0.4f, 0.0f, 0.2f, 0.1f}, 0.15f);        // t1 == 0 skipped
        add(4, {0.4f, 0.3f, 0.0f, 0.0f}, 0.05f);        // none found
        add(3, {0.3f, 0.3f, 0.1f}, 0.3f);               // equal times
        add(3, {0.3f, -0.0f, 0.1f}, 0.2f);              // -0 is 0
        add(3, {NAN, 0.2f, 0.1f}, 0.25f);               // NaN times
        add(3, {0.3f, 0.2f, 0.1f}, NAN);
        add(5, {1.0f, 0.9f, 0.8f, 0.7f, 0.6f}, 0.83f);
        add(20, {0.3f, 0.2f, 0.1f}, 0.01f);             // count past the array (reads into the generator)
        AnimDef a = A, b = B, c = C;
        slot_scenario("S2 snapshot pair search", {a, b, c}, fs);
    }
    // S3 events NULL
    slot_scenario("S3 events NULL keeps the list",
                  {A, B},
                  {fr({V(0, 0.4f)}, {V(0, 0.2f)}), fr({V(1, 0.4f)}, {V(1, 0.2f)}, false), fr({V(0, 0.5f)}, {V(0, 0.3f)}),
                   fr({V(1, 0.4f)}, {V(1, 0.2f)}, false), fr({V(1, 0.4f)}, {V(1, 0.2f)})});
    // S4 rebuild variants, each followed by an advance that shows the rebuilt entry
    {
        std::vector<Frame> fs;
        const float pairs[][2] = {{0.2f, 0.4f}, {0.4f, 0.2f}, {0.3f, 0.3f}, {0.95f, 0.05f}, {0.05f, 0.95f}, {0.1f, 0.0f},
                                  {0.0f, 0.1f},  {0.14f, 0.5f}, {0.15f, 0.5f}, {0.16f, 0.5f}, {0.84f, 0.5f}, {0.85f, 0.5f},
                                  {0.86f, 0.5f}, {NAN, 0.5f},   {0.5f, NAN},   {1.5f, 0.2f},  {-0.2f, 0.3f}, {0.25f, 0.15f},
                                  {0.2f, 0.1f},  {0.2000001f, 0.1f}};
        for (int loop = 0; loop < 2; loop++)
            for (auto &p : pairs) {
                const int an = loop ? 1 : 0;
                fs.push_back(fr({V(an, p[1])}, {V(an, p[0])}));                 // rebuild (a = older, b = newer)
                fs.push_back(fr({V(2, 0.5f)}, {V(an, p[0])}));                  // advance main: reveals dir / phase
                fs.push_back(fr({V(2, 0.5f)}, {V(an, p[0] + 0.3f)}));
                fs.push_back(fr({V(3, 0.5f)}, {V(3, 0.3f)}));                   // reset
                fs.push_back(fr({V(3, 0.5f)}, {V(2, 0.3f)}));
            }
        AnimDef a = A, l = Lp, c = C, d = B;
        a.frames = 61, l.frames = 61;
        slot_scenario("S4 rebuild looping / non-looping thresholds", {a, l, c, d}, fs);
    }
    // S5 advance main
    {
        AnimDef a = A, b = B, one = A, zero = A, l = Lp;
        one.frames = 1, zero.frames = 0, one.key = 0xe1, zero.key = 0xe2;
        std::vector<Frame> fs;
        // forward entry, newer keys differ
        fs.push_back(fr({V(0, 0.4f)}, {V(0, 0.2f)}));
        for (float p : {0.25f, 0.3f, 0.9f, 0.99f, 1.0f, 0.5f})
            fs.push_back(fr({V(1, 0.5f)}, {V(0, p)}));
        // backward entry
        fs.push_back(fr({V(1, 0.5f)}, {V(1, 0.1f)}));   // reset
        fs.push_back(fr({V(0, 0.2f)}, {V(0, 0.8f)}));   // rebuild backward
        for (float p : {0.75f, 0.6f, 0.1f, 0.01f, 0.0f, 0.3f})
            fs.push_back(fr({V(1, 0.5f)}, {V(0, p)}));
        // duplicate keys: two entries with the same key (the second does not fire)
        fs.push_back(fr({V(1, 0.5f)}, {V(4, 0.1f)}));
        fs.push_back(fr({V(0, 0.4f), V(0, 0.6f)}, {V(0, 0.2f), V(0, 0.3f)}));
        fs.push_back(fr({V(1, 0.5f), V(1, 0.5f)}, {V(0, 0.3f), V(0, 0.4f)}));
        // frames 1 and 0 (NaN path)
        fs.push_back(fr({V(1, 0.5f)}, {V(4, 0.1f)}));
        fs.push_back(fr({V(2, 0.6f)}, {V(2, 0.2f)}));
        fs.push_back(fr({V(1, 0.5f)}, {V(2, 0.3f)}));
        fs.push_back(fr({V(1, 0.5f)}, {V(2, 0.4f)}));
        fs.push_back(fr({V(1, 0.5f)}, {V(4, 0.1f)}));
        fs.push_back(fr({V(3, 0.6f)}, {V(3, 0.2f)}));
        fs.push_back(fr({V(1, 0.5f)}, {V(3, 0.3f)}));
        fs.push_back(fr({V(1, 0.5f)}, {V(3, 0.4f)}));
        // huge dt (clamp at frames1 and 0)
        Frame big = fr({V(0, 0.4f)}, {V(0, 0.2f)});
        fs.push_back(fr({V(1, 0.5f)}, {V(4, 0.1f)}));
        fs.push_back(big);
        Frame b2 = fr({V(1, 0.5f)}, {V(0, 0.3f)});
        b2.t = {100.0f, 0.2f, 0.1f}, b2.now = 50.0f;
        fs.push_back(b2);
        slot_scenario("S5 advance main", {a, b, one, zero, l}, fs);
    }
    // S6 advance same: all branches
    {
        AnimDef a = A, l = Lp, a2 = A, l2 = Lp;
        a2.key = 0xa1;   // same key as A (duplicate-key suppression)
        l2.key = 0xd4;
        std::vector<Frame> fs;
        auto seq = [&](int an, std::vector<std::pair<float, float>> ph) {
            fs.push_back(fr({V(4, 0.5f)}, {V(5, 0.3f)}));   // reset (case C)
            for (auto &p : ph) fs.push_back(fr({V(an, p.second)}, {V(an, p.first)}));
        };
        seq(0, {{0.2f, 0.4f}, {0.3f, 0.5f}, {0.6f, 0.5f}, {0.7f, 0.9f}});           // non-looping forward / branch 2
        seq(1, {{0.2f, 0.4f}, {0.3f, 0.5f}, {0.95f, 0.05f}, {0.1f, 0.3f}, {0.5f, 0.45f}});   // looping forward / wrap to 0
        seq(0, {{0.8f, 0.6f}, {0.7f, 0.5f}, {0.5f, 0.6f}, {0.4f, 0.3f}});           // backward / branch 5 non-looping
        seq(1, {{0.8f, 0.6f}, {0.7f, 0.5f}, {0.05f, 0.95f}, {0.5f, 0.55f}, {0.4f, 0.3f}});   // looping backward wrap to 1
        // suppression |delta| < 0.1 with equal keys
        fs.push_back(fr({V(4, 0.5f)}, {V(5, 0.3f)}));
        fs.push_back(fr({V(0, 0.4f), V(2, 0.45f)}, {V(0, 0.2f), V(2, 0.25f)}));
        fs.push_back(fr({V(0, 0.5f), V(2, 0.55f)}, {V(0, 0.3f), V(2, 0.35f)}));
        fs.push_back(fr({V(0, 0.6f), V(2, 0.85f)}, {V(0, 0.4f), V(2, 0.7f)}));
        fs.push_back(fr({V(0, 0.7f), V(2, 0.9f)}, {V(0, 0.5f), V(2, 0.6f)}));
        fs.push_back(fr({V(4, 0.5f)}, {V(5, 0.3f)}));
        fs.push_back(fr({V(1, 0.4f), V(3, 0.45f)}, {V(1, 0.2f), V(3, 0.21f)}));
        fs.push_back(fr({V(1, 0.5f), V(3, 0.55f)}, {V(1, 0.3f), V(3, 0.395f)}));
        AnimDef b = B, c = C;
        slot_scenario("S6 advance same", {a, l, a2, l2, b, c}, fs);
    }
    // S7 keep-or-clear
    slot_scenario("S7 keep-or-clear",
                  {A, B},
                  {fr({V(0, 0.4f)}, {V(0, 0.2f)}), fr({V(0, 1.0f)}, {V(0, 0.99f)}), fr({V(0, 1.0f)}, {V(0, 1.0f)}),
                   fr({V(0, 0.4f)}, {V(0, 0.2f)}), fr({}, {}), fr({}, {}), fr({V(0, 0.4f)}, {V(0, 0.2f)}),
                   fr({V(0, 0.6f), V(1, 0.2f)}, {V(0, 0.3f), V(1, 0.4f)}),
                   fr({V(0, 0.6f), V(1, 0.2f)}, {V(0, 0.95f), V(1, 0.05f)}),
                   fr({V(0, 0.6f), V(1, 0.2f)}, {V(0, 1.0f), V(1, 0.0f)})});
    // S8 case C, list full, mark, other slots, visit rejects
    {
        AnimDef noinfo = A, notable = A, zerocount = A, zerokey = A;
        noinfo.no_info = true, notable.no_table = true, zerocount.tcount = 0, zerokey.key = 0;
        std::vector<Act> many, many2;
        for (int i = 0; i < 20; i++) many.push_back(V(i % 2, 0.05f * float(i), uint32_t(i)));
        for (int i = 0; i < 20; i++) many2.push_back(V(i % 2, 0.05f * float(i) + 0.01f, uint32_t(i)));
        std::vector<Frame> fs;
        fs.push_back(fr({V(0, 0.4f)}, {V(1, 0.2f)}));
        fs.push_back(fr({V(0, 0.4f), V(1, 0.4f)}, {V(1, 0.2f)}));
        fs.push_back(fr(many, many2));
        fs.push_back(fr(many2, many));
        fs.push_back(fr({Rst(), V(0, 0.3f), Mk(5), V(1, 0.4f), V(0, 0.5f), Mk(), Mk(9), Emp(1), Emp(2), Emp(5), Emp(6)},
                        {Mk(), V(-1, 0.3f), Mk(), V(2, 0.3f), Mk(), V(3, 0.3f), Mk(), V(4, 0.3f), Mk(), V(5, 0.3f), Mk(),
                         Rst(), Mk()}));
        fs.push_back(fr({V(5, 0.3f), Mk(), V(0, 0.2f), V(5, 0.4f), Mk()}, {V(5, 0.1f), Mk(), Rst()}));
        slot_scenario("S8 case C, full list, mark, visit rejects", {A, B, noinfo, notable, zerocount, zerokey}, fs);
    }
}

// random slot 4 sequences
static void slot_fuzz(int seeds, int frames_per) {
    for (int sd = 0; sd < seeds; sd++) {
        std::mt19937_64 r(5000 + sd);
        auto rf = [&](float lo, float hi) { return lo + float(double(r() % 1000000) / 1000000.0) * (hi - lo); };
        std::vector<AnimDef> anims;
        const int na = 2 + int(r() % 5);
        static const uint32_t keys[] = {0x11, 0x22, 0x33, 0x44, 0x11, 0x22};
        static const uint16_t frs[] = {0, 1, 2, 5, 8, 9, 31, 64, 65, 200, 61, 61, 31};
        for (int i = 0; i < na; i++) {
            AnimDef d;
            d.key = keys[r() % 6];
            d.flags = uint32_t(r() % 2) | (r() % 4 == 0 ? 0xf0 : 0);
            d.frames = frs[r() % 13];
            const float rates[] = {1.0f / 30.0f, 0.5f / 30.0f, 1.0f, 0.0f, -1.0f / 30.0f, 2.0f / 30.0f};
            d.rate = r() % 30 == 0 ? NAN : rates[r() % 6];
            d.no_info = r() % 25 == 0;
            d.no_table = r() % 25 == 0;
            d.tcount = r() % 25 == 0 ? 0 : 1;
            anims.push_back(d);
        }
        // the anims currently playing and their phases
        std::vector<int> playing;
        std::vector<float> phase;
        const int np = 1 + int(r() % 3);
        for (int i = 0; i < np; i++) playing.push_back(int(r() % na)), phase.push_back(rf(0, 1));
        std::vector<Frame> fs;
        float t = 1.0f;
        for (int k = 0; k < frames_per; k++) {
            Frame f;
            const int cc = int(r() % 12);
            f.count = cc == 0 ? uint32_t(r() % 3) : cc == 1 ? 4 + uint32_t(r() % 4) : 3;
            f.t.clear();
            for (uint32_t i = 0; i < std::max<uint32_t>(f.count, 3); i++) f.t.push_back(t - 0.033f * float(i));
            if (r() % 10 == 0) f.t[1 + r() % 2] = 0.0f;
            const int tc = int(r() % 6);
            f.now = tc == 0 ? f.t[0] + 0.01f : tc == 1 ? f.t[1] - 0.001f : tc == 2 ? rf(f.t[2], f.t[0]) : rf(f.t[1], f.t[0]);
            if (r() % 50 == 0) f.now = NAN;
            t += 0.033f;
            f.ev = r() % 10 != 0;
            // newer snapshot phases = older + step
            std::vector<float> older = phase, newer = phase;
            for (size_t i = 0; i < playing.size(); i++) {
                const int sc = int(r() % 10);
                float step = sc < 5 ? rf(0.0f, 0.15f) : sc < 7 ? -rf(0.0f, 0.15f) : sc < 8 ? 0.0f : rf(-1.0f, 1.0f);
                newer[i] = older[i] + step;
                if (newer[i] > 1.0f && r() % 2) newer[i] -= 1.0f;   // looping wrap
                if (newer[i] < 0.0f && r() % 2) newer[i] += 1.0f;
                if (r() % 40 == 0) newer[i] = r() % 2 ? NAN : (r() % 2 ? 1.0f : 0.0f);
                if (r() % 40 == 0) older[i] = r() % 2 ? -0.0f : 1.2f;
            }
            for (size_t i = 0; i < playing.size(); i++) {
                f.newer.push_back(V(playing[i], newer[i], uint32_t(i)));
                f.older.push_back(V(playing[i], older[i], uint32_t(i)));
            }
            if (r() % 8 == 0) f.newer.push_back(V(int(r() % na), rf(0, 1)));          // newer keys differ
            if (r() % 12 == 0 && !f.newer.empty()) f.newer.pop_back();
            if (r() % 15 == 0) f.older.push_back(V(int(r() % na), rf(0, 1)));
            if (r() % 10 == 0) f.older.insert(f.older.begin(), Mk());
            if (r() % 10 == 0) f.newer.push_back(Mk());
            if (r() % 20 == 0) f.newer.insert(f.newer.begin(), Rst());
            if (r() % 30 == 0) f.older.push_back(V(-1, 0.5f));
            fs.push_back(f);
            phase = newer;
            for (auto &p : phase)
                if (std::isnan(p)) p = 0.5f;
            if (r() % 10 == 0) {   // change what plays
                const size_t i = r() % playing.size();
                playing[i] = int(r() % na);
                phase[i] = rf(0, 1);
            }
            if (r() % 15 == 0 && playing.size() < 5) playing.push_back(int(r() % na)), phase.push_back(rf(0, 1));
            if (r() % 15 == 0 && playing.size() > 1) playing.pop_back(), phase.pop_back();
        }
        slot_scenario("S fuzz seed " + std::to_string(sd) + " (" + std::to_string(frames_per) + " frames)", anims, fs);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// natives
constexpr uint64_t NGM = 0x113b760, SESSION = 0x1065b08, NETSTATE = 0x14ce97c, CLOCK = 0x1116180, CONVPTR = 0x10666c8;
constexpr uint64_t PROCS = 0x1200000, PSLOTS = 0x1210000, BUCKETS = 0x1220000, NODES = 0x1230000, NPC = 0x1240000,
                   NPC_VT = 0x1250000, HEALTH = 0x1258000, HEALTH_VT = 0x1259000, NETGAMES = 0x1270000,
                   RECORDS = 0x1280000, TASKLIST = 0x1290000, TASKS = 0x12a0000, DEFTASK = 0x12b0000, SPAWNER = 0x12c0000,
                   STABLE = 0x12d0000, SENTRIES = 0x12e0000, SVALUES = 0x12f0000, SDATA = 0x1300000, STUBS = 0x1310000;

static const Check kNatAnchors[] = {
    {0x668b39, {0x39, 0x94, 0x30, 0x30, 0x01, 0x00, 0x00}},
    {0x6428f6, {0x39, 0x8b, 0x88, 0x01, 0x00, 0x00}},
    {0x642880, {0x8b, 0x44, 0x08, 0x0c}},
    {0x60ecaf, {0x48, 0x8b, 0x09}},
    {0x60ecbb, {0x8b, 0x89, 0x20, 0x01, 0x00, 0x00}},
    {0x668a6c, {0x48, 0x63, 0x8e, 0xdc, 0x1b, 0x00, 0x00}},
    {0x668a7a, {0x48, 0x8b, 0x96, 0xa0, 0x1b, 0x00, 0x00}},
    {0x668a98, {0x39, 0x78, 0x10}},
    {0x668abf, {0x48, 0x8d, 0x40, 0x18}},
    {0x668acc, {0x48, 0xc7, 0x42, 0x24, 0x00, 0x00, 0x00, 0x00}},
    {0x668d4c, {0xff, 0x90, 0x50, 0x02, 0x00, 0x00}},
    {0x25eb1a, {0xff, 0x90, 0x70, 0x03, 0x00, 0x00}},
    {0x61f11a, {0x48, 0x8b, 0x98, 0xf0, 0x01, 0x00, 0x00}},
    {0x61f12b, {0xff, 0x50, 0x50}},
    {0x249ef2, {0x41, 0x80, 0x8f, 0x2a, 0x26, 0x00, 0x00, 0x02}},
    {0x6696d0, {0x44, 0x8b, 0x60, 0x68}},
    {0x669795, {0x48, 0x0f, 0xbf, 0x8a, 0xa0, 0x1c, 0x00, 0x00}},
    {0x6697a1, {0x48, 0x8d, 0x0c, 0xc9, 0x48, 0xc1, 0xe1, 0x04}},
    {0x6697a9, {0x48, 0x8b, 0x4c, 0x08, 0x48}},
    {0x6697b5, {0x48, 0x63, 0x92, 0x6c, 0x1d, 0x00, 0x00}},
    {0x6697c5, {0x3b, 0x51, 0x08}},
    {0x6697d2, {0x48, 0x03, 0x41, 0x10}},
    {0x66418f, {0x48, 0x8d, 0x48, 0x18}},
    {0x669c92, {0x48, 0x83, 0x79, 0x10, 0x00}},
    {0x669c99, {0x48, 0x8b, 0x81, 0x00, 0x0b, 0x00, 0x00}},
    {0x60ec4c, {0x0f, 0x44, 0x81, 0x04, 0x02, 0x00, 0x00}},
    {0x64ba83, {0x48, 0x8b, 0x48, 0x68}},
    {0x20d7cb, {0x83, 0xff, 0x0d}},
    {0x20d7db, {0x83, 0xfa, 0x05}},
    {0x64b960, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}},
    {0x60eb90, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}},
    {0xa15e70, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}},
};
struct Ref {
    uint64_t at;
    unsigned len, disp;
    uint64_t target;
};
static const Ref kNatRefs[] = {{0x669c20, 7, 3, 0x113b760}, {0x669c50, 7, 2, 0x14ce97c}, {0x669c89, 7, 3, 0x1065b08},
                               {0x60ec45, 7, 3, 0x1116180}, {0x6698fb, 7, 3, 0x10666c8}, {0x6697e9, 5, 1, 0xa15e70},
                               {0x249eed, 5, 1, 0x60eb90}};
struct Nat {
    const char *name;
    uint64_t off;
};
static const Nat kNatives[] = {{"net-coop-host-script-id", 0x669c40}, {"net-coop-script-id", 0x669c30},
                               {"net-player-coop-id", 0x669240},      {"net-get-capture-zone", 0x6687d0},
                               {"net-apply-npc-fight-damage", 0x668db0}, {"net-get-player-by-net-id", 0x6690b0},
                               {"net-coop-task", 0x6697e0}};

static void natives_bytes(int broken_anchor = -1, int broken_ref = -1, int broken_stub = -1, size_t stub_byte = 0) {
    for (int i = 0; i < 32; i++) {
        put_bytes(mem, kNatAnchors[i].off, kNatAnchors[i].bytes);
        if (i == broken_anchor) mem[kNatAnchors[i].off + kNatAnchors[i].bytes.size() - 1] ^= 0x10;
    }
    for (int i = 0; i < 6; i++) put_bytes(mem, kNatives[i].off, {0x31, 0xc0, 0xc3, 0x66, 0x66});
    put_bytes(mem, 0x6697e0, {0x55, 0x48, 0x89, 0xe5, 0xbf, 0x7d, 0x06, 0x9a, 0x3b, 0xe8, 0x82, 0xc6, 0x3a, 0x00, 0x31,
                              0xc0, 0x5d, 0xc3});
    for (int i = 0; i < 7; i++) {
        const Ref &f = kNatRefs[i];
        int32_t d = int32_t(int64_t(f.target) - int64_t(f.at + f.len));
        if (i == broken_ref) d += 1;
        put<int32_t>(f.at + f.disp, d);
    }
    if (broken_stub >= 0) mem[kNatives[broken_stub].off + stub_byte] ^= 0x02;
}

static int install_natives() {
    int n = 0;
    if (g_old) u2mp_old::install_natives(n);
    else u2mp::install_natives(n);
    rec("natives installed %d", n);
    return n;
}

// guest fakes
static uint64_t g_lookup_ret = 0, g_spawner_ret = 0;
static std::vector<uint32_t> g_alive;
static size_t g_alive_i = 0;
static uint64_t g_maxhp = 0;
static int g_damage_action = 0;   // 1: make host, 2: make client

U2MP_GUEST uint64_t f_lookup(uint32_t sid) {
    rec("lookup %08x", sid);
    return g_lookup_ret;
}
U2MP_GUEST uint64_t f_spawner(void *ngm, int32_t id) {
    rec("spawner ngm=%s id=%d", where(ngm).c_str(), id);
    return g_spawner_ret;
}
U2MP_GUEST void f_send(uint32_t kind, void *att, void *vic, const uint32_t *frame, bool a, bool b, bool c) {
    rec("send kind=%08x att=%s vic=%s frame=%08x %d %d %d", kind, where(att).c_str(), where(vic).c_str(), *frame, a, b, c);
}
U2MP_GUEST uint32_t f_conv(uint64_t zero, const char *s, int one) {
    rec("convert %llx %s \"%s\" %d", (unsigned long long)zero, where(s).c_str(), s ? s : "", one);
    return 0xc0de0000u | uint32_t(s ? std::strlen(s) : 0xffff);
}
U2MP_GUEST uint64_t f_alive(void *o) {
    const uint32_t v = g_alive_i < g_alive.size() ? g_alive[g_alive_i] : 1;
    g_alive_i++;
    rec("alive %s -> %08x", where(o).c_str(), v);
    return 0xabcdef0000000000ull | v;
}
U2MP_GUEST void f_damage(void *o, int32_t d) {
    rec("take damage %s %d", where(o).c_str(), d);
    if (g_damage_action == 1) put<uint64_t>(SESSION + 0x10, 0);
    if (g_damage_action == 2) put<uint64_t>(SESSION + 0x10, 5), put<uint32_t>(SESSION + 0xb00, 1), put<uint32_t>(SESSION + 0xb04, 2);
}
U2MP_GUEST uint64_t f_maxhp(void *h) {
    rec("max health %s", where(h).c_str());
    return g_maxhp;
}

static void natives_setup() {
    natives_bytes();
    install_natives();
    stub(0xa15e70, reinterpret_cast<void *>(&f_lookup));
    stub(0x64b960, reinterpret_cast<void *>(&f_spawner));
    stub(0x60eb90, reinterpret_cast<void *>(&f_send));
    put<uint64_t>(CONVPTR, 0);
    stub(STUBS, reinterpret_cast<void *>(&f_conv));
    stub_ptr(NPC_VT + 74 * 8, STUBS + 0x20, reinterpret_cast<void *>(&f_alive));
    stub_ptr(NPC_VT + 110 * 8, STUBS + 0x40, reinterpret_cast<void *>(&f_damage));
    stub_ptr(HEALTH_VT + 10 * 8, STUBS + 0x60, reinterpret_cast<void *>(&f_maxhp));
    put<uint64_t>(NPC, addr(NPC_VT));
    put<uint64_t>(HEALTH, addr(HEALTH_VT));
    g_lookup_ret = g_spawner_ret = 0;
    g_alive.clear();
    g_alive_i = 0;
    g_maxhp = 0;
    g_damage_action = 0;
}

static uint64_t call_native(int i, int argc, std::vector<uint64_t> argv) {
    argv.resize(8, 0xeeeeeeeeeeeeeeeeull);
    const uint64_t r = reinterpret_cast<uint64_t(U2MP_GUEST *)(int, const uint64_t *)>(mem + kNatives[i].off)(argc, argv.data());
    rec("%s(argc %d) = %016llx", kNatives[i].name, argc, (unsigned long long)r);
    ++g_cases;
    return r;
}
static void call_all(int argc, const std::vector<uint64_t> &argv) {
    for (int i = 0; i < 7; i++) call_native(i, argc, argv);
}

// state helpers
static uint64_t tracker(int i) { return NGM + 0x10 + uint64_t(i) * 0x2c0; }
static void set_process(int j, uint32_t id, uint32_t name) {   // process j at PROCS + j*0x100, its slot at PSLOTS + j*8
    put<uint32_t>(PROCS + j * 0x100 + 0x44, id);
    put<uint32_t>(PROCS + j * 0x100 + 0x68, name);
    put<uint64_t>(PSLOTS + j * 8, addr(PROCS + j * 0x100));
}
// handle at `h`: mode 0 valid -> process j; 1 slot null; 2 id 0; 3 slot holds null; 4 id mismatch
static void set_handle(uint64_t h, int mode, int j, uint32_t id) {
    put<uint64_t>(h, mode == 1 ? 0 : addr(mode == 3 ? PSLOTS + 0x800 : PSLOTS + j * 8));
    put<uint32_t>(h + 8, mode == 2 ? 0 : mode == 4 ? id + 1 : id);
}
static uint32_t fnv(uint32_t v) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xff)) * 0x01000193u;
    return h;
}
// npc map with `count` buckets; node k at NODES + k*0x100 with key; npc tracker handle -> the NPC object
static void npc_map(int32_t count, std::vector<uint32_t> names, uint64_t end_marker_node = ~0ull) {
    put<uint64_t>(NGM + 0x1ba0, addr(BUCKETS));
    put<int32_t>(NGM + 0x1bdc, count);
    if (count <= 0) return;
    for (int32_t b = 0; b <= count; b++) put<uint64_t>(BUCKETS + b * 8, 0);
    for (size_t k = 0; k < names.size(); k++) {
        const uint64_t node = NODES + k * 0x100;
        const uint32_t b = fnv(names[k]) % uint32_t(count);
        put<uint64_t>(node, get<uint64_t>(BUCKETS + b * 8));   // push front
        put<uint32_t>(node + 0x10, names[k]);
        put<uint64_t>(BUCKETS + b * 8, addr(node));
    }
    if (end_marker_node != ~0ull) put<uint64_t>(BUCKETS + uint64_t(count) * 8, addr(NODES + end_marker_node * 0x100));
}
static void npc_valid(int k, uint32_t id) {   // node k's tracker handle -> NPC object with unique id `id`
    const uint64_t t = NODES + uint64_t(k) * 0x100 + 0x18;
    put<uint64_t>(t, addr(PSLOTS + 0x400));
    put<uint32_t>(t + 8, id);
    put<uint64_t>(PSLOTS + 0x400, addr(NPC));
    put<uint32_t>(NPC + 0x44, id);
    put<uint64_t>(NPC + 0x1f0, addr(HEALTH));
}
static void set_mp(int32_t s) { put<int32_t>(NETSTATE, s); }
static void set_host(int mode) {   // 0 host by ctx 0, 1 host by ids, 2 client
    put<uint64_t>(SESSION + 0x10, mode == 0 ? 0 : 0x1234);
    put<uint32_t>(SESSION + 0xb00, 7);
    put<uint32_t>(SESSION + 0xb04, mode == 2 ? 8 : 7);
}
static std::string npc_tracker_dump(int k) {
    const uint64_t t = NODES + uint64_t(k) * 0x100 + 0x18;
    char b[160];
    std::snprintf(b, sizeof b, "npc tracker ids %08x %08x %08x dmg %08x %08x %08x flag %02x", get<uint32_t>(t + 0x30),
                  get<uint32_t>(t + 0x34), get<uint32_t>(t + 0x38), get<uint32_t>(t + 0x3c), get<uint32_t>(t + 0x40),
                  get<uint32_t>(t + 0x44), mem[NPC + 0x262a]);
    return b;
}

static void net_games(int32_t rec_count, bool records_null, int16_t index, int32_t type, bool list_null,
                      int32_t task_count, bool tasks_null, int32_t task_index, bool default_null) {
    g_lookup_ret = addr(NETGAMES);
    put<int32_t>(NETGAMES, rec_count);
    put<uint64_t>(NETGAMES + 8, records_null ? 0 : addr(RECORDS));
    put<int16_t>(NGM + 0x1ca0, index);
    for (int i = 0; i < 4; i++) {
        put<int32_t>(RECORDS + i * 0x90 + 0x0c, type + i);
        put<uint64_t>(RECORDS + i * 0x90 + 0x48, list_null ? 0 : addr(TASKLIST));
    }
    put<uint64_t>(TASKLIST, default_null ? 0 : addr(DEFTASK));
    put<int32_t>(TASKLIST + 8, task_count);
    put<uint64_t>(TASKLIST + 0x10, tasks_null ? 0 : addr(TASKS));
    put<int32_t>(NGM + 0x1d6c, task_index);
    for (int i = 0; i < 4; i++) {
        put<uint32_t>(TASKS + i * 0x28, 0x7a500 + i);
        put<uint32_t>(TASKS + i * 0x28 + 0x1c, i == 1 ? 0 : 0x40500 + i);
        put<uint32_t>(TASKS + i * 0x28 + 0x20, i == 2 ? 0 : 0x50500 + i);
    }
    put<uint32_t>(DEFTASK, 0xdef0);
    put<uint32_t>(DEFTASK + 0x1c, 0xdef1c);
    put<uint32_t>(DEFTASK + 0x20, 0xdef20);
}

// spawner table: entries {key, value}; value index -> SVALUES + i*0x20 {type, data}
struct SEntry {
    uint32_t key;
    int value;   // -1 null
    uint16_t type;
    int data;    // -1 null; else SDATA + data*0x40 (u32 sid / char pointer to a string)
};
static void spawner(std::vector<SEntry> es, bool table_null = false, bool entries_null = false, int32_t count = -1000) {
    g_spawner_ret = addr(SPAWNER);
    put<uint64_t>(SPAWNER + 0x68, table_null ? 0 : addr(STABLE));
    put<uint64_t>(STABLE, entries_null ? 0 : addr(SENTRIES));
    put<int32_t>(STABLE + 8, count == -1000 ? int32_t(es.size()) : count);
    for (size_t i = 0; i < es.size(); i++) {
        put<uint32_t>(SENTRIES + i * 16, es[i].key);
        put<uint64_t>(SENTRIES + i * 16 + 8, es[i].value < 0 ? 0 : addr(SVALUES + uint64_t(es[i].value) * 0x20));
        if (es[i].value >= 0) {
            const uint64_t v = SVALUES + uint64_t(es[i].value) * 0x20;
            put<uint16_t>(v, es[i].type);
            put<uint64_t>(v + 8, es[i].data < 0 ? 0 : addr(SDATA + uint64_t(es[i].data) * 0x40));
            if (es[i].data >= 0) {
                const uint64_t d = SDATA + uint64_t(es[i].data) * 0x40;
                put<uint32_t>(d, 0x51d00 + uint32_t(es[i].data));
                if (es[i].type == 5) {
                    put<uint64_t>(d, addr(d + 0x10));
                    std::snprintf(reinterpret_cast<char *>(mem + d + 0x10), 32, "zone-%d", es[i].data);
                }
            }
        }
    }
}

static void natives_install_scenarios() {
    scenario("I1 all good", [&] {
        natives_bytes();
        install_natives();
    });
    for (int a : {0, 1, 15, 29, 31}) {
        scenario("I2 anchor " + std::to_string(a + 1) + " broken", [&] {
            natives_bytes(a);
            install_natives();
        });
    }
    scenario("I2 anchors 5 and 20 broken", [&] {
        natives_bytes(4);
        mem[0x6697b5] ^= 1;
        install_natives();
    });
    for (int f = 0; f < 7; f++)
        scenario("I3 rip ref " + std::to_string(f + 1) + " wrong", [&] {
            natives_bytes(-1, f);
            install_natives();
        });
    for (uint64_t size : {uint64_t(0xa15e78), uint64_t(0xa15e77), uint64_t(0x669c26)})
        scenario("I3 image size 0x" + [&] { char b[32]; std::snprintf(b, 32, "%llx", (unsigned long long)size); return std::string(b); }(), [&] {
            natives_bytes();
            set_image_both(mem, size);
            install_natives();
        });
    for (int s = 0; s < 7; s++)
        scenario("I4 stub of " + std::string(kNatives[s].name) + " changed", [&] {
            natives_bytes(-1, -1, s, s == 6 ? 17 : 4);
            install_natives();
        });
    scenario("I4 replace_function fails for one native", [&] {
        natives_bytes();
        fail_offsets.push_back(0x6687d0);
        install_natives();
    });
}

static void natives_scenarios() {
    // C1 / C2: task fields and coop-task
    scenario("C1/C2 script ids and coop-task", [&] {
        natives_setup();
        auto run = [&] {
            call_native(0, 0, {});
            call_native(1, 0, {});
            call_native(6, 0, {});
        };
        g_lookup_ret = 0;
        run();
        //        rec_count records_null index type list_null task_count tasks_null task_index default_null
        net_games(2, false, -1, 4, false, 3, false, 0, false);
        run();
        net_games(2, false, 2, 4, false, 3, false, 0, false);
        run();
        net_games(2, true, 0, 4, false, 3, false, 0, false);
        run();
        net_games(2, false, 1, 4, true, 3, false, 0, false);
        run();
        for (int32_t ti : {-1, 3, 4})
            for (bool dn : {false, true}) {
                net_games(2, false, 1, 4, false, 3, false, ti, dn);
                run();
            }
        for (bool tn : {false, true})
            for (bool dn : {false, true}) {
                net_games(2, false, 0, 4, false, 3, tn, 0, dn);
                run();
            }
        for (int32_t ti : {0, 1, 2})
            for (bool dn : {false, true}) {
                net_games(3, false, 2, 4, false, 4, false, ti, dn);
                run();
            }
        net_games(0, false, 0, 4, false, 0, false, 0, false);
        run();
        net_games(0x10000, false, int16_t(0x7fff), 4, false, 0x7fffffff, false, 2, false);
        run();
    });
    // C3 player-coop-id
    scenario("C3 player-coop-id", [&] {
        natives_setup();
        for (int32_t s : {0, 1, -5, 2}) {
            set_mp(s);
            g_lookup_ret = 0;
            call_native(2, 0, {});
        }
        set_mp(2);
        for (int32_t type : {0, 4, 8, 9, 12, 5, 13, -4}) {
            net_games(1, false, 0, type, false, 1, false, 0, false);
            put<uint64_t>(NGM, addr(tracker(3)));
            put<int32_t>(tracker(3) + 0x198, 1);
            call_native(2, 0, {});
        }
        net_games(1, false, 0, 9, false, 1, false, 0, false);
        put<uint64_t>(NGM, 0);
        call_native(2, 0, {});
        put<uint64_t>(NGM, addr(tracker(3)));
        for (int32_t ci : {0, 1, 2, 3, -1, 0x7fffffff}) {
            put<int32_t>(tracker(3) + 0x198, ci);
            call_native(2, 0, {});
        }
        net_games(1, true, 0, 9, false, 1, false, 0, false);   // no record
        call_native(2, 0, {});
        set_mp(1);
        for (int i = 0; i < 10; i++) call_native(2, 0, {});   // 8 lines
        set_mp(3);
        put<int32_t>(tracker(3) + 0x198, 7);
        net_games(1, false, 0, 4, false, 1, false, 0, false);
        for (int i = 0; i < 10; i++) call_native(2, 0, {});
        put<uint64_t>(NGM, 0);
        for (int i = 0; i < 10; i++) call_native(2, 0, {});
        net_games(1, false, 0, 3, false, 1, false, 0, false);
        for (int i = 0; i < 10; i++) call_native(2, 0, {});
    });
    // C4 capture zone
    scenario("C4 capture zone", [&] {
        natives_setup();
        const uint32_t K = 0x7af5d23b;
        g_spawner_ret = 0;
        call_native(3, 0, {});
        spawner({{K, 0, 13, 0}}, true);
        call_native(3, 0, {});
        spawner({{K, 0, 13, 0}}, false, true);
        call_native(3, 0, {});
        spawner({{K, 0, 13, 0}}, false, false, 0);
        call_native(3, 0, {});
        spawner({{K, 0, 13, 0}}, false, false, -1);
        call_native(3, 0, {});
        spawner({{1, 0, 13, 0}, {2, 1, 13, 1}});   // key above all
        call_native(3, 0, {});
        spawner({{0x80000000, 0, 13, 0}, {0xffffffff, 1, 13, 1}});   // below all
        call_native(3, 0, {});
        spawner({{1, 0, 13, 0}, {K + 1, 1, 13, 1}});   // between
        call_native(3, 0, {});
        for (int type : {13, 5, 7, 0}) {
            for (int dn = 0; dn < 2; dn++)
                for (int cn = 0; cn < 2; cn++) {
                    spawner({{1, 0, 13, 0}, {K, 1, uint16_t(type), dn ? -1 : 1}, {K + 5, 2, 13, 2}});
                    put<uint64_t>(CONVPTR, cn ? addr(STUBS) : 0);
                    call_native(3, 0, {});
                }
        }
        spawner({{K, -1, 13, 0}});   // value null
        call_native(3, 0, {});
        // unsorted tables pin the probe sequence; count 1, 2, 3, 7
        put<uint64_t>(CONVPTR, addr(STUBS));
        std::mt19937_64 r(31);
        for (int i = 0; i < 300; i++) {
            const int n = int(r() % 9) + 1;
            std::vector<SEntry> es;
            for (int k = 0; k < n; k++) {
                const uint32_t keys[] = {K, K - 1, K + 1, 0, 0xffffffff, 0x10, K};
                es.push_back({keys[r() % 7], r() % 10 == 0 ? -1 : k, uint16_t(r() % 3 == 0 ? 5 : r() % 5 == 0 ? 9 : 13),
                              r() % 10 == 0 ? -1 : k});
            }
            if (r() % 2) std::sort(es.begin(), es.end(), [](const SEntry &a, const SEntry &b) { return a.key < b.key; });
            spawner(es, false, false, r() % 8 == 0 ? int32_t(r() % 12) - 2 : -1000);
            call_native(3, 0, {});
        }
    });
    // C5 player by net id
    scenario("C5 player-by-net-id", [&] {
        natives_setup();
        set_mp(0);
        call_native(5, 1, {3});
        set_mp(2);
        call_native(5, 0, {3});
        call_native(5, -1, {3});
        for (int i = 0; i < 10; i++) put<uint32_t>(tracker(i) + 0x120, 100 + i);
        call_native(5, 1, {3});   // no tracker
        call_native(5, 1, {0xffffffff00000067ull});   // upper bits ignored -> tracker 3
        put<uint32_t>(tracker(4) + 0x120, 0);
        call_native(5, 1, {0x500000000ull});   // net id 0 matches the empty tracker 4
        set_process(1, 0x77, 0x4a4e);
        for (int mode = 0; mode < 5; mode++) {
            set_handle(tracker(3) + 0x10, mode, 1, 0x77);
            call_native(5, 2, {0x67});
        }
        put<uint32_t>(tracker(7) + 0x120, 0x67);   // duplicate net id: the first tracker wins
        set_handle(tracker(7) + 0x10, 0, 1, 0x77);
        set_handle(tracker(3) + 0x10, 1, 1, 0x77);
        call_native(5, 1, {0x67});
        for (int i = 0; i < 10; i++) call_native(5, 1, {12345});
    });
    // C6 apply npc fight damage
    scenario("C6 npc fight damage exits", [&] {
        natives_setup();
        set_mp(0);
        call_native(4, 4, {1, 2, 3, 4});
        set_mp(2);
        call_native(4, 3, {1, 2, 3, 4});
        for (int i = 0; i < 10; i++) put<uint32_t>(tracker(i) + 0x120, 100 + i);
        call_native(4, 4, {1, 0x9a, 3, 4});   // no tracker
        set_process(0, 0x55, 0x1111);
        set_handle(tracker(2) + 0x10, 3, 0, 0x55);
        call_native(4, 4, {102, 0x9a, 3, 4});   // no player process
        set_handle(tracker(2) + 0x10, 0, 0, 0x55);
        npc_map(0, {});
        call_native(4, 4, {102, 0x9a, 3, 4});   // bucket count 0
        npc_map(-3, {});
        call_native(4, 4, {102, 0x9a, 3, 4});
        put<uint64_t>(NGM + 0x1ba0, 0);
        put<int32_t>(NGM + 0x1bdc, 4);
        call_native(4, 4, {102, 0x9a, 3, 4});   // buckets null
        // chain walk: names colliding in one bucket (count 1)
        npc_map(1, {0x9a, 0x9b, 0x9c});
        npc_valid(0, 0x99);
        call_native(4, 4, {102, 0x9a, 3, 4});   // found at the end of the chain (after two mismatches)
        call_native(4, 4, {102, 0x9d, 3, 4});   // not found (null)
        npc_map(1, {0x9a, 0x9b, 0x9c}, 0);      // end marker = node 0 (the 0x9a node)
        call_native(4, 4, {102, 0x9a, 3, 4});   // equal to the end marker: none
        npc_map(5, {0x9a, 0x9b, 0x9c, 0x1234, 0xffffffff});
        npc_valid(4, 0x98);
        set_host(0);
        g_alive = {1, 1};
        call_native(4, 4, {102, 0xffffffff, 3, 4});
        npc_valid(0, 0x99);
        put<uint32_t>(NODES + 0x18 + 8, 0x98);   // id mismatch
        call_native(4, 4, {102, 0x9a, 3, 4});    // no npc
        put<uint64_t>(NODES + 0x18, 0);
        call_native(4, 4, {102, 0x9a, 3, 4});
        rec("%s", npc_tracker_dump(0).c_str());
        for (int i = 0; i < 10; i++) call_native(4, 4, {1, 0x9a, 3, 4});
        for (int i = 0; i < 10; i++) call_native(4, 4, {102, 0x9a, 3, 4});
    });
    scenario("C6 npc fight damage host / client", [&] {
        natives_setup();
        set_mp(5);
        for (int i = 0; i < 10; i++) put<uint32_t>(tracker(i) + 0x120, 100 + i);
        set_process(0, 0x55, 0x1111);
        set_handle(tracker(2) + 0x10, 0, 0, 0x55);
        npc_map(3, {0x9a});
        npc_valid(0, 0x99);
        put<uint32_t>(CLOCK + 0x204, 0x31337);
        mem[NPC + 0x262a] = 0x81;
        const uint64_t nt = NODES + 0x18;
        auto hit = [&](int host, std::vector<uint32_t> alive, uint64_t maxhp, uint64_t dmg, uint32_t netid = 102) {
            set_host(host);
            g_alive = alive;
            g_alive_i = 0;
            g_maxhp = maxhp;
            call_native(4, 4, {netid, 0x9a, dmg, 0x4b1d});
            rec("%s", npc_tracker_dump(0).c_str());
        };
        // host: new attacker, same attacker (sum, wrap), three others
        hit(0, {1, 1}, 100, 5);
        hit(1, {1, 1}, 100, 7);
        hit(0, {1, 1}, 100, 0x7fffffff);
        hit(0, {1, 1}, 100, 0xffffffff);
        put<uint32_t>(nt + 0x30, 1), put<uint32_t>(nt + 0x34, 2), put<uint32_t>(nt + 0x38, 3);
        hit(0, {1, 1}, 100, 9);
        put<uint32_t>(nt + 0x30, 1), put<uint32_t>(nt + 0x34, 0), put<uint32_t>(nt + 0x38, 102);
        hit(0, {1, 1}, 100, 9);   // empty slot before the matching one
        // net id 0 meeting an empty slot (attacker id 0)
        put<uint32_t>(tracker(5) + 0x120, 0);
        set_handle(tracker(5) + 0x10, 0, 0, 0x55);
        put<uint32_t>(nt + 0x30, 0), put<uint32_t>(nt + 0x34, 0), put<uint32_t>(nt + 0x38, 0);
        put<uint32_t>(nt + 0x3c, 40), put<uint32_t>(nt + 0x40, 50), put<uint32_t>(nt + 0x44, 60);
        hit(0, {1, 1}, 100, 3, 0);
        hit(0, {1, 1}, 100, 4, 0);
        put<uint32_t>(nt + 0x30, 9), put<uint32_t>(nt + 0x34, 0);
        hit(0, {1, 1}, 100, 4, 0);
        // alive changed: send, flag set, other flag bits kept
        hit(0, {1, 0}, 100, 50);
        hit(1, {0, 1}, 100, 50);
        hit(0, {0x101, 0x100}, 100, 50);   // only al counts
        // client
        hit(2, {1, 0}, 100, 50);      // below max: only the max-health call
        hit(2, {1, 0}, 100, 100);     // >= max: applied, no send
        hit(2, {1, 1}, 100, 0xffffffff);
        hit(2, {1, 0}, 0xffffffffffffffffull, 0);   // max -1
        hit(2, {1, 0}, 0x0000000080000000ull, 5);   // max INT_MIN
        hit(2, {1, 0}, 0xdeadbeef00000005ull, 5);   // upper bits ignored: max 5
        hit(2, {1, 0}, 0xdeadbeef00000006ull, 5);
        put<uint64_t>(NPC + 0x1f0, 0);
        hit(2, {1, 0}, 100, 0);   // health null: max 0
        hit(0, {1, 0}, 100, 0);
        put<uint64_t>(NPC + 0x1f0, addr(HEALTH));
        // host status changing between reads (the take-damage virtual writes the match session)
        g_damage_action = 2;
        hit(0, {1, 0}, 100, 5);   // host at the start, client after the damage: no send
        g_damage_action = 1;
        hit(2, {1, 0}, 100, 500);   // client at the start, host after: send
        g_damage_action = 0;
    });
    // outside the ABI: a bool result must be 0 or 1 in al; 0xff then 0x01 (both "true")
    scenario("C6x alive test returns al=0xff then al=0x01 (not a valid bool)", [&] {
        natives_setup();
        set_mp(5);
        for (int i = 0; i < 10; i++) put<uint32_t>(tracker(i) + 0x120, 100 + i);
        set_process(0, 0x55, 0x1111);
        set_handle(tracker(2) + 0x10, 0, 0, 0x55);
        npc_map(3, {0x9a});
        npc_valid(0, 0x99);
        set_host(0);
        g_alive = {0xff, 0x01};
        g_maxhp = 100;
        call_native(4, 4, {102, 0x9a, 50, 0x4b1d});
    }, true);
}

// random natives states
static void natives_fuzz(int batches, int per) {
    for (int bt = 0; bt < batches; bt++) {
        scenario("C fuzz batch " + std::to_string(bt) + " (" + std::to_string(per) + " states x 7 natives)", [&] {
            natives_setup();
            std::mt19937_64 r(9000 + bt);
            auto pick = [&](std::initializer_list<uint64_t> l) { return *(l.begin() + r() % l.size()); };
            for (int c = 0; c < per; c++) {
                set_mp(int32_t(pick({2, 2, 2, 2, 3, 0, 1, 0xffffffff, 0x80000000})));
                set_host(int(r() % 3));
                for (int i = 0; i < 10; i++) put<uint32_t>(tracker(i) + 0x120, uint32_t(pick({100 + uint64_t(i), 100 + uint64_t(i), 100 + uint64_t(i), 0, 1, 2, 7})));
                for (int j = 0; j < 4; j++) set_process(j, 0x50 + j, 0x1000 + j);
                for (int i = 0; i < 10; i++) {
                    const int j = int(r() % 4);
                    set_handle(tracker(i) + 0x10, int(r() % 6 == 0 ? r() % 5 : 0), j, uint32_t(0x50 + j));
                }
                put<uint64_t>(NGM, r() % 5 == 0 ? 0 : addr(tracker(int(r() % 10))));
                for (int i = 0; i < 10; i++) put<int32_t>(tracker(i) + 0x198, int32_t(pick({0, 1, 2, 3, 0xffffffff})));
                if (r() % 6 == 0) g_lookup_ret = 0;
                else
                    net_games(int32_t(pick({0, 1, 2, 3})), r() % 10 == 0, int16_t(pick({0, 1, 2, 0xffff, 5})),
                              int32_t(pick({4, 8, 9, 12, 3, 0, 5})), r() % 8 == 0, int32_t(pick({0, 1, 3, 4})), r() % 8 == 0,
                              int32_t(pick({0, 1, 2, 3, 0xffffffff})), r() % 3 == 0);
                std::vector<uint32_t> names;
                const int nn = int(r() % 5);
                if (nn) names.push_back(0x9a);
                for (int k = 1; k < nn; k++) names.push_back(uint32_t(pick({0x9a, 0x9b, 0x100, 0xffffffff, 0, 0x9a})));
                const int bc = int(pick({1, 2, 3, 7, 1, 2, 3, 7, 0, 0xffffffff}));
                npc_map(bc, names, r() % 4 == 0 && nn ? r() % nn : ~0ull);
                if (r() % 10 == 0) put<uint64_t>(NGM + 0x1ba0, 0);
                for (int k = 0; k < nn; k++) {
                    npc_valid(k, 0x99);
                    if (r() % 6 == 0) put<uint32_t>(NODES + k * 0x100 + 0x18 + 8, uint32_t(pick({0, 0x98})));
                    for (int s = 0; s < 3; s++) {
                        put<uint32_t>(NODES + k * 0x100 + 0x18 + 0x30 + 4 * s, uint32_t(pick({0, 0, 1, 2, 7, 102})));
                        put<uint32_t>(NODES + k * 0x100 + 0x18 + 0x3c + 4 * s, uint32_t(r()));
                    }
                }
                if (r() % 8 == 0) put<uint64_t>(NPC + 0x1f0, 0);
                mem[NPC + 0x262a] = uint8_t(r());
                put<uint32_t>(CLOCK + 0x204, uint32_t(r()));
                g_alive = {uint32_t(r() % 2), uint32_t(r() % 2)};
                if (r() % 5 == 0) g_alive[1] = g_alive[0];
                g_alive_i = 0;
                g_maxhp = pick({0, 5, 100, 0xffffffff, 0x80000000, 0x1234567800000010ull});
                g_damage_action = int(r() % 6 == 0 ? 1 + r() % 2 : 0);
                put<uint64_t>(CONVPTR, r() % 3 ? addr(STUBS) : 0);
                if (r() % 5 == 0) g_spawner_ret = 0;
                else {
                    std::vector<SEntry> es;
                    const int n = int(r() % 6);
                    for (int k = 0; k < n; k++)
                        es.push_back({uint32_t(pick({0x7af5d23b, 0x7af5d23a, 0x7af5d23c, 1, 0xffffffff})),
                                      r() % 10 == 0 ? -1 : k, uint16_t(pick({13, 5, 6})), r() % 8 == 0 ? -1 : k});
                    if (r() % 2) std::sort(es.begin(), es.end(), [](const SEntry &a, const SEntry &b) { return a.key < b.key; });
                    spawner(es, r() % 12 == 0, r() % 12 == 0);
                }
                const int argc = int(pick({4, 4, 4, 4, 4, 4, 0, 1, 3, 5, 0xffffffff}));
                std::vector<uint64_t> argv = {pick({102, 102, 103, 104, 0, 1, 2, 7, 103, 0xffffffff00000066ull, 104}) | (r() % 2 ? r() << 32 : 0),
                                              pick({0x9a, 0x9a, 0x9a, 0x9b, 0x100, 0xffffffff, 0}) | (r() % 2 ? r() << 32 : 0),
                                              pick({0, 1, 5, 100, 0x7fffffff, 0xffffffff, 0x80000000}) | (r() % 2 ? r() << 32 : 0),
                                              r()};
                call_all(argc, argv);
                for (int k = 0; k < nn; k++) rec("%s", npc_tracker_dump(k).c_str());
            }
        });
    }
}

int main(int argc, char **argv) {
    hx::init();
    const int scale = argc > 1 ? std::atoi(argv[1]) : 1;
    set_env("UNCHARTED_U2MP_NETANIM", nullptr);
    install_scenarios();
    stream_scenarios();
    slot_scenarios();
    slot_fuzz(60 * scale, 40);
    natives_install_scenarios();
    natives_scenarios();
    natives_fuzz(20 * scale, 150);
    std::printf("\nnetanim: %d scenarios, %d passed, %d failed; %d driven cases (calls / frames / natives)\n", g_scen, g_pass,
                g_scen - g_pass, g_cases);
    for (const auto &n : g_failed) std::printf("  failed: %s\n", n.c_str());
    return g_scen == g_pass ? 0 : 1;
}
