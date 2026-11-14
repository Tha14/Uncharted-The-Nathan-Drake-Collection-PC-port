// u2mp rewrite test "unlocks": the old (git) and the new unlocks.cpp on the same inputs.
//   bash gow3_pc/tests/u2mp_rewrite/run.sh unlocks unlocks.cpp
//
// unlocks.cpp keeps process-wide state that is never reset (net-patch9 is loaded once, budgets, given up). So each
// "world" runs in its own child process (the test program starts itself with the world's name). A world runs one
// scripted sequence of steps on the old module, then resets the fake image, the heaps and the fake DC and runs the
// same sequence on the new module; then every step is compared: guest calls and logs in order (DcFind / BindSymbol
// strict; BindSymbol by the pointer it binds, by meaning), the fake image (nonzero words) and the fake net heap
// (word by word). The net heap and the host buffer of net-patch9 live at the same addresses for both sides, so equal
// behaviour gives equal bytes; pointers to static strings of a module are compared by their text.
#include <direct.h>
#include <malloc.h>
#include <process.h>

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "harness.h"
#include "u2mp_internal.h"
#include "unlocks.h"

extern "C" __declspec(dllimport) void *__stdcall GetModuleHandleA(const char *);
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);

namespace u2mp_old {
void install_unlocks(int &installed);
void unlocks_refresh();
const char *unlocks_text(uint32_t id);
uint32_t unlocks_skin_fingerprint();
}  // namespace u2mp_old

using u2mp::sid;
using hx::addr;
using hx::get;
using hx::put;

namespace {

// ---- the two sides -------------------------------------------------------------------------------------
struct Side {
    const char *tag;
    void (*install)(int &);
    void (*refresh)();
    const char *(*text)(uint32_t);
    uint32_t (*fingerprint)();
};
const Side SIDES[2] = {{"old", u2mp_old::install_unlocks, u2mp_old::unlocks_refresh, u2mp_old::unlocks_text,
                        u2mp_old::unlocks_skin_fingerprint},
                       {"new", u2mp::install_unlocks, u2mp::unlocks_refresh, u2mp::unlocks_text,
                        u2mp::unlocks_skin_fingerprint}};
int S = 0;   // the side running
const Side &side() { return SIDES[S]; }

// ---- heaps at fixed addresses ----------------------------------------------------------------------------
constexpr size_t kHeap = 8u << 20, kHost = 4u << 20;
uint8_t *heap = nullptr, *host = nullptr;
size_t heap_used = 0, host_used = 0;
int heap_calls = 0;
std::set<int> heap_fail;   // netheap_alloc call numbers (0-based) that return NULL
bool host_fail = false;    // the aligned host allocation (net-patch9 buffer) fails
uintptr_t exe_lo = 0, exe_hi = 0;

bool in_range(const void *p, const uint8_t *base, size_t n) {
    return base && uintptr_t(p) >= uintptr_t(base) && uintptr_t(p) < uintptr_t(base) + n;
}

std::string M(const void *p) {   // a pointer by meaning
    char b[160];
    if (!p) return "null";
    if (in_range(p, hx::mem, hx::kImageSize)) std::snprintf(b, sizeof b, "img+0x%llx", (unsigned long long)(uintptr_t(p) - uintptr_t(hx::mem)));
    else if (in_range(p, heap, kHeap)) std::snprintf(b, sizeof b, "heap+0x%llx", (unsigned long long)(uintptr_t(p) - uintptr_t(heap)));
    else if (in_range(p, host, kHost)) std::snprintf(b, sizeof b, "np9+0x%llx", (unsigned long long)(uintptr_t(p) - uintptr_t(host)));
    else if (uintptr_t(p) >= exe_lo && uintptr_t(p) < exe_hi) std::snprintf(b, sizeof b, "str'%.100s'", static_cast<const char *>(p));
    else return "host";
    return b;
}

uint64_t canon(uint64_t w) {   // a word with a module's static string pointer replaced by its text's hash
    if (w >= exe_lo && w < exe_hi) {
        uint32_t h = 2166136261u;
        for (const char *s = reinterpret_cast<const char *>(uintptr_t(w)); *s; ++s) h = (h ^ uint8_t(*s)) * 16777619u;
        return 0xE5E5000000000000ull | h;
    }
    return w;
}

}  // namespace

// replaced so the net-patch9 copy (the only aligned allocation unlocks.cpp makes) lands at a fixed address
void *operator new(size_t n, std::align_val_t a, const std::nothrow_t &) noexcept {
    if (host) {
        hx::rec("host alloc 0x%zx align %zu", n, size_t(a));
        if (host_fail) return nullptr;
        host_used = (host_used + 15) & ~size_t(15);
        if (host_used + n > kHost) return nullptr;
        void *p = host + host_used;
        host_used += n;
        return p;
    }
    return _aligned_malloc(n, size_t(a));
}
void operator delete(void *p, std::align_val_t) noexcept {
    if (!p) return;
    if (in_range(p, host, kHost)) {
        hx::rec("host free %s", M(p).c_str());
        return;
    }
    _aligned_free(p);
}

namespace {

// ---- fake DC (DcFind / BindSymbol) -------------------------------------------------------------------------
constexpr uint64_t kSlots = 0x1600000;
std::map<uint32_t, uint64_t> dc;   // id -> slot offset in the image
std::set<uint32_t> dc_absent, dc_ineffective;
int dc_next = 0;
uint64_t slot(uint32_t id) {
    auto it = dc.find(id);
    if (it != dc.end()) return it->second;
    const uint64_t o = kSlots + 16 * uint64_t(dc_next++);
    dc[id] = o;
    return o;
}
void dc_set(uint32_t id, uint64_t abs) {
    put<uint64_t>(slot(id), abs);
    dc_absent.erase(id);
}
void dc_set_img(uint32_t id, uint64_t off) { dc_set(id, off ? addr(off) : 0); }
uint64_t dc_value(uint32_t id) { return dc.count(id) ? get<uint64_t>(dc[id]) : 0; }

U2MP_GUEST void *fake_dcfind(uint32_t id) {
    hx::rec("DcFind %08x", id);
    if (!dc.count(id) || dc_absent.count(id)) return nullptr;
    return hx::mem + dc[id];
}
U2MP_GUEST void fake_bind(uint32_t id, const uint64_t *v) {
    hx::rec("BindSymbol %08x %s", id, M(reinterpret_cast<const void *>(uintptr_t(*v))).c_str());
    if (dc_ineffective.count(id)) return;
    dc_set(id, *v);
}
int reloc_mode = 0;   // 0 normal, 1 wrong result, 2 root slot not relocated
U2MP_GUEST void *fake_relocate(void *buf) {
    hx::rec("RelocateModule %s", M(buf).c_str());
    uint8_t *b = static_cast<uint8_t *>(buf);
    uint64_t r;
    uint32_t n;
    std::memcpy(&r, b + 8, 8);
    std::memcpy(&n, b + r, 4);
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t o, v;
        std::memcpy(&o, b + r + 8 + 8 * i, 8);
        if (reloc_mode == 2 && o == 0x10) continue;
        std::memcpy(&v, b + o, 8);
        v += uint64_t(uintptr_t(b));
        std::memcpy(b + o, &v, 8);
    }
    return reloc_mode == 1 ? b + 0x18 : b + 0x10;
}
// condition record (test format, 0x18): +0 met, +4 out, +8 u32 item offset, +0xc u32 list offset: when +8 is set the
// call points that item's list at that list (the "list changes during the loop" case)
U2MP_GUEST uint8_t fake_cond(void *ctx, const uint8_t *c, int32_t *out) {
    hx::rec("ConditionMet %s %s out=%s%s", M(ctx).c_str(), M(c).c_str(), out ? "ptr" : "null",
            out ? (*out == 0 ? " (0)" : " (not 0)") : "");
    uint32_t met, val, item, list;
    std::memcpy(&met, c, 4), std::memcpy(&val, c + 4, 4), std::memcpy(&item, c + 8, 4), std::memcpy(&list, c + 12, 4);
    if (item) put<uint64_t>(item + 0x18, addr(list));
    if (out && met) *out = int32_t(val);
    return met ? 1 : 0;
}
uint64_t art_result = 0;
U2MP_GUEST void *fake_art(void *table, const char *name) {
    hx::rec("ArtLookup %s '%s'", M(table).c_str(), name ? name : "(NULL)");
    return reinterpret_cast<void *>(uintptr_t(art_result));
}
U2MP_GUEST uint64_t fake_playerdata(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    hx::rec("PlayerData %llx %llx %llx %llx %llx %llx hero %08x villain %08x", (unsigned long long)a0,
            (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)a4,
            (unsigned long long)a5, get<uint32_t>(0x11474f8 + 0x10c), get<uint32_t>(0x11474f8 + 0x110));
    return 0x8877665544332211ull;
}
uint64_t random_result = 0;
U2MP_GUEST uint64_t fake_random(void *list, int32_t side_) {
    hx::rec("RandomSkin %s %d", M(list).c_str(), side_);
    return random_result;
}

void write_stubs() {
    hx::stub(0xa15300, (void *)&fake_dcfind);
    hx::stub(0xa16d70, (void *)&fake_bind);
    hx::stub(0xa19a00, (void *)&fake_relocate);
    hx::stub(0xa4bcb0, (void *)&fake_cond);
    hx::stub(0x56a980, (void *)&fake_art);
    hx::stub(0x6144b0, (void *)&fake_playerdata);
    hx::stub(0x64b430, (void *)&fake_random);
}

