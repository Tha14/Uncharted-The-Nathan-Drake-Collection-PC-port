// u2mp rewrite test, group cinema: the old (git) and the new cinema.cpp + cinema_store.cpp on the same fake image
// and the same store directory. Scenarios follow u2mp/docs/spec/cinema.md section 6 plus the branches of the old code.
//
// Per scenario and side: fresh image, fresh store directory (<exe dir>/cinema_fs), fixed clock (time() is faked
// for the whole program), then the body. Compared: the record (guest calls, log lines, state dumps, file listing
// with contents) and the image. Host pointers the modules write into the image (stream buffer, list entries,
// vtable slots) are compared by kind in the image and by content in the dumps.
#include "harness.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <io.h>
#include <system_error>

#include "cinema.h"
#include "cinema_store.h"
#define u2mp u2mp_old
#include "../../out/u2mp_rewrite/cinema/old/cinema.h"
#include "../../out/u2mp_rewrite/cinema/old/cinema_store.h"
#undef u2mp

namespace fs = std::filesystem;
using hx::addr;
using hx::get;
using hx::put;
using hx::rec;

// ---- fake clock and user dir -----------------------------------------------------------------------------------
static long long g_now = 1763100000;   // 2025-11-14
extern "C" long long hx_time64(long long *t) __asm__("_time64");
extern "C" long long hx_time64(long long *t) {
    if (t) *t = g_now;
    return g_now;
}
static std::string g_root, g_user;
static bool g_user_null = false;
extern "C" const char *runtime_file_user_dir(void) { return g_user_null ? nullptr : g_user.c_str(); }
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);

// ---- guest layout ----------------------------------------------------------------------------------------------
constexpr uint64_t kC = 0x113a990, kStream = 0x113a258, kClockPtr = 0x113a040, kNgm = 0x113b760,
                   kNetMode = 0x104f1c8 + 0x2c4, kShowsLocal = 0x104f1b9, kRemoteRow = 0x14cfeb0, kPressedRow = 0x14ce098,
                   kMenu = 0x1136640;
constexpr uint64_t kBox = 0x1400000, kClockObj = 0x1410000, kEvent = 0x1420000, kEvent2 = 0x1420100, kVtbl = 0x1421000,
                   kWriteStub = 0x1422000, kTask = 0x1430000, kState = 0x1440000, kArgs = 0x1450000, kSession = 0x1460000,
                   kForeign = 0x1470000, kThunk = 0x700000;
constexpr uint64_t kSlotWRE = 0x100abd0, kSlotOLE = 0x100a9c8, kSlotRPE = 0x100a7a0, kSlotRLE = 0x100a6d8;

struct TStream {
    uint32_t *data;
    uint32_t bits, max_bits, bytes;
};
static TStream *gs() { return reinterpret_cast<TStream *>(hx::mem + kStream); }
static bool in_image(uint64_t v) { return v >= addr(0) && v < addr(hx::kImageSize); }

static uint64_t fnv(const void *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t *>(p)[i]) * 1099511628211ull;
    return h;
}
static std::string hex(const uint8_t *p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) std::snprintf(b, sizeof b, "%02x", p[i]), s += b;
    return s;
}

// ---- host stand-ins for the game -------------------------------------------------------------------------------
static void put_bits(TStream *s, uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        const uint32_t k = s->bits + i, m = 0x80000000u >> (k & 31);
        if ((v >> (n - 1 - i)) & 1) s->data[k >> 5] |= m;
        else s->data[k >> 5] &= ~m;
    }
    s->bits += n;
}
U2MP_GUEST static uint64_t orig_send(void *ngm, void *ev, uint64_t mask) {
    rec("orig SendNetEvent ngm=%s ev=%s mask=0x%llx", hx::where(ngm).c_str(), hx::where(ev).c_str(), (unsigned long long)mask);
    return mask ^ 0x5a5a;
}
U2MP_GUEST static uint64_t orig_session_send(void *ses, void *ev, uint64_t mask) {
    rec("orig NetSession::SendNetEvent ses=%s ev=%s mask=0x%llx", hx::where(ses).c_str(), hx::where(ev).c_str(),
        (unsigned long long)mask);
    return mask + 0x77;
}
static const char *dkind(const void *p) { return !p ? "null" : in_image(uint64_t(uintptr_t(p))) ? "img" : "host"; }
U2MP_GUEST static void orig_wre(void *st) {
    TStream *s = gs();
    rec("orig WaitReady::Exit state=%s data=%s bits=0x%x max=0x%x bytes=0x%x C0=%d", hx::where(st).c_str(), dkind(s->data),
        s->bits, s->max_bits, s->bytes, get<int32_t>(kC));
    if (s->data && !in_image(uint64_t(uintptr_t(s->data)))) {   // the stream start, as the game writes it
        put_bits(s, 5, 32);
        put_bits(s, 7, 32);
        put_bits(s, 0x3f800000, 32);
        put_bits(s, 42, 32);
    }
}
U2MP_GUEST static void orig_ole(void *st) { rec("orig online Leave::Enter state=%s C0=%d rec?", hx::where(st).c_str(), get<int32_t>(kC)); }
U2MP_GUEST static void orig_rpe(void *st) {
    rec("orig RePlaying::Exit state=%s C0=%d reading=%d", hx::where(st).c_str(), get<int32_t>(kC), hx::mem[kC + 0x94]);
}
U2MP_GUEST static void orig_rle(void *st) {
    rec("orig replay Leave::Enter state=%s C0=%d reading=%d", hx::where(st).c_str(), get<int32_t>(kC), hx::mem[kC + 0x94]);
}
U2MP_GUEST static void ev_write(void *ev, void *stream) {
    TStream *s = static_cast<TStream *>(stream);
    rec("Write ev=%s stream=%s bits=0x%x", hx::where(ev).c_str(), hx::where(stream).c_str(), s->bits);
    uint16_t v;
    std::memcpy(&v, static_cast<uint8_t *>(ev) + 0x10, 2);
    put_bits(s, v, 16);
}
U2MP_GUEST static void g_pop(void *mm) { rec("pop_page %s", hx::where(mm).c_str()); }
U2MP_GUEST static void g_setnetgame(void *ngm, int32_t g, void *p, int32_t ten) {
    rec("SetNetGame %s %d %s %d", hx::where(ngm).c_str(), g, hx::where(p).c_str(), ten);
}
U2MP_GUEST static void g_menureset(void *mm) { rec("menu reset %s", hx::where(mm).c_str()); }
U2MP_GUEST static void *g_findtask(uint32_t set, uint32_t id) {
    rec("find task 0x%08x 0x%08x", set, id);
    return reinterpret_cast<void *>(addr(kTask));
}
U2MP_GUEST static void g_playtask(void *t) { rec("play task %s", hx::where(t).c_str()); }

// ---- the fake image --------------------------------------------------------------------------------------------
static const uint8_t kPump[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xe4};
static const uint8_t kSend[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x58};
static const uint8_t kProgress[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x8b, 0x07};
static const uint8_t kCinemaPage[] = {0x83, 0x3f, 0x06, 0x75, 0x0c, 0x48, 0x8d, 0x3d, 0xd4,
                                      0x25, 0xb5, 0x00, 0xe9, 0x7f, 0x86, 0xfe, 0xff, 0xc3};
static const uint8_t kList[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec};
static const uint8_t kRefresh[] = {0x48, 0x8d, 0x3d, 0xa9, 0x21, 0xb5, 0x00, 0xbe, 0x1e, 0xd1, 0x84, 0x1d, 0x31, 0xd2, 0xe9, 0xed};
static const uint8_t kToggle[] = {0x48, 0x8d, 0x05, 0xe2, 0xab, 0xa6, 0x00, 0x80, 0x30, 0x01, 0x48, 0x8d, 0x3d, 0x5f, 0x20, 0xb5};
static const uint8_t kDelete[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x1d, 0x43, 0x24, 0xb5, 0x00, 0x48, 0x89, 0xdf};
static const uint8_t kUpload[] = {0x48, 0x8d, 0x05, 0xb9, 0x66, 0xb5, 0x00, 0x8b, 0x80, 0xb8, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x0d};
static const uint8_t kStart[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x3d, 0x95, 0x20, 0xb5, 0x00, 0xe8, 0x30, 0x8c, 0xfe, 0xff};
static const uint8_t kQuery[] = {0x84, 0xc9, 0x0f, 0x85, 0x68, 0xfd, 0xff, 0xff, 0x44, 0x89,
                                 0x05, 0x27, 0x80, 0xe7, 0x00, 0xe9, 0x5c, 0xfd, 0xff, 0xff};
static const uint8_t kRet[] = {0xc3, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0, 0, 0, 0, 0, 0x66, 0x90};

