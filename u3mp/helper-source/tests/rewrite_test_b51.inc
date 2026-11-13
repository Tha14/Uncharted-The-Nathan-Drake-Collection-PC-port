// Row 17: party join wait/fail state and timeout tick.
static rw::JoinState t51_js;
static uint8_t t51_base[0x1900000];
static uint8_t t51_ps[0x1800];
static uint8_t t51_be[0x100];
extern "C" void *u3h_join_state(void) { return &t51_js; }
// differential tests (rd17): settable answers, logged
static int32_t rd_conn_state = -1, rd_mq_find_ret = 1, rd_mq_get_ret = 1;
static uint8_t *rd_conn_entry = nullptr;
static uint8_t rd_mq_from[0x24], rd_mq_data[0x10];
extern "C" int32_t u3h_gt_conn_state(uintptr_t, int32_t id) { rd_log("conn_state", static_cast<uint32_t>(id)); return rd_on ? rd_conn_state : -1; }
extern "C" uint8_t *u3h_gt_entry_by_id(uintptr_t, int32_t id) { rd_log("conn_entry", static_cast<uint32_t>(id)); return rd_on ? rd_conn_entry : nullptr; }
extern "C" int32_t u3h_mq_find(uintptr_t, uint32_t type, uint32_t *idx, int remove) {
    rd_log("mq_find", type, static_cast<uint32_t>(remove));
    if (!rd_on) return 1;
    *idx = 3;
    return rd_mq_find_ret;
}
extern "C" int32_t u3h_mq_get_np(uintptr_t, uint32_t idx, uint8_t *from, void *data, uint32_t *len) {
    rd_log("mq_get", idx, *len);
    if (!rd_on) return 1;
    std::memcpy(from, rd_mq_from, sizeof rd_mq_from);
    std::memcpy(data, rd_mq_data, *len < 0x10 ? *len : 0x10);
    return rd_mq_get_ret;
}

static void batch51() {
    uint8_t names[0x14] = {};
    for (int i = 0; i < 16; ++i) names[i] = static_cast<uint8_t>('a' + i);
    uint8_t *ps = t51_ps;
    for (int i = 0; i < 16; ++i) ps[0x680 + 2 * 0x120 + i] = names[i];
    ps[0x680 + 2 * 0x120 + 0x25] = 1;
    CHECK(u3x_fj_member(ps, names) == ps + 0x680 + 2 * 0x120);
    ps[0x680 + 2 * 0x120 + 0x25] = 0;
    CHECK(u3x_fj_member(ps, names) == nullptr);

    t51_js.waiting = 1;
    u3x_fj_fail(reinterpret_cast<uintptr_t>(t51_base), nullptr, "x");
    CHECK(t51_js.waiting == 0);
    CHECK(reinterpret_cast<uint32_t *>(t27_ext + 0x3c50)[110] == 1);

    uint8_t *bp = reinterpret_cast<uint8_t *>(t51_base);
    uint8_t *sp = bp + 0x17dc298u;
    *reinterpret_cast<uint8_t **>(sp + 8) = t51_be;
    *reinterpret_cast<uint8_t *>(bp + 0x17d5c61u) = 1;
    *reinterpret_cast<uint32_t *>(sp + 0x1338) = 5;
    *reinterpret_cast<uint64_t *>(sp + 0x1340) = 0x1234;
    *reinterpret_cast<uint8_t *>(sp + 0xb8) = 0;
    *reinterpret_cast<uint64_t *>(sp + 0x18) = 0x99;
    t51_js.pending.phase = 0;
    u3x_pj_tick(reinterpret_cast<uintptr_t>(t51_base));
    CHECK(t51_js.pending.phase == 1 && t51_js.pending.id == 0x1234);

    t51_js.pending.since -= 5 * 300;
    u3x_pj_tick(reinterpret_cast<uintptr_t>(t51_base));
    CHECK(t51_js.pending.phase == 2);
    CHECK(*reinterpret_cast<uint32_t *>(sp + 0x1338) == 1);

    *reinterpret_cast<uint32_t *>(sp + 0x1338) = 5;
    *reinterpret_cast<uint8_t *>(sp + 0xb8) = 0;
    *reinterpret_cast<uint64_t *>(sp + 0x1340) = 0x55;
    u3x_pj_giveup(reinterpret_cast<uintptr_t>(t51_base));
    CHECK(t51_js.pending.dropped == 1);
    CHECK(t51_js.pending.phase == 0);

    t51_js.timeout = 0;
    t51_js.answered = 1;
    t51_js.seen = 7;
    int32_t ev[2] = {0, 0};
    u3x_fj_cb(reinterpret_cast<uintptr_t>(t51_base), nullptr, ev);
    CHECK(t51_js.answered == 0 && t51_js.seen == 0);
    CHECK(t51_js.timeout != 0);
}
