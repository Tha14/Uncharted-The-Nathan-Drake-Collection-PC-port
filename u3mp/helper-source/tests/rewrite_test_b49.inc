// Row 11: start popup, start checks, start/game-now gates and start preparation.
static uint8_t t49_state[0x4000];
static uint8_t t49_base[0x1800000];
static uint8_t t49_pd[0x40];
static uint32_t t49_pt[5], t49_cg_t0;
static uint32_t t49_start_calls, t49_pad2;
static int t49_show_calls;
static uintptr_t t49_show_arg;

static int t49_show(void *m, uint32_t page, int, int) {
    ++t49_show_calls;
    t49_show_arg = reinterpret_cast<uintptr_t>(m);
    return page == 0x453470ddu ? 1 : 0;
}
extern "C" uint32_t *u3h_pt_state(void) { rd_log("pt_state"); return t49_pt; }
extern "C" uint32_t *u3h_cg_t0_ptr(void) { rd_log("cg_t0"); return &t49_cg_t0; }
extern "C" void u3h_start_calls_inc(void *) { rd_log("start_calls"); ++t49_start_calls; }
extern "C" void u3h_probe_pad2_inc(void *) { rd_log("pad2"); ++t49_pad2; }
static int32_t rd_limit_most = 10, rd_limit_fewest = 1;
static int rd_onb_start = 0, rd_m03_start = 0;
extern "C" void u3h_ng_limits(int32_t type, int32_t *mx, int32_t *mn) {
    if (rd_on) { rd_log("ng_limits", static_cast<uint32_t>(type)); *mx = rd_limit_most; *mn = rd_limit_fewest; return; }
    *mx = type == 16 ? 1 : 10;
    *mn = 1;
}
extern "C" int u3h_onb_startmatch(uintptr_t, void *) { rd_log("onb_start"); return rd_on ? rd_onb_start : 0; }
extern "C" int u3h_m03_startmatch(uintptr_t, void *) { rd_log("m03_start"); return rd_on ? rd_m03_start : 0; }
extern "C" void u3h_cg_commit_selection(uintptr_t, void *) { rd_log("commit"); }

static uint8_t *t49_ev(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17bd080u); }

static void batch49() {
    uint8_t *s = t49_state;
    *reinterpret_cast<uintptr_t *>(s + 648) = reinterpret_cast<uintptr_t>(t49_show);
    uint8_t *m = t49_ev(reinterpret_cast<uintptr_t>(t49_base));
    *reinterpret_cast<uint8_t **>(m + 0x14f0) = t49_pd;
    *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(t49_base) + 0x17b4a90u + 0x114) = 100;
    *reinterpret_cast<uint32_t *>(m + 0x1544) = 0xaaaa;
    *reinterpret_cast<uint32_t *>(m + 0x1548) = 0xbbbb;
    CHECK(u3x_popup_timed(reinterpret_cast<uintptr_t>(t49_base), s, 0x1234u, 5, 0x2222u, 0x3333u) == 1);
    CHECK(t49_show_calls == 1 && t49_show_arg == reinterpret_cast<uintptr_t>(m));
    CHECK(*reinterpret_cast<uint32_t *>(t49_pd + 8) == 0x1234u);
    CHECK(*reinterpret_cast<uint32_t *>(t49_pd + 0x10) == 100);
    CHECK(*reinterpret_cast<uint32_t *>(t49_pd + 0x14) == 100 + 5 * 300);
    CHECK(*reinterpret_cast<uint32_t *>(m + 0x1544) == 0x2222u);
    CHECK(*reinterpret_cast<uint32_t *>(m + 0x1548) == 0x3333u);
    CHECK(t49_pt[0] == 1 && t49_pt[1] == 0xaaaa && t49_pt[2] == 0xbbbb && t49_pt[3] == 0x2222u && t49_pt[4] == 0);
    CHECK(u3x_popup_timed(reinterpret_cast<uintptr_t>(t49_base), s, 1, 1, 2, 3) == 1);
    CHECK(t49_pt[1] == 0xaaaa && t49_pt[2] == 0xbbbb && t49_pt[3] == 2);

    *reinterpret_cast<uintptr_t *>(s + 768 + 7 * 8) = 0;
    *reinterpret_cast<uint32_t *>(s + 0x3d78) = 0;
    t27_netgame_type_value = 0;
    const uint32_t dlg0 = t27_show_dialog_calls;
    CHECK(u3x_start_checks(reinterpret_cast<uintptr_t>(t49_base), s) == 1);
    CHECK(t27_show_dialog_calls == dlg0);
    CHECK(reinterpret_cast<uint32_t *>(t27_ext + 0x2238)[6] == 0);
    CHECK(reinterpret_cast<uint32_t *>(t27_ext + 0x2238)[7] == (1u | (1u << 16) | (10u << 24)));
    *reinterpret_cast<uint8_t *>(reinterpret_cast<uint8_t *>(t49_base) + 0x17d7b06) = 1;
    t27_netgame_type_value = 16;
    CHECK(u3x_start_checks(reinterpret_cast<uintptr_t>(t49_base), s) == 0);
    CHECK(reinterpret_cast<uint32_t *>(t27_ext + 0x2238)[6] == 2);
    CHECK(t27_show_dialog_calls == dlg0 + 1);
    t27_netgame_type_value = 10;
    *reinterpret_cast<uint8_t *>(reinterpret_cast<uint8_t *>(t49_base) + 0x17d7b06) = 0;
    CHECK(u3x_start_checks(reinterpret_cast<uintptr_t>(t49_base), s) == 0);
    CHECK((reinterpret_cast<uint32_t *>(t27_ext + 0x2238)[6] & 4) != 0);
    CHECK(t27_show_dialog_calls == dlg0 + 2);

    t27_netgame_type_value = 0;
    *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(t49_base) + 0x17d603c) = 0;
    const uint32_t cd0 = reinterpret_cast<uint32_t *>(t27_ext + 0x2238)[0];
    u3x_start_game(reinterpret_cast<uintptr_t>(t49_base), s);
    CHECK(reinterpret_cast<uint32_t *>(t27_ext + 0x2238)[0] == cd0 + 1);

    *reinterpret_cast<uint32_t *>(s + 0x3d78) = 0x20000000u;
    const uint32_t calls0 = t49_start_calls;
    u3x_start_prepare(reinterpret_cast<uintptr_t>(t49_base), s);
    CHECK(t49_start_calls == calls0 + 1);
    u3x_start_game_now(reinterpret_cast<uintptr_t>(t49_base), s);
    CHECK(t49_start_calls == calls0 + 1);
}
