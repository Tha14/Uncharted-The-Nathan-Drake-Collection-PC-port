/* u3fix: Uncharted 3 fixes in C++, built into the runtime (Windows build: gpu/CMakeLists.txt compiles every .cpp file in src/u3fix).
 * The C runtime calls u3fix_install once, after the image is linked and before the game runs. Every fix finds its place
 * by bytes and does nothing in an image that is not the Uncharted 3 1.02 build it was written for. */
#ifndef U3FIX_H
#define U3FIX_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* image: the guest image base; size: its size. Returns the number of fixes applied. */
int u3fix_install(unsigned char *image, uint64_t size);

#ifdef __cplusplus
}
#endif
#endif
