// row 7 (a020): invite rows, deletion, unseen state and refresh/send bookkeeping.
struct T47State {
    uint64_t time[10];
    uint8_t blob[10][0x30];
    uint8_t newrows[12][0x30];
    int32_t sel, lsel, ticket, dticket, notify, oldn, del;
    uint64_t prev, seen;
};
static_assert(sizeof(T47State) == 0x4a0, "FiState layout");
static T47State t47_state;
static uint8_t t47_mem[0x1c00000];
static uint8_t t47_profile[0x100], t47_record[0x2000];
static uint8_t t47_inv_blob[0x30], t47_inv_target[0x24];
static int32_t t47_inv_result, t47_inv_calls, t47_poll_result;
static uint8_t t47_diag_seen;

extern "C" void *u3h_fi_state(void) { return &t47_state; }
extern "C" int32_t u3h_inv_send(uintptr_t, const uint8_t *target, const uint8_t *blob48) {
    rd_log("inv_send", target ? target[0] : 0, blob48 ? *(const uint64_t *)(blob48 + 0x28) ^ *(const uint32_t *)blob48 : 0);
    ++t47_inv_calls;
    std::memcpy(t47_inv_target, target, 0x24);
    std::memcpy(t47_inv_blob, blob48, 0x30);
    return t47_inv_result;
}
extern "C" int32_t u3h_inv_list(uintptr_t, const uint8_t *me, uint8_t *, uint32_t max) { ++t47_inv_calls; rd_log("inv_list", me ? me[0] : 0, max); return t47_inv_result; }
extern "C" int32_t u3h_inv_delete(uintptr_t, const uint8_t *, const uint8_t *blob48) {
    rd_log("inv_delete", blob48 ? *(const uint64_t *)(blob48 + 0x28) : 0);
    ++t47_inv_calls;
    std::memcpy(t47_inv_blob, blob48, 0x30);
    return t47_inv_result;
}
static int32_t rd_poll_answer[4];
extern "C" int32_t u3h_inv_poll(uintptr_t, int32_t ticket) {
    if (!rd_on) return t47_poll_result;
    rd_log("inv_poll", static_cast<uint32_t>(ticket));
    return rd_poll_answer[ticket & 3];
}
extern "C" void u3h_so_inv_diag(uintptr_t, uint32_t rows, uint32_t kept, uint32_t w1, uint32_t w2, uint32_t w3, uint32_t wf, uint32_t bad, const uint8_t *first, uint32_t fw) {
    rd_log("inv_diag", (uint64_t)rows << 48 | (uint64_t)kept << 32 | w1 << 24 | w2 << 16 | w3 << 8 | wf, (uint64_t)bad << 32 | fw << 16 | (first ? first[0x28] : 0xffff));
    t47_diag_seen = 1;
}

