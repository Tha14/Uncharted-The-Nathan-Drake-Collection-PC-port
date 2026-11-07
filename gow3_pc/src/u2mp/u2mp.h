/* u2mp: Uncharted 2 multiplayer restoration (C++, built into the runtime). See u2mp/README.md.
 *
 * The C runtime calls u2mp_install once, after the image is linked and before it is protected,
 * when the Uncharted 2 executable is loaded. It does nothing unless UNCHARTED_U2MP=1 (set by the
 * launcher) and the image is the Uncharted 2 1.02 build it was written for (byte checks at every
 * address it touches). */
#ifndef U2MP_H
#define U2MP_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* image: the guest image base; size: its size; segments: count triples {address, size, flags}
 * (flags bit 0 = executable). Returns the number of functions installed (0 when off or not
 * this build). */
int u2mp_install(unsigned char *image, uint64_t size, const uint64_t *segments, uint64_t count);

#ifdef __cplusplus
}
#endif
#endif