// ---- install signatures --------------------------------------------------------------------------------------
struct Sig {
    uint64_t off;
    std::vector<uint8_t> b;
};
const std::vector<Sig> &sigs() {
    static const std::vector<Sig> s = {
        {0xa19a00, {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x18, 0x48, 0x89, 0x7D, 0xD0, 0x40, 0xF6, 0xC7}},
        {0xa16d70, {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x18, 0x4C, 0x8B, 0x0D, 0x78, 0x3E, 0x63, 0x00}},
        {0xa15300, {0x55, 0x48, 0x89, 0xE5, 0x53, 0x48, 0x83, 0xEC, 0x18, 0x4C, 0x8B, 0x05, 0xF0, 0x58, 0x63, 0x00}},
        {0xa4bb40, {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x49, 0x89, 0xF6, 0x49, 0x89, 0xFF}},
        {0xa4bb82, {0x48, 0x8D, 0x05, 0xEF, 0x85, 0x6C, 0x00}},
        {0xa4bbdc, {0xE8, 0xCF, 0x00, 0x00, 0x00}},
        {0xa4bcb0, {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC}},
        {0xa4d280, {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x89, 0x75, 0xB8, 0x31,
                    0xC0, 0x48, 0x85, 0xF6, 0x0F, 0x84, 0x78, 0x01, 0x00, 0x00, 0x8B, 0x0D, 0xD6, 0x16, 0xA8, 0x00, 0x83, 0xF9, 0x02}},
        {0xa4d2f4, {0x48, 0x8D, 0x05, 0x7D, 0x6E, 0x6C, 0x00}},
        {0x6144b0, {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x18}},
        {0x6145a7, {0x41, 0x8B, 0x9F, 0x0C, 0x01, 0x00, 0x00, 0xBF, 0xD2, 0x31, 0x47, 0x5A}},
        {0x614632, {0x41, 0x8B, 0x9F, 0x10, 0x01, 0x00, 0x00, 0xBF, 0xD2, 0x31, 0x47, 0x5A}},
    };
    return s;
}
void write_sigs() {
    for (const Sig &s : sigs()) std::memcpy(hx::mem + s.off, s.b.data(), s.b.size());
}

// ---- records -------------------------------------------------------------------------------------------------
struct Rec {
    std::string name;
    std::vector<std::string> calls;
    std::vector<std::pair<uint64_t, uint64_t>> img;
    std::vector<uint64_t> heap;
};
std::vector<Rec> R[2];

std::vector<std::pair<uint64_t, uint64_t>> image_words() {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    const uint64_t *w = reinterpret_cast<const uint64_t *>(hx::mem);
    for (uint64_t i = 0; i < hx::kImageSize / 8; ++i) {
        if (!w[i]) continue;
        const uint64_t o = i * 8;
        if ((o >= 0xa4bb40 && o < 0xa4bb50) || (o >= 0xa4d280 && o < 0xa4d290)) continue;   // stubs to module code
        v.push_back({o, canon(w[i])});
    }
    return v;
}
std::vector<uint64_t> heap_words() {
    std::vector<uint64_t> v;
    const uint64_t *w = reinterpret_cast<const uint64_t *>(heap);
    for (size_t i = 0; i < (heap_used + 7) / 8; ++i) v.push_back(canon(w[i]));
    return v;
}

void step(const std::string &name, const std::function<void()> &f) {
    hx::calls.clear();
    hx::recording = true;
    f();
    R[S].push_back({name, hx::calls, image_words(), heap_words()});
}

// ---- stand-ins (deps_unlocks.cpp) -----------------------------------------------------------------------------
std::string dir;   // this side's temp dir
bool np9_translate_fail = false;
std::map<std::string, char> actor_cat;   // 'p' PS4 game, 'o' overlay, 'f' translation fails; else no file
std::map<uint32_t, uint64_t> packages;   // package_by_id answers (image offsets)

void mkdirs(const std::string &p) {
    for (size_t i = 3; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '\\') _mkdir(p.substr(0, i).c_str());
}
void touch(const std::string &p) {
    if (std::FILE *f = std::fopen(p.c_str(), "wb")) std::fclose(f);
}
void actor(const std::string &name, char cat) {
    actor_cat[name] = cat;
    if (cat == 'p') touch(dir + "\\CUSA02320\\" + name + ".pak");
    if (cat == 'o') touch(dir + "\\overlay\\" + name + ".pak");
}
void write_np9(const std::vector<uint8_t> &b) {
    std::FILE *f = std::fopen((dir + "\\dc1\\net-patch9.bin").c_str(), "wb");
    std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
}

}  // namespace

namespace ul {
void *netheap_alloc(size_t size, size_t align) {
    const int k = heap_calls++;
    hx::rec("netheap_alloc #%d 0x%zx %zu", k, size, align);
    if (heap_fail.count(k)) return nullptr;
    heap_used = (heap_used + 15) & ~size_t(15);
    void *p = heap + heap_used;
    std::memset(p, 0xCD, size);   // not zeroed by the heap
    heap_used += size;
    return p;
}
const uint8_t *package_by_id(uint32_t id) {
    hx::rec("package_by_id %08x", id);
    auto it = packages.find(id);
    return it == packages.end() ? nullptr : hx::mem + it->second;
}
int translate(const char *guest, char *out, size_t size) {
    hx::rec("translate %s %zu", guest, size);
    const std::string g(guest);
    if (g == "/app0/u2data/build/main/dc1/ps4/net-patch9.bin") {
        if (np9_translate_fail) return -1;
        std::snprintf(out, size, "%s\\dc1\\net-patch9.bin", dir.c_str());
        return 0;
    }
    const std::string pre = "/app0/u2data/build/main/actor27/ps4/";
    if (g.rfind(pre, 0) != 0 || g.size() < pre.size() + 4) return 3;
    const std::string name = g.substr(pre.size(), g.size() - pre.size() - 4);
    const char cat = actor_cat.count(name) ? actor_cat[name] : 'm';
    if (cat == 'f') return 1;
    std::snprintf(out, size, "%s\\%s\\%s.pak", dir.c_str(), cat == 'p' ? "CUSA02320" : cat == 'o' ? "overlay" : "missing",
                  name.c_str());
    return 0;
}
}  // namespace ul

