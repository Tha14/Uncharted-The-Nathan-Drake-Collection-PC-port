// u2mp rewrite tests, group "progression" (progression.cpp, netinit.cpp, netpatch.cpp): stand-ins for what the
// sources call in other modules (online.cpp, unlocks.cpp, netheap.cpp, the C runtime's file functions), plus the
// unchanged progression_store.cpp (compiled into each namespace, so each build has its own copy, as in the game).
// Compiled once per namespace by run.sh; every call is recorded and answered from hx::pg::st (deps_progression.h).
// The C functions get the OLD_FLAGS renames (..._old) for the old build.
#include "../../src/u2mp/progression_store.cpp"

#include "online.h"
#include "u2mp_internal.h"
#include "unlocks.h"

#include "deps_progression.h"
#include "harness.h"

using hx::pg::st;

namespace u2mp {

void unlocks_refresh() { hx::rec("unlocks_refresh"); }

void *netheap_alloc(size_t size, size_t align) {
    const int call = st.heap_calls++;
    if (call < 64 && ((st.heap_fail_mask >> call) & 1)) {
        hx::rec("netheap_alloc %zu %zu -> NULL", size, align);
        return nullptr;
    }
    const uint64_t a = align ? align : 1;
    const uint64_t off = (st.heap_next + a - 1) / a * a;
    if (off + size > st.heap_end) {
        hx::rec("netheap_alloc %zu %zu -> NULL (full)", size, align);
        return nullptr;
    }
    st.heap_next = off + size;
    hx::rec("netheap_alloc %zu %zu -> img+0x%llx", size, align, (unsigned long long)off);
    return hx::mem + off;
}

namespace online {

void member_data_changed() { hx::rec("online::member_data_changed"); }
bool linked() {
    hx::rec("online::linked -> %d", int(st.linked));
    return st.linked;
}
bool board_request(uint32_t board, int mode, uint32_t start, uint32_t count) {
    bool r = st.request_default;
    if (!st.request_results.empty()) {
        r = st.request_results.front();
        st.request_results.pop_front();
    }
    hx::rec("online::board_request %u %d %u %u -> %d", board, mode, start, count, int(r));
    return r;
}
int board_state() {
    hx::rec("online::board_state -> %d", st.board_state);
    return st.board_state;
}
int board_rows() {
    hx::rec("online::board_rows -> %d", st.rows);
    return st.rows;
}
uint32_t board_total() {
    hx::rec("online::board_total -> %u", st.total);
    return st.total;
}
const U2oScore *board_row(int i) {
    const bool ok = i >= 0 && size_t(i) < st.scores.size() && !(size_t(i) < st.null_row.size() && st.null_row[size_t(i)]);
    hx::rec("online::board_row %d -> %s", i, ok ? "row" : "NULL");
    return ok ? &st.scores[size_t(i)] : nullptr;
}
std::string my_npid_text() {
    hx::rec("online::my_npid_text -> [%s]", st.my_npid.c_str());
    return st.my_npid;
}
bool record_score(uint32_t board, int64_t score, const char *comment, const uint8_t *info, uint32_t info_len) {
    std::string hex;
    char b[4];
    for (uint32_t i = 0; info && i < info_len; ++i) {
        std::snprintf(b, sizeof b, "%02x", info[i]);
        hex += b;
    }
    hx::rec("online::record_score %u %lld comment=%s info_len=%u info=%s", board, (long long)score,
            comment ? comment : "(NULL)", info_len, info ? hex.c_str() : "(NULL)");
    return true;
}

}  // namespace online
}  // namespace u2mp

extern "C" const char *runtime_file_user_dir(void) {
    hx::rec("runtime_file_user_dir -> %s", st.user_dir_null ? "(NULL)" : ("[" + st.user_dir + "]").c_str());
    return st.user_dir_null ? nullptr : st.user_dir.c_str();
}

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size) {
    hx::rec("runtime_file_translate [%s] %zu -> %d", guest ? guest : "(NULL)", size, st.translate_result);
    if (st.translate_result == 0 && out && size) std::snprintf(out, size, "%s", st.translate_out.c_str());
    return st.translate_result;
}
