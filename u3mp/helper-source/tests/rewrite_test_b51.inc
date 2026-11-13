// Row 17: party join wait/fail state and timeout tick.
static JoinStateExt t51_js;
static uint8_t t51_base[0x1900000];
static uint8_t t51_ps[0x1800];
static uint8_t t51_be[0x100];
extern "C" void *u3h_join_state(void) { return &t51_js; }
extern "C" int32_t u3h_gt_conn_state(uintptr_t, int32_t) { return -1; }
extern "C" uint8_t *u3h_gt_entry_by_id(uintptr_t, int32_t) { return nullptr; }
extern "C" int32_t u3h_mq_find(uintptr_t, uint32_t, uint32_t *, int) { return 1; }
extern "C" int32_t u3h_mq_get_np(uintptr_t, uint32_t, uint8_t *, void *, uint32_t *) { return 1; }

static void batch51() {
    uint8_t names[0x14] = {};
    for (int i = 0; i < 16; ++i) names[i] = static_cast<uint8_t>('a' + i);
    uint8_t *ps = t51_ps;
    for (int i = 0; i < 16; ++i) ps[0x680 + 2 * 0x120 + i] = names[i];
    ps[0x680 + 2 * 0x120 + 0x25] = 1;
    CHECK(u3x_fj_member(ps, names) == ps + 0x680 + 2 * 0x120);
    ps[0x680 + 2 * 0x120 + 0x25] = 0;
    CHECK(u3x_fj_member(ps, names) == nullptr);

    t51_js.wait = 1;
    u3x_fj_fail(reinterpret_cast<uintptr_t>(t51_base), nullptr, "x");
    CHECK(t51_js.wait == 0);
    CHECK(reinterpret_cast<uint32_t *>(t27_ext + 0x3c50)[110] == 1);

    uint8_t *bp = reinterpret_cast<uint8_t *>(t51_base);
    uint8_t *sp = bp + 0x17dc298u;
    *reinterpret_cast<uint8_t **>(sp + 8) = t51_be;
    *reinterpret_cast<uint8_t *>(bp + 0x17d5c61u) = 1;
    *reinterpret_cast<uint32_t *>(sp + 0x1338) = 5;
    *reinterpret_cast<uint64_t *>(sp + 0x1340) = 0x1234;
    *reinterpret_cast<uint8_t *>(sp + 0xb8) = 0;
    *reinterpret_cast<uint64_t *>(sp + 0x18) = 0x99;
    t51_js.pj.ph = 0;
    u3x_pj_tick(reinterpret_cast<uintptr_t>(t51_base));
    CHECK(t51_js.pj.ph == 1 && t51_js.pj.id == 0x1234);

    t51_js.pj.t -= 5 * 300;
    u3x_pj_tick(reinterpret_cast<uintptr_t>(t51_base));
    CHECK(t51_js.pj.ph == 2);
    CHECK(*reinterpret_cast<uint32_t *>(sp + 0x1338) == 1);

    *reinterpret_cast<uint32_t *>(sp + 0x1338) = 5;
    *reinterpret_cast<uint8_t *>(sp + 0xb8) = 0;
    *reinterpret_cast<uint64_t *>(sp + 0x1340) = 0x55;
    u3x_pj_giveup(reinterpret_cast<uintptr_t>(t51_base));
    CHECK(t51_js.pj.drop == 1);
    CHECK(t51_js.pj.ph == 0);

    t51_js.timeout = 0;
    t51_js.joined = 1;
    t51_js.seen = 7;
    int32_t ev[2] = {0, 0};
    u3x_fj_cb(reinterpret_cast<uintptr_t>(t51_base), nullptr, ev);
    CHECK(t51_js.joined == 0 && t51_js.seen == 0);
    CHECK(t51_js.timeout != 0);
}