static void put_bytes(uint64_t off, const uint8_t *b, size_t n) { std::memcpy(hx::mem + off, b, n); }
// after the moved prologue: call the host stand-in, then the matching epilogue
static void send_body(uint64_t off, void *host) {
    put_bytes(off, kSend, sizeof kSend);
    uint8_t *p = hx::mem + off + 17;
    const uint64_t h = uint64_t(uintptr_t(host));
    const uint8_t tail[] = {0x41, 0xff, 0xd3, 0x48, 0x83, 0xc4, 0x58, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3};
    p[0] = 0x49, p[1] = 0xbb;
    std::memcpy(p + 2, &h, 8);
    std::memcpy(p + 10, tail, sizeof tail);
}
static void build_image() {
    put_bytes(0x608c40, kPump, sizeof kPump);
    send_body(0x644ab0, reinterpret_cast<void *>(&orig_send));
    send_body(0x66a3d0, reinterpret_cast<void *>(&orig_session_send));
    for (uint64_t o : {0x5e9d80ull, 0x5f0ea0ull, 0x5e41d0ull, 0x5e41e0ull}) put_bytes(o, kRet, sizeof kRet);
    hx::stub_ptr(kSlotWRE, 0x5f2830, reinterpret_cast<void *>(&orig_wre));
    hx::stub_ptr(kSlotOLE, 0x5f00d0, reinterpret_cast<void *>(&orig_ole));
    hx::stub_ptr(kSlotRPE, 0x5e8740, reinterpret_cast<void *>(&orig_rpe));
    hx::stub_ptr(kSlotRLE, 0x5e5520, reinterpret_cast<void *>(&orig_rle));
    put_bytes(0x5e3f60, kProgress, sizeof kProgress);
    put_bytes(0x5e4060, kCinemaPage, sizeof kCinemaPage);
    put_bytes(0x5e4080, kList, sizeof kList);
    put_bytes(0x5e4490, kRefresh, sizeof kRefresh);
    put_bytes(0x5e45d0, kToggle, sizeof kToggle);
    put_bytes(0x5e41f0, kDelete, sizeof kDelete);
    put_bytes(0x5e42d0, kUpload, sizeof kUpload);
    put_bytes(0x5e45a0, kStart, sizeof kStart);
    put_bytes(0x657e7a, kQuery, sizeof kQuery);
    const uint8_t epi[] = {0x48, 0x83, 0xc4, 0x08, 0xc3};   // add rsp, 8; ret (the query's common exit, for the test)
    put_bytes(0x657bea, epi, sizeof epi);
    const uint8_t th[] = {0x48, 0x83, 0xec, 0x08, 0xe9};    // sub rsp, 8; jmp query site
    put_bytes(kThunk, th, sizeof th);
    put<int32_t>(kThunk + 5, int32_t(int64_t(0x657e7a) - int64_t(kThunk + 9)));
    hx::stub(0x5cc6f0, reinterpret_cast<void *>(&g_pop));
    hx::stub(0x63dc30, reinterpret_cast<void *>(&g_setnetgame));
    hx::stub(0x5cd1e0, reinterpret_cast<void *>(&g_menureset));
    hx::stub(0xa572d0, reinterpret_cast<void *>(&g_findtask));
    hx::stub(0xa5be10, reinterpret_cast<void *>(&g_playtask));
    put<uint64_t>(kMenu + 0x17d0, addr(kBox));
    put<uint64_t>(kClockPtr, addr(kClockObj));
    put<uint32_t>(kClockObj + 0xc, 1000);
    put<int16_t>(kNgm + 0x1ca0, int16_t(-3));
    put<uint16_t>(kNgm + 0x1ca2, 0x7777);
    for (int i = 0; i < 0x18; i++) hx::mem[kNgm + 0x21b0 + i] = uint8_t(0xa0 + i);
    put<uint64_t>(kEvent, addr(kVtbl));
    hx::mem[kEvent + 8] = 0x11;
    put<uint16_t>(kEvent + 0x10, 0x1234);
    put<uint64_t>(kEvent2, addr(kVtbl));
    hx::mem[kEvent2 + 8] = 0x22;
    put<uint16_t>(kEvent2 + 0x10, 0xbeef);
    hx::stub_ptr(kVtbl + 0x18, kWriteStub, reinterpret_cast<void *>(&ev_write));
    // header bytes the module does not write (stay as they are)
    for (int i = 0; i < 0x4b0; i++) hx::mem[kC + 0x1c8 + i] = uint8_t(i * 3 + 1);
}

// ---- entry points through the image ----------------------------------------------------------------------------
template <class F> static F at(uint64_t off) { return reinterpret_cast<F>(hx::mem + off); }
using V1 = void(U2MP_GUEST *)(void *);
static bool hooked(uint64_t off) { return hx::mem[off] == 0x49 && hx::mem[off + 1] == 0xbb; }
static bool slot_host(uint64_t slot) {
    const uint64_t v = get<uint64_t>(slot);
    return v > 0xffffffffull && !in_image(v) && v != addr(0x5e8740) + 0x100000000ull;
}
static uint64_t E1() { return at<uint64_t(U2MP_GUEST *)(void *)>(0x608c40)(reinterpret_cast<void *>(addr(kC))); }
static uint64_t E2(uint64_t ev, uint64_t mask) {
    return at<uint64_t(U2MP_GUEST *)(void *, void *, uint64_t)>(0x644ab0)(reinterpret_cast<void *>(addr(kNgm)),
                                                                         reinterpret_cast<void *>(ev), mask);
}
static uint64_t E3(uint64_t ev, uint64_t mask) {
    return at<uint64_t(U2MP_GUEST *)(void *, void *, uint64_t)>(0x66a3d0)(reinterpret_cast<void *>(addr(kSession)),
                                                                         reinterpret_cast<void *>(ev), mask);
}
static void E4(uint64_t ev) { at<void(U2MP_GUEST *)(void *, float)>(0x5e9d80)(reinterpret_cast<void *>(ev), 0.5f); }
static void slotcall(uint64_t slot) { reinterpret_cast<V1>(get<uint64_t>(slot))(reinterpret_cast<void *>(addr(kState))); }
static void E5() { slotcall(kSlotWRE); }
static void E6() { at<V1>(0x5f0ea0)(reinterpret_cast<void *>(addr(kState))); }
static void E7() { slotcall(kSlotOLE); }
static void E8() { slotcall(kSlotRPE); }
static void E9() { slotcall(kSlotRLE); }
static void E10(int32_t e) {
    put<int32_t>(kArgs, e);
    at<void(U2MP_GUEST *)(int32_t *)>(0x5e3f60)(reinterpret_cast<int32_t *>(hx::mem + kArgs));
}
static void E11(int32_t e) {
    put<int32_t>(kArgs + 0x40, e);
    at<void(U2MP_GUEST *)(int32_t *, uint64_t)>(0x5e4060)(reinterpret_cast<int32_t *>(hx::mem + kArgs + 0x40), 0);
}
static void E12(int32_t row) {
    put<int32_t>(kArgs + 0x10, 77);
    put<int32_t>(kArgs + 0x14, row);
    at<void(U2MP_GUEST *)(int32_t *)>(0x5e4080)(reinterpret_cast<int32_t *>(hx::mem + kArgs + 0x10));
}
static void E13() { at<V1>(0x5e4490)(nullptr); }
static void E14() { at<V1>(0x5e45d0)(nullptr); }
static void E15() { at<V1>(0x5e41d0)(nullptr); }
static void E16() { at<V1>(0x5e41e0)(nullptr); }
static void E17() { at<V1>(0x5e41f0)(nullptr); }
static void E18() { at<V1>(0x5e42d0)(nullptr); }
static void E19() { at<V1>(0x5e45a0)(nullptr); }
static void E20(int32_t value, uint64_t rcx) {   // r8d = value, cl = local flag
    at<void(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)>(kThunk)(0, 0, 0, rcx,
                                                                                     0xdead00000000ull | uint32_t(value));
}

// ---- sides -----------------------------------------------------------------------------------------------------
struct Side {
    const char *name;
    void (*install)(int &);
    bool (*recording)();
    void (*record_event)(void *);
    bool (*read_selected)(std::vector<uint8_t> &, std::string &);
    int (*add_received)(const uint8_t *, size_t, const std::string &);
};
static Side OLD{"old", &u2mp_old::install_cinema, &u2mp_old::cinema::recording, &u2mp_old::cinema::record_event,
                &u2mp_old::cinema::read_selected, &u2mp_old::cinema::add_received};
static Side NEW{"new", &u2mp::install_cinema, &u2mp::cinema::recording, &u2mp::cinema::record_event,
                &u2mp::cinema::read_selected, &u2mp::cinema::add_received};

