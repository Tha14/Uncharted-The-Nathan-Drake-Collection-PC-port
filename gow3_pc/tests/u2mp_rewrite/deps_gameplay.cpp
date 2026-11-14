// u2mp rewrite tests, gameplay group: what the seven sources need from other modules, compiled for both namespaces.
// package_by_id (session.cpp) is recorded and answered by the test (test_gameplay.cpp: gp_package_by_id).
#include "u2mp_internal.h"
#include "harness.h"

extern const uint8_t *(*gp_package_by_id)(uint32_t id);

namespace u2mp {

const uint8_t *package_by_id(uint32_t id) {
    hx::rec("package_by_id %08x", id);
    return gp_package_by_id ? gp_package_by_id(id) : nullptr;
}

}  // namespace u2mp
