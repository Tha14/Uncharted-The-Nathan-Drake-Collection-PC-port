// Row 13: film HUD tab update/action helpers.
static uint8_t t50_base[0x1b30000];
static uint8_t t50_pad[0x200], t50_el[0x2500], t50_hud[0x2500];
static int t50_draws;
extern "C" int u3h_k26_draw(uintptr_t, void *, void *) { ++t50_draws; return 0; }

static void batch50() {
    CHECK(u3x_k29_eok(reinterpret_cast<void *>(0x10000)) == 1);
    CHECK(u3x_k29_eok(reinterpret_cast<void *>(0x10004)) == 1);
    CHECK(u3x_k29_eok(reinterpret_cast<void *>(0x10006)) == 0);
    CHECK(u3x_k29_eok(reinterpret_cast<void *>(0x1000)) == 0);
    CHECK(u3x_k29_eok(nullptr) == 0);

    *reinterpret_cast<uint32_t *>(t50_base + 0x64ce90u) = 0xe5894855u;
    *reinterpret_cast<uint32_t *>(t50_base + 0x1007610u) = 0xe5894855u;
    *reinterpret_cast<uint32_t *>(t50_base + 0x119b250u) = 0xe5894855u;
    CHECK(u3x_k23_callees(reinterpret_cast<uintptr_t>(t50_base)) == 1);

    *reinterpret_cast<uint32_t *>(t50_pad + 0x94) = 0x80;
    CHECK(u3x_k23_rep(reinterpret_cast<uintptr_t>(t50_base), t50_pad, 0x80) == 1);
    *reinterpret_cast<uint32_t *>(t50_pad + 0x94) = 0;
    *reinterpret_cast<uint32_t *>(t50_pad + 0x9c) = 0x20;
    CHECK(u3x_k23_rep(reinterpret_cast<uintptr_t>(t50_base), t50_pad, 0x20) == 0);

    *reinterpret_cast<uint32_t *>(t50_el + 0x15c) = 0;
    u3x_k23_update(reinterpret_cast<uintptr_t>(t50_base), t50_el);
    CHECK(*reinterpret_cast<uint32_t *>(t50_el + 0x1d0) == 0);
    CHECK(reinterpret_cast<uint32_t *>(t27_ext + 0x4050)[206] == 0);

    uint64_t r[16] = {};
    r[3] = reinterpret_cast<uintptr_t>(t50_hud);
    *reinterpret_cast<uint32_t *>(t50_hud + 0x2408 + 0x15c) = 0;
    u3x_k23_hud(reinterpret_cast<uintptr_t>(t50_base), r);
    CHECK(t50_draws == 1);
    CHECK(reinterpret_cast<uint32_t *>(t27_ext + 0x4050)[201] == 1);
}
