// Row 27: party list rows, selection ops and row helpers.
#include <sys/mman.h>
static I01StateExt t55_state;
static I05StateExt t55_i05;
static uint8_t t55_base[0x1d00000];
extern "C" void *u3h_i01_state(void) { return &t55_state; }
extern "C" void *u3h_i05_state(void) { return &t55_i05; }
extern "C" void u3h_set_text(int32_t *ev, const char *t) { *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(ev) + 0x40) = t; }
extern "C" void u3h_clan_tag_poll(uintptr_t, void *) {}
extern "C" char *u3h_fmt_append(char *o, char *e, const char *t) { while (*t && o < e) *o++ = *t++; return o; }
extern "C" uint8_t *u3h_player_profile(uintptr_t) { return nullptr; }
extern "C" int32_t u3h_ox_rank_of(const char *) { return -1; }
extern "C" uint8_t *u3h_ec_find_np(uintptr_t, const uint8_t *, int32_t) { return nullptr; }
extern "C" uint32_t u3h_pad_pressed(uintptr_t, int32_t) { return 0; }
extern "C" uint8_t *u3h_active_page_object(uintptr_t) { return nullptr; }
extern "C" void u3h_fr_qa_push(uintptr_t, void *) {}

static int64_t t55_canfocus(void *) { return 1; }
static int64_t t55_badfocus(void *) { return 0; }

static void batch55() {
    uint8_t *bp = t55_base;
    uint8_t *md = bp + 0x7a3400u;
    const uintptr_t page = reinterpret_cast<uintptr_t>(md) & ~static_cast<uintptr_t>(0xfff);
    CHECK(mprotect(reinterpret_cast<void *>(page), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
    md[0] = 0x31; md[1] = 0xc0; md[2] = 0xc3;
    uint8_t *ps = bp + 0x17dc298u;
    *reinterpret_cast<uint32_t *>(ps + 0x12e4) = 0x11;
    *reinterpret_cast<uint32_t *>(ps + 0x12e8) = 0x22;
    *reinterpret_cast<uint32_t *>(t55_state.pseudo + 0x2c) = 0x11;
    u3x_i01_rows(reinterpret_cast<uintptr_t>(bp));
    CHECK(t55_state.ntab == 1 && t55_state.nrow == 1 && t55_state.d[0] == 1);
    CHECK(u3x_i01_self(reinterpret_cast<uintptr_t>(bp), t55_state.pseudo) == 1);
    {
        int32_t cev[0x20] = {}; cev[1] = 0;
        t55_state.d[5] = 0;
        u3x_i01_click(reinterpret_cast<uintptr_t>(bp), cev);
        CHECK(t55_i05.self == 1);
        CHECK((t55_state.d[5] & 0xffu) == 0 && ((t55_state.d[5] >> 8) & 0xffu) >= 1);
        CHECK(((t55_state.d[5] >> 16) & 1u) == 1 && ((t55_state.d[5] >> 24) & 1u) == 0);
    }

    int32_t ev[0x20] = {};
    ev[0] = 5; CHECK(u3x_i01_party(reinterpret_cast<uintptr_t>(bp), ev, ps, "self") == 1 && ev[1] == t55_state.sel);
    ev[0] = 10; ev[1] = 4; CHECK(u3x_i01_party(reinterpret_cast<uintptr_t>(bp), ev, ps, "self") == 1 && t55_state.sel == 4);
    ev[0] = 21; t55_state.fok = 1; CHECK(u3x_i01_party(reinterpret_cast<uintptr_t>(bp), ev, ps, "self") == 1 && ev[1] == 1);
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

    t55_state.fok = 1; t55_state.flast = t55_state.fparty = reinterpret_cast<uint8_t *>(1);
    CHECK(u3x_i01_user_party() == 1);
    t55_state.fok = 0; CHECK(u3x_i01_user_party() == 0);

}
