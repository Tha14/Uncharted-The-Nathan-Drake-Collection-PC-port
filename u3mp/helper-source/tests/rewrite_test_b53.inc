// Row 21: respawn/spawn helpers and their native-call wrappers.
static uint8_t t53_base[0x1d00000];
static uint8_t t53_state[0x3000];
static uint8_t t53_p[0x800];
static uint8_t t53_h[0x20], t53_obj[0x100], t53_obj2[0x100];
static void *t53_table_slot;
static uint8_t t53_table[0x40], t53_entries[0x200];
static uint8_t *t53_handle_slot;
static uint8_t t53_node[0x40], t53_b[0x10];
static void *t53_dc[] = {t53_node, nullptr};
static int t53_effect(uint8_t *, uint32_t, void *) { return 0; }
static float t53_delay(uint8_t *) { return 1.25f; }
static void **t53_lookup(uint32_t) { return &t53_table_slot; }
static void t53_pending(uintptr_t, void *out) { *reinterpret_cast<uint32_t *>(out) = 0; }
static void *t53_find(uintptr_t, uint32_t) { return nullptr; }
static void t53_clear(uintptr_t, void *) {}
static int t53_locator(uint8_t *, void *, uint32_t, uint32_t) { return 1; }

static void batch53() {
    uint8_t *bp = t53_base;
    t53_handle_slot = t53_obj2;
    *reinterpret_cast<uint8_t ***>(t53_h) = &t53_handle_slot;
    *reinterpret_cast<uint32_t *>(t53_h + 8) = 7;
    *reinterpret_cast<uint32_t *>(t53_obj2 + 0x7c) = 7;
    CHECK(u3x_handle_obj_valid(t53_h) == 1);
    CHECK(u3x_handle_obj(t53_h) == t53_obj2);
    *reinterpret_cast<uint32_t *>(t53_h + 8) = 8;
    CHECK(u3x_handle_obj_valid(t53_h) == 0);

    *reinterpret_cast<int32_t *>(bp + 0x17b4ca4u) = 11;
    *reinterpret_cast<int32_t *>(bp + 0x17b4ba4u) = 12;
    CHECK(u3x_net_now(reinterpret_cast<uintptr_t>(bp)) == 11);
    CHECK(u3x_loc_now(reinterpret_cast<uintptr_t>(bp)) == 12);

    *reinterpret_cast<int32_t *>(t53_table) = 1;
    *reinterpret_cast<int32_t *>(t53_entries + 0xc) = 4;
    *reinterpret_cast<void **>(t53_table + 8) = t53_entries;
    t53_table_slot = t53_table;
    *reinterpret_cast<uintptr_t *>(t53_state + 656) = reinterpret_cast<uintptr_t>(&t53_lookup);
    *reinterpret_cast<int16_t *>(bp + 0x17cb060u + 0x661c) = 0;
    CHECK(u3x_netgame_cur(reinterpret_cast<uintptr_t>(bp), t53_state) == t53_entries);
    CHECK(u3x_netgame_type(reinterpret_cast<uintptr_t>(bp), t53_state) == 4);

    *reinterpret_cast<uint8_t *>(bp + 0x17d5c61u) = 1;
    CHECK(u3x_net_coop_mode(reinterpret_cast<uintptr_t>(bp), t53_state) == 1);
    *reinterpret_cast<uint8_t *>(bp + 0x17d5c61u) = 0;
    CHECK(u3x_net_coop_mode(reinterpret_cast<uintptr_t>(bp), t53_state) == 0);

    uint8_t *ms = bp + 0x17d6420u;
    *reinterpret_cast<uint64_t *>(ms + 0x18) = 1;
    *reinterpret_cast<uint32_t *>(ms + 0x12e4) = 1;
    *reinterpret_cast<uint32_t *>(ms + 0x12e8) = 2;
    *reinterpret_cast<int32_t *>(bp + 0x1b69530u) = 4;
    CHECK(u3x_net_late_joining(reinterpret_cast<uintptr_t>(bp)) == 1);

    *reinterpret_cast<uintptr_t *>(t53_state + 768 + 56 * 8) = reinterpret_cast<uintptr_t>(&t53_effect);
    CHECK(u3x_np_has_effect(t53_state, t53_p, 1) == 0);
    t53_p[0x770] = 1;
    CHECK(u3x_np_alive(t53_state, t53_p) == 1);

    t53_handle_slot = t53_obj2;
    *reinterpret_cast<uint8_t ***>(bp + 0x1bb9500u) = &t53_handle_slot;
    *reinterpret_cast<uint32_t *>(bp + 0x1bb9508u) = 7;
    *reinterpret_cast<uint32_t *>(t53_obj2 + 0x7c) = 7;
    *reinterpret_cast<int32_t *>(t53_p + 0x1e8) = 0;
    CHECK(u3x_screen_player(reinterpret_cast<uintptr_t>(bp), t53_p) == t53_obj2);

    *reinterpret_cast<uintptr_t *>(t53_state + 768 + 55 * 8) = reinterpret_cast<uintptr_t>(&t53_delay);
    CHECK(u3x_respawn_delay_bits(t53_state, t53_p) == 0x3fa00000u);

    CHECK(u3x_f_ticks(0, 2) == -300);
    CHECK(u3x_f_positive(0x3f800000u) == 1);
    CHECK(u3x_f_positive(0x80000000u) == 0);

    uint8_t *t = bp + 0x1b3ee70u;
    *reinterpret_cast<uint64_t **>(t) = reinterpret_cast<uint64_t *>(t53_b);
    *reinterpret_cast<int32_t *>(t + 0x3c) = 1;
    *reinterpret_cast<uint64_t *>(t53_b) = reinterpret_cast<uint64_t>(t53_node);
    *reinterpret_cast<uint64_t *>(t53_b + 8) = 0;
    *reinterpret_cast<uint32_t *>(t53_node + 0x10) = 0x6e51b48fu;
    *reinterpret_cast<uint64_t *>(t53_node + 0x18) = reinterpret_cast<uint64_t>(t53_obj2);
    CHECK(u3x_dc_type_object(reinterpret_cast<uintptr_t>(bp), 0x6e51b48fu) == t53_obj2);

    t53_p[0x3b8] = 1;
    *reinterpret_cast<uintptr_t *>(t53_state + 768 + 54 * 8) = reinterpret_cast<uintptr_t>(&t53_locator);
    CHECK(u3x_spawn_locator(reinterpret_cast<uintptr_t>(bp), t53_state, t53_p, nullptr, 1, 0) == 1);

    *reinterpret_cast<uintptr_t *>(t53_state + 768 + 59 * 8) = reinterpret_cast<uintptr_t>(&t53_pending);
    *reinterpret_cast<uintptr_t *>(t53_state + 768 + 58 * 8) = reinterpret_cast<uintptr_t>(&t53_find);
    CHECK(u3x_spawn_levels_ready(reinterpret_cast<uintptr_t>(bp), t53_state, 1) == 1);
    u3x_spawn_fade(reinterpret_cast<uintptr_t>(bp), t53_state, t53_p, 1);
}
