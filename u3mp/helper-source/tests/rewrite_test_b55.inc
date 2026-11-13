// Row 27: party list rows, selection ops and row helpers.
static rw::PartyList t55_state;
static rw::StatsTarget t55_i05;
static uint8_t t55_base[0x1d00000];
extern "C" void *u3h_i01_state(void) { return &t55_state; }
extern "C" void *u3h_i05_state(void) { return &t55_i05; }
extern "C" void u3h_set_text(int32_t *ev, const char *t) { rd_log("set_text", rd_text(t)); *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(ev) + 0x40) = t; }
extern "C" void u3h_clan_tag_poll(uintptr_t, void *) { rd_log("clan_tag"); }
extern "C" char *u3h_fmt_append(char *o, char *e, const char *t) { while (*t && o < e) *o++ = *t++; return o; }
static uint8_t *rd_profile = nullptr;
static int32_t rd_ox_rank = -1;
static uint8_t *rd_found_np = nullptr;
static uint32_t rd_pad = 0;
static uint8_t *rd_page = nullptr;
extern "C" uint8_t *u3h_player_profile(uintptr_t) { rd_log("profile"); return rd_on ? rd_profile : nullptr; }
extern "C" int32_t u3h_ox_rank_of(const char *n) { rd_log("ox_rank", rd_text(n)); return rd_on ? rd_ox_rank : -1; }
extern "C" uint8_t *u3h_ec_find_np(uintptr_t, const uint8_t *m, int32_t i) { rd_log("find_np", rd_text(reinterpret_cast<const char *>(m)), static_cast<uint32_t>(i)); return rd_on && m[0] == 'b' ? rd_found_np : nullptr; }
extern "C" uint32_t u3h_pad_pressed(uintptr_t, int32_t screen) { rd_log("pad", static_cast<uint32_t>(screen)); return rd_on ? rd_pad : 0; }
extern "C" uint8_t *u3h_active_page_object(uintptr_t) { rd_log("page"); return rd_on ? rd_page : nullptr; }
extern "C" void u3h_fr_qa_push(uintptr_t, void *q) { rd_log("qa_push", rd_bytes(q, 12 + 4 * 2 * static_cast<uint32_t>(static_cast<int32_t *>(q)[2] < 0 ? 0 : static_cast<int32_t *>(q)[2] > 8 ? 8 : static_cast<int32_t *>(q)[2]))); }

static int64_t t55_canfocus(void *) { return 1; }
static int64_t t55_badfocus(void *) { return 0; }

static void batch55() {
    uint8_t *bp = t55_base;
    uint8_t *md = bp + 0x7a3400u;
    const uintptr_t page = reinterpret_cast<uintptr_t>(md) & ~static_cast<uintptr_t>(0xfff);
    CHECK(test_make_exec(reinterpret_cast<void *>(page), 0x1000));
    md[0] = 0x31; md[1] = 0xc0; md[2] = 0xc3;
    uint8_t *ps = bp + 0x17dc298u;
    *reinterpret_cast<uint32_t *>(ps + 0x12e4) = 0x11;
    *reinterpret_cast<uint32_t *>(ps + 0x12e8) = 0x22;
    *reinterpret_cast<uint32_t *>(t55_state.pseudo + 0x2c) = 0x11;
    u3x_i01_rows(reinterpret_cast<uintptr_t>(bp));
    CHECK(t55_state.member_count == 1 && t55_state.row_count == 1 && t55_state.d[0] == 1);
    CHECK(u3x_i01_self(reinterpret_cast<uintptr_t>(bp), t55_state.pseudo) == 1);
    {
        int32_t cev[0x20] = {}; cev[1] = 0;
        t55_state.d[5] = 0;
        u3x_i01_click(reinterpret_cast<uintptr_t>(bp), cev);
        CHECK(t55_i05.whose == 1);
        CHECK((t55_state.d[5] & 0xffu) == 0 && ((t55_state.d[5] >> 8) & 0xffu) >= 1);
        CHECK(((t55_state.d[5] >> 16) & 1u) == 1 && ((t55_state.d[5] >> 24) & 1u) == 0);
    }

    int32_t ev[0x20] = {};
    ev[0] = 5; CHECK(u3x_i01_party(reinterpret_cast<uintptr_t>(bp), ev, ps, "self") == 1 && ev[1] == t55_state.selected);
    ev[0] = 10; ev[1] = 4; CHECK(u3x_i01_party(reinterpret_cast<uintptr_t>(bp), ev, ps, "self") == 1 && t55_state.selected == 4);
    ev[0] = 21; t55_state.focus_in_party = 1; CHECK(u3x_i01_party(reinterpret_cast<uintptr_t>(bp), ev, ps, "self") == 1 && ev[1] == 1);
    ev[0] = 8; ext_eb()[1] = 0; CHECK(u3x_i01_party(reinterpret_cast<uintptr_t>(bp), ev, ps, "self") == 1 && ev[1] == 1);
    ev[0] = 1; CHECK(u3x_i01_party(reinterpret_cast<uintptr_t>(bp), ev, ps, "self") == 0);

    *reinterpret_cast<int16_t *>(bp + 0x17cb060u + 0x661c) = -1;
    *reinterpret_cast<uint32_t *>(bp + 0x17d603cu) = 2;
    int16_t keep = u3x_i01_ngi_push(reinterpret_cast<uintptr_t>(bp));
    CHECK(keep == -1 && *reinterpret_cast<int16_t *>(bp + 0x17cb060u + 0x661c) == -1);
    u3x_i01_ngi_pop(reinterpret_cast<uintptr_t>(bp), keep);
    CHECK(*reinterpret_cast<int16_t *>(bp + 0x17cb060u + 0x661c) == -1);

    uint8_t sa[4] = {'a', 0, 0, 0}, sb[4] = {'a', 0, 0, 0};
    CHECK(u3x_i01_strcmp(sa, sb) == 0);
    sb[0] = 'b'; CHECK(u3x_i01_strcmp(sa, sb) < 0);
    uint8_t *np = bp + 0x17cb060u + 0x390;
    *reinterpret_cast<uint32_t *>(np) = 7;
    CHECK(u3x_i01_np_by_code(reinterpret_cast<uintptr_t>(bp), 0xabcdu) == np);
    CHECK(u3x_i01_np_by_code(reinterpret_cast<uintptr_t>(bp), 8) == nullptr);

    uintptr_t vt[9] = {};
    vt[8] = reinterpret_cast<uintptr_t>(&t55_canfocus);
    uint8_t ctl[0x40] = {};
    *reinterpret_cast<uintptr_t **>(ctl) = vt;
    CHECK(u3x_i01_canfocus(ctl) == 1);
    vt[8] = reinterpret_cast<uintptr_t>(&t55_badfocus);
    CHECK(u3x_i01_canfocus(ctl) == 0);
    CHECK(u3x_i01_leftctl(0x53cbb5f2u, 0xf75ffe57u) == 1);
    CHECK(u3x_i01_leftctl(0xb99a3235u, 0x70e80076u) == 1);

    t55_state.focus_in_party = 1; t55_state.focus_last = t55_state.focus_party = reinterpret_cast<uint8_t *>(1);
    CHECK(u3x_i01_user_party() == 1);
    t55_state.focus_in_party = 0; CHECK(u3x_i01_user_party() == 0);

}
