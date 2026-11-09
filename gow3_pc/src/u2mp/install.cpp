// u2mp: identity checks and the function-replacement mechanism.
#include "u2mp.h"
#include "events.h"
#include "u2mp_internal.h"
#include "progression.h"
#include "cinema.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace u2mp {

Image image;

void log(const char *format, ...) {
    std::va_list args;
    va_start(args, format);
    std::fputs("u2mp: ", stdout);
    std::vprintf(format, args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    va_end(args);
}

bool Image::executable(uint64_t offset, uint64_t length) const {
    for (uint64_t i = 0; i < segment_count; ++i) {
        const uint64_t *s = segments + 3 * i;
        if ((s[2] & 1) && offset >= s[0] && length <= s[1] && offset - s[0] <= s[1] - length) return true;
    }
    return false;
}

bool Image::matches(uint64_t offset, const unsigned char *bytes, size_t length) const {
    return offset <= size && length <= size - offset && std::memcmp(base + offset, bytes, length) == 0;
}

// Executable memory within rel32 range of the image, carved from shared 64 KiB blocks. The
// near-image space is small (test run 23: one 64 KiB block per redirected call used it up and
// 63 functions, the Multiplayer button among them, were not installed).
unsigned char *near_stub(size_t bytes) {
    static unsigned char *pool = nullptr;
    static size_t used = 0;
    constexpr size_t pool_size = 0x10000;
    bytes = (bytes + 15) & ~size_t(15);
    if (bytes > pool_size) return nullptr;
    if (!pool || used + bytes > pool_size) {
        pool = runtime_near_image_alloc(pool_size, 1);
        used = 0;
        if (!pool) return nullptr;
    }
    unsigned char *p = pool + used;
    used += bytes;
    return p;
}

bool replace_function(uint64_t offset, unsigned length, void *host, const char *name) {
    if (length < 5 || !image.executable(offset, length)) {
        log("%s at 0x%llx: not in the executable segments, left alone", name, (unsigned long long)offset);
        return false;
    }
    // A stub next to the image (a rel32 jump reaches it): mov rax, host; jmp rax.
    unsigned char *stub = near_stub(16);
    if (!stub) {
        log("%s: no stub memory near the image", name);
        return false;
    }
    const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + offset + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) {
        log("%s: stub out of jump range", name);
        return false;
    }
    const uint64_t target = uint64_t(uintptr_t(host));
    stub[0] = 0x48;
    stub[1] = 0xb8;
    std::memcpy(stub + 2, &target, 8);
    stub[10] = 0xff;
    stub[11] = 0xe0;
    unsigned char *code = image.base + offset;
    const int32_t relative = int32_t(delta);
    code[0] = 0xe9;
    std::memcpy(code + 1, &relative, 4);
    std::memset(code + 5, 0xcc, length - 5);
    log("%s: 0x%llx -> C++", name, (unsigned long long)offset);
    return true;
}

bool retarget_call(uint64_t offset, uint64_t expected_target, void *host, const char *name) {
    if (!image.executable(offset, 5) || image.base[offset] != 0xe8) {
        log("%s at 0x%llx: not a call, left alone", name, (unsigned long long)offset);
        return false;
    }
    int32_t old = 0;
    std::memcpy(&old, image.base + offset + 1, 4);
    if (offset + 5 + int64_t(old) != int64_t(expected_target)) {
        log("%s at 0x%llx: calls something else, left alone", name, (unsigned long long)offset);
        return false;
    }
    // Reuse the function-replacement stub: a jump stub to `host` written at a scratch location is
    // not needed; build one directly.
    unsigned char *stub = near_stub(16);
    if (!stub) {
        log("%s: no stub memory near the image", name);
        return false;
    }
    const uint64_t target = uint64_t(uintptr_t(host));
    stub[0] = 0x48;
    stub[1] = 0xb8;
    std::memcpy(stub + 2, &target, 8);
    stub[10] = 0xff;
    stub[11] = 0xe0;
    const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + offset + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    const int32_t relative = int32_t(delta);
    std::memcpy(image.base + offset + 1, &relative, 4);
    log("%s: call at 0x%llx -> C++", name, (unsigned long long)offset);
    return true;
}

namespace {

// Bytes this build must have where u2mp reads or writes (Uncharted 2 1.02, CUSA02320/CUSA02344).
bool is_supported_build() {
    static const unsigned char show_page[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                              0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00,
                                              0x41, 0x89, 0xd5, 0x49, 0x89, 0xfe};
    static const unsigned char reg_multiplayer[] = {0xc7, 0x84, 0xc3, 0xf0, 0x18, 0x00, 0x00,
                                                    0xe1, 0x8a, 0x9d, 0xed};
    // the stripped callback: ret, then the alignment padding up to the next function
    static const unsigned char ret_stub[] = {0xc3, 0x66, 0x66, 0x66, 0x66};
    return image.matches(addr::show_page, show_page, sizeof(show_page)) &&
           image.matches(addr::reg_multiplayer, reg_multiplayer, sizeof(reg_multiplayer)) &&
           image.matches(addr::cb_multiplayer, ret_stub, sizeof(ret_stub)) &&
           addr::menu_manager + 0x1a00 <= image.size;
}

}  // namespace
}  // namespace u2mp

extern "C" int u2mp_install(unsigned char *base, uint64_t size, const uint64_t *segments, uint64_t count) {
    using namespace u2mp;
    const char *on = std::getenv("UNCHARTED_U2MP");
    if (!on || std::strcmp(on, "1") != 0) return 0;
    image = Image{base, size, segments, count};
    if (!is_supported_build()) {
        log("not the Uncharted 2 1.02 executable: multiplayer restoration off");
        return 0;
    }
    int installed = 0;
    install_crashlog();
    install_frontend(installed);
    install_events(installed);
    install_gameplay2(installed);
    install_gameplay3(installed);
    install_natives(installed);
    install_progression(installed);
    install_cinema(installed);
    install_remaining(installed);
    install_menus(installed);
    install_menuimages(installed);
    install_unlocks(installed);
    install_loading(installed);
    install_dlc(installed);
    install_menudraw(installed);
    install_frontend2(installed);
    install_frontend3(installed);
    install_frontend6(installed);
    install_spawn(installed);
    install_nullsites(installed);
    install_scripttime(installed);
    install_hud(installed);
    install_scoreboard(installed);   // after install_loading: chains its 2D draw call
    install_matchend(installed);     // after install_frontend: wraps session.cpp's Playing slot
    install_visibility(installed);   // per-frame call comes from matchend
    install_netanim(installed);
    install_netpatch(installed);   // after install_cinema: only a fallback when cinema's stream hook is absent
    log("%d functions restored", installed);
    return installed;
}