// ---- state dumps -----------------------------------------------------------------------------------------------
static void dump(const char *tag, Side &S) {
    TStream *s = gs();
    rec("[%s] stream data=%s bits=0x%x max=0x%x bytes=0x%x recording=%d", tag, dkind(s->data), s->bits, s->max_bits,
        s->bytes, S.recording());
    if (s->data && !in_image(uint64_t(uintptr_t(s->data))))
        rec("[%s] buffer %016llx", tag, (unsigned long long)fnv(s->data, 0x20040));
    rec("[%s] C0=%d reading=%d eof=%d flags=%s sel=%d count=%d scount=%d local=%d remote=%d pressed=%d hdr=%016llx %s", tag,
        get<int32_t>(kC), hx::mem[kC + 0x94], hx::mem[kC + 0x95], hex(hx::mem + kC + 0x9c, 8).c_str(), get<int32_t>(kC + 0x1a4),
        get<int32_t>(kC + 0x1a8), get<int32_t>(kC + 0x1b8), hx::mem[kShowsLocal], get<int32_t>(kRemoteRow),
        get<int32_t>(kPressedRow), (unsigned long long)fnv(hx::mem + kC + 0x1c8, 0x4b0), hex(hx::mem + kC + 0x1c8, 0x34).c_str());
    const uint64_t le = get<uint64_t>(kC + 0x1b0), se = get<uint64_t>(kC + 0x1c0);
    if (le && !in_image(le)) {
        const uint8_t *e = reinterpret_cast<const uint8_t *>(uintptr_t(le));
        rec("[%s] local entries %016llx", tag, (unsigned long long)fnv(e, 20 * 0x6c8));
        for (int i = 0; i < 20; i++) {
            const uint8_t *x = e + i * 0x6c8;
            bool any = false;
            for (int k = 0; k < 0x6c8 && !any; k++) any = x[k] != 0;
            if (!any) continue;
            rec("[%s]  %d date='%.255s' name='%.255s' stat=%d time=%llu size=%u mode=0x%x hdr=%016llx", tag, i,
                reinterpret_cast<const char *>(x), reinterpret_cast<const char *>(x + 0x100), x[0x200],
                (unsigned long long)*reinterpret_cast<const uint64_t *>(x + 0x208),
                *reinterpret_cast<const uint32_t *>(x + 0x210), *reinterpret_cast<const uint32_t *>(x + 0x214),
                (unsigned long long)fnv(x + 0x218, 0x4b0));
        }
    } else {
        rec("[%s] local entries ptr %s", tag, le ? "img" : "null");
    }
    if (se && !in_image(se)) rec("[%s] server entries %016llx", tag, (unsigned long long)fnv(reinterpret_cast<void *>(se), 20 * 0x6c8));
    else rec("[%s] server entries ptr %s", tag, se ? "img" : "null");
    rec("[%s] box %s", tag, hex(hx::mem + kBox, 0x60).c_str());
}

static std::vector<uint8_t> read_file(const fs::path &p) {
    std::vector<uint8_t> v;
    std::FILE *f = std::fopen(p.string().c_str(), "rb");
    if (!f) return v;
    uint8_t b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
    std::fclose(f);
    return v;
}
static bool readonly(const fs::path &p) {
    std::error_code ec;
    auto st = fs::status(p, ec);
    return !ec && (st.permissions() & fs::perms::owner_write) == fs::perms::none;
}
static void dump_files() {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(g_root, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        const std::string rel = fs::relative(p, g_root).generic_string();
        char b[600];
        if (fs::is_directory(p)) {
            std::snprintf(b, sizeof b, "file D %s", rel.c_str());
        } else {
            auto v = read_file(p);
            std::snprintf(b, sizeof b, "file F %s size=%zu ro=%d %016llx head=%s", rel.c_str(), v.size(), readonly(p),
                          (unsigned long long)fnv(v.data(), v.size()), hex(v.data(), std::min<size_t>(v.size(), 0x38)).c_str());
        }
        out.push_back(b);
    }
    std::sort(out.begin(), out.end());
    for (auto &s : out) rec("%s", s.c_str());
}

static void wipe() {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(g_root, ec), end; !ec && it != end; it.increment(ec))
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
    fs::remove_all(g_root, ec);
    if (ec) std::printf("wipe failed: %s\n", ec.message().c_str());
    fs::create_directories(g_root + "/user", ec);
}
static std::string store() { return g_user + "/u2mp/cinema"; }

// ---- recording files for the store -----------------------------------------------------------------------------
static std::vector<uint8_t> header(uint64_t t, int32_t game = 3, int32_t players = 2, uint32_t magic = 0x04b00005) {
    std::vector<uint8_t> h(0x4b0, 0);
    for (size_t i = 0x34; i < 0x4b0; i++) h[i] = uint8_t(i ^ t);
    std::memcpy(h.data(), &magic, 4);
    std::memcpy(h.data() + 4, &game, 4);
    std::memcpy(h.data() + 0x18, &t, 8);
    std::memcpy(h.data() + 0x114, &players, 4);
    return h;
}
static void write_raw(const std::string &path, const std::vector<uint8_t> &v) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::printf("cannot create %s\n", path.c_str());
        return;
    }
    if (!v.empty()) std::fwrite(v.data(), 1, v.size(), f);
    std::fclose(f);
}
static void set_ro(const std::string &path, bool ro) {
    std::error_code ec;
    fs::permissions(path, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    ro ? fs::perm_options::remove : fs::perm_options::add, ec);
}
// a valid recording: header + `stream` bytes of pattern
static void mkrec(const std::string &name, uint64_t t, size_t stream = 64, bool ro = false, int32_t game = 3,
                  const std::string &dir = "") {
    auto v = header(t, game);
    for (size_t i = 0; i < stream; i++) v.push_back(uint8_t(i * 31 + t + (i >> 8)));
    const std::string path = (dir.empty() ? store() : dir) + "/" + name;
    write_raw(path, v);
    if (ro) set_ro(path, true);
}

static void set_env(const char *v) { _putenv_s("UNCHARTED_U2MP_CINEMA", v ? v : ""); }

// install with optional image changes; records the count and what the patches look like
static void inspect() {
    for (uint64_t off : {0x644ab0ull, 0x66a3d0ull}) {
        if (hx::mem[off] != 0xe9) {
            rec("detour 0x%llx not patched: %s", (unsigned long long)off, hex(hx::mem + off, 17).c_str());
            continue;
        }
        const int32_t rel = get<int32_t>(off + 1);
        const uint8_t *jump = hx::mem + off + 5 + rel;
        rec("detour 0x%llx rel=0x%x jump=%s %s..%s tramp=%s rest=%s", (unsigned long long)off, rel, hx::where(jump).c_str(),
            hex(jump, 2).c_str(), hex(jump + 10, 2).c_str(), hex(jump - 32, 31).c_str(), hex(hx::mem + off + 5, 12).c_str());
    }
    if (hx::mem[0x657e7a] == 0xe9) {
        const int32_t rel = get<int32_t>(0x657e7b);
        const uint8_t *st = hx::mem + 0x657e7a + 5 + rel;
        int32_t back;
        std::memcpy(&back, st + 19, 4);
        rec("query rel=0x%x stub=%s %s %s back=%s rest=%s", rel, hx::where(st).c_str(), hex(st, 8).c_str(), hex(st + 16, 3).c_str(),
            hx::where(st + 23 + back).c_str(), hex(hx::mem + 0x657e7f, 15).c_str());
    } else {
        rec("query not patched");
    }
    for (uint64_t s : {kSlotWRE, kSlotOLE, kSlotRPE, kSlotRLE})
        rec("slot 0x%llx %s", (unsigned long long)s, slot_host(s) ? "host" : hx::where(reinterpret_cast<void *>(get<uint64_t>(s))).c_str());
}
static int install(Side &S, const std::function<void()> &tweak = {}) {
    build_image();
    if (tweak) tweak();
    int n = 100;
    S.install(n);
    rec("installed %d", n - 100);
    inspect();
    return n - 100;
}

// ---- scenario runner -------------------------------------------------------------------------------------------
static std::vector<uint8_t> normalized() {
    auto s = hx::snapshot();
    for (uint64_t off : {kStream, kC + 0x1b0, kC + 0x1c0, kSlotWRE, kSlotOLE, kSlotRPE, kSlotRLE}) {
        uint64_t v;
        std::memcpy(&v, s.data() + off, 8);
        if (v && !in_image(v)) {
            v = 0x54534f48;   // "HOST"
            std::memcpy(s.data() + off, &v, 8);
        }
    }
    return s;
}
static void cleanup(Side &S) {
    hx::recording = false;
    if (S.recording()) {
        if (hooked(0x5f0ea0) && gs()->data) E6();
        else std::printf("cleanup: %s still recording\n", S.name);
    }
    if (slot_host(kSlotRLE)) E9();
    if (hooked(0x5e3f60)) {
        put<int32_t>(kNetMode, 0);
        E10(6);
    }
    set_env(nullptr);
    g_user_null = false;
}

static int scenarios = 0, passed = 0;
using Body = std::function<void(Side &)>;
static void run(const char *name, const Body &body, const std::function<void()> &fs_setup = {}) {
    std::vector<std::string> r[2];
    std::vector<uint8_t> img[2];
    Side *sides[2] = {&OLD, &NEW};
    for (int k = 0; k < 2; k++) {
        Side &S = *sides[k];
        hx::clear();
        hx::bind_both();
        g_user = g_root + "/user";
        g_user_null = false;
        g_now = 1763100000;
        set_env(nullptr);
        wipe();
        if (fs_setup) fs_setup();
        hx::calls.clear();
        hx::recording = true;
        body(S);
        hx::recording = true;
        dump("end", S);
        dump_files();
        r[k] = hx::calls;
        img[k] = normalized();
        cleanup(S);
        hx::bind_both();
    }
    scenarios++;
    if (hx::same(name, r[0], r[1], &img[0], &img[1])) {
        passed++;
        std::printf("PASS %s (%zu records)\n", name, r[0].size());
    }
    if (std::getenv("HX_VERBOSE"))
        for (auto &l : r[0]) std::printf("   | %s\n", l.c_str());
}

