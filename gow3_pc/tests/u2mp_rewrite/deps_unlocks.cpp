// u2mp rewrite test "unlocks": what unlocks.cpp needs from other modules, compiled for both builds (run.sh).
// Every stand-in forwards to the test (test_unlocks.cpp, namespace ul), which records the call (hx::rec) and
// answers from the scenario's state. runtime_file_translate is renamed for the old build (flags_unlocks.sh).
#include <cstddef>
#include <cstdint>

#include "u2mp_internal.h"

namespace ul {
void *netheap_alloc(size_t size, size_t align);
const uint8_t *package_by_id(uint32_t id);
int translate(const char *guest, char *out, size_t size);
}  // namespace ul

namespace u2mp {
void *netheap_alloc(size_t size, size_t align) { return ul::netheap_alloc(size, align); }
const uint8_t *package_by_id(uint32_t id) { return ul::package_by_id(id); }
}  // namespace u2mp

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size) { return ul::translate(guest, out, size); }
