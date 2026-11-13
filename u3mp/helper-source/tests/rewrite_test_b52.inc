// Row 19: the online backend's member info, room messages and update entry.
static int64_t t52_stub(void *) { return 0; }
static uintptr_t t52_vt[8];
static uint8_t t52_base[0x1900000];

static void t52_ss(uint8_t *ss, uint8_t *hello) {
    for (int i = 0; i < 8; ++i) t52_vt[i] = reinterpret_cast<uintptr_t>(&t52_stub);
    *reinterpret_cast<uintptr_t **>(ss) = t52_vt;
    *reinterpret_cast<uint8_t **>(ss + 8) = hello;
}

static void batch52() {
    uint8_t rec[0x100] = {};
    for (int i = 0; i < 0x24; ++i) rec[i] = static_cast<uint8_t>('a' + i % 26);
    *reinterpret_cast<uint16_t *>(rec + 36) = 0x1234;
    rec[38] = 7; rec[39] = 9; rec[40] = 0x5a;
    MemberInfo in = {};
    u3x_onb_info(&in, rec);
    CHECK(in.name == reinterpret_cast<const char *>(rec));
    CHECK(in.id == 0x1234 && in.nat == 7 && in.length == 9 && in.data == rec + 40 && in.has_data == 1);

    uint8_t *bp = reinterpret_cast<uint8_t *>(t52_base);
    uint8_t *cm = bp + 0x17f31b0u;
    cm[0x4db4] = 1;
    uint8_t ss[0x1400] = {}, hello[0x40] = {};
    t52_ss(ss, hello);
    u3x_onb_member_link(reinterpret_cast<uintptr_t>(t52_base), ss, "nobody");
    u3x_onb_peer_begin(reinterpret_cast<uintptr_t>(t52_base), ss);

    rw::OnlineBackend be = {};
    be.base = reinterpret_cast<uintptr_t>(t52_base);
    be.bound[0] = ss;
    be.received = 160;
    *reinterpret_cast<uint32_t *>(be.rx) = 305;
    *reinterpret_cast<uint32_t *>(be.rx + 8) = 1;
    *reinterpret_cast<uint16_t *>(be.rx + 26) = 0;
    *reinterpret_cast<uint16_t *>(be.rx + 14) = 0x55;
    *reinterpret_cast<uint16_t *>(be.rx + 12) = 0x66;
    *reinterpret_cast<uint32_t *>(be.rx + 24) = 3;
    CHECK(u3x_onb_msg_in(&be) == 160);
    CHECK(*reinterpret_cast<uint64_t *>(ss + 0x18) == 0);
    CHECK(ss[0xb8] == 1 && ss[0xa1] == 1 && *reinterpret_cast<uint32_t *>(ss + 0x20c) == 3);

    be.received = 16;
    *reinterpret_cast<uint32_t *>(be.rx) = 310;
    *reinterpret_cast<uint32_t *>(be.rx + 8) = 1;
    *reinterpret_cast<uint32_t *>(be.rx + 12) = 0;
    CHECK(u3x_onb_msg_in(&be) == 16);
    CHECK(ss[0xb0] == 1);

    CHECK(u3x_onb_s_nop() == 0);
    u3x_onb_vtable();
    void **vt = u3h_onb_vt();
    CHECK(vt[0] != nullptr && vt[17] != nullptr && vt[1] != nullptr);

    be.received = 0;
    CHECK(u3x_onb_s1(&be) == 0);
}
