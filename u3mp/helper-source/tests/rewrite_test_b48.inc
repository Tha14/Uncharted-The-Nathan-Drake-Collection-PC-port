// Row 9 (b017): matchmaking vote page retag, net-game/playlist/rule lookups and the vote source.
static uint8_t t48_state[0x1000];
static uint8_t t48_net[8 + 2 * 0xd0], t48_play[8 + 2 * 0x98], t48_rule[8 + 2 * 0x58], t48_coop[8 + 0x68];
static uint8_t t48_net_entries[2 * 0xd0], t48_play_entries[2 * 0x98], t48_rule_entries[2 * 0x58], t48_coop_entries[0x68];
static uint8_t t48_zero[0x58];
static uint8_t t48_page[0x80], t48_container[0x10], t48_widgets[2 * 0x68];
static uint8_t t48_source[0x40];
static uint8_t t48_base[0x17d8000];
static const uintptr_t t48_base_addr = reinterpret_cast<uintptr_t>(t48_base);
static int32_t t48_ev[0x40];
static int t48_n06;
static int32_t o48_sel;
static void t48_clear_ev() { std::memset(t48_ev, 0, sizeof t48_ev); *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(t48_ev) + 0x98) = t48_source; }
static void *t48_net_ptr = t48_net, *t48_play_ptr = t48_play, *t48_rule_ptr = t48_rule, *t48_coop_ptr = t48_coop;

static void *t48_lookup(uint32_t id) {
    if (id == 0x3b9a067du) return &t48_net_ptr;
    if (id == 0x0dec97a4u) return &t48_play_ptr;
    if (id == 0xf985c7b6u) return &t48_rule_ptr;
    if (id == 0xc1b0be0du) return &t48_coop_ptr;
    return nullptr;
}
extern "C" uint8_t *u3x_sym_table(void *, uint32_t);
extern "C" int u3h_n06_on(void) { return t48_n06; }
extern "C" void u3h_n06_mapimg(uintptr_t, void *, int32_t *, uint8_t *) {}
extern "C" void u3h_h11_tag(int32_t *) {}
extern "C" int u3h_n08_vq(uintptr_t, int32_t *) { return 0; }