// common: full install, then the list refreshed and file 0 loaded (E10 event 6 in cinema mode)
static void open_list(Side &S) {
    install(S);
    put<int32_t>(kNetMode, 3);
    E10(6);
    put<int32_t>(kNetMode, 0);
    dump("list", S);
}

// close every CRT file descriptor past stderr (the module's open recording): later writes fail
static int close_fds() {
    int n = 0;
    for (int fd = 3; fd < 64; fd++)
        if (_get_osfhandle(fd) != -1) {
            _close(fd);
            n++;
        }
    return n;
}

// ---- store-level tests (both namespaces through the same templates) --------------------------------------------
#define TRAITS(Name, NS)                                                                                               \
    struct Name {                                                                                                      \
        using File = NS::cinema::File;                                                                                 \
        using Stream = NS::cinema::Stream;                                                                             \
        using Writer = NS::cinema::Writer;                                                                             \
        using Reader = NS::cinema::Reader;                                                                             \
        static std::vector<File> list(const std::string &d) { return NS::cinema::list(d); }                            \
        static void fill_entry(const File &f, uint8_t *o) { NS::cinema::fill_entry(f, o); }                            \
        static bool make_room(const std::string &d) { return NS::cinema::make_room(d); }                               \
        static bool remove(const std::string &d, const std::string &n) { return NS::cinema::remove(d, n); }            \
        static bool set_protected(const std::string &d, const std::string &n, bool p) {                                \
            return NS::cinema::set_protected(d, n, p);                                                                 \
        }                                                                                                              \
        static std::string new_name(const std::string &d, std::time_t t) { return NS::cinema::new_name(d, t); }        \
        static bool ensure_dir(const std::string &d) { return NS::cinema::ensure_dir(d); }                             \
        static bool valid_header(const uint8_t *h) { return NS::cinema::valid_header(h); }                             \
        static uint32_t get32(const uint8_t *p) { return NS::cinema::get32(p); }                                       \
        static uint64_t get64(const uint8_t *p) { return NS::cinema::get64(p); }                                       \
        static void put32(uint8_t *p, uint32_t v) { NS::cinema::put32(p, v); }                                         \
        static void put64(uint8_t *p, uint64_t v) { NS::cinema::put64(p, v); }                                         \
    };
TRAITS(OldT, u2mp_old)
TRAITS(NewT, u2mp)

template <class B> static void run_store(const char *name, B body, const std::function<void()> &fs_setup = {}) {
    std::vector<std::string> r[2];
    for (int k = 0; k < 2; k++) {
        g_user = g_root + "/user";
        g_now = 1763100000;
        wipe();
        if (fs_setup) fs_setup();
        hx::calls.clear();
        hx::recording = true;
        if (k == 0) body(OldT{});
        else body(NewT{});
        dump_files();
        r[k] = hx::calls;
    }
    scenarios++;
    if (hx::same(name, r[0], r[1])) {
        passed++;
        std::printf("PASS %s (%zu records)\n", name, r[0].size());
    }
    if (std::getenv("HX_VERBOSE"))
        for (auto &l : r[0]) std::printf("   | %s\n", l.c_str());
}
template <class F> static void rec_files(const std::vector<F> &v) {
    rec("list n=%zu", v.size());
    for (auto &f : v)
        rec("  %s time=%llu size=%u prot=%d hdr=%zu %016llx", f.name.c_str(), (unsigned long long)f.time, f.size, f.protect,
            f.header.size(), (unsigned long long)fnv(f.header.data(), f.header.size()));
}

alignas(64) static uint8_t g_sbuf[0x20040];
template <class S> static void rec_stream(const char *tag, const S &s) {
    rec("%s bits=0x%x max=0x%x bytes=0x%x buf=%016llx", tag, s.bits, s.max_bits, s.bytes, (unsigned long long)fnv(g_sbuf, sizeof g_sbuf));
}
static void fill_pattern(uint32_t seed) {
    for (size_t i = 0; i < sizeof g_sbuf; i++) g_sbuf[i] = uint8_t(i * 7 + seed + (i >> 9));
}

