// u3fix_hook.h: what the u3fix fixes share: finding code by its bytes, stub pages within jump range of the image, jumps.
#pragma once
#include <stdint.h>

namespace u3fix {

constexpr uint64_t kNotFound = ~uint64_t{0};

// offset of the only place the bytes occur in the image; kNotFound when none or more than one
uint64_t find_unique(const unsigned char *image, uint64_t size, const unsigned char *bytes, uint64_t length);
// a read-write page within rel32 range of the image (nullptr when none is free)
unsigned char *stub_page_near(const unsigned char *image, uint64_t size);
// makes the stub page read-execute
bool finish_stub(unsigned char *stub);
// writes jmp rel32 to stub at image+at, the rest of the replaced length as nops (the image is still writable then)
bool jump_to(unsigned char *image, uint64_t at, unsigned length, const unsigned char *stub);

bool raise_net_process_pool(unsigned char *image, uint64_t size);
bool install_particle_jobs_wait(unsigned char *image, uint64_t size);

}  // namespace u3fix
