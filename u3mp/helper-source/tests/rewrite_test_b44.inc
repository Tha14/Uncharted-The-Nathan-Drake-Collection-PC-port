// row 23 (c008): the anim packer's early exits, plus its empty command stream with the game calls stubbed.
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#endif
static uint8_t t44_frame[2][0x60], t44_loc[0x40], t44_cmd[0x8000];
static int t44_cmd_null = 1;

extern "C" {
uint8_t *u3h_anim_cmd_buffer(void) { return t44_cmd_null ? nullptr : t44_cmd; }
uint8_t *u3h_repl_frame(int i) { return t44_frame[i & 1]; }
uint8_t *u3h_repl_loc(void) { return t44_loc; }
uint8_t *u3h_handle_obj(uint8_t *h) { return h && *reinterpret_cast<uint8_t **>(h) ? *reinterpret_cast<uint8_t **>(h) : nullptr; }
uint8_t *u3h_cam_mgr(uintptr_t, int32_t) { return nullptr; }
void u3h_loop_src119(uint8_t *, uint32_t) {}
int u3h_layer_counts(uint32_t) { return 1; }
}

static void batch44() {
    alignas(16) static uint8_t mem[0x200];
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem) - 0x16e79f0u;
    std::memset(mem, 0x5a, sizeof mem);
    uint32_t *ec = reinterpret_cast<uint32_t *>(t27_ext + 0x4450);
    alignas(16) uint8_t actor[0x200] = {}, msg[0x240] = {}, bs[0x40] = {};
    uint8_t *tabs[8] = {};
    uint8_t *buf = nullptr;
    uint8_t ctrl[0x20] = {};

    ec[66] = ec[74] = 0;
    CHECK(u3x_anim_pack_body(base, msg, bs, nullptr, nullptr, 0, 0, &buf) == 0);
    CHECK(ec[66] == 1);
    CHECK(mem[0] == 0x5a);

    ec[66] = ec[74] = 0;
    CHECK(u3x_anim_pack_body(base, msg, bs, actor, nullptr, 0, 0, &buf) == 0);
    CHECK(ec[66] == 1);

    *(uint8_t **)(actor + 0x138) = nullptr;
    mem[0] = 0x5a;
    CHECK(u3x_anim_pack_body(base, msg, bs, actor, tabs, 0, 0, &buf) == 0);
    CHECK(mem[0] == 0);

    *(uint8_t **)(actor + 0x138) = ctrl;
    mem[0] = 0x5a;
    ec[74] = 0;
    CHECK(u3x_anim_pack_body(base, msg, bs, actor, tabs, 0, 0, &buf) == 0);
    CHECK(ec[74] == 1);
    CHECK(mem[0] == 0);

#if defined(__linux__) && defined(__x86_64__)
    constexpr size_t map_size = 0x1800000;
    void *m = mmap(nullptr, map_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return;
    uint8_t *mb = static_cast<uint8_t *>(m);
    const uint8_t create_cmds[] = {0xc7, 0x46, 0x0c, 0, 0, 0, 0, 0xc3};
    const uint8_t ret_zero[] = {0x31, 0xc0, 0xc3};
    std::memcpy(mb + 0xfbe270, create_cmds, sizeof create_cmds);
    std::memcpy(mb + 0x1197380, ret_zero, sizeof ret_zero);
    mb[0x7befc0] = 0xc3;
    mb[0x16e79f0] = 0x5a;
    t44_cmd_null = 0;
    uint32_t words[8] = {}, expect[8] = {};
    std::memset(bs, 0, sizeof bs);
    *(uint32_t **)bs = words;
    *(uint32_t *)(bs + 0xc) = 0x100;
    *(uint32_t *)(bs + 8) = 0;
    uint8_t eb[0x20] = {};
    *(uint32_t **)eb = expect;
    *(uint32_t *)(eb + 0xc) = 0x100;
    uint32_t ee = 0;
    old_bsw43(eb, 0, 1, &ee);
    old_bsw43(eb, 0, 1, &ee);
    old_bsw43(eb, 0, 5, &ee);
    old_bsw43(eb, 1, 1, &ee);
    old_bsw43(eb, 1, 1, &ee);
    old_bsw43(eb, 0, 3, &ee);
    old_bsw43(eb, 3, 2, &ee);
    old_bsw43(eb, 1, 3, &ee);
    CHECK(ee == 0);
    CHECK(*(uint32_t *)(eb + 8) == 17);
    CHECK(u3x_anim_pack_body(reinterpret_cast<uintptr_t>(mb), msg, bs, actor, tabs, 0, 0, &buf) == 17);
    CHECK(*(uint32_t *)(bs + 8) == 17);
    CHECK(mb[0x16e79f0] == 0);
    CHECK(buf == t44_cmd);
    CHECK(std::memcmp(words, expect, sizeof words) == 0);

    const uint8_t create_one[] = {0xc7, 0x46, 0x0c, 1, 0, 0, 0, 0xc3};
    std::memcpy(mb + 0xfbe270, create_one, sizeof create_one);
    ((uint32_t *)t44_cmd)[0] = 0x0001000du;
    uint32_t words2[8] = {}, expect2[8] = {};
    std::memset(bs, 0, sizeof bs);
    *(uint32_t **)bs = words2;
    *(uint32_t *)(bs + 0xc) = 0x100;
    *(uint32_t *)(bs + 8) = 0;
    std::memset(eb, 0, sizeof eb);
    *(uint32_t **)eb = expect2;
    *(uint32_t *)(eb + 0xc) = 0x100;
    ee = 0;
    old_bsw43(eb, 0, 1, &ee);
    old_bsw43(eb, 0, 1, &ee);
    old_bsw43(eb, 0, 5, &ee);
    old_bsw43(eb, 1, 1, &ee);
    old_bsw43(eb, 1, 1, &ee);
    old_bsw43(eb, 0, 3, &ee);
    old_bsw43(eb, 3, 2, &ee);
    old_bsw43(eb, 3, 3, &ee);
    old_bsw43(eb, 3, 2, &ee);
    old_bsw43(eb, 1, 3, &ee);
    CHECK(ee == 0);
    CHECK(*(uint32_t *)(eb + 8) == 22);
    mb[0x16e79f0] = 0x5a;
    CHECK(u3x_anim_pack_body(reinterpret_cast<uintptr_t>(mb), msg, bs, actor, tabs, 0, 1, &buf) == 22);
    CHECK(*(uint32_t *)(bs + 8) == 22);
    CHECK(mb[0x16e79f0] == 0);
    CHECK(std::memcmp(words2, expect2, sizeof words2) == 0);
    munmap(m, map_size);
    t44_cmd_null = 1;
#endif
}
