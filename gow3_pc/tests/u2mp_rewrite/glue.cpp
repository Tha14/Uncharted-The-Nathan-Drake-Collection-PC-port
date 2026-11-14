// u2mp rewrite tests: what install.cpp provides to a module (image, log, replace_function ...), for the fake image.
// Compiled twice by run.sh: for the new code, and with -Du2mp=u2mp_old -DHX_GLUE=hx::glue_old for the old code.
#include "u2mp_internal.h"
#include "harness.h"

#ifndef HX_GLUE
#define HX_GLUE hx::glue_new
#endif

namespace u2mp {

Image image;

void log(const char *format, ...) {
    char b[1536];
    va_list a;
    va_start(a, format);
    std::vsnprintf(b, sizeof b, format, a);
    va_end(a);
    hx::rec("log %s", b);
}

bool Image::executable(uint64_t offset, uint64_t length) const { return hx::executable(offset, length); }
bool Image::matches(uint64_t offset, const unsigned char *bytes, size_t length) const {
    return offset <= size && length <= size - offset && std::memcmp(base + offset, bytes, length) == 0;
}

unsigned char *near_stub(size_t bytes) { return hx::exec_alloc(bytes); }

bool replace_function(uint64_t offset, unsigned length, void *host, const char *name) {
    hx::rec("replace_function 0x%llx %u %s", (unsigned long long)offset, length, name ? name : "");
    if (hx::fails_at(offset)) {
        hx::rec("replace_function 0x%llx failed (test)", (unsigned long long)offset);
        return false;
    }
    hx::stub(offset, host);
    return true;
}

bool retarget_call(uint64_t offset, uint64_t expected_target, void *host, const char *name) {
    hx::rec("retarget_call 0x%llx 0x%llx %s", (unsigned long long)offset, (unsigned long long)expected_target, name ? name : "");
    if (hx::fails_at(offset)) {
        hx::rec("retarget_call 0x%llx failed (test)", (unsigned long long)offset);
        return false;
    }
    hx::note_retarget(offset, host);
    return true;
}

namespace {
void bind() {
    image.base = hx::mem;
    image.size = hx::kImageSize;
}
void set(uint8_t *base, uint64_t size) {
    image.base = base;
    image.size = size;
}
struct Register {
    Register() {
        HX_GLUE.bind_image = bind;
        HX_GLUE.set_image = set;
    }
} registered;
}  // namespace

}  // namespace u2mp