static void invalid_parameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t) {}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    _set_invalid_parameter_handler(invalid_parameter);
    {
        char b[1024];
        GetModuleFileNameA(nullptr, b, sizeof b);
        g_root = (fs::path(b).parent_path() / "cinema_fs").generic_string();
    }
    hx::init();

    // ============================== install ==============================
    run("1a install full, env unset", [](Side &S) { install(S); });
    run("1b install env=0 (recording off)", [](Side &S) { set_env("0"); install(S); });
    run("1c install env='' (Windows CRT removes it: same as unset)", [](Side &S) { set_env(""); install(S); });
    run("1d install env=1", [](Side &S) { set_env("1"); install(S); });
    run("1e install env=05 (first char 0)", [](Side &S) { set_env("05"); install(S); });
    run("1f install env=x0", [](Side &S) { set_env("x0"); install(S); });
    run("2a pump bytes differ", [](Side &S) { install(S, [] { hx::mem[0x608c40 + 15] ^= 1; }); });
    run("2b pump replace_function fails", [](Side &S) { install(S, [] { hx::fail_offsets.push_back(0x608c40); }); });
    struct Br {
        const char *name;
        std::function<void()> f;
    };
    const Br breaks[] = {
        {"3 send bytes differ", [] { hx::mem[0x644ab0 + 16] ^= 1; }},
        {"3 session send bytes differ", [] { hx::mem[0x66a3d0 + 16] ^= 1; }},
        {"3 send + session differ", [] { hx::mem[0x644ab0] ^= 1, hx::mem[0x66a3d0] ^= 1; }},
        {"3 receive hook not a ret stub", [] { hx::mem[0x5e9d80 + 4] = 0x90; }},
        {"3 receive hook replace fails", [] { hx::fail_offsets.push_back(0x5e9d80); }},
        {"3 WaitReady slot differs", [] { put<uint64_t>(kSlotWRE, addr(0x5f2831)); }},
        {"3 WaitReady slot past image end", [] { hx::set_image_both(hx::mem, kSlotWRE + 4); }},
        {"3 online Leave slot past image end (and WaitReady)", [] { hx::set_image_both(hx::mem, kSlotOLE + 7); }},
        {"3 ShowResults not a ret stub", [] { hx::mem[0x5f0ea0] = 0x90; }},
        {"3 online Leave slot differs", [] { put<uint64_t>(kSlotOLE, 0); }},
        {"3 RePlaying slot differs", [] { put<uint64_t>(kSlotRPE, addr(0x5e8740) + 0x100000000ull); }},
        {"3 replay Leave slot differs", [] { put<uint64_t>(kSlotRLE, 0x5e5520); }},
        {"3 PAGE_PROGRESS differs", [] { hx::mem[0x5e3f60 + 15] ^= 1; }},
        {"3 PAGE_CINEMA differs (byte 17)", [] { hx::mem[0x5e4060 + 17] ^= 1; }},
        {"3 LIST_CINEMA differs", [] { hx::mem[0x5e4080] ^= 1; }},
        {"3 cinema refresh differs", [] { hx::mem[0x5e4490 + 15] ^= 1; }},
        {"3 list toggle differs", [] { hx::mem[0x5e45d0 + 7] ^= 1; }},
        {"3 PROTECT not a ret stub", [] { hx::mem[0x5e41d0 + 1] = 0; }},
        {"3 UNPROTECT not a ret stub", [] { hx::mem[0x5e41e0] = 0xcc; }},
        {"3 delete differs", [] { hx::mem[0x5e41f0 + 15] ^= 1; }},
        {"3 upload differs", [] { hx::mem[0x5e42d0 + 15] ^= 1; }},
        {"3 start cinema differs", [] { hx::mem[0x5e45a0 + 15] ^= 1; }},
        {"3 query differs (byte 19)", [] { hx::mem[0x657e7a + 19] ^= 1; }},
        {"3 LIST_CINEMA replace fails", [] { hx::fail_offsets.push_back(0x5e4080); }},
        {"3 PROTECT replace fails", [] { hx::fail_offsets.push_back(0x5e41d0); }},
        {"4 send not executable", [] { hx::noexec.push_back({0x644ab0 + 16, 1}); }},
        {"4 session send not executable", [] { hx::noexec.push_back({0x66a3d0, 1}); }},
        {"4 query not executable", [] { hx::noexec.push_back({0x657e7a + 19, 1}); }},
        {"4 near_stub none", [] { hx::exec_left = 0; }},
        {"4 near_stub 1 (send trampoline only)", [] { hx::exec_left = 1; }},
        {"4 near_stub 3 (session jump missing)", [] { hx::exec_left = 3; }},
        {"4 near_stub 4 (query stub missing)", [] { hx::exec_left = 4; }},
        {"4 everything differs but the pump", [] {
             for (uint64_t o : {0x644ab0ull, 0x66a3d0ull, 0x5e9d80ull, 0x5f0ea0ull, 0x5e3f60ull, 0x5e4060ull, 0x5e4080ull,
                                0x5e4490ull, 0x5e45d0ull, 0x5e41d0ull, 0x5e41e0ull, 0x5e41f0ull, 0x5e42d0ull, 0x5e45a0ull,
                                0x657e7aull})
                 hx::mem[o] ^= 0x10;
             for (uint64_t s : {kSlotWRE, kSlotOLE, kSlotRPE, kSlotRLE}) put<uint64_t>(s, 1);
         }},
    };
    for (const Br &b : breaks) run(b.name, [&b](Side &S) {
            install(S, b.f);
            hx::bind_both();
        });

    // ============================== recording ==============================
    run("5a E5 stream NULL: attach, original, start; then E6 saves", [](Side &S) {
        install(S);
        put<uint32_t>(kC, 7);
        hx::mem[kC + 0x94] = 1, hx::mem[kC + 0x95] = 1;
        E5();
        dump("after E5", S);
        rec("recording()=%d", S.recording());
        g_now += 100;
        E6();
        dump("after E6", S);
        E6();   // not recording: nothing
    });
    run("5b E5 with a foreign stream / our buffer with other max bits", [](Side &S) {
        install(S);
        put<uint64_t>(kStream, addr(kForeign));
        put<uint32_t>(kStream + 8, 99);
        put<uint32_t>(kStream + 12, 0x100000);
        put<uint32_t>(kStream + 16, 55);
        E5();
        dump("foreign", S);
        put<uint32_t>(kStream + 12, 0x1234);   // our buffer, max bits changed: attach again (clears the buffer)
        E5();
        dump("max bits", S);
        put<uint32_t>(kStream + 8, 0x777);     // our buffer, max bits right: attach keeps the cursor
        E5();
        dump("same", S);
        E6();
    });
    run("6a E5 with recording disabled (env 0)", [](Side &S) {
        set_env("0");
        install(S);
        E5();
        dump("e5", S);
        E6();
        E7();
    });
    run("6b E5 with can_record false (receive hook missing)", [](Side &S) {
        install(S, [] { hx::mem[0x5e9d80] = 0x90; });
        E5();
        dump("e5", S);
        E6();
    });
    run("6c E5 with can_record false (send hook missing; session send alone does not matter)", [](Side &S) {
        install(S, [] { hx::mem[0x644ab0] = 0x90; });
        E5();
        E6();
    });
    run("6d E5 with session send missing only: records", [](Side &S) {
        install(S, [] { hx::mem[0x66a3d0] = 0x90; });
        E5();
        E4(addr(kEvent));
        E6();
    });
    run("7a E5 store directory cannot be created", [](Side &S) {
        install(S);
        g_user = g_root + "/blocker";
        write_raw(g_user + "/u2mp", {1, 2, 3});
        E5();
        dump("e5", S);
    });
    run("7b E5 with 20 protected recordings", [](Side &S) {
        install(S);
        for (int i = 0; i < 20; i++) mkrec("p" + std::to_string(i) + ".u2cinema", 1000 + i, 16, true);
        E5();
        dump("e5", S);
    });
    run("7c E5 recording file cannot be created (every candidate name is a directory)", [](Side &S) {
        install(S);
        const std::string stem = store() + "/" + u2mp::cinema::new_name(store(), g_now);
        std::error_code ec;
        fs::create_directories(stem, ec);
        const std::string base = stem.substr(0, stem.size() - 9);
        for (int i = 2; i <= 99; i++) fs::create_directories(base + "-" + std::to_string(i) + ".u2cinema", ec);
        E5();
        dump("e5", S);
    });
    run("7d E5 with 20 writable recordings: oldest deleted", [](Side &S) {
        install(S);
        for (int i = 0; i < 20; i++) mkrec("w" + std::to_string(i) + ".u2cinema", 1000 + i);
        E5();
        E6();
    });
    run("7e E5 with 21 recordings, oldest 3 protected", [](Side &S) {
        install(S);
        for (int i = 0; i < 21; i++) mkrec("w" + std::to_string(i) + ".u2cinema", 1000 + i, 16, i < 3);
        E5();
        E6();
    });
    run("7f E5 with user dir NULL (store under ./u2mp/cinema)", [](Side &S) {
        install(S);
        std::error_code ec;
        fs::create_directories(g_root + "/cwd", ec);
        const fs::path old = fs::current_path();
        fs::current_path(g_root + "/cwd");
        g_user_null = true;
        E5();
        E6();
        fs::current_path(old);
    });
    run("7g E5 with user dir empty", [](Side &S) {
        install(S);
        std::error_code ec;
        fs::create_directories(g_root + "/cwd", ec);
        const fs::path old = fs::current_path();
        fs::current_path(g_root + "/cwd");
        g_user = "";
        E5();
        E6();
        fs::current_path(old);
    });
    run("8 E5 while recording: the old one is saved first, new name -2", [](Side &S) {
        install(S);
        E5();
        E4(addr(kEvent));
        E5();
        dump("second", S);
        E4(addr(kEvent2));
        E6();
    });
    run("9a E2/E3/E4 while recording (clock variants), then saved", [](Side &S) {
        install(S);
        E5();
        rec("E2 -> 0x%llx", (unsigned long long)E2(addr(kEvent), 0x10));
        put<uint32_t>(kClockObj + 0xc, 0);
        rec("E3 -> 0x%llx", (unsigned long long)E3(addr(kEvent2), 0xffffffff00000001ull));
        put<uint64_t>(kClockPtr, 0);
        E4(addr(kEvent));
        put<uint64_t>(kClockPtr, addr(kClockObj));
        put<uint32_t>(kClockObj + 0xc, 0xfedcba98);
        S.record_event(reinterpret_cast<void *>(addr(kEvent2)));
        rec("recording()=%d", S.recording());
        dump("events", S);
        E6();
        rec("recording()=%d", S.recording());
    });
    run("9b E2/E3/E4 not recording, NULL event, foreign data, cursor limit", [](Side &S) {
        install(S);
        rec("E2 -> 0x%llx", (unsigned long long)E2(addr(kEvent), 1));
        rec("E3 -> 0x%llx", (unsigned long long)E3(addr(kEvent), 2));
        E4(addr(kEvent));
        S.record_event(reinterpret_cast<void *>(addr(kEvent)));
        E5();
        rec("E2 -> 0x%llx", (unsigned long long)E2(0, 3));
        E4(0);
        S.record_event(nullptr);
        const uint64_t buf = get<uint64_t>(kStream);
        put<uint64_t>(kStream, addr(kForeign));
        rec("E2 -> 0x%llx", (unsigned long long)E2(addr(kEvent), 4));
        E4(addr(kEvent));
        put<uint64_t>(kStream, buf);
        put<uint32_t>(kStream + 8, 0xfe001);   // (bits + 7) / 8 = 0x1fc01: skipped
        E3(addr(kEvent), 5);
        put<uint32_t>(kStream + 8, 0xfdffa);   // 0x1fc00: recorded
        E3(addr(kEvent2), 6);
        dump("limit", S);
        put<uint32_t>(kStream + 8, 0xfffffffa);   // bits + 7 wraps: (u32) 0 -> recorded? both decide the same
        E4(addr(kEvent));
        dump("wrap", S);
        put<uint32_t>(kStream + 8, 0x400);
        E6();
    });
    run("10 E1 while recording: no flush, one flush, several, foreign data, wrapped cursor", [](Side &S) {
        install(S);
        rec("E1 before -> %llu", (unsigned long long)E1());
        E5();
        uint8_t *buf = reinterpret_cast<uint8_t *>(uintptr_t(get<uint64_t>(kStream)));
        for (size_t i = 16; i < 0x20000; i++) buf[i] = uint8_t(i * 13 + 7 + (i >> 10));
        for (uint32_t bits : {0x3fff9u, 0x40000u, 0x40001u, 0xc0010u, 0x100000u}) {
            put<uint32_t>(kStream + 8, bits);
            rec("E1 -> %llu", (unsigned long long)E1());
            dump("flush", S);
        }
        put<uint64_t>(kStream, addr(kForeign));
        put<uint32_t>(kStream + 8, 0x90000);
        E1();
        dump("foreign", S);
        put<uint64_t>(kStream, uint64_t(uintptr_t(buf)));
        put<uint32_t>(kStream + 8, 0xfffffffa);
        E1();
        dump("wrap", S);
        put<uint32_t>(kStream + 8, 0x4321);
        E6();
    });
    run("11a E7 finishes, then the original", [](Side &S) {
        install(S);
        E5();
        E2(addr(kEvent), 0);
        E7();
        dump("e7", S);
        E7();
    });
    run("11b E19 finishes the recording first (then nothing to play)", [](Side &S) {
        install(S);
        E5();
        E4(addr(kEvent));
        E19();
        dump("e19", S);
    });
    run("11c write failure: failed, removed", [](Side &S) {
        install(S);
        E5();
        rec("closed fds %d", close_fds());
        put<uint32_t>(kStream + 8, 0x40001);
        E1();
        put<uint32_t>(kStream + 8, 0x100);
        E6();
        dump("e6", S);
    });

    // ============================== playback ==============================
    run("12a E19 plays the selected file", [](Side &S) {
        open_list(S);
        E12(1);
        E19();
        dump("e19", S);
        E9();
    }, [] { mkrec("a.u2cinema", 5000, 300); mkrec("b.u2cinema", 4000, 0x1234, false, 9); });
    run("12b E19 with selected out of range (no files)", [](Side &S) {
        open_list(S);
        E19();
        dump("e19", S);
        put<int32_t>(kC + 0x1a4, -1);
        E19();
    });
    run("12c E19 file missing", [](Side &S) {
        open_list(S);
        fs::remove(store() + "/a.u2cinema");
        E19();
        dump("e19", S);
    }, [] { mkrec("a.u2cinema", 5000, 300); });
    run("12d E19 invalid header (C+0x1c8 holds what was read)", [](Side &S) {
        open_list(S);
        auto h = header(5000, 3, 11);
        h.resize(0x600, 0x5a);
        write_raw(store() + "/a.u2cinema", h);
        E19();
        dump("e19", S);
    }, [] { mkrec("a.u2cinema", 5000, 300); });
    run("12e E19 short file", [](Side &S) {
        open_list(S);
        auto h = header(5000);
        h.resize(0x100);
        write_raw(store() + "/a.u2cinema", h);
        E19();
        dump("e19", S);
    }, [] { mkrec("a.u2cinema", 5000, 300); });
    for (size_t len : {size_t(0), size_t(100), size_t(0x10000), size_t(0x10001), size_t(0x20000), size_t(0x23456), size_t(0x50000)}) {
        static char names[16][64];
        static int ni = 0;
        char *nm = names[ni++];
        std::snprintf(nm, 64, "13 playback pump, stream 0x%zx bytes", len);
        run(nm, [](Side &S) {
            open_list(S);
            E19();
            dump("open", S);
            rec("E1 -> %llu", (unsigned long long)E1());
            dump("pump", S);
            for (uint32_t bits : {0x20000u, 0x20001u, 0x7fff0u, 0x30000u, 0xfffffu, 0x41000u, 0x100u, 0xfffffffau, 0u}) {
                put<uint32_t>(kStream + 8, bits);
                E1();
                dump("pump", S);
            }
            E9();
            E1();
            dump("closed", S);
        }, [len] { mkrec("a.u2cinema", 5000, len); });
    }
    run("14 E8/E9 with and without an open reader", [](Side &S) {
        open_list(S);
        E8();
        E9();
        E19();
        E8();
        dump("e8", S);
        E19();
        E9();
        dump("e9", S);
        E19();
        put<uint32_t>(kC, 5), hx::mem[kC + 0x94] = 7;
        E9();
    }, [] { mkrec("a.u2cinema", 5000, 300); });

    // ============================== menus ==============================
    run("15a E10 events 2/3/4 (flags), 5/7/0/-1 (nothing)", [](Side &S) {
        open_list(S);
        hx::mem[kC + 0x9c + 7] = 9;
        for (int e : {2, 3, 4, 5, 7, 0, -1, 1}) {
            hx::mem[kC + 0xa0] = 9;
            E10(e);
            dump("ev", S);
        }
        put<int32_t>(kC + 0x1a4, 5);
        E10(3);
        dump("ev oor", S);
        put<int32_t>(kC + 0x1a4, 1);
        E10(4);
        dump("ev prot", S);
    }, [] { mkrec("a.u2cinema", 5000); mkrec("b.u2cinema", 4000, 64, true); });
    run("15b E10 event 6, no request, mode not 3", [](Side &S) {
        install(S);
        put<int32_t>(kNetMode, 2);
        E10(6);
        dump("e6", S);
    }, [] { mkrec("a.u2cinema", 5000); });
    run("15c E10 event 6 in cinema mode with files", [](Side &S) {
        install(S);
        put<int32_t>(kNetMode, 3);
        E10(6);
        dump("e6", S);
        E10(6);   // the request is not kept: same again because mode is still 3
    }, [] { mkrec("a.u2cinema", 5000); mkrec("b.u2cinema", 6000, 10, false, 0x12345); });
    run("15d E10 event 6 cinema mode, no files, box NULL", [](Side &S) {
        install(S);
        put<uint64_t>(kMenu + 0x17d0, 0);
        put<int32_t>(kNetMode, 3);
        E10(6);
    });
    run("15e E10 event 6 cinema mode, no files, box set", [](Side &S) {
        install(S);
        put<int32_t>(kNetMode, 3);
        E10(6);
    });
    run("15f E13 then event 6: refresh, no load, list page", [](Side &S) {
        install(S);
        E13();
        dump("e13", S);
        E10(6);
        E10(6);
    }, [] { mkrec("a.u2cinema", 5000); });
    run("15g E14 then event 6: refresh, no ShowPage", [](Side &S) {
        install(S);
        E14();
        E10(6);
    }, [] { mkrec("a.u2cinema", 5000); });
    run("15h E13, E14, event 2, event 6 (last request wins, kept over other events)", [](Side &S) {
        install(S);
        E13();
        E14();
        E10(2);
        E10(6);
        E14();
        E13();
        E10(6);
    });
    struct Ref {
        const char *name;
        int32_t sel;
        std::function<void()> fsu;
    };
    auto many = [](int n, int prot_every) {
        return [n, prot_every] {
            for (int i = 0; i < n; i++) mkrec("r" + std::to_string(100 + i) + ".u2cinema", 7000 + i * 11, 20 + i, prot_every && i % prot_every == 0, i);
        };
    };
    const Ref refs[] = {
        {"16a refresh 0 files", 0, [] {}},
        {"16b refresh 1 file", 0, many(1, 0)},
        {"16c refresh 20 files", 3, many(20, 0)},
        {"16d refresh 25 files (newest 20), sel past end", 24, many(25, 0)},
        {"16e refresh sel -1", -1, many(5, 0)},
        {"16f refresh sel in range", 2, many(5, 2)},
        {"16g refresh sel past end", 7, many(5, 0)},
        {"16h refresh sel -5 with 0 files", -5, [] {}},
        {"16i refresh junk files", 1, [] {
             mkrec("good.u2cinema", 9000);
             mkrec("upper.U2CINEMA", 9001);
             mkrec("bak.u2cinema.bak", 9002);
             mkrec("noext", 9003);
             mkrec(".u2cinema", 9004);
             write_raw(store() + "/short.u2cinema", std::vector<uint8_t>(0x4af, 0));
             auto h = header(9005, 3, 2, 0x04b00006);
             write_raw(store() + "/magic.u2cinema", h);
             write_raw(store() + "/game-2.u2cinema", header(9006, -2));
             write_raw(store() + "/game8000.u2cinema", header(9007, 0x8000));
             write_raw(store() + "/game7fff.u2cinema", header(9008, 0x7fff));
             write_raw(store() + "/gamem1.u2cinema", header(9009, -1));
             write_raw(store() + "/pl11.u2cinema", header(9010, 1, 11));
             write_raw(store() + "/pl-1.u2cinema", header(9011, 1, -1));
             write_raw(store() + "/pl10.u2cinema", header(9012, 1, 10));
             write_raw(store() + "/pl0.u2cinema", header(9012, 1, 0));
             write_raw(store() + "/exact.u2cinema", header(9013));
             write_raw(store() + "/hugetime.u2cinema", header(0x4000000000000000ull));
             write_raw(store() + "/negtime.u2cinema", header(uint64_t(-5)));
             std::error_code ec;
             fs::create_directories(store() + "/dir.u2cinema", ec);
             mkrec("sub.u2cinema", 9014, 64, false, 3, store() + "/dir.u2cinema");
             mkrec("ro.u2cinema", 9015, 64, true);
         }},
        {"16j refresh equal times: larger name first", 0, [] {
             mkrec("a.u2cinema", 100);
             mkrec("c.u2cinema", 100);
             mkrec("b.u2cinema", 100);
             mkrec("B.u2cinema", 100);
             mkrec("z.u2cinema", 99);
             mkrec("0.u2cinema", 101);
         }},
    };
    for (const Ref &r : refs)
        run(r.name, [&r](Side &S) {
            install(S);
            E10(2);
            put<int32_t>(kC + 0x1a4, r.sel);
            E14();
            E10(6);
            dump("refresh", S);
        }, r.fsu);
    run("17 E12 rows -1, count, selected, other", [](Side &S) {
        open_list(S);
        for (int row : {-1, 3, 4, 0, 2, 2, 1}) {
            E12(row);
            dump("row", S);
        }
    }, [] { mkrec("a.u2cinema", 5000); mkrec("b.u2cinema", 4000, 64, true, 7); mkrec("c.u2cinema", 3000, 64, false, 0x10005); });
    run("17b E12 before the lists were attached", [](Side &S) {
        install(S);
        put<int32_t>(kC + 0x1a8, 5);
        put<int32_t>(kC + 0x1a4, 1);
        E12(1);
        dump("row", S);
    });
    run("18 E15/E16 protect / unprotect / out of range / failure", [](Side &S) {
        open_list(S);
        E15();
        dump("prot", S);
        E15();
        dump("prot again", S);
        E16();
        dump("unprot", S);
        E12(1);
        E16();
        dump("unprot b", S);
        fs::remove(store() + "/a.u2cinema");
        E12(0);
        E15();
        dump("missing", S);
        E16();
        put<int32_t>(kC + 0x1a4, 9);
        E15();
        E16();
        put<int32_t>(kC + 0x1a4, -1);
        E15();
    }, [] { mkrec("a.u2cinema", 5000); mkrec("b.u2cinema", 4000, 64, true); });
    run("18b E15 with no files", [](Side &S) {
        install(S);
        E15();
        E16();
    });
    run("19a E17 at index 0", [](Side &S) {
        open_list(S);
        E17();
        dump("del", S);
    }, [] { mkrec("a.u2cinema", 5000); mkrec("b.u2cinema", 4000); mkrec("c.u2cinema", 3000); });
    run("19b E17 at index 2, then 1", [](Side &S) {
        open_list(S);
        E12(2);
        E17();
        dump("del", S);
        E17();
        dump("del", S);
    }, [] { mkrec("a.u2cinema", 5000); mkrec("b.u2cinema", 4000); mkrec("c.u2cinema", 3000); });
    run("19c E17 on a protected file", [](Side &S) {
        open_list(S);
        E12(1);
        E17();
        dump("del", S);
    }, [] { mkrec("a.u2cinema", 5000); mkrec("b.u2cinema", 4000, 64, true); mkrec("c.u2cinema", 3000); });
    run("19d E17 deletes the last file (message), then out of range", [](Side &S) {
        open_list(S);
        E17();
        dump("del", S);
        E17();
        dump("del", S);
    }, [] { mkrec("a.u2cinema", 5000); });
    run("19e E17 on a file already gone", [](Side &S) {
        open_list(S);
        fs::remove(store() + "/a.u2cinema");
        E17();
    }, [] { mkrec("a.u2cinema", 5000); mkrec("b.u2cinema", 4000); });
    run("20 E20 list query set", [](Side &S) {
        install(S);
        put<int32_t>(kC + 0x1a4, 7);
        E20(7, 1);        // before attach: equals the raw selected -> nothing
        dump("q", S);
        E20(1, 1);        // load attaches (count 0) -> out of range
        dump("q", S);
        put<int32_t>(kNetMode, 3);
        E10(6);
        put<int32_t>(kNetMode, 0);
        E20(5, 0);
        E20(-9, 0x100);   // cl = 0: remote row
        dump("q", S);
        E20(0, 0x01);
        E20(2, 0xff);
        dump("q", S);
        E20(3, 1);
        E20(-1, 1);
        dump("q", S);
    }, [] { mkrec("a.u2cinema", 5000); mkrec("b.u2cinema", 4000); mkrec("c.u2cinema", 3000, 64, false, 0x8001 - 0x10000); });
    run("21 E11 does nothing, E18 picks a friend", [](Side &S) {
        install(S);
        E11(6);
        E11(1);
        E18();
        dump("menus", S);
    });

    // ============================== public functions ==============================
    run("22 read_selected", [](Side &S) {
        install(S);
        std::vector<uint8_t> bytes{1, 2, 3};
        std::string name = "keep";
        auto rs = [&](const char *t) {
            const bool ok = S.read_selected(bytes, name);
            rec("read_selected %s -> %d name='%s' bytes=%zu %016llx", t, ok, name.c_str(), bytes.size(),
                (unsigned long long)fnv(bytes.data(), bytes.size()));
        };
        rs("no lists");
        put<int32_t>(kNetMode, 3);
        E10(6);
        rs("valid");
        E12(1);
        auto h = header(1, 3, 11);
        h.resize(0x700, 3);
        write_raw(store() + "/b.u2cinema", h);
        rs("invalid header");
        write_raw(store() + "/b.u2cinema", std::vector<uint8_t>(0x4af, 0));
        rs("short");
        std::error_code ec;
        fs::resize_file(store() + "/b.u2cinema", 0x4000000, ec);
        rs("64 MiB");
        fs::resize_file(store() + "/b.u2cinema", 0x3ffffff, ec);
        rs("64 MiB - 1");
        write_raw(store() + "/b.u2cinema", header(1));
        rs("exactly header");
        fs::remove(store() + "/b.u2cinema");
        rs("missing");
        put<int32_t>(kC + 0x1a4, 2);
        rs("out of range");
    }, [] { mkrec("a.u2cinema", 5000, 1000); mkrec("b.u2cinema", 4000, 77); });
    run("23a add_received invalid input", [](Side &S) {
        install(S);
        auto good = header(1234);
        rec("null -> %d", S.add_received(nullptr, 0x4b0, "x.u2cinema"));
        rec("short -> %d", S.add_received(good.data(), 0x4af, "x.u2cinema"));
        auto bad = header(1234, 3, 2, 5);
        rec("bad magic -> %d", S.add_received(bad.data(), bad.size(), "x.u2cinema"));
        bad = header(1234, 0x8000);
        rec("bad game -> %d", S.add_received(bad.data(), bad.size(), "x.u2cinema"));
        g_user = g_root + "/blocker";
        write_raw(g_user + "/u2mp", {1});
        rec("no dir -> %d", S.add_received(good.data(), good.size(), "x.u2cinema"));
        dump("a", S);
    });
    run("23b add_received all 20 protected", [](Side &S) {
        install(S);
        auto good = header(1234);
        rec("full -> %d", S.add_received(good.data(), good.size(), "x.u2cinema"));
    }, [] { for (int i = 0; i < 20; i++) mkrec("p" + std::to_string(i) + ".u2cinema", 1000 + i, 16, true); });
    run("23c add_received names", [](Side &S) {
        install(S);
        auto data = header(4444);
        for (int i = 0; i < 100; i++) data.push_back(uint8_t(i));
        const std::string names[] = {
            "match.u2cinema", "match.u2cinema", "", std::string(55, 'n') + ".u2cinema", std::string(54, 'm') + ".u2cinema",
            "a/b.u2cinema", "a\\b.u2cinema", "c:x.u2cinema", ".hidden.u2cinema", "file.bin", ".u2cinema", "x.u2cinema",
            "u2cinema", "X.U2CINEMA", "space name.u2cinema", "trail.u2cinema ", "dot.u2cinema."};
        for (const auto &n : names) {
            data[0x20] = uint8_t(data[0x20] + 1);
            rec("add '%s' -> %d", n.c_str(), S.add_received(data.data(), data.size(), n));
            dump("add", S);
        }
    });
    run("23d add_received with 20 writable files (oldest deleted) and a selected row", [](Side &S) {
        open_list(S);
        E12(4);
        auto data = header(1);
        rec("add -> %d", S.add_received(data.data(), data.size(), "new.u2cinema"));
        dump("add", S);
    }, many(20, 0));

    // ============================== store (cinema_store directly) ==============================
    run_store("24a new_name collisions 0/1/98/99/100, bad time", [](auto T) {
        using X = decltype(T);
        const std::string d = store();
        X::ensure_dir(d);
        const std::time_t t = 1763100000;
        const std::string base = X::new_name(d, t);
        rec("0: %s", base.c_str());
        const std::string stem = base.substr(0, base.size() - 9);
        write_raw(d + "/" + base, {1});
        rec("1: %s", X::new_name(d, t).c_str());
        for (int i = 2; i <= 98; i++) write_raw(d + "/" + stem + "-" + std::to_string(i) + ".u2cinema", {1});
        rec("98: %s", X::new_name(d, t).c_str());
        write_raw(d + "/" + stem + "-99.u2cinema", {1});
        rec("99: %s", X::new_name(d, t).c_str());
        rec("-1: %s", X::new_name(d, std::time_t(-1)).c_str());
        rec("huge: %s", X::new_name(d, std::time_t(0x7fffffffffffffffll)).c_str());
        rec("0: %s", X::new_name(d, std::time_t(0)).c_str());
        rec("missing dir: %s", X::new_name(d + "/nope", t).c_str());
        fs::remove(d + "/" + stem + "-50.u2cinema");
        rec("gap: %s", X::new_name(d, t).c_str());
        write_raw(d + "/recording.u2cinema", {1});
        rec("-1 taken: %s", X::new_name(d, std::time_t(-1)).c_str());
    });
    for (int n : {19, 20, 21, 25}) {
        static char names[8][64];
        static int ni = 0;
        char *nm = names[ni++];
        std::snprintf(nm, 64, "24b make_room with %d files", n);
        run_store(nm, [n](auto T) {
            using X = decltype(T);
            rec("make_room -> %d", X::make_room(store()));
            rec_files(X::list(store()));
        }, [n] {
            for (int i = 0; i < n; i++) mkrec("f" + std::to_string(i) + ".u2cinema", 100 + (i * 7) % 23, 8);
            mkrec("junk.bin", 1);
            write_raw(store() + "/bad.u2cinema", std::vector<uint8_t>(0x500, 0));
        });
    }
    run_store("24c make_room 20 with the oldest protected", [](auto T) {
        using X = decltype(T);
        rec("make_room -> %d", X::make_room(store()));
        rec_files(X::list(store()));
    }, [] { for (int i = 0; i < 22; i++) mkrec("f" + std::to_string(i) + ".u2cinema", 100 + i, 8, i < 5); });
    run_store("24d make_room 20 all protected / 19 protected + 1", [](auto T) {
        using X = decltype(T);
        rec("make_room -> %d", X::make_room(store()));
        set_ro(store() + "/f7.u2cinema", false);
        rec("make_room -> %d", X::make_room(store()));
        rec("make_room missing dir -> %d", X::make_room(store() + "/nope"));
        rec_files(X::list(store()));
    }, [] { for (int i = 0; i < 20; i++) mkrec("f" + std::to_string(i) + ".u2cinema", 100 + i, 8, true); });
    run_store("24e list sort / cut / junk", [](auto T) {
        using X = decltype(T);
        rec_files(X::list(store()));
        rec_files(X::list(store() + "/nope"));
        rec_files(X::list(store() + "/a.u2cinema"));
    }, [] {
        for (int i = 0; i < 23; i++) mkrec(std::string(1, char('a' + i)) + ".u2cinema", 100 + (i % 4), 8, i % 5 == 0);
        mkrec("Z.u2cinema", 101);
        write_raw(store() + "/s.u2cinema", std::vector<uint8_t>(0x4b0, 0));
    });
    run_store("24f fill_entry", [](auto T) {
        using X = decltype(T);
        std::vector<uint8_t> e(0x6c8 + 16, 0xee);
        auto f = typename X::File{};
        auto show = [&](const char *t) {
            X::fill_entry(f, e.data());
            rec("%s %016llx tail=%s date='%.255s' name='%.255s' name[0xff]=%d", t, (unsigned long long)fnv(e.data(), 0x6c8),
                hex(e.data() + 0x6c8, 16).c_str(), reinterpret_cast<char *>(e.data()), reinterpret_cast<char *>(e.data() + 0x100), e[0x1ff]);
        };
        show("empty");
        f.name = "x.u2cinema";
        f.time = 1763100000;
        f.size = 0x12345;
        f.protect = true;
        f.header = header(3);
        show("full");
        f.protect = false;
        f.header.resize(0x4af);
        show("short header");
        f.header.resize(0x4b1, 9);
        show("long header");
        f.name = std::string(300, 'q');
        show("long name");
        f.name = std::string(255, 'r');
        show("255 name");
        f.name = std::string(256, 's');
        show("256 name");
        f.time = 0x4000000000000000ull;
        show("bad time");
        f.time = uint64_t(-1);
        show("time -1");
    });
    run_store("24g valid_header / get / put", [](auto T) {
        using X = decltype(T);
        for (int32_t g : {-2, -1, 0, 0x7fff, 0x8000, int32_t(0x80000000)})
            for (int32_t p : {-1, 0, 10, 11}) {
                auto h = header(1, g, p);
                rec("game %d players %d -> %d", g, p, X::valid_header(h.data()));
            }
        auto h = header(1);
        h[0] ^= 1;
        rec("magic -> %d", X::valid_header(h.data()));
        uint8_t b[16] = {};
        X::put32(b + 1, 0x11223344);
        X::put64(b + 7, 0x8877665544332211ull);
        rec("put %s get %08x %016llx", hex(b, 16).c_str(), X::get32(b + 3), (unsigned long long)X::get64(b + 5));
    });
    run_store("24h remove / set_protected / ensure_dir", [](auto T) {
        using X = decltype(T);
        const std::string d = store();
        rec("ensure new nested -> %d", X::ensure_dir(d + "/x/y"));
        rec("ensure existing -> %d", X::ensure_dir(d));
        write_raw(d + "/file", {1});
        rec("ensure file -> %d", X::ensure_dir(d + "/file"));
        rec("ensure under file -> %d", X::ensure_dir(d + "/file/sub"));
        rec("remove empty -> %d", X::remove(d, ""));
        rec("remove missing -> %d", X::remove(d, "nope.u2cinema"));
        rec("remove protected -> %d", X::remove(d, "p.u2cinema"));
        rec("remove ok -> %d", X::remove(d, "w.u2cinema"));
        rec("remove dir -> %d", X::remove(d, "x"));
        rec("remove empty dir -> %d", X::remove(d + "/x", "y"));
        rec("protect missing -> %d", X::set_protected(d, "nope", true));
        rec("unprotect missing -> %d", X::set_protected(d, "nope", false));
        rec("protect -> %d", X::set_protected(d, "v.u2cinema", true));
        rec("protect again -> %d", X::set_protected(d, "v.u2cinema", true));
        rec("remove now -> %d", X::remove(d, "v.u2cinema"));
        rec("unprotect -> %d", X::set_protected(d, "p.u2cinema", false));
        rec("remove now -> %d", X::remove(d, "p.u2cinema"));
        rec("protect empty name -> %d", X::set_protected(d, "", true));
        X::set_protected(d, "", false);
    }, [] { mkrec("p.u2cinema", 1, 8, true); mkrec("w.u2cinema", 2); mkrec("v.u2cinema", 3); });
    run_store("24i Writer", [](auto T) {
        using X = decltype(T);
        const std::string d = store();
        X::ensure_dir(d);
        typename X::Stream s{reinterpret_cast<uint32_t *>(g_sbuf), 0, 0x100000, 0};
        {
            typename X::Writer w;
            rec("open bad -> %d open=%d", w.open(d + "/nope/x.u2cinema"), w.is_open());
            fill_pattern(1);
            s.bits = 0x40001;
            w.flush(s);
            rec_stream("flush closed", s);
            rec("finish closed -> %d", w.finish(s, header(9).data()));
            rec_stream("after", s);
            w.abandon();
            rec("open a -> %d open=%d", w.open(d + "/a.u2cinema"), w.is_open());
            fill_pattern(2);
            for (uint32_t b : {0x40000u, 0x40001u, 0xc0007u, 0xfffffffau, 0x123u}) {
                s.bits = b;
                w.flush(s);
                rec_stream("flush", s);
            }
            s.bits = 0x40021;
            rec("finish -> %d open=%d", w.finish(s, header(10).data()), w.is_open());
            rec_stream("finished", s);
            rec("finish again -> %d", w.finish(s, header(10).data()));
            rec("open b -> %d", w.open(d + "/b.u2cinema"));
            s.bits = 0;
            rec("open c (abandons b) -> %d", w.open(d + "/c.u2cinema"));
            s.bits = 1;
            rec("finish c -> %d", w.finish(s, header(11).data()));
            rec("open d -> %d", w.open(d + "/d.u2cinema"));
            w.abandon();
            rec("abandoned open=%d", w.is_open());
            w.abandon();
            rec("open e -> %d", w.open(d + "/e.u2cinema"));
            s.bits = 32;
            rec("finish e 32 -> %d", w.finish(s, header(12).data()));
            rec("open f -> %d", w.open(d + "/f.u2cinema"));
        }   // destructor abandons f
    });
    for (size_t len : {size_t(0), size_t(0x4af), size_t(0x4b0), size_t(0x4b0 + 5), size_t(0x4b0 + 0x10000), size_t(0x4b0 + 0x20000),
                       size_t(0x4b0 + 0x34567)}) {
        static char names[16][64];
        static int ni = 0;
        char *nm = names[ni++];
        std::snprintf(nm, 64, "24j Reader, file 0x%zx bytes", len);
        run_store(nm, [](auto T) {
            using X = decltype(T);
            const std::string d = store();
            typename X::Stream s{reinterpret_cast<uint32_t *>(g_sbuf), 0, 0x100000, 0};
            std::vector<uint8_t> h(0x4b0, 0xab);
            typename X::Reader r;
            rec("open missing -> %d open=%d hdr=%016llx", r.open(d + "/nope", h.data()), r.is_open(),
                (unsigned long long)fnv(h.data(), h.size()));
            rec("open -> %d open=%d eof=%d hdr=%016llx", r.open(d + "/a.u2cinema", h.data()), r.is_open(), r.eof(),
                (unsigned long long)fnv(h.data(), h.size()));
            fill_pattern(3);
            s.bytes = 0;
            for (uint32_t b : {0u, 0u, 0x20000u, 0x20001u, 0x7fff0u, 0xfffffu, 0x20008u, 0x300u}) {
                s.bits = b;
                r.fill(s);
                rec("eof=%d open=%d", r.eof(), r.is_open());
                rec_stream("fill", s);
            }
            r.close();
            rec("closed eof=%d open=%d", r.eof(), r.is_open());
            s.bits = 0x40000;
            s.bytes = 0x20000;
            r.fill(s);
            rec_stream("fill closed", s);
            s.bits = 0x40000;
            s.bytes = 0x3fff;
            r.fill(s);
            rec_stream("fill small", s);
        }, [len] {
            if (len < 0x4b0) {
                auto h = header(7);
                h.resize(len);
                write_raw(store() + "/a.u2cinema", h);
            } else {
                mkrec("a.u2cinema", 7, len - 0x4b0);
            }
        });
    }
    run_store("24k Reader invalid header", [](auto T) {
        using X = decltype(T);
        std::vector<uint8_t> h(0x4b0, 0xab);
        typename X::Reader r;
        rec("open -> %d open=%d hdr=%016llx", r.open(store() + "/a.u2cinema", h.data()), r.is_open(),
            (unsigned long long)fnv(h.data(), h.size()));
    }, [] {
        auto h = header(7, 3, 12);
        h.resize(0x5000, 1);
        write_raw(store() + "/a.u2cinema", h);
    });

    hx::recording = false;
    wipe();
    std::printf("cinema: %d scenarios, %d passed, %d failed\n", scenarios, passed, hx::failures);
    return hx::failures ? 1 : 0;
}
