// u2mp rewrite tests: the old and the new version of a u2mp source file run against the same fake game image.
//
// The old version is taken from git (run.sh) and compiled with -Du2mp=u2mp_old, so both live in one test program.
// Both see the same fake image: hx::mem, kImageSize bytes of executable memory standing for the game's image (image
// offsets are offsets into it). A guest function the code calls is a stub at its offset that jumps to a host function
// (U2MP_GUEST, System V like the game); the test's host functions record what they were called with (hx::rec) and
// return what the scenario wants. A scenario runs the old code, keeps the record and the image, restores the image,
// runs the new code and compares (hx::same).
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef U2MP_GUEST
#define U2MP_GUEST __attribute__((sysv_abi))
#endif

namespace hx {

constexpr uint64_t kImageSize = 0x1800000;   // past the last 1.02 image offset the module uses
extern uint8_t *mem;                         // the fake image (executable, zeroed by init / clear)
extern std::vector<std::string> calls;       // what the code under test did, in order
extern bool recording;

void init();    // once: allocate the fake image
void clear();   // zero the image, forget stubs, the record and both modules' image / log state

// a guest function at `off` that jumps to `host` (13 bytes: movabs r11, host; jmp r11)
void stub(uint64_t off, void *host);
// a guest vtable slot / pointer slot holding a pointer to a stub of `host` placed at `stub_off`
void stub_ptr(uint64_t slot_off, uint64_t stub_off, void *host);
// executable bytes outside the image (near_stub of the modules), from a pool reset by clear()
unsigned char *exec_alloc(size_t n);
// opt-in, reset by clear(): exec_alloc returns NULL once this many allocations were given (-1 = no limit)
extern long exec_left;
// opt-in, reset by clear(): exec_alloc call number i (0-based since clear) returns NULL when bit i is set;
// exec_calls counts the calls (also the failed ones)
extern uint64_t exec_fail_mask;
extern int exec_calls;
// opt-in, reset by clear(): replace_function / retarget_call at these offsets fail (recorded, return false)
extern std::vector<uint64_t> fail_offsets;
bool fails_at(uint64_t off);
// opt-in, reset by clear(): Image::executable is false for a range overlapping one of these {offset, length}
extern std::vector<std::pair<uint64_t, uint64_t>> noexec;
bool executable(uint64_t off, uint64_t len);
// the host function the last retarget_call at `off` was given (glue.cpp notes it), NULL when none
void note_retarget(uint64_t off, void *host);
void *retarget_host(uint64_t off);

void rec(const char *format, ...) __attribute__((format(printf, 1, 2)));
// a stable value for a host pointer: an image offset ("img+0x..."), or a hash of the bytes it points at
std::string where(const void *p);

template <typename T> T get(uint64_t off) {
    T v;
    std::memcpy(&v, mem + off, sizeof v);
    return v;
}
template <typename T> void put(uint64_t off, T v) { std::memcpy(mem + off, &v, sizeof v); }
inline uint64_t addr(uint64_t off) { return uint64_t(uintptr_t(mem + off)); }   // a guest pointer to image + off

std::vector<uint8_t> snapshot();
void restore(const std::vector<uint8_t> &s);

// both modules' glue state, set by glue.cpp (one copy per namespace)
struct Glue {
    void (*bind_image)() = nullptr;   // points the module's `image` at hx::mem
    void (*set_image)(uint8_t *base, uint64_t size) = nullptr;   // any base / size (e.g. a too small image)
};
extern Glue glue_old, glue_new;
void bind_both();
void set_image_both(uint8_t *base, uint64_t size);   // both modules' `image` = {base, size}

// the comparison of one scenario: old and new records equal, images equal (first difference printed)
extern int failures;
bool same(const char *scenario, const std::vector<std::string> &a, const std::vector<std::string> &b,
          const std::vector<uint8_t> *img_a = nullptr, const std::vector<uint8_t> *img_b = nullptr);

}  // namespace hx