namespace {

// ---- guest data in the image ------------------------------------------------------------------------------------
uint64_t img_next = 0;
uint64_t ia(size_t n) {
    img_next = (img_next + 15) & ~uint64_t(15);
    const uint64_t o = img_next;
    img_next += n;
    return o;
}
uint64_t istr(const char *s) {
    const uint64_t o = ia(std::strlen(s) + 1);
    std::memcpy(hx::mem + o, s, std::strlen(s) + 1);
    return o;
}
uint64_t itable(int32_t n, uint64_t recs) {   // {n, pad, recs*}; recs 0 = NULL
    const uint64_t o = ia(16);
    put<int32_t>(o, n);
    put<uint64_t>(o + 8, recs ? addr(recs) : 0);
    return o;
}

constexpr uint64_t NPD = 0x11474f8, BUCKETS = 0x15340d0, NETSM = 0x14ce97c, DEBUG = 0x1114178, BLIST = 0x113a788;
constexpr uint64_t ART = 0x14705f0, ARTN = 0x14775f0;
constexpr uint32_t STORE = 0x92c6db02, SKIN = 0x5a4731d2, BOOST = 0x916e03b0, NPC = 0xa977dad3, CHAR = 0x42d1ec02,
                   SPK = 0xbdf0cfd6, MAT = 0xef68c757, SET = 0x1ad223b4;
constexpr uint32_t HERO_SEC = 0x681022f5, VILL_SEC = 0x4252add5;

// lists / conditions for IsLocked
uint64_t icond(uint32_t met, uint32_t out, uint32_t item = 0, uint32_t list = 0) {
    const uint64_t o = ia(0x18);
    put<uint32_t>(o, met), put<uint32_t>(o + 4, out), put<uint32_t>(o + 8, item), put<uint32_t>(o + 12, list);
    return o;
}
uint64_t ilist(int32_t any, const std::vector<std::array<uint32_t, 4>> &conds) {
    const uint64_t arr = ia(0x18 * (conds.size() ? conds.size() : 1));
    for (size_t i = 0; i < conds.size(); ++i) {
        const uint64_t c = arr + 0x18 * i;
        put<uint32_t>(c, conds[i][0]), put<uint32_t>(c + 4, conds[i][1]), put<uint32_t>(c + 8, conds[i][2]),
            put<uint32_t>(c + 12, conds[i][3]);
    }
    const uint64_t l = ia(0x18);
    put<int32_t>(l, int32_t(conds.size()));
    put<uint64_t>(l + 8, addr(arr));
    put<int32_t>(l + 0x10, any);
    return l;
}
uint64_t iitem(uint64_t list) {   // a non-store item (single-player reward)
    const uint64_t o = ia(0x50);
    put<uint64_t>(o + 0x18, list ? addr(list) : 0);
    return o;
}

struct Item {
    uint32_t bit, type, id;
    uint32_t weapon = 0;
    int32_t tier = 0;
};
uint64_t g_locked_list = 0;   // every store item's condition list: one unmet condition ("all") -> locked
uint64_t items_img(const std::vector<Item> &items) {
    const uint64_t arr = ia(0x50 * (items.size() + 1));   // + a guard item after the end
    for (size_t i = 0; i <= items.size(); ++i) {
        const uint64_t it = arr + 0x50 * i;
        const Item d = i < items.size() ? items[i] : Item{1, 10, 0xdddd};
        put<uint32_t>(it, d.bit), put<uint32_t>(it + 4, d.type), put<uint32_t>(it + 8, d.id);
        put<uint32_t>(it + 0xc, 0x7700 + uint32_t(i)), put<uint32_t>(it + 0x14, 500 + uint32_t(i));
        put<uint64_t>(it + 0x18, addr(g_locked_list));
        put<uint32_t>(it + 0x28, 0xa0 + uint32_t(i)), put<uint32_t>(it + 0x2c, d.weapon);
        put<uint32_t>(it + 0x30, 0xb0), put<int32_t>(it + 0x34, d.tier), put<uint32_t>(it + 0x38, 0xc0 + uint32_t(i));
    }
    return arr;
}
struct Sec {
    uint32_t id;
    int32_t n;
    uint64_t items;   // offset, 0 = NULL
};
uint64_t store_img(int32_t count, const std::vector<Sec> &secs, bool null_array = false) {
    const uint64_t arr = ia(0x18 * (secs.size() + 1));
    for (size_t i = 0; i < secs.size(); ++i) {
        const uint64_t s = arr + 0x18 * i;
        put<uint32_t>(s, secs[i].id), put<uint32_t>(s + 4, 0x7171 + uint32_t(i)), put<int32_t>(s + 8, secs[i].n);
        put<uint64_t>(s + 0x10, secs[i].items ? addr(secs[i].items) : 0);
    }
    return itable(count, null_array ? 0 : arr);
}

// ---- net-patch9 builder ----------------------------------------------------------------------------------------
struct Np9 {
    std::vector<uint8_t> b;
    std::vector<uint64_t> rel;
    std::vector<std::pair<uint32_t, uint64_t>> syms;
    static constexpr uint64_t kReloc = 0x240, kData = 0x8000;
    Np9() { b.assign(kData, 0); }
    uint64_t add(const void *p, size_t n) {
        while (b.size() % 16) b.push_back(0);
        const uint64_t o = b.size();
        b.resize(b.size() + n);
        if (p) std::memcpy(b.data() + o, p, n);
        return o;
    }
    uint64_t zero(size_t n) { return add(nullptr, n); }
    uint64_t str(const char *s) { return add(s, std::strlen(s) + 1); }
    void w32(uint64_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
    void w64(uint64_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); }
    void p(uint64_t at, uint64_t target) {   // a pointer into the file (relocated)
        w64(at, target);
        rel.push_back(at);
    }
    uint64_t tab(int32_t n, uint64_t recs) {
        const uint64_t o = zero(16);
        w32(o, uint32_t(n));
        p(o + 8, recs);
        return o;
    }
    void abs(uint64_t at, uint64_t v) {   // a pointer outside the file (not relocated)
        for (size_t i = 0; i < rel.size(); ++i)
            if (rel[i] == at) rel.erase(rel.begin() + i--);
        w64(at, v);
    }
    void sym(uint32_t id, uint64_t value) { syms.push_back({id, value}); }
    std::vector<uint8_t> finish() {
        std::memcpy(b.data(), "00CD", 4);
        w64(8, kReloc);
        p(0x10, 0x20);
        w32(0x1c, 0x916aa605);
        w32(0x24, uint32_t(syms.size()));
        for (size_t i = 0; i < syms.size(); ++i) {
            w32(0x28 + 16 * i, syms[i].first);
            p(0x28 + 16 * i + 8, syms[i].second);
        }
        w32(kReloc, uint32_t(rel.size()));
        for (size_t i = 0; i < rel.size(); ++i) w64(kReloc + 8 + 8 * i, rel[i]);
        return b;
    }
};

// ---- world configuration ------------------------------------------------------------------------------------------
struct SkinDef {
    uint32_t idx, side;
    const char *pkg;
    uint32_t id, name, photo, icon, v24, v28;
};
struct Cfg {
    int file = 0;    // 0 ok, 1 no file, 2 translate fails, 3 bad magic, 4 bad root slot, 5 bad root type, 6 short,
                     // 7 reloc < 0x28, 8 reloc not 8-aligned, 9 reloc+4 > size, 10 count past end, 11 relocate wrong
                     // result, 12 relocate wrong root, 13 host alloc fails
    int skins = 0;   // 0 PS4 layout, 1 converted, 2 unknown, 3 missing
    std::vector<SkinDef> skin_recs;   // empty: the default set
    int skin_stub = 0;   // 0 two records, 1 NULL, 2 nine records, 3 bad package, 4 Drake + new + same new, 5 count 0
    int boosters = 0;    // 0 ok, 1 missing, 2 n 0, 3 n 64, 4 records outside the copy, 5 id 0, 6 slot 2, 7 a None missing
    int npc = 0;         // 0 ok, 1 missing, 2 archetype 0, 3 n 1025
    int chars = 0;       // 0 PS4 layout, 1 converted, 2 name not a string, 3 n 0, 4 key 0, 5 neither (ps4 +0xc and conv +0x1c)
    int spk = 0;         // 0 ok, 1 unsorted, 2 pad set, 3 value outside, 4 value pointer outside, 5 missing
    int mat = 0;         // 0 ok, 1 type 2, 2 cache set, 3 from outside, 4 empty string, 5 missing, 6 type-3 'to' too short
    int set = 0;         // 0 ok, 1 missing, 2 health 0, 3 health 1001, 4 g not in 0x34
    int xstub = 0;       // extra stubs: 0 ok, 1 NULL, 2 count 0, 3 count 9
    bool store = true;
    int store_variant = 0;   // 0 normal, 1 sections 0, 2 sections 33, 3 array NULL, 4 hero n 0 / villain n 201, 5 hero items NULL
    bool ineffective = false;   // every BindSymbol does not stick
    bool break_skin_sig = false;   // the skin hooks are not installed
    std::set<int> heap_fail;
};

std::vector<SkinDef> default_skins() {
    return {
        {0, 0, "mp-hero-frosty", sid("mp-hero"), 0x1001, 0x2001, 0x3001, 7, 8},
        {1, 0, "mp-hero-killzone-sev", sid("mp-hero-killzone-sev"), 0x1002, 0x2002, 0x3002, 0, 0},
        {2, 0, "mp-hero-u1-merc-brsr-2a", sid("mp-hero-u1-merc-brsr-2a"), 0x1003, 0x2003, 0x3003, 1, 1},
        {3, 1, "mp-hero-lazarevic", 0x345329d1, 0x1004, 0x2004, 0x3004, 0, 0},
        {4, 1, "mp-hero-x_y--z", sid("mp-hero-x_y--z"), 0x1002, 0x2005, 0x3005, 0, 0},
        {5, 0, "mp-hero", sid("mp-hero-plain"), 0x1005, 0x2006, 0x3006, 0, 0},
        {6, 0, "mp-hero-nolabel", sid("mp-hero-nolabel"), 0, 0, 0, 0, 0},
        {7, 1, "mp-x-", sid("mp-x-"), 0x1006, 0, 0, 0, 0},
        {8, 0, "mp-hero-nd", 0x258034ff, 0x1007, 0, 0, 0, 0},
        {9, 0, "mp-hero-ab", sid("mp-hero-ab"), 0x1008, 0, 0, 0, 0},
        {10, 0, "mp-hero-a", sid("mp-hero-a"), 0x1009, 0, 0, 0, 0},
        {11, 1, "mp-hero-pirate", 0x5a2e4839, 0x100a, 0, 0, 0, 0},
    };
}

std::vector<uint8_t> make_np9(const Cfg &c) {
    Np9 f;
    uint64_t mat_recs = 0;
    // skins
    std::vector<SkinDef> sk = c.skin_recs.empty() ? default_skins() : c.skin_recs;
    if (c.skins != 3) {
        std::vector<uint64_t> names;
        for (auto &s : sk) names.push_back(f.str(c.skins == 2 && &s == &sk[2] ? "xx-bad" : s.pkg));
        const uint64_t stride = c.skins == 1 ? 0x28 : 0x30;
        const uint64_t recs = f.zero(stride * sk.size());
        for (size_t i = 0; i < sk.size(); ++i) {
            const uint64_t r = recs + stride * i;
            f.w32(r, sk[i].idx), f.w32(r + 4, sk[i].side), f.p(r + 8, names[i]), f.w32(r + 0x10, sk[i].id);
            const uint64_t base = c.skins == 1 ? 0x14 : 0x18;
            const uint32_t v[5] = {sk[i].name, sk[i].photo, sk[i].icon, sk[i].v24, sk[i].v28};
            for (int k = 0; k < 5; ++k) f.w32(r + base + 4 * k, v[k]);
        }
        f.sym(SKIN, f.tab(int32_t(sk.size()), recs));
    }
    // boosters
    if (c.boosters != 1) {
        struct B { uint32_t id, slot, listed; };
        std::vector<B> bs = {{0x05454b23, 0, 1}, {0x01845694, 1, 1}, {0xB1, 0, 0}, {0xB2, 1, 1}, {0xB3, 0, 1}};
        if (c.boosters == 5) bs[3].id = 0;
        if (c.boosters == 6) bs[2].slot = 2;
        if (c.boosters == 7) bs[1].id = 0xB4;
        const uint64_t recs = f.zero(0x20 * 64);
        for (size_t i = 0; i < bs.size(); ++i) {
            const uint64_t r = recs + 0x20 * i;
            f.w32(r, 0x100 + uint32_t(i)), f.w32(r + 4, 0x200 + uint32_t(i)), f.w32(r + 8, 0x300 + uint32_t(i));
            f.w32(r + 0xc, bs[i].id), f.w32(r + 0x10, bs[i].slot), f.w32(r + 0x14, 3), f.w32(r + 0x18, bs[i].listed);
        }
        int32_t n = int32_t(bs.size());
        if (c.boosters == 2) n = 0;
        if (c.boosters == 3) n = 64;
        const uint64_t t = f.zero(16);
        f.w32(t, uint32_t(n));
        if (c.boosters == 4) f.w64(t + 8, addr(0x1650000));   // records in the image, not in the copy
        else f.p(t + 8, recs);
        f.sym(BOOST, t);
    }
    // net-npc-info
    if (c.npc != 1) {
        const uint32_t rec[3][3] = {{0xA1, 0xC1, 10}, {0xA2, 0xC2, 20}, {0xA3, 0xC3, 30}};
        const uint64_t recs = f.add(rec, sizeof rec);
        if (c.npc == 2) f.w32(recs + 12, 0);
        if (c.npc == 3) {
            const uint64_t big = f.zero(12 * 1025);
            for (int i = 0; i < 1025; ++i) f.w32(big + 12 * i, 0x5000 + i);
            f.sym(NPC, f.tab(1025, big));
        } else {
            f.sym(NPC, f.tab(3, recs));
        }
    }
    // character attributes
    {
        const char *nm[3] = {"drake", "chloe", "flynn"};
        uint64_t s[3];
        for (int i = 0; i < 3; ++i) s[i] = f.str(nm[i]);
        const uint64_t recs = f.zero(0x60);
        for (int i = 0; i < 3; ++i) {
            const uint64_t r = recs + 0x20 * i;
            f.p(r, s[i]);
            f.w32(r + 8, c.chars == 4 && i == 1 ? 0 : sid(nm[i]));
            if (c.chars == 1) f.w32(r + 0xc, 1 + i), f.w32(r + 0x10, 2), f.w32(r + 0x14, 3), f.w32(r + 0x18, 0xfeed0 + i);
            else f.w32(r + 0x10, 1 + i), f.w32(r + 0x14, 2), f.w32(r + 0x18, 3), f.w32(r + 0x1c, 0xfeed0 + i);
            if (c.chars == 5 && i == 2) f.w32(r + 0xc, 9);   // PS4 layout fails; converted fails (+0x1c set)
        }
        if (c.chars == 2) f.abs(recs + 0x20, addr(istr("chloe")));   // name outside the copy
        f.sym(CHAR, f.tab(c.chars == 3 ? 0 : 3, recs));
    }
    // speakers
    if (c.spk != 5) {
        const uint64_t recs = f.zero(0x30);
        const uint32_t keys[3] = {0x10, 0x20, c.spk == 1 ? 0x05u : 0x30u};
        for (int i = 0; i < 3; ++i) {
            const uint64_t v = f.zero(0x28);
            for (int k = 0; k < 4; ++k) f.p(v + 8 * k, f.str(k == 0 ? "speaker" : "line"));
            if (c.spk == 4 && i == 1) f.abs(v + 16, addr(0x1650000));
            const uint64_t r = recs + 0x10 * i;
            f.w32(r, keys[i]);
            if (c.spk == 2 && i == 2) f.w32(r + 4, 1);
            if (c.spk == 3 && i == 0) f.w64(r + 8, addr(0x1650000));
            else f.p(r + 8, v);
        }
        f.sym(SPK, f.tab(3, recs));
    }
    // material parameter set
    if (c.mat != 5) {
        const uint64_t recs = f.zero(0x60);
        for (int i = 0; i < 2; ++i) {
            const uint64_t r = recs + 0x30 * i;
            f.p(r, f.str(c.mat == 4 && i == 1 ? "" : "mat/drake"));
            f.p(r + 8, f.str("tint"));
            const uint32_t type = c.mat == 1 && i == 1 ? 2 : i == 0 ? 1 : 3;
            f.w32(r + 0x10, type);
            f.p(r + 0x18, f.zero(12));
            if (c.mat == 3 && i == 0) f.abs(r + 0x18, addr(0x1650000));
            if (!(c.mat == 6 && i == 1)) f.p(r + 0x20, f.zero(16));
            if (c.mat == 2 && i == 1) f.w64(r + 0x28, 5);
        }
        mat_recs = recs;   // mat 6: the type-3 record's "to" (the last 8 bytes of the file, set below)
        f.sym(MAT, f.tab(2, recs));
    }
    // default game settings (last, so "not in 0x34" can end the file)
    if (c.set != 1) {
        uint8_t g[0x34];
        for (int i = 0; i < 0x34; ++i) g[i] = uint8_t(0x40 + i);
        int32_t health = c.set == 2 ? 0 : c.set == 3 ? 1001 : 90;
        std::memcpy(g, &health, 4);
        const uint64_t o = f.add(g, c.set == 4 ? 0x20 : 0x34);
        f.sym(SET, o);
    }
    if (c.mat == 6) f.p(mat_recs + 0x30 + 0x20, f.zero(8));   // only 8 bytes left in the copy (needs 12)
    std::vector<uint8_t> b = f.finish();
    switch (c.file) {
    case 3: b[1] = 'X'; break;
    case 4: f.w64(0x10, 0x28), b = f.b; break;
    case 5: b[0x1c] ^= 1; break;
    case 6: b.resize(0x3f); break;
    case 7: { uint64_t v = 0x20; std::memcpy(b.data() + 8, &v, 8); } break;
    case 8: { uint64_t v = Np9::kReloc + 4; std::memcpy(b.data() + 8, &v, 8); } break;
    case 9: { uint64_t v = (b.size() + 7) & ~uint64_t(7); std::memcpy(b.data() + 8, &v, 8); } break;
    case 10: { uint32_t n = uint32_t(b.size()); std::memcpy(b.data() + Np9::kReloc, &n, 4); } break;
    default: break;
    }
    return b;
}

// PS4 stubs, store, NetPlayerData, art table ... in the image
uint64_t g_store_img = 0, g_hero_items = 0, g_vill_items = 0, g_weap_items = 0;
uint64_t skin_stub_img(int mode) {
    if (mode == 1) return 0;
    struct D { const char *pkg; uint32_t id; };
    std::vector<D> d = {{"mp-hero", sid("mp-hero")}, {"mp-hero-eddie", sid("mp-hero-eddie")}};
    if (mode == 2) for (int i = 0; i < 7; ++i) d.push_back({"mp-hero-more", 0x9900 + uint32_t(i)});
    if (mode == 3) d[1].pkg = "hero-eddie";
    if (mode == 4) d = {{"mp-hero", sid("mp-hero")}, {"mp-hero-new", 0x4e4e4e4e}, {"mp-hero-new2", 0x4e4e4e4e}};
    const uint64_t recs = ia(0x30 * d.size());
    for (size_t i = 0; i < d.size(); ++i) {
        const uint64_t r = recs + 0x30 * i;
        put<uint32_t>(r, uint32_t(i)), put<uint32_t>(r + 4, uint32_t(i & 1)), put<uint64_t>(r + 8, addr(istr(d[i].pkg)));
        put<uint32_t>(r + 0x10, d[i].id), put<uint32_t>(r + 0x18, 0x1101 + uint32_t(i)), put<uint32_t>(r + 0x1c, 0x2101);
        put<uint32_t>(r + 0x20, 0x3101), put<uint32_t>(r + 0x24, 0x11), put<uint32_t>(r + 0x28, 0x22), put<uint32_t>(r + 0x2c, 0x33);
    }
    return itable(mode == 5 ? 0 : int32_t(d.size()), recs);
}
uint64_t xstub(int mode, size_t stride, int n, const std::function<void(uint64_t, int)> &fill) {
    if (mode == 1) return 0;
    const uint64_t recs = ia(stride * 9);
    for (int i = 0; i < 9; ++i) fill(recs + stride * i, i);
    return itable(mode == 2 ? 0 : mode == 3 ? 9 : n, recs);
}

void world_setup(const Cfg &c) {
    img_next = 0x1610000;
    write_sigs();
    g_locked_list = ilist(0, {{0, 0, 0, 0}});
    // store
    g_hero_items = items_img({{1, 10, sid("mp-hero")}, {2, 10, 0x258034ff}, {3, 10, sid("mp-hero-killzone-sev")}});
    g_vill_items = items_img({{4, 10, 0x345329d1}, {5, 10, sid("mp-hero-eddie")}});
    g_weap_items = items_img({{5, 12, 0x51, 77, 1}, {40, 12, 0x52, 77, 2}, {300, 12, 0x53, 77, 3}, {6, 11, 0x54, 77, 9},
                              {7, 12, 0x55, 78, 8}, {8, 13, 0x56, 0, 0}});
    g_store_img = store_img(3, {{HERO_SEC, 3, g_hero_items}, {VILL_SEC, 2, g_vill_items}, {0x1234, 6, g_weap_items}});
    if (c.store_variant == 1) g_store_img = store_img(0, {{HERO_SEC, 3, g_hero_items}});
    if (c.store_variant == 2) {   // 33 real sections (the upgrade level walks them all: no 32 limit there)
        std::vector<Sec> v(33, Sec{0x3333, 0, 0});
        v[0] = {HERO_SEC, 3, g_hero_items};
        v[32] = {0x1234, 6, g_weap_items};
        g_store_img = store_img(33, v);
    }
    if (c.store_variant == 3) g_store_img = store_img(2, {{HERO_SEC, 3, g_hero_items}}, true);
    if (c.store_variant == 4) g_store_img = store_img(3, {{HERO_SEC, 0, g_hero_items}, {VILL_SEC, 201, items_img(std::vector<Item>(201, Item{9, 10, 0x6666}))}, {0x1234, 6, g_weap_items}});
    if (c.store_variant == 5) g_store_img = store_img(3, {{HERO_SEC, 3, 0}, {VILL_SEC, 2, g_vill_items}, {0x1234, 6, g_weap_items}});
    if (c.store) dc_set_img(STORE, g_store_img);
    if (c.ineffective) for (uint32_t id : {STORE, SKIN, BOOST, NPC, CHAR, SPK, MAT, SET}) dc_ineffective.insert(id);
    if (c.break_skin_sig) hx::mem[0x614632] = 0;
    // PS4 stubs
    if (uint64_t s = skin_stub_img(c.skin_stub)) dc_set_img(SKIN, s);
    const uint64_t npc = xstub(c.xstub, 12, 2, [](uint64_t r, int i) { put<uint32_t>(r, i == 0 ? 0xA2 : 0xA9 + i), put<uint32_t>(r + 4, 0xC9), put<uint32_t>(r + 8, 5); });
    const uint64_t chr = xstub(c.xstub, 0x20, 3, [](uint64_t r, int i) {
        const char *nm = i == 0 ? "drake" : i == 1 ? "eddie" : nullptr;
        put<uint64_t>(r, nm ? addr(istr(nm)) : 0);
        put<uint32_t>(r + 8, nm ? sid(nm) : 0x77777777u + i), put<uint32_t>(r + 0x10, 7), put<uint32_t>(r + 0x1c, 0xabc);
    });
    const uint64_t spk = xstub(c.xstub, 0x10, 1, [](uint64_t r, int i) { put<uint32_t>(r, 0x99 + i); });
    const uint64_t mat = xstub(c.xstub, 0x30, 1, [](uint64_t r, int) { put<uint32_t>(r + 0x10, 1); });
    uint64_t set = 0;
    if (c.xstub != 1) {
        set = ia(0x38);
        for (int i = 0; i < 0x38; ++i) hx::mem[set + i] = uint8_t(0x90 + i);
        put<int32_t>(set, 80);
    }
    if (npc) dc_set_img(NPC, npc);
    if (chr) dc_set_img(CHAR, chr);
    if (spk) dc_set_img(SPK, spk);
    if (mat) dc_set_img(MAT, mat);
    if (set) dc_set_img(SET, set);
    // the game's booster list (2 PS4 stub entries) and the player
    put<uint32_t>(BLIST, 2);
    put<uint64_t>(BLIST + 8, addr(0x1650100));
    put<uint64_t>(BLIST + 16, addr(0x1650120));
    put<uint32_t>(NPD + 0x10c, sid("mp-hero-killzone-sev"));
    put<uint32_t>(NPD + 0x110, 0xffffffffu);
    put<uint32_t>(NPD + 0xe0, 1u << 5);            // owned bit 5 (tier 1); bit 40 clear
    put<uint32_t>(NPD + 0xe0 + 4 * 9, 0xffffffffu);   // bits 288..319: past the limit, must not count
    // art name table
    put<int32_t>(ARTN, 4);
    put<uint32_t>(ART + 0x20, sid("mp-hero-killzone-sev"));
    put<uint32_t>(ART + 0x38 + 0x20, 0);
    put<uint32_t>(ART + 0x70 + 0x20, sid("mp-hero-killzone-sev"));
    put<uint32_t>(ART + 0xa8 + 0x20, sid("x"));
    // package records
    const uint64_t pk = ia(0x40);
    put<int32_t>(pk + 0x30, 3);
    packages[sid("mp-hero-killzone-sev")] = pk;
    // files
    np9_translate_fail = c.file == 2;
    if (c.file != 1 && c.file != 2) write_np9(make_np9(c));
    else std::remove((dir + "\\dc1\\net-patch9.bin").c_str());
    reloc_mode = c.file == 11 ? 1 : c.file == 12 ? 2 : 0;
    host_fail = c.file == 13;
    heap_fail = c.heap_fail;
    actor("mp-hero-killzone-sev", 'p');
    actor("mp-hero-u1-merc-brsr-2a", 'o');
    actor("mp-hero-lazarevic", 'f');
    actor("mp-hero-red", 'p');
    actor("mp-hero-eddie", 'o');
    art_result = addr(0x1650200);
    random_result = 0x1234567800000001ull;
}

// ---- common steps ------------------------------------------------------------------------------------------------
void do_install(const char *name = "install") {
    step(name, [] {
        int n = 0;
        side().install(n);
        hx::rec("installed %d", n);
    });
}
void install_and_stub() {
    do_install();
    write_stubs();
    put<uint64_t>(BUCKETS, 1);
}
void refresh(const std::string &name) { step(name, [] { side().refresh(); }); }
void texts(const std::string &name) {
    step(name, [] {
        const uint32_t ids[] = {0, 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008, 0x1009, 0x100a,
                                0x1101, 0x1102, 0x1103, 0x9a88ecdc, 0xe2ce50df, 0xc0f12cfd, 0x899e21dd, 0x1d8dbe6a,
                                0xba9eb65d, 0x9b7568c5, 0xaacd3194, 0xdead};
        for (uint32_t id : ids) {
            const char *t = side().text(id);
            hx::rec("text %08x -> %s", id, t ? (std::string("'") + t + "'").c_str() : "NULL");
        }
        hx::rec("fingerprint %08x", side().fingerprint());
    });
}
using IsLockedFn = bool(U2MP_GUEST *)(void *, const uint8_t *, int32_t *);
using LevelFn = int32_t(U2MP_GUEST *)(void *, uint8_t *);
void islocked(const std::string &name, uint64_t item, bool with_reason, bool null_item = false) {
    step(name, [=] {
        auto f = reinterpret_cast<IsLockedFn>(hx::mem + 0xa4bb40);
        const uint64_t reason = 0x1650300;
        put<int32_t>(reason, 77);
        const bool r = f(reinterpret_cast<void *>(addr(0x1650400)), null_item ? nullptr : hx::mem + item,
                         with_reason ? reinterpret_cast<int32_t *>(hx::mem + reason) : nullptr);
        hx::rec("IsLocked -> %d reason %d", int(r), get<int32_t>(reason));
    });
}
uint64_t weapon(uint32_t id, int32_t cached) {
    const uint64_t w = ia(0x28);
    put<uint32_t>(w, id);
    put<int32_t>(w + 0x20, cached);
    return w;
}
void level(const std::string &name, uint64_t w) {
    step(name, [=] {
        auto f = reinterpret_cast<LevelFn>(hx::mem + 0xa4d280);
        const int32_t r = f(nullptr, w ? hx::mem + w : nullptr);
        hx::rec("level -> %d", r);
    });
}
using PdFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using RsFn = uint64_t(U2MP_GUEST *)(void *, int32_t);
using ArtFn = void *(U2MP_GUEST *)(void *, const char *);
void playerdata(const std::string &name, uint64_t site = 0x5f2f49) {
    step(name, [=] {
        auto f = reinterpret_cast<PdFn>(hx::retarget_host(site));
        if (!f) { hx::rec("no hook at %llx", (unsigned long long)site); return; }
        const uint64_t r = f(0x1111111111111111ull, 2, 0xfffffffffffffff3ull, 4, 0x8000000000000005ull, 6);
        hx::rec("player data -> %llx", (unsigned long long)r);
    });
}
void randompick(const std::string &name, uint64_t result, int32_t sd, uint64_t site = 0x6145f5) {
    step(name, [=] {
        auto f = reinterpret_cast<RsFn>(hx::retarget_host(site));
        if (!f) { hx::rec("no hook"); return; }
        random_result = result;
        const uint64_t r = f(hx::mem + 0x113b760, sd);
        hx::rec("random -> %llx", (unsigned long long)r);
    });
}
void spawn(const std::string &name, uint64_t site, const char *model) {
    step(name, [=] {
        auto f = reinterpret_cast<ArtFn>(hx::retarget_host(site));
        if (!f) { hx::rec("no hook"); return; }
        void *r = f(hx::mem + 0x1650500, model);
        hx::rec("spawn -> %s", M(r).c_str());
    });
}

// the standard flow every table world runs: install, the first refresh builds and binds, then the later refreshes
void table_flow(const Cfg &c) {
    world_setup(c);
    install_and_stub();
    texts("texts before the build");
    put<int32_t>(NETSM, 2);
    refresh("refresh 1 (load, build, bind)");
    texts("texts after the build");
    refresh("refresh 2 (skin choice at bind, fast check)");
    refresh("refresh 3 (fast check)");
    islocked("IsLocked store item after the build", g_hero_items, true);
    playerdata("player data hook");
    randompick("random pick 1", 0x1234567800000001ull, 0);
    level("upgrade level after the build", weapon(77, -1));
}

// ---- worlds ---------------------------------------------------------------------------------------------------------
using World = std::function<void()>;
std::map<std::string, std::pair<std::function<void()>, World>> worlds;   // name -> (environment, script)

void env(const char *unlocks, const char *goty, const char *skin) {
    _putenv_s("UNCHARTED_U2MP_UNLOCKS", unlocks ? unlocks : "");
    _putenv_s("UNCHARTED_U2MP_GOTY_TABLES", goty ? goty : "");
    _putenv_s("UNCHARTED_U2MP_SKIN", skin ? skin : "");
}
// _putenv_s with "" removes the variable on Windows, so an empty UNCHARTED_U2MP_SKIN cannot be set this way; the
// "empty" case is covered by unset (both read getenv: NULL and "" take the same branch in both versions)

void add(const std::string &n, std::function<void()> e, World w) { worlds[n] = {std::move(e), std::move(w)}; }
std::function<void()> no_env = [] { env(nullptr, nullptr, nullptr); };

void define_worlds() {
    // ---- install signature variants (one process: install state is only set / sticky) ----
    add("install", no_env, [] {
        img_next = 0x1610000;
        do_install("install: empty image (nothing matches)");
        struct V { const char *name; std::function<void()> f; };
        auto brk = [](uint64_t off) { return [off] { hx::mem[off] ^= 0xff; }; };
        auto nox = [](uint64_t off, uint64_t len) { return [off, len] { hx::noexec.push_back({off, len}); }; };
        auto fail = [](uint64_t off) { return [off] { hx::fail_offsets.push_back(off); }; };
        std::vector<V> vs = {
            {"all match", [] {}},
            {"loader relocate bytes", brk(0xa19a00 + 23)},
            {"loader relocate not executable", nox(0xa19a00 + 23, 1)},
            {"loader bind bytes", brk(0xa16d70 + 5)},
            {"loader bind not executable", nox(0xa16d70, 1)},
            {"loader find bytes", brk(0xa15300 + 15)},
            {"loader find not executable", nox(0xa15300 + 8, 1)},
            {"IsLocked not executable (range end)", nox(0xa4bb40 + 0xff, 1)},
            {"IsLocked executable just past range", nox(0xa4bb40 + 0x100, 1)},
            {"IsLocked prologue", brk(0xa4bb40 + 22)},
            {"IsLocked lea", brk(0xa4bb82 + 6)},
            {"IsLocked call", brk(0xa4bbdc + 1)},
            {"IsLocked cond prologue", brk(0xa4bcb0 + 15)},
            {"IsLocked replace fails", fail(0xa4bb40)},
            {"upgrade not executable", nox(0xa4d280 + 0x7f, 1)},
            {"upgrade bytes", brk(0xa4d280 + 40)},
            {"upgrade lea", brk(0xa4d2f4)},
            {"upgrade replace fails", fail(0xa4d280)},
            {"spawn A fails", fail(0x663339)},
            {"spawn B fails", fail(0x663782)},
            {"skin prologue", brk(0x6144b0 + 16)},
            {"skin hero read", brk(0x6145a7 + 3)},
            {"skin villain read", brk(0x614632 + 11)},
            {"skin retargets all fail", [] { for (uint64_t o : {0x5f2f49, 0x609551, 0x609c2b, 0x6145f5, 0x614688}) hx::fail_offsets.push_back(o); }},
            {"skin retargets: only the last works", [] { for (uint64_t o : {0x5f2f49, 0x609551, 0x609c2b, 0x6145f5}) hx::fail_offsets.push_back(o); }},
            {"skin executable not tested", nox(0x6144b0, 0x200000)},
        };
        for (auto &v : vs) {
            hx::noexec.clear();
            hx::fail_offsets.clear();
            write_sigs();
            v.f();
            do_install((std::string("install: ") + v.name).c_str());
        }
        hx::noexec.clear();
        hx::fail_offsets.clear();
    });
    add("install_env_off", [] { env("off", "off", "mp-hero-navarro"); }, [] {
        img_next = 0x1610000;
        write_sigs();
        do_install("install with UNLOCKS=off, GOTY_TABLES=off, SKIN set");
        write_sigs();
        hx::mem[0x6144b0] = 0;
        do_install("install again, skin signature broken (forced name kept)");
    });
    add("install_env_Off", [] { env("Off", "OFF", nullptr); }, [] {
        img_next = 0x1610000;
        write_sigs();
        do_install("install with UNLOCKS=Off (not off)");
    });

    // ---- main world: hooks, refresh life cycle, budgets ----
    add("main", no_env, [] {
        Cfg c;
        world_setup(c);
        install_and_stub();
        put<uint64_t>(BUCKETS, 0);
        texts("texts / fingerprint before the build");
        islocked("IsLocked store item, buckets 0 (no DcFind)", g_hero_items, true);
        refresh("refresh with buckets 0");
        put<uint64_t>(BUCKETS, 1);
        put<int32_t>(NETSM, 1);
        refresh("refresh with NET_SM 1");
        islocked("IsLocked NULL item (reason untouched)", 0, true, true);
        islocked("IsLocked store item, first (one-time note)", g_hero_items, true);
        islocked("IsLocked store item, second (no note)", g_hero_items + 0x50, false);
        islocked("IsLocked store item last of villain", g_vill_items + 0x50, true);
        islocked("IsLocked inside an item, not on a boundary", g_hero_items + 0x50 + 8, true);
        islocked("IsLocked past the end (guard item)", g_hero_items + 0x50 * 3, true);
        // store variants
        const uint64_t s0 = store_img(0, {{HERO_SEC, 3, g_hero_items}});
        const uint64_t s33 = store_img(33, {{HERO_SEC, 3, g_hero_items}});
        const uint64_t snull = store_img(1, {{HERO_SEC, 3, g_hero_items}}, true);
        const uint64_t sc0 = store_img(1, {{HERO_SEC, 0, g_hero_items}});
        const uint64_t sc1025 = store_img(1, {{HERO_SEC, 1025, g_hero_items}});
        const uint64_t sc1024 = store_img(1, {{HERO_SEC, 1024, g_hero_items}});
        const uint64_t snitems = store_img(2, {{HERO_SEC, 3, 0}, {VILL_SEC, 2, g_vill_items}});
        const uint64_t s32 = store_img(32, {{HERO_SEC, 3, g_hero_items}});
        for (auto [nm, s] : std::vector<std::pair<const char *, uint64_t>>{{"sections 0", s0}, {"sections 33", s33}, {"section array NULL", snull},
                                                                             {"count 0", sc0}, {"count 1025", sc1025}, {"count 1024", sc1024},
                                                                             {"items NULL", snitems}, {"sections 32", s32}}) {
            dc_set_img(STORE, s);
            islocked(std::string("IsLocked store variant: ") + nm, g_hero_items, true);
        }
        dc_absent.insert(STORE);
        islocked("IsLocked store absent", g_hero_items, true);
        dc_set_img(STORE, 0);
        islocked("IsLocked store slot holds NULL", g_hero_items, true);
        dc_set_img(STORE, g_store_img);
        // non-store items
        islocked("any: first unmet then met", iitem(ilist(1, {{0, 3, 0, 0}, {1, 5, 0, 0}, {1, 6, 0, 0}})), true);
        islocked("any: none met", iitem(ilist(1, {{0, 3, 0, 0}, {0, 4, 0, 0}})), true);
        islocked("any: count 0", iitem(ilist(1, {})), true);
        islocked("all: count 0", iitem(ilist(0, {})), true);
        islocked("all: met, unmet, met", iitem(ilist(0, {{1, 8, 0, 0}, {0, 9, 0, 0}, {1, 10, 0, 0}})), true);
        islocked("all: all met", iitem(ilist(0, {{1, 0, 0, 0}, {1, 12, 0, 0}})), true);
        islocked("all: all met, reason NULL", iitem(ilist(0, {{1, 0, 0, 0}, {1, 12, 0, 0}})), false);
        islocked("met with out 0 then met", iitem(ilist(1, {{1, 0, 0, 0}, {1, 13, 0, 0}})), true);
        islocked("list NULL", iitem(0), true);
        {   // list changes during the loops
            const uint64_t it = iitem(0);
            const uint64_t lb = ilist(1, {{0, 1, 0, 0}, {1, 21, 0, 0}, {1, 22, 0, 0}});
            const uint64_t la = ilist(0, {{1, 0, uint32_t(it), uint32_t(lb)}, {1, 0, 0, 0}});
            put<uint64_t>(it + 0x18, addr(la));
            islocked("list changes after the first call (all -> any)", it, true);
            const uint64_t it2 = iitem(0);
            const uint64_t lc = ilist(0, {{0, 0, 0, 0}});
            const uint64_t ld = ilist(1, {{0, 0, 0, 0}, {0, 31, uint32_t(it2), uint32_t(lc)}, {1, 32, 0, 0}});
            put<uint64_t>(it2 + 0x18, addr(ld));
            islocked("list changes on the second call (any -> all, count 1)", it2, true);
        }
        put<uint8_t>(DEBUG, 1);
        islocked("debug switch: reason loop still runs", iitem(ilist(0, {{0, 1, 0, 0}, {1, 41, 0, 0}})), true);
        islocked("debug switch, reason NULL", iitem(ilist(0, {{0, 1, 0, 0}, {1, 41, 0, 0}})), false);
        islocked("debug switch, store item", g_hero_items, true);
        put<uint8_t>(DEBUG, 0);
        // upgrade level (spoof on)
        level("level: NET_SM 1", weapon(77, -1));
        put<int32_t>(NETSM, 2);
        level("level: weapon NULL", 0);
        level("level: cached 3", weapon(77, 3));
        level("level: cached 0", weapon(77, 0));
        level("level: spoof on, weapon 77", weapon(77, -1));
        level("level: weapon 78", weapon(78, -1));
        level("level: weapon without items", weapon(99, -1));
        dc_absent.insert(STORE);
        level("level: store absent", weapon(77, -1));
        dc_set_img(STORE, store_img(0, {}));
        level("level: store sections 0", weapon(77, -1));
        dc_set_img(STORE, store_img(2, {{0x1234, 6, g_weap_items}, {0x1235, 3, 0}}));
        level("level: section with NULL items", weapon(77, -1));
        dc_set_img(STORE, store_img(1, {{0x1234, 6, g_weap_items}}, true));
        level("level: section array NULL", weapon(77, -1));
        dc_set_img(STORE, g_store_img);
        put<int32_t>(NETSM, 1);
        islocked("IsLocked, NET_SM 1: refresh does nothing, called again next time", g_hero_items, true);
        // the build
        put<int32_t>(NETSM, 2);
        refresh("refresh 1 (load, build, bind)");
        texts("texts after the build");
        refresh("refresh 2 (skin choice at bind, fast check)");
        refresh("refresh 3 (fast check only)");
        islocked("IsLocked bound: no refresh; game store item in our copy", g_weap_items, true);
        islocked("IsLocked bound: hero item of the game array (copy has its own)", g_hero_items, true);
        step("IsLocked: added item of our store copy", [] {
            const uint8_t *st = reinterpret_cast<const uint8_t *>(uintptr_t(dc_value(STORE)));
            uint64_t sp;
            std::memcpy(&sp, st + 8, 8);
            const uint8_t *secs = reinterpret_cast<const uint8_t *>(uintptr_t(sp));
            uint64_t items;
            std::memcpy(&items, secs + 0x10, 8);
            auto f = reinterpret_cast<IsLockedFn>(hx::mem + 0xa4bb40);
            const bool r = f(nullptr, reinterpret_cast<const uint8_t *>(uintptr_t(items)) + 0x50 * 3, nullptr);
            hx::rec("IsLocked -> %d", int(r));
        });
        level("level after the build (our store copy)", weapon(77, -1));
        // skin hooks
        playerdata("player data 0x5f2f49");
        playerdata("player data 0x609551", 0x609551);
        playerdata("player data 0x609c2b", 0x609c2b);
        randompick("random pick in range", 0x1234567800000001ull, 0);
        randompick("random pick negative", 0x00000001ffffffffull, 1, 0x614688);
        randompick("random pick too large", 9999, 0);
        randompick("random pick index 0", 0, 0);
        step("random pick index = count", [] {
            const uint8_t *t = reinterpret_cast<const uint8_t *>(uintptr_t(dc_value(SKIN)));
            int32_t n;
            std::memcpy(&n, t, 4);
            random_result = uint64_t(uint32_t(n));
            auto f = reinterpret_cast<RsFn>(hx::retarget_host(0x6145f5));
            hx::rec("random -> %llx", (unsigned long long)f(nullptr, 1));
            random_result = uint64_t(uint32_t(n - 1));
            hx::rec("random -> %llx", (unsigned long long)f(nullptr, 1));
        });
        {
            const uint64_t keep = dc_value(SKIN);
            dc_absent.insert(SKIN);
            randompick("random pick, skin table missing", 2, 0);
            playerdata("player data, skin table missing (no choice)");
            dc_set(SKIN, keep);
        }
        // spawn lookups
        spawn("spawn A known model", 0x663339, "mp-hero-killzone-sev");
        spawn("spawn B NULL name", 0x663782, nullptr);
        art_result = 0;
        spawn("spawn A unknown model, NULL result", 0x663339, "mp-hero-unknown");
        spawn("spawn B overlay model", 0x663782, "mp-hero-u1-merc-brsr-2a");
        spawn("spawn A translate fails", 0x663339, "mp-hero-lazarevic");
        put<int32_t>(ARTN, 600);
        put<uint32_t>(ART + 0x38 * 511 + 0x20, sid("x-last"));
        put<uint32_t>(ART + 0x38 * 512 + 0x20, sid("x-past"));
        spawn("spawn art count 600: entry 511 listed", 0x663339, "x-last");
        spawn("spawn art count 600: entry 512 not scanned", 0x663339, "x-past");
        put<int32_t>(ARTN, -3);
        spawn("spawn art count negative", 0x663339, "x");
        put<int32_t>(ARTN, 4);
        for (int i = 0; i < 26; ++i) spawn("spawn budget " + std::to_string(i), i & 1 ? 0x663782 : 0x663339, "x");
        // the game replaces things
        put<uint32_t>(BLIST, 0);
        refresh("booster list count 0: refilled");
        put<uint32_t>(BLIST, 64);
        refresh("booster list count 64: refilled");
        put<uint32_t>(BLIST, 2);
        put<uint64_t>(BLIST + 8, addr(0x1650100));
        refresh("booster list pointer not ours: refilled");
        {
            const uint64_t ours = dc_value(CHAR);
            dc_set_img(CHAR, 0x1650600);
            refresh("char attributes replaced: rebound (fast check stops at it)");
            dc_set(NPC, addr(0x1650600));
            refresh("npc info replaced: rebound");
            dc_absent.insert(SET);
            refresh("settings symbol gone: rebound");
            (void)ours;
        }
        {
            dc_set_img(SKIN, 0x1650600);
            refresh("skin table replaced: rebound, then skin choice at bind does not run again");
            dc_set_img(BOOST, 0x1650600);
            refresh("booster table replaced: rebound");
        }
        // store replaced by the game: rebuilt (4 times), then bound
        const uint64_t alt1 = store_img(33, {{HERO_SEC, 3, g_hero_items}});
        const uint64_t h201 = items_img(std::vector<Item>(201, Item{1, 10, 0x4444}));
        const uint64_t alt2 = store_img(3, {{HERO_SEC, 201, h201}, {VILL_SEC, 0, g_vill_items}, {0x77, 1, g_hero_items}});
        const uint64_t vall = items_img({{1, 10, 0x345329d1}, {2, 10, 0x5a2e4839}, {3, 10, 0x1c14ad7b}});
        const uint64_t hall = items_img({{1, 10, 0x1b3af509}, {2, 10, 0x7a66b9bb}, {3, 10, 0x258034ff}, {4, 10, 0xf0d8079a}});
        const uint64_t alt3 = store_img(2, {{VILL_SEC, 3, vall}, {HERO_SEC, 4, hall}});
        const uint64_t alt4 = store_img(2, {{HERO_SEC, 1, g_hero_items}, {VILL_SEC, 200, items_img(std::vector<Item>(200, Item{2, 10, 0x5555}))}});
        const uint64_t alt5 = store_img(1, {{HERO_SEC, 3, g_hero_items}});
        dc_set_img(STORE, alt1);
        refresh("store replaced (33 sections): rebuild returns, old copy rebound");
        dc_set_img(STORE, alt2);
        refresh("store replaced (hero 201, villain 0): copy with 0 added");
        dc_set_img(STORE, alt3);
        refresh("store replaced (all ids present): no new arrays");
        dc_set_img(STORE, alt4);
        heap_fail.insert(heap_calls + 1);   // the hero array of this rebuild
        refresh("store replaced (villain 200), hero array allocation fails");
        dc_set_img(STORE, alt5);
        refresh("store replaced a 5th time: no rebuild, bound again");
        // ineffective binds
        dc_ineffective.insert(SKIN);
        dc_set_img(SKIN, 0x1650600);
        refresh("skin bind ineffective 1");
        refresh("skin bind ineffective 2");
        refresh("skin bind ineffective 3");
        dc_ineffective.insert(STORE);
        dc_set_img(STORE, alt5);
        refresh("store bind ineffective (budget 0 after it)");
        refresh("budget used up: no more BindSymbol");
        dc_set_img(MAT, 0x1650600);
        refresh("budget used up: extra not rebound");
        // skin log budget: shared lines run out, choices still applied
        for (int i = 0; i < 12; ++i) {
            put<uint32_t>(NPD + 0x10c, i & 1 ? 0x12345678u : sid("mp-hero-killzone-sev"));
            put<uint32_t>(NPD + 0x110, 0xfffffff0u + i);
            playerdata("player data, budget run " + std::to_string(i));
        }
        randompick("random pick after the budget", 1, 0);
        texts("texts at the end");
    });

    // ---- environment worlds ----
    add("env_off", [] { env("off", "off", "mp-hero-killzone-sev"); }, [] {
        Cfg c;
        world_setup(c);
        install_and_stub();
        put<int32_t>(NETSM, 1);
        islocked("spoof off: store item evaluated, no store DcFind", g_hero_items, true);
        put<int32_t>(NETSM, 2);
        level("spoof off: owned bits (bit 5 set, 40 clear, 300 past the limit)", weapon(77, -1));
        level("spoof off: weapon 78 (bit 7 clear)", weapon(78, -1));
        put<uint32_t>(NPD + 0xe0, 0xffffffffu);
        put<uint32_t>(NPD + 0xe0 + 4, 0xffffffffu);
        level("spoof off: bits 5 and 40 set", weapon(77, -1));
        put<uint32_t>(NPD + 0xe0, 0);
        put<uint32_t>(NPD + 0xe0 + 4, 0);
        level("spoof off: nothing owned", weapon(77, -1));
        put<uint8_t>(DEBUG, 1);
        level("spoof off: debug switch", weapon(77, -1));
        put<uint8_t>(DEBUG, 0);
        refresh("refresh 1, extras off");
        refresh("refresh 2 (forced skin at bind)");
        refresh("refresh 3");
        playerdata("player data, forced");
        texts("texts");
    });
    add("env_Off", [] { env("Off", "OFF", nullptr); }, [] { table_flow(Cfg{}); });
    add("skin_not_found", [] { env(nullptr, nullptr, "mp-hero-navarro"); }, [] {
        Cfg c;
        world_setup(c);
        install_and_stub();
        put<int32_t>(NETSM, 2);
        refresh("refresh 1");
        refresh("refresh 2: not forced log at bind");
        for (int i = 0; i < 9; ++i) playerdata("player data, not forced " + std::to_string(i));
    });
    add("skin_forced_before_bind", [] { env(nullptr, nullptr, "mp-hero-eddie"); }, [] {
        Cfg c;
        world_setup(c);
        install_and_stub();
        playerdata("player data before the build: PS4 stub table");
        put<int32_t>(NETSM, 2);
        refresh("refresh 1");
        refresh("refresh 2");
        playerdata("player data after the build");
    });

    // ---- skin table variants ----
    add("skins_converted", no_env, [] { Cfg c; c.skins = 1; c.skin_stub = 1; c.xstub = 1; table_flow(c); });
    add("skins_unknown", no_env, [] { Cfg c; c.skins = 2; table_flow(c); });
    add("skins_missing", no_env, [] { Cfg c; c.skins = 3; c.boosters = 1; c.xstub = 2; table_flow(c); });
    add("stub_nine", no_env, [] { Cfg c; c.skin_stub = 2; c.xstub = 3; table_flow(c); });
    add("stub_bad_name", no_env, [] { Cfg c; c.skin_stub = 3; c.boosters = 2; table_flow(c); });
    add("stub_count0", no_env, [] { Cfg c; c.skin_stub = 5; c.boosters = 3; table_flow(c); });
    add("stub_new_ids", no_env, [] { Cfg c; c.skin_stub = 4; c.boosters = 4; table_flow(c); });
    add("red_and_campaign_present", no_env, [] {
        Cfg c;
        c.skin_recs = default_skins();
        c.skin_recs.push_back({12, 0, "mp-hero-red", sid("mp-hero-red"), 0x1010, 0, 0, 0, 0});
        c.skin_recs.push_back({13, 1, "mp-hero-healed", 0x1c14ad7b, 0x1011, 0, 0, 0, 0});
        c.skin_recs.push_back({14, 0, "mp-hero-beat", 0x1b3af509, 0x1012, 0, 0, 0, 0});
        c.skin_recs.push_back({15, 0, "mp-hero-skel", 0x7a66b9bb, 0, 0, 0, 0, 0});
        c.skin_recs.push_back({16, 0, "mp-hero-frost", 0xf0d8079a, 0x1013, 0, 0, 0, 0});
        c.boosters = 5;
        table_flow(c);
    });
    add("many_missing_actors", no_env, [] {
        Cfg c;
        static std::vector<std::string> names;
        names.clear();
        for (int i = 0; i < 40; ++i) names.push_back("mp-hero-very-long-skin-name-number-" + std::to_string(i));
        for (int i = 0; i < 40; ++i)
            c.skin_recs.push_back({uint32_t(i), uint32_t(i & 1), names[i].c_str(), sid(names[i].c_str()), 0x1001 + uint32_t(i % 5), 0, 0, 0, 0});
        c.boosters = 6;
        table_flow(c);
    });
    add("skins_max_512", no_env, [] {
        Cfg c;
        for (int i = 0; i < 512; ++i) c.skin_recs.push_back({uint32_t(i), 0, "mp-hero-z", 0x60000 + uint32_t(i), 0, 0, 0, 0, 0});
        c.boosters = 7;
        table_flow(c);
    });
    add("skins_513", no_env, [] {
        Cfg c;
        for (int i = 0; i < 513; ++i) c.skin_recs.push_back({uint32_t(i), 0, "mp-hero-z", 0x60000 + uint32_t(i), 0, 0, 0, 0, 0});
        table_flow(c);
    });

    // ---- allocation failures ----
    add("alloc_skin_main", no_env, [] { Cfg c; c.heap_fail = {0}; table_flow(c); });
    add("alloc_skin_pkg", no_env, [] { Cfg c; c.heap_fail = {1}; table_flow(c); });
    add("alloc_store_header", no_env, [] { Cfg c; c.heap_fail = {3}; table_flow(c); });
    add("alloc_store_arrays", no_env, [] { Cfg c; c.heap_fail = {4, 5}; table_flow(c); });
    add("alloc_extras_a", no_env, [] { Cfg c; c.heap_fail = {6, 8, 10, 12}; table_flow(c); });
    add("alloc_extras_b", no_env, [] { Cfg c; c.heap_fail = {7, 11}; table_flow(c); });
    add("alloc_extras_c", no_env, [] { Cfg c; c.heap_fail = {9, 13}; table_flow(c); });

    // ---- extra table failures ----
    add("extras_fail_1", no_env, [] { Cfg c; c.npc = 2; c.chars = 2; c.spk = 1; c.mat = 1; c.set = 2; table_flow(c); });
    add("extras_fail_2", no_env, [] { Cfg c; c.npc = 1; c.chars = 3; c.spk = 2; c.mat = 2; c.set = 4; table_flow(c); });
    add("extras_fail_3", no_env, [] { Cfg c; c.npc = 3; c.chars = 4; c.spk = 3; c.mat = 3; c.set = 3; table_flow(c); });
    add("extras_fail_4", no_env, [] { Cfg c; c.chars = 5; c.spk = 4; c.mat = 4; c.set = 1; table_flow(c); });
    add("extras_fail_5", no_env, [] { Cfg c; c.chars = 1; c.spk = 5; c.mat = 5; c.xstub = 1; table_flow(c); });
    add("extras_mat6_stub0", no_env, [] { Cfg c; c.mat = 6; c.xstub = 2; table_flow(c); });
    add("store_sections0", no_env, [] { Cfg c; c.store_variant = 1; table_flow(c); });
    add("store_sections33", no_env, [] { Cfg c; c.store_variant = 2; table_flow(c); });
    add("store_array_null", no_env, [] { Cfg c; c.store_variant = 3; table_flow(c); });
    add("store_hero0_villain201", no_env, [] { Cfg c; c.store_variant = 4; table_flow(c); });
    add("store_hero_items_null", no_env, [] { Cfg c; c.store_variant = 5; table_flow(c); });
    add("ineffective_all", no_env, [] {
        Cfg c;
        c.ineffective = true;
        table_flow(c);
        refresh("refresh 4 (budget used up)");
    });
    add("no_skin_hooks", [] { env(nullptr, nullptr, "mp-hero-eddie"); }, [] {
        Cfg c;
        c.break_skin_sig = true;
        table_flow(c);
    });
    add("nothing_built", [] { env(nullptr, "off", nullptr); }, [] {
        Cfg c; c.skins = 3; c.boosters = 1; c.store = false;
        table_flow(c);
        refresh("refresh 4 (given up)");
    });

    // ---- file failures ----
    const char *fnames[] = {"", "file_missing", "file_translate_fails", "file_bad_magic", "file_bad_root_slot",
                            "file_bad_root_type", "file_short", "file_reloc_low", "file_reloc_unaligned",
                            "file_reloc_past_end", "file_reloc_count", "file_relocate_result", "file_relocate_root",
                            "file_host_alloc"};
    for (int k = 1; k <= 13; ++k)
        add(fnames[k], no_env, [k] {
            Cfg c;
            c.file = k;
            world_setup(c);
            install_and_stub();
            put<int32_t>(NETSM, 2);
            refresh("refresh 1 (load fails)");
            refresh("refresh 2 (given up: fast check only)");
            islocked("IsLocked (bound set)", g_hero_items, true);
            texts("texts");
        });
}

// ---- running ------------------------------------------------------------------------------------------------------
std::string exe_dir() {
    char b[1024];
    GetModuleFileNameA(nullptr, b, sizeof b);
    std::string s(b);
    return s.substr(0, s.find_last_of("\\/"));
}

void reset_side(const std::string &world, int s) {
    hx::clear();
    hx::bind_both();
    std::memset(heap, 0, heap_used + 16);
    std::memset(host, 0, host_used + 16);
    heap_used = host_used = 0;
    heap_calls = 0;
    heap_fail.clear();
    host_fail = false;
    dc.clear(), dc_absent.clear(), dc_ineffective.clear();
    dc_next = 0;
    reloc_mode = 0;
    np9_translate_fail = false;
    actor_cat.clear();
    packages.clear();
    dir = exe_dir() + "\\unl_tmp\\" + world + "\\" + SIDES[s].tag;
    mkdirs(dir + "\\dc1");
    mkdirs(dir + "\\CUSA02320");
    mkdirs(dir + "\\overlay");
    S = s;
}

bool diff(const Rec &a, const Rec &b) {
    bool ok = hx::same(a.name.c_str(), a.calls, b.calls);
    if (a.img != b.img) {
        size_t i = 0;
        while (i < a.img.size() && i < b.img.size() && a.img[i] == b.img[i]) ++i;
        auto show = [](const std::vector<std::pair<uint64_t, uint64_t>> &v, size_t i) {
            char t[64];
            if (i >= v.size()) return std::string("(end)");
            std::snprintf(t, sizeof t, "+0x%llx=%016llx", (unsigned long long)v[i].first, (unsigned long long)v[i].second);
            return std::string(t);
        };
        std::printf("FAIL %s: image differs: old %s new %s\n", a.name.c_str(), show(a.img, i).c_str(), show(b.img, i).c_str());
        if (ok) hx::failures++;
        ok = false;
    }
    if (a.heap != b.heap) {
        size_t i = 0;
        while (i < a.heap.size() && i < b.heap.size() && a.heap[i] == b.heap[i]) ++i;
        std::printf("FAIL %s: net heap differs at heap+0x%zx (old %zu words, new %zu): old %016llx new %016llx\n", a.name.c_str(),
                    i * 8, a.heap.size(), b.heap.size(), (unsigned long long)(i < a.heap.size() ? a.heap[i] : 0),
                    (unsigned long long)(i < b.heap.size() ? b.heap[i] : 0));
        if (ok) hx::failures++;
        ok = false;
    }
    return ok;
}

int run_world(const std::string &name) {
    auto it = worlds.find(name);
    if (it == worlds.end()) {
        std::printf("no world %s\n", name.c_str());
        return 2;
    }
    it->second.first();
    for (int s = 0; s < 2; ++s) {
        reset_side(name, s);
        it->second.second();
    }
    if (R[0].size() != R[1].size()) {
        std::printf("FAIL %s: %zu steps old, %zu new\n", name.c_str(), R[0].size(), R[1].size());
        return 1;
    }
    int pass = 0;
    for (size_t i = 0; i < R[0].size(); ++i) {
        const bool ok = diff(R[0][i], R[1][i]);
        pass += ok;
        std::printf("%s [%s] %s (%zu calls)\n", ok ? "PASS" : "FAIL", name.c_str(), R[0][i].name.c_str(), R[0][i].calls.size());
    }
    std::printf("WORLD %s: %d/%zu steps pass\n", name.c_str(), pass, R[0].size());
    if (std::getenv("UNL_DUMP")) {
        for (size_t i = 0; i < R[0].size(); ++i) {
            std::printf("== %s\n", R[0][i].name.c_str());
            for (auto &c : R[0][i].calls) std::printf("   %s\n", c.c_str());
        }
    }
    return pass == int(R[0].size()) ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const uint8_t *base = static_cast<const uint8_t *>(GetModuleHandleA(nullptr));
    uint32_t pe, size_of_image;
    std::memcpy(&pe, base + 0x3c, 4);
    std::memcpy(&size_of_image, base + pe + 0x18 + 56, 4);
    exe_lo = uintptr_t(base), exe_hi = uintptr_t(base) + size_of_image;
    hx::init();
    heap = static_cast<uint8_t *>(_aligned_malloc(kHeap, 64));
    host = static_cast<uint8_t *>(_aligned_malloc(kHost, 64));
    std::memset(heap, 0, kHeap);
    std::memset(host, 0, kHost);
    define_worlds();
    if (argc >= 2) return run_world(argv[1]);
    char self[1024];
    GetModuleFileNameA(nullptr, self, sizeof self);
    const std::string quoted = std::string("\"") + self + "\"";
    int bad = 0;
    std::vector<std::string> failed;
    for (auto &[name, w] : worlds) {
        const intptr_t rc = _spawnl(_P_WAIT, self, quoted.c_str(), name.c_str(), nullptr);
        if (rc != 0) {
            ++bad;
            failed.push_back(name + (rc == 1 ? "" : " (exit " + std::to_string(rc) + ")"));
        }
    }
    std::printf("\nunlocks: %zu worlds, %d failed\n", worlds.size(), bad);
    for (auto &f : failed) std::printf("  failed world: %s\n", f.c_str());
    return bad ? 1 : 0;
}