static void o48_retag(uint8_t *page) {
    if (*reinterpret_cast<uint32_t *>(page + 8) != 0xe35f0f1au) return;
    uint8_t *c = *reinterpret_cast<uint8_t **>(page + 0x48);
    if (!c) return;
    const uint32_t n = *reinterpret_cast<uint32_t *>(c);
    uint8_t *it = *reinterpret_cast<uint8_t **>(c + 8);
    if (!it || n > 64) return;
    for (uint32_t j = 0; j < n; ++j) {
        uint8_t *w = it + static_cast<uintptr_t>(j) * 0x68;
        if (*reinterpret_cast<uint32_t *>(w + 8) == 0xdc6e89d8u &&
            *reinterpret_cast<uint32_t *>(w + 0x30) == 21 && *reinterpret_cast<uint32_t *>(w + 0x34) == 0x50 &&
            *reinterpret_cast<uint32_t *>(w + 0x40) == 0) {
            *reinterpret_cast<uint32_t *>(w + 0x34) = 0xcf;
            *reinterpret_cast<uint32_t *>(w + 0x40) = 0x4c414e50u;
            ++reinterpret_cast<volatile uint32_t *>(t27_ext + 0x4050)[170];
        }
    }
}
static uint8_t *o48_netgame(void *s, int16_t i) {
    uint8_t *t = u3x_sym_table(s, 0x3b9a067du);
    if (!t || i < 0 || static_cast<int32_t>(i) >= *reinterpret_cast<int32_t *>(t)) return nullptr;
    return *reinterpret_cast<uint8_t **>(t + 8) + static_cast<uintptr_t>(i) * 0xd0;
}
static uint8_t *o48_playlist(void *s, uint32_t id) {
    uint8_t *t = u3x_sym_table(s, 0x0dec97a4u);
    if (!t) return nullptr;
    uint8_t *e = *reinterpret_cast<uint8_t **>(t + 8);
    for (int32_t i = 0; e && i < *reinterpret_cast<int32_t *>(t); ++i)
        if (*reinterpret_cast<uint32_t *>(e + static_cast<uintptr_t>(i) * 0x98) == id)
            return e + static_cast<uintptr_t>(i) * 0x98;
    return nullptr;
}
static const uint8_t *o48_rule(void *s, int32_t i) {
    uint8_t *t = u3x_sym_table(s, 0xf985c7b6u);
    if (!t || i < 0 || i >= *reinterpret_cast<int32_t *>(t) || !*reinterpret_cast<uint8_t **>(t + 8)) return t48_zero;
    return *reinterpret_cast<uint8_t **>(t + 8) + static_cast<uintptr_t>(i) * 0x58;
}
static const char *o48_diff(uintptr_t base, void *s, uint32_t n) {
    const uint32_t *ids = u3x_difficulty_texts();
    return n < 5 ? u3h_text(base, s, ids[n]) : "";
}
static void o48_set_text(int32_t *ev, const char *t) { *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(ev) + 0x40) = t; }
static int o48_source(uintptr_t base, int32_t *ev, void *s) {
    uint8_t *src = *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(ev) + 0x98);
    if (!src || *reinterpret_cast<uint32_t *>(src + 0x1c) != 0x4c414e50u) return 0;
    if (u3h_native_gate_closed(base, ev)) return 1;
    uint8_t *V = reinterpret_cast<uint8_t *>(base + 0x17d7760u), *W = reinterpret_cast<uint8_t *>(base + 0x17d7784u),
            *mgr = reinterpret_cast<uint8_t *>(base + 0x17cb060u);
    ++reinterpret_cast<volatile uint32_t *>(t27_ext + 0x4050)[171];
    if (!V[0]) return 1;
    if (u3h_fr_tab(base, s) != 0xbd1d2c53u) return 1;
    if (t48_n06 ? (W[0] && !W[1]) : (!W[0] && !W[1])) return 1;
    ++reinterpret_cast<volatile uint32_t *>(t27_ext + 0x4050)[172];
    reinterpret_cast<volatile uint32_t *>(t27_ext + 0x4050)[173] = static_cast<uint32_t>(ev[0]);
    const uint32_t o = ev[1] != 0 ? 4u : 0u;
    switch (ev[0]) {
    case 1: o48_set_text(ev, V[1] ? " " : u3h_text(base, s, 0x81cc5e93u)); break;
    case 2: {
        uint8_t *P = o48_netgame(s, static_cast<int16_t>(*reinterpret_cast<int32_t *>(V + 8 + o)));
        if (!P) break;
        uint8_t *R = o48_playlist(s, *reinterpret_cast<uint32_t *>(P + 4));
        if (!R) break;
        const int32_t col = ev[0x15];
        if (col == 0) o48_set_text(ev, u3h_text(base, s, *reinterpret_cast<uint32_t *>(R + 4)));
        else if (static_cast<uint32_t>(col) == 0x3cf3527au) {
            if (!*reinterpret_cast<uint32_t *>(R + 8)) o48_set_text(ev, u3h_text(base, s, *reinterpret_cast<uint32_t *>(R + 4)));
        }
        break;
    }
    case 4: {
        uint8_t *P = o48_netgame(s, static_cast<int16_t>(*reinterpret_cast<int32_t *>(V + 8 + o)));
        if (!P) break;
        if (t48_n06) { u3h_n06_mapimg(base, s, ev, o48_playlist(s, *reinterpret_cast<uint32_t *>(P + 4))); u3h_h11_tag(ev); break; }
        ev[2] = 36; ev[4] = 0; ev[5] = 0; ev[6] = *reinterpret_cast<int32_t *>(P + 4); ev[7] = 0; ev[8] = 0;
        ev[9] = -1; ev[10] = 0; ev[11] = -1; ev[12] = 0x3f800000;
        break;
    }
    case 5: ev[1] = o48_sel; break;
    case 6: case 8: ev[1] = 1; break;
    case 10: o48_sel = ev[1]; break;
    case 11: {
        const uint32_t q = static_cast<uint32_t>(ev[0x13]);
        if (q == 0xbedebbb3u) {
            const int32_t n = *reinterpret_cast<int32_t *>(V + 0x10 + o);
            ev[14] = n ? n : (V[1] ? -1 : 0);
        } else if (q == 0x88abe7afu) ev[14] = V[1] ? (static_cast<int32_t>(*reinterpret_cast<int16_t *>(mgr + 0x661c)) != *reinterpret_cast<int32_t *>(V + 8)) : -1;
        else if (q == 0xe87f9498u) ev[14] = V[1] ? 0 : (*reinterpret_cast<uint32_t *>(V + 4) > 1u);
        else if (q == 0x7bc431fbu) ev[14] = *reinterpret_cast<int32_t *>(V + 4);
        else u3h_n08_vq(base, ev);
        break;
    }
    }
    return 1;
}
static void batch48() {
    *reinterpret_cast<uintptr_t *>(t48_state + 656) = reinterpret_cast<uintptr_t>(t48_lookup);
    *reinterpret_cast<uint32_t *>(t48_net) = 2;
    *reinterpret_cast<uint8_t **>(t48_net + 8) = t48_net_entries;
    *reinterpret_cast<uint32_t *>(t48_play) = 2;
    *reinterpret_cast<uint8_t **>(t48_play + 8) = t48_play_entries;
    *reinterpret_cast<uint32_t *>(t48_play_entries) = 0x2222u;
    *reinterpret_cast<uint32_t *>(t48_play_entries + 4) = 0x4444u;
    *reinterpret_cast<uint32_t *>(t48_play_entries + 0x98) = 0x6666u;
    *reinterpret_cast<uint32_t *>(t48_play_entries + 0x98 + 4) = 0x7777u;
    *reinterpret_cast<uint32_t *>(t48_rule) = 2;
    *reinterpret_cast<uint8_t **>(t48_rule + 8) = t48_rule_entries;
    *reinterpret_cast<uint32_t *>(t48_rule_entries) = 0x8888u;
    *reinterpret_cast<uint32_t *>(t48_rule_entries + 0x58) = 0x9999u;
    *reinterpret_cast<uint32_t *>(t48_net_entries + 4) = 0x2222u;
    *reinterpret_cast<uint32_t *>(t48_net_entries + 8) = 0x3333u;
    *reinterpret_cast<uint32_t *>(t48_net_entries + 0xc) = 0;
    *reinterpret_cast<uint32_t *>(t48_net_entries + 0xd0 + 4) = 0x6666u;
    *reinterpret_cast<uint32_t *>(t48_net_entries + 0xd0 + 8) = 0x5555u;

    *reinterpret_cast<uint32_t *>(t48_page + 8) = 0xe35f0f1au;
    *reinterpret_cast<uint8_t **>(t48_page + 0x48) = t48_container;
    *reinterpret_cast<uint32_t *>(t48_container) = 2;
    *reinterpret_cast<uint8_t **>(t48_container + 8) = t48_widgets;
    *reinterpret_cast<uint32_t *>(t48_widgets + 8) = 0xdc6e89d8u;
    *reinterpret_cast<uint32_t *>(t48_widgets + 0x30) = 21;
    *reinterpret_cast<uint32_t *>(t48_widgets + 0x34) = 0x50;
    *reinterpret_cast<uint32_t *>(t48_widgets + 0x40) = 0;
    *reinterpret_cast<uint32_t *>(t48_widgets + 0x68 + 8) = 0xdc6e89d8u;
    *reinterpret_cast<uint32_t *>(t48_widgets + 0x68 + 0x30) = 22;
    const uint32_t before = reinterpret_cast<volatile uint32_t *>(t27_ext + 0x4050)[170];
    o48_retag(t48_page);
    const uint32_t old_count = reinterpret_cast<volatile uint32_t *>(t27_ext + 0x4050)[170];
    *reinterpret_cast<uint32_t *>(t48_widgets + 0x34) = 0x50;
    *reinterpret_cast<uint32_t *>(t48_widgets + 0x40) = 0;
    u3x_retag_mm_vote(t48_page);
    CHECK(reinterpret_cast<volatile uint32_t *>(t27_ext + 0x4050)[170] == old_count + (old_count - before));
    CHECK(*reinterpret_cast<uint32_t *>(t48_widgets + 0x34) == 0xcf);
    CHECK(*reinterpret_cast<uint32_t *>(t48_widgets + 0x40) == 0x4c414e50u);

    CHECK(u3x_mmv_netgame(t48_state, 0) == t48_net_entries);
    CHECK(u3x_mmv_netgame(t48_state, 1) == t48_net_entries + 0xd0);
    CHECK(u3x_mmv_netgame(t48_state, 2) == nullptr);
    CHECK(u3x_mmv_netgame(t48_state, -1) == nullptr);
    CHECK(u3x_mmv_playlist(t48_state, 0x2222u) == t48_play_entries);
    CHECK(u3x_mmv_playlist(t48_state, 0x6666u) == t48_play_entries + 0x98);
    CHECK(u3x_mmv_playlist(t48_state, 1) == nullptr);
    CHECK(u3x_mmv_rule(t48_state, 0) == t48_rule_entries);
    CHECK(*reinterpret_cast<const uint32_t *>(u3x_mmv_rule(t48_state, 0)) == 0x8888u);
    CHECK(std::memcmp(u3x_mmv_rule(t48_state, 2), t48_zero, 0x58) == 0);
    const char *d0 = u3x_mmv_diff(t48_base_addr, t48_state, 0), *d5 = u3x_mmv_diff(t48_base_addr, t48_state, 5);
    CHECK(d0 && d5 && std::strcmp(d5, "") == 0);

    uint8_t *V = t48_base + 0x17d7760u, *W = t48_base + 0x17d7784u;
    V[0] = 1; V[1] = 0;
    W[0] = 1; W[1] = 0;
    *reinterpret_cast<int32_t *>(V + 8) = 0;
    *reinterpret_cast<int32_t *>(V + 0x10) = 0;
    t27_fr_tab_value = 0xbd1d2c53u;
    *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(t48_ev) + 0x98) = t48_source;
    *reinterpret_cast<uint32_t *>(t48_source + 0x1c) = 0x4c414e50u;
    t48_clear_ev();
    t48_ev[0] = 1;
    CHECK(o48_source(t48_base_addr, t48_ev, t48_state) == u3x_mmv_source(t48_base_addr, t48_ev, t48_state));
    CHECK(*reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(t48_ev) + 0x40) != nullptr);
    t48_clear_ev();
    t48_ev[0] = 2;
    CHECK(o48_source(t48_base_addr, t48_ev, t48_state) == u3x_mmv_source(t48_base_addr, t48_ev, t48_state));
    CHECK(*reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(t48_ev) + 0x40) != nullptr);
    t48_clear_ev();
    t48_ev[0] = 4;
    CHECK(o48_source(t48_base_addr, t48_ev, t48_state) == u3x_mmv_source(t48_base_addr, t48_ev, t48_state));
    CHECK(t48_ev[2] == 36 && t48_ev[6] == 0x2222);
    t48_clear_ev();
    t48_ev[0] = 6;
    CHECK(o48_source(t48_base_addr, t48_ev, t48_state) == u3x_mmv_source(t48_base_addr, t48_ev, t48_state));
    CHECK(t48_ev[1] == 1);
    t48_ev[0] = 10; t48_ev[1] = 7;
    CHECK(o48_source(t48_base_addr, t48_ev, t48_state) == u3x_mmv_source(t48_base_addr, t48_ev, t48_state));
    t48_ev[0] = 5; t48_ev[1] = 0;
    CHECK(o48_source(t48_base_addr, t48_ev, t48_state) == u3x_mmv_source(t48_base_addr, t48_ev, t48_state));
    CHECK(t48_ev[1] == 7);
    t48_clear_ev();
    t48_ev[0] = 11; t48_ev[0x13] = static_cast<int32_t>(0xbedebbb3u);
    CHECK(o48_source(t48_base_addr, t48_ev, t48_state) == u3x_mmv_source(t48_base_addr, t48_ev, t48_state));
    CHECK(t48_ev[14] == 0);
    V[0] = 0;
    t48_clear_ev();
    t48_ev[0] = 1;
    CHECK(o48_source(t48_base_addr, t48_ev, t48_state) == u3x_mmv_source(t48_base_addr, t48_ev, t48_state));
    V[0] = 1; V[1] = 1;
    W[0] = 1; W[1] = 0;
    t48_clear_ev();
    t48_ev[0] = 1;
    CHECK(o48_source(t48_base_addr, t48_ev, t48_state) == u3x_mmv_source(t48_base_addr, t48_ev, t48_state));
    *reinterpret_cast<uint32_t *>(t48_source + 0x1c) = 0;
    t48_clear_ev();
    t48_ev[0] = 1;
    CHECK(o48_source(t48_base_addr, t48_ev, t48_state) == u3x_mmv_source(t48_base_addr, t48_ev, t48_state));
    CHECK(u3x_mmv_source(t48_base_addr, t48_ev, t48_state) == 0);
}