static int t47_strcmp_equal(const void *, const void *) { return 0; }
static uint64_t o47_rec_seen(uintptr_t base, void *s) {
    uint8_t *r = u3h_lg_rec(base, s, u3h_lg_profile(base, 0));
    return (static_cast<uint64_t>(*reinterpret_cast<uint32_t *>(r + 0x1610)) << 32) | *reinterpret_cast<uint32_t *>(r + 0x1614);
}
static uint32_t o47_row_why(uintptr_t base, const uint8_t *b, uint64_t now) {
    const uint32_t key = *reinterpret_cast<const uint32_t *>(b);
    (void)u3h_fr_key(base);
    if ((key & 0xffffff00u) != 0x13000000u && (key & 0xffffff00u) != 0x05000000u) return 1u;
    if (static_cast<int32_t>(now / 1000000) - static_cast<int32_t>(*reinterpret_cast<const uint64_t *>(b + 0x28) / 1000000) >= 0x15180) return 2u;
    const uint8_t *me = reinterpret_cast<const uint8_t *>(base + 0x17d6010u);
    int k = 0;
    while (k < 0x14 && me[k] && me[k] == b[0x10 + k]) ++k;
    if (me[0] && (k == 0x14 || me[k] == b[0x10 + k])) return 3u;
    return 0;
}
static int o47_is_friend(uintptr_t base, const uint8_t *nm20) {
    char nm[0x15];
    std::memcpy(nm, nm20, 0x14);
    nm[0x14] = 0;
    uint8_t *nf = reinterpret_cast<uint8_t *>(base + 0x17caeb0u);
    if (!nm[0] || *reinterpret_cast<uint64_t *>(nf + 0xc8)) return 0;
    for (uint32_t k = 0; k < *reinterpret_cast<uint32_t *>(nf); ++k) {
        uint8_t *x = u3h_fr_ent(base, k);
        if (x[1] && u3h_fr_strcmp(base, x + 1, nm) == 0) return 1;
    }
    return 0;
}
static void o47_delete(uintptr_t base, int32_t i) {
    T47State *st = &t47_state;
    uint8_t *o = reinterpret_cast<uint8_t *>(base + 0x17d5c58u + 0x430);
    if (i >= static_cast<int32_t>(*reinterpret_cast<uint32_t *>(o)) || i < 0) { o[0x198] = 0; return; }
    o[0x198] = 1;
    st->del = i;
    st->dticket = u3h_inv_delete(base, reinterpret_cast<const uint8_t *>(base + 0x17d6010u), st->blob[i]);
    ++reinterpret_cast<uint32_t *>(t27_ext + 0x3c50)[102];
    if (st->dticket < 0) {
        st->dticket = -1;
        if (!*reinterpret_cast<uint32_t *>(o)) *reinterpret_cast<uint32_t *>(o + 4) = 0;
        o[0x198] = 0;
    }
}
static void o47_delete_done(uintptr_t base) {
    T47State *st = &t47_state;
    uint8_t *o = reinterpret_cast<uint8_t *>(base + 0x17d5c58u + 0x430);
    const uint32_t n = *reinterpret_cast<uint32_t *>(o);
    const int32_t i = st->del;
    if (i >= 0 && static_cast<uint32_t>(i) < n) {
        for (uint32_t k = static_cast<uint32_t>(i); k + 1 < n; ++k) {
            std::memcpy(o + 8 + static_cast<uintptr_t>(k) * 0x28, o + 8 + static_cast<uintptr_t>(k + 1) * 0x28, 0x28);
            std::memcpy(st->blob[k], st->blob[k + 1], 0x30);
            st->time[k] = st->time[k + 1];
        }
        *reinterpret_cast<uint32_t *>(o) = n - 1;
    }
    if (!*reinterpret_cast<uint32_t *>(o)) *reinterpret_cast<uint32_t *>(o + 4) = 0;
    o[0x198] = 0;
}
static void o47_mark_seen(uintptr_t base, void *s) {
    T47State *st = &t47_state;
    uint8_t *o = reinterpret_cast<uint8_t *>(base + 0x17d5c58u + 0x430);
    if (o[0x198] || !*reinterpret_cast<uint32_t *>(o)) return;
    if (o47_rec_seen(base, s) < st->time[0]) {
        uint8_t *p = u3h_lg_profile(base, 0);
        uint8_t *r = u3h_lg_rec(base, s, p);
        *reinterpret_cast<uint32_t *>(r + 0x1610) = static_cast<uint32_t>(st->time[0] >> 32);
        r = u3h_lg_rec(base, s, p);
        *reinterpret_cast<uint32_t *>(r + 0x1614) = static_cast<uint32_t>(st->time[0]);
        *reinterpret_cast<uint32_t *>(o + 4) = 0;
        u3h_publish(base, s, p);
        ++reinterpret_cast<uint32_t *>(t27_ext + 0x3c50)[103];
    }
}
static void o47_refresh(uintptr_t base, void *s, int32_t notify) {
    T47State *st = &t47_state;
    uint8_t *o = reinterpret_cast<uint8_t *>(base + 0x17d5c58u + 0x430);
    st->seen = o47_rec_seen(base, s);
    st->oldn = static_cast<int32_t>(*reinterpret_cast<uint32_t *>(o));
    st->prev = st->oldn ? st->time[0] : 0;
    st->notify = notify;
    *reinterpret_cast<uint32_t *>(o + 4) = 0;
    o[0x198] = 1;
    st->ticket = u3h_inv_list(base, reinterpret_cast<const uint8_t *>(base + 0x17d6010u), &st->newrows[0][0], 12);
    ++reinterpret_cast<uint32_t *>(t27_ext + 0x3c50)[99];
}
static void o47_send(uintptr_t base) {
    uint8_t b[0x30] = {};
    uint8_t *ps = reinterpret_cast<uint8_t *>(base + 0x17dc298u);
    *reinterpret_cast<uint32_t *>(b) = u3h_fr_key(base);
    if (ps[0xc8]) *reinterpret_cast<uint64_t *>(b + 8) = *reinterpret_cast<uint64_t *>(ps + 0x18);
    std::memcpy(b + 0x10, reinterpret_cast<const void *>(base + 0x17d6010u), 0x14);
    *reinterpret_cast<uint64_t *>(b + 0x28) = u3x_network_usec();
    if (u3h_inv_send(base, reinterpret_cast<const uint8_t *>(base + 0x17c3871u), b) < 0) ++reinterpret_cast<uint32_t *>(t27_ext + 0x3c50)[106];
    else ++reinterpret_cast<uint32_t *>(t27_ext + 0x3c50)[105];
}
static void t47_reset(uintptr_t base) {
    std::memset(&t47_state, 0, sizeof t47_state);
    std::memset(t47_profile, 0, sizeof t47_profile);
    std::memset(t47_record, 0, sizeof t47_record);
    std::memset(t47_inv_blob, 0, sizeof t47_inv_blob);
    std::memset(t47_inv_target, 0, sizeof t47_inv_target);
    std::memset(reinterpret_cast<void *>(base + 0x17d5c58u + 0x430), 0, 0x200);
    t47_inv_result = t47_inv_calls = t47_poll_result = 0;
    t47_diag_seen = 0;
    t27_fr_key_value = 0x13000074u;
    t27_lg_profile_ptr = t47_profile;
    t27_lg_rec_ptr = t47_record;
    t27_strcmp_fn = t47_strcmp_equal;
    uint8_t *nf = reinterpret_cast<uint8_t *>(base + 0x17caeb0u);
    *reinterpret_cast<uint32_t *>(nf) = 0;
    *reinterpret_cast<uint8_t **>(nf + 8) = t27_ext;
}
static void batch47() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(t47_mem);
    t47_reset(base);
    std::memcpy(reinterpret_cast<void *>(base + 0x17d6010u), "me", 2);
    *reinterpret_cast<uint32_t *>(t47_record + 0x1610) = 0x11111111u;
    *reinterpret_cast<uint32_t *>(t47_record + 0x1614) = 0x22222222u;
    CHECK(o47_rec_seen(base, nullptr) == u3x_fi_rec_seen(base, nullptr));

    uint8_t row[0x30] = {};
    *reinterpret_cast<uint32_t *>(row) = 0x13000074u;
    *reinterpret_cast<uint64_t *>(row + 0x28) = 1000000;
    std::memcpy(row + 0x10, "other", 5);
    CHECK(o47_row_why(base, row, 2000000) == u3x_fi_row_why(base, row, 2000000));
    *reinterpret_cast<uint32_t *>(row) = 0;
    CHECK(o47_row_why(base, row, 2000000) == u3x_fi_row_why(base, row, 2000000));
    *reinterpret_cast<uint32_t *>(row) = 0x13000074u;
    *reinterpret_cast<uint64_t *>(row + 0x28) = 0;
    CHECK(o47_row_why(base, row, 0x1518000000ull) == u3x_fi_row_why(base, row, 0x1518000000ull));
    std::memcpy(row + 0x10, "me", 2);
    CHECK(o47_row_why(base, row, 2000000) == u3x_fi_row_why(base, row, 2000000));

    CHECK(o47_is_friend(base, row + 0x10) == u3x_fi_is_friend(base, row + 0x10));
    *reinterpret_cast<uint8_t *>(base + 0x17caeb0u + 0xc8) = 1;
    CHECK(o47_is_friend(base, row + 0x10) == u3x_fi_is_friend(base, row + 0x10));
    *reinterpret_cast<uint8_t *>(base + 0x17caeb0u + 0xc8) = 0;

    uint8_t *o = reinterpret_cast<uint8_t *>(base + 0x17d5c58u + 0x430);
    *reinterpret_cast<uint32_t *>(o) = 3;
    *reinterpret_cast<uint64_t *>(t47_state.time) = 0x123456789abcdef0ull;
    std::memcpy(t47_state.blob[0], "a", 1);
    std::memcpy(t47_state.blob[1], "b", 1);
    std::memcpy(t47_state.blob[2], "c", 1);
    t47_inv_result = 7;
    o47_delete(base, 1);
    const int32_t old_dt = t47_state.dticket;
    const int32_t old_del = t47_state.del;
    const uint8_t old_flag = o[0x198];
    const uint32_t old_calls = t47_inv_calls;
    t47_state.dticket = t47_state.del = 0;
    o[0x198] = 0;
    t47_inv_calls = 0;
    u3x_fi_delete(base, 1);
    CHECK(t47_state.dticket == old_dt && t47_state.del == old_del && o[0x198] == old_flag && t47_inv_calls == old_calls);

    std::memset(&t47_state, 0, sizeof t47_state);
    *reinterpret_cast<uint32_t *>(o) = 3;
    t47_state.del = 1;
    std::memcpy(t47_state.blob[1], "b", 1);
    std::memcpy(t47_state.blob[2], "c", 1);
    o47_delete_done(base);
    T47State old = t47_state;
    const uint32_t old_o0 = *reinterpret_cast<uint32_t *>(o);
    std::memset(&t47_state, 0, sizeof t47_state);
    *reinterpret_cast<uint32_t *>(o) = 3;
    t47_state.del = 1;
    std::memcpy(t47_state.blob[1], "b", 1);
    std::memcpy(t47_state.blob[2], "c", 1);
    u3x_fi_delete_done(base);
    CHECK(std::memcmp(&old, &t47_state, sizeof old) == 0 && *reinterpret_cast<uint32_t *>(o) == old_o0);

    std::memset(&t47_state, 0, sizeof t47_state);
    std::memset(t47_record, 0, sizeof t47_record);
    *reinterpret_cast<uint32_t *>(o) = 1;
    t47_state.time[0] = 0x123456789abcdef0ull;
    o47_mark_seen(base, nullptr);
    old = t47_state;
    const uint32_t old_seen0 = *reinterpret_cast<uint32_t *>(t47_record + 0x1610);
    const uint32_t old_seen1 = *reinterpret_cast<uint32_t *>(t47_record + 0x1614);
    std::memset(&t47_state, 0, sizeof t47_state);
    std::memset(t47_record, 0, sizeof t47_record);
    *reinterpret_cast<uint32_t *>(o) = 1;
    t47_state.time[0] = 0x123456789abcdef0ull;
    u3x_fi_mark_seen(base, nullptr);
    CHECK(std::memcmp(&old, &t47_state, sizeof old) == 0);
    CHECK(*reinterpret_cast<uint32_t *>(t47_record + 0x1610) == old_seen0 && *reinterpret_cast<uint32_t *>(t47_record + 0x1614) == old_seen1);

    std::memset(&t47_state, 0, sizeof t47_state);
    t47_inv_result = 5;
    t47_inv_calls = 0;
    o47_refresh(base, nullptr, 1);
    old = t47_state;
    const int32_t old_calls2 = t47_inv_calls;
    std::memset(&t47_state, 0, sizeof t47_state);
    t47_inv_calls = 0;
    u3x_fi_refresh(base, nullptr, 1);
    CHECK(std::memcmp(&old, &t47_state, sizeof old) == 0 && t47_inv_calls == old_calls2);

    *reinterpret_cast<uint32_t *>(t27_ext + 0x3c50 + 105 * 4) = 0;
    *reinterpret_cast<uint32_t *>(t27_ext + 0x3c50 + 106 * 4) = 0;
    t47_inv_result = 0;
    o47_send(base);
    const uint32_t old105 = *reinterpret_cast<uint32_t *>(t27_ext + 0x3c50 + 105 * 4);
    const uint32_t old106 = *reinterpret_cast<uint32_t *>(t27_ext + 0x3c50 + 106 * 4);
    const uint8_t old_send_blob[0x30] = {};
    CHECK(old_send_blob[0] == 0 && old105 == 1 && old106 == 0);
    uint8_t saved[0x30];
    std::memcpy(saved, t47_inv_blob, sizeof saved);
    std::memcpy(t47_inv_blob, old_send_blob, sizeof old_send_blob);
    *reinterpret_cast<uint32_t *>(t27_ext + 0x3c50 + 105 * 4) = 0;
    t47_inv_result = 0;
    u3x_fi_send(base);
    CHECK(std::memcmp(saved, t47_inv_blob, sizeof saved) == 0);
    CHECK(*reinterpret_cast<uint32_t *>(t27_ext + 0x3c50 + 105 * 4) == 1);

    std::memset(&t47_state, 0, sizeof t47_state);
    *reinterpret_cast<uint32_t *>(o) = 3;
    t47_state.ticket = -1;
    t47_state.dticket = 3;
    t47_state.del = 1;
    std::memcpy(t47_state.blob[1], "b", 1);
    std::memcpy(t47_state.blob[2], "c", 1);
    o[0x198] = 1;
    t47_poll_result = 0;
    u3x_fi_tick(base, nullptr);
    CHECK(t47_state.dticket == -1 && *reinterpret_cast<uint32_t *>(o) == 2 && o[0x198] == 0);

    std::memset(&t47_state, 0, sizeof t47_state);
    *reinterpret_cast<uint32_t *>(o) = 0;
    o[0x198] = 0;
    t47_state.ticket = 3;
    t47_state.dticket = -1;
    t47_state.notify = 1;
    std::memset(t47_state.newrows[0], 0, 0x30);
    *reinterpret_cast<uint32_t *>(t47_state.newrows[0]) = 0x13000074u;
    *reinterpret_cast<uint64_t *>(t47_state.newrows[0] + 0x28) = 1;
    std::memcpy(t47_state.newrows[0] + 0x10, "other", 5);
    t47_poll_result = 1;
    u3x_fi_tick(base, nullptr);
    CHECK(t47_state.ticket == -1 && *reinterpret_cast<uint32_t *>(o) == 1 && *reinterpret_cast<uint32_t *>(o + 4) == 1);
}
