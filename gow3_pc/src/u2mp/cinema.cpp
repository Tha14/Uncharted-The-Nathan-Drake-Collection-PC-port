// u2mp cinema (theater): recording and playback, ported from PS3 1.10 cinema.cpp onto the parts the
// PS4 1.02 build kept. See cinema.h; the evidence for every address is in u2mp/docs/CINEMA.md.
//
// Addresses are Uncharted 2 PS4 1.02 image offsets, PS3 1.10 originals in the comments. Every
// address that is patched is checked first (bytes, ret stub or vtable slot value).
#include "cinema.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "bitstream.h"
#include "cinema_store.h"
#include "u2mp_internal.h"

extern "C" const char *runtime_file_user_dir(void);

namespace u2mp::cinema {
namespace {

// ---- guest data -----------------------------------------------------------------------------
constexpr uint64_t kCinema = 0x113a990;      // cinema manager (PS3 0xf1b590)
constexpr uint64_t kStream = 0x113a258;      // replay stream {data, bits, maxBits, bytes} (PS3 0xf119b4)
constexpr uint64_t kNgm = 0x113b760;         // g_netGameManager (PS3 0xf1dc80)
constexpr uint64_t kNetInfo = 0x104f1c8;     // PS3 0xf1fa48
constexpr uint64_t kLocalList = 0x104f1b9;   // u8, 1 = the list shows local files (PS3 0xe1552c)
constexpr uint64_t kRemoteRow = 0x14cfeb0;   // remote list row (cond 69 query)
constexpr uint64_t kListRow = 0x14ce098;     // row LIST_CINEMA was pressed on (PS3 0xf10b78)
constexpr uint64_t kClock = 0x113a040;       // -> net clock, +0xc frame counter (PS3 [0xf116d4]+12)

// cinema manager fields (PS3 offset + 0x10 from +0x94 on)
constexpr uint64_t C_FILE = 0x00, C_READING = 0x94, C_EOF = 0x95, C_FLAGS = 0x9c, C_SELECTED = 0x1a4,
                   C_COUNT = 0x1a8, C_ENTRIES = 0x1b0, C_REMOTE_COUNT = 0x1b8, C_REMOTE_ENTRIES = 0x1c0,
                   C_HEADER = 0x1c8;
constexpr uint64_t NGM_GAME = 0x1ca0, NGM_OVERRIDES = 0x21b4, NI_MODE = 0x2c4;
constexpr int32_t kModeCinema = 3;

// guest functions we call
constexpr uint64_t fPopPage = 0x5cc6f0, fSetNetGame = 0x63dc30, fMenuReset = 0x5cd1e0, fFindTask = 0xa572d0,
                   fPlayTask = 0xa5be10;
// functions we replace or hook
constexpr uint64_t hPump = 0x608c40;            // PS3 0x463ce8 (file I/O was FIOS; replaced whole)
constexpr uint64_t hReceiveHook = 0x5e9d80;     // M+0x150 no-op = PS3 recorder 0x440c38
constexpr uint64_t hSend = 0x644ab0;            // NetGameManager::SendNetEvent (PS3 0x493728)
constexpr uint64_t hSessionSend = 0x66a3d0;     // NetSession::SendNetEvent (PS3 0x4d3754)
constexpr uint64_t hShowResultsEnter = 0x5f0ea0;   // ret stub (PS3 0x443e40 called finish 0x464464)
constexpr uint64_t hProgressPage = 0x5e3f60;    // PAGE_PROGRESS handler (PS3 0x4336e4)
constexpr uint64_t hCinemaPage = 0x5e4060;      // PAGE_CINEMA handler (PS3 0x43367c)
constexpr uint64_t hListCinema = 0x5e4080;      // LIST_CINEMA (PS3 0x435248)
constexpr uint64_t hRefreshToList = 0x5e4490;   // bf6960e8 / c9f003a8 (PS3 0x432c8c)
constexpr uint64_t hToggleList = 0x5e45d0;      // fb637366 (PS3 0x432c00)
constexpr uint64_t hProtect = 0x5e41d0, hUnprotect = 0x5e41e0;   // ret stubs (PS3 0x433644, 0x43360c)
constexpr uint64_t hDeleteConfirmed = 0x5e41f0; // 7bb3c875 (PS3 0x433588)
constexpr uint64_t hUpload = 0x5e42d0;          // BUTTON_CINEMA_UPLOAD (PS3 0x433240)
constexpr uint64_t hStartCinema = 0x5e45a0;     // BUTTON_MULTIPLAYER_STARTCINEMA (PS3 0x432ea4)
constexpr uint64_t hQuerySet = 0x657e7a;        // cond 69 query, op 9 (set) (PS3 0x4bb170)
constexpr uint64_t kQueryDone = 0x657bea;       // its common exit
// state vtable slots: {slot, expected function}
constexpr uint64_t sWaitReadyExit = 0x100abd0, fWaitReadyExit = 0x5f2830;     // PS3 0x443f00
constexpr uint64_t sOnlineLeaveEnter = 0x100a9c8, fOnlineLeaveEnter = 0x5f00d0; // PS3 0x445608
constexpr uint64_t sRePlayingExit = 0x100a7a0, fRePlayingExit = 0x5e8740;     // PS3 0x436028
constexpr uint64_t sReplayLeaveEnter = 0x100a6d8, fReplayLeaveEnter = 0x5e5520; // PS3 0x434848

constexpr uint32_t kPageProgress = sid("PAGE_PROGRESS");
constexpr uint32_t kPageList = sid("PAGE_MULTIPLAYER_CINEMA_LIST");
constexpr uint32_t kPageCinema = sid("PAGE_CINEMA");
constexpr uint32_t kTaskSet = 0x9f2aeafb, kTaskReplay = sid("replay");
static_assert(kPageProgress == 0x1d84d11eu && kPageList == 0x735c4dbeu && kPageCinema == 0xca9509a0u &&
                  kTaskReplay == 0xad76460eu, "string ids");

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
uint8_t *P(uint64_t offset) { return image.at<uint8_t>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }
using StateFn = void(U2MP_GUEST *)(void *);
using SendFn = uint64_t(U2MP_GUEST *)(void *, void *, uint64_t);
using WriteFn = void(U2MP_GUEST *)(void *, void *);

// ---- our state ------------------------------------------------------------------------------
alignas(64) uint32_t buffer[(kBufferBytes + 64) / 4];   // spare words: the readers look one word ahead
alignas(16) uint8_t local_entries[kMaxFiles * kEntrySize];
alignas(16) uint8_t remote_entries[kMaxFiles * kEntrySize];   // no server: always empty
Writer writer;
Reader reader;
bool recording_now = false, record_enabled = true, can_record = false;
std::chrono::steady_clock::time_point record_start;
struct Request { bool pending = false; bool load_first = false; uint32_t next = 0; } request;
StateFn orig_waitready_exit, orig_online_leave_enter, orig_replaying_exit, orig_replay_leave_enter;
SendFn orig_send, orig_session_send;

std::string dir() {
    const char *u = runtime_file_user_dir();
    return std::string(u && *u ? u : ".") + "/u2mp/cinema";
}

Stream &stream() { return G<Stream>(kStream); }

// PS3 NetInit attached a 128 KB buffer (0x448d68). PS4 has no NetInit and its static initializer
// (0x5ef970) leaves data = NULL, so WaitReady::Exit (0x5f2830) would write to address 0. Called
// lazily because that initializer runs after install.
void ensure_stream() {
    Stream &s = stream();
    if (s.data == buffer && s.max_bits == kBufferBytes * 8) return;
    std::memset(buffer, 0, sizeof buffer);
    s = Stream{buffer, 0, uint32_t(kBufferBytes * 8), 0};
}

void ensure_lists() {
    const uint64_t local = uint64_t(uintptr_t(local_entries)), remote = uint64_t(uintptr_t(remote_entries));
    if (G<uint64_t>(kCinema + C_ENTRIES) != local) {
        G<uint64_t>(kCinema + C_ENTRIES) = local;
        G<int32_t>(kCinema + C_COUNT) = 0;
        G<int32_t>(kCinema + C_SELECTED) = 0;
    }
    G<uint64_t>(kCinema + C_REMOTE_ENTRIES) = remote;
    G<int32_t>(kCinema + C_REMOTE_COUNT) = 0;
}

int32_t count() { return G<int32_t>(kCinema + C_COUNT); }
int32_t selected() { return G<int32_t>(kCinema + C_SELECTED); }
uint8_t *entry_at(int32_t i) { return local_entries + size_t(i) * kEntrySize; }
std::string entry_name(int32_t i) { return reinterpret_cast<const char *>(entry_at(i) + entry::kName); }

uint32_t frame_clock() {
    const uint64_t clock = G<uint64_t>(kClock);
    return clock ? *reinterpret_cast<const uint32_t *>(uintptr_t(clock) + 0xc) : 0;
}

void pop_page() { guest<void(U2MP_GUEST *)(void *)>(fPopPage)(P(addr::menu_manager)); }

// The "no cinema files" box, as 0x5e41f0 shows it (PS3 0x42a3e8 with 2760b1fd / c6467f2d).
void message_no_files() {
    uint8_t *box = *reinterpret_cast<uint8_t **>(P(addr::menu_manager + 0x17d0));
    if (!box) return;
    put32(box + 0x08, 0x2760b1fd);
    const uint32_t words[] = {0, 0xc6467f2d, 0x11d8e78a, 0, 0x1519fa3d, 0, 0};   // +0x40..+0x58
    std::memcpy(box + 0x40, words, sizeof words);
    show_page(0x719e9fec);
}

// PS3 0x4630a0: the flags the cinema page's controls read (+0x9c protect, +0x9d delete,
// +0x9e unprotect, +0x9f upload).
void update_flags() {
    std::memset(P(kCinema + C_FLAGS), 0, 8);
    const int32_t i = selected();
    if (i < 0 || i >= count()) return;
    if ((get32(entry_at(i) + entry::kMode) & kModeWritable) == kModeWritable) {
        P(kCinema + C_FLAGS)[0] = 1;
        P(kCinema + C_FLAGS)[1] = 1;
    } else {
        P(kCinema + C_FLAGS)[2] = 1;
    }
    P(kCinema + C_FLAGS)[3] = 1;   // never in the (absent) server list
}

// PS3 0x4631d8 (refresh), into the game's local list.
void refresh() {
    ensure_lists();
    const std::string d = dir();
    ensure_dir(d);
    const std::vector<File> files = cinema::list(d);
    std::memset(local_entries, 0, sizeof local_entries);
    for (size_t i = 0; i < files.size(); ++i) fill_entry(files[i], entry_at(int32_t(i)));
    const int32_t n = int32_t(files.size());
    G<int32_t>(kCinema + C_COUNT) = n;
    if (selected() >= n || selected() < 0) G<int32_t>(kCinema + C_SELECTED) = n ? n - 1 : 0;
    G<uint8_t>(kLocalList) = 1;
    update_flags();
}

// PS3 0x463738 (load): select a recording, take its header, preselect its net game.
bool load(int32_t index) {
    ensure_lists();
    if (index < 0 || index >= count()) return false;
    G<int32_t>(kCinema + C_SELECTED) = index;
    const uint8_t *header = entry_at(index) + entry::kHeader;
    std::memcpy(P(kCinema + C_HEADER), header, kHeaderSize);
    const int32_t game = int16_t(get32(header + hdr::kGame));
    guest<void(U2MP_GUEST *)(void *, int32_t, const void *, int32_t)>(fSetNetGame)(P(kNgm), game, nullptr, 10);
    update_flags();
    return true;
}

// ---- recording ------------------------------------------------------------------------------
// PS3 0x440c38: { u32 time, u8 type, Write payload } for every event sent or received.
void record(void *event) {
    if (!recording_now || !event) return;
    Stream &s = stream();
    if (s.data != buffer || (s.bits + 7) / 8 > 0x1fc00) return;   // PS3 skips at 0x1fc00 bytes
    const uint32_t t = frame_clock();
    BitStream b{s.data, s.bits, s.max_bits, 0};
    write_bits(b, t ? t : 1, 32);   // 0 is the end marker
    write_bits(b, static_cast<const uint8_t *>(event)[8], 8);
    s.bits = b.bits;
    const uint64_t vtable = *static_cast<const uint64_t *>(event);
    reinterpret_cast<WriteFn>(*reinterpret_cast<const uint64_t *>(uintptr_t(vtable) + 0x18))(event, &s);
}

void finish_recording() {
    if (!recording_now) return;
    recording_now = false;
    Stream &s = stream();
    BitStream b{s.data, s.bits, s.max_bits, 0};
    write_bits(b, 0, 32);
    s.bits = b.bits;
    uint8_t *header = P(kCinema + C_HEADER);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                          record_start).count();
    put32(header + hdr::kDuration, uint32_t(seconds));
    const bool ok = writer.finish(s, header);
    s.bits = 0;
    s.bytes = 0;
    std::memset(buffer, 0, sizeof buffer);
    G<int32_t>(kCinema + C_FILE) = 0;
    log("cinema: recording %s (%lld s)", ok ? "saved" : "failed, removed", static_cast<long long>(seconds));
}

// PS3 0x4638cc(cinema, 0), which WaitReady::Exit called before writing the stream start.
void begin_recording() {
    if (!record_enabled || !can_record) return;
    finish_recording();
    reader.close();
    const std::string d = dir();
    if (!ensure_dir(d)) {
        log("cinema: cannot create %s, match not recorded", d.c_str());
        return;
    }
    if (!make_room(d)) {
        log("cinema: all %d recordings are protected, match not recorded", kMaxFiles);
        return;
    }
    uint8_t *header = P(kCinema + C_HEADER);
    const std::time_t now = std::time(nullptr);
    put32(header + hdr::kMagicOff, kMagic);
    put32(header + hdr::kGame, uint32_t(int32_t(G<int16_t>(kNgm + NGM_GAME))));
    put32(header + hdr::kDuration, 0);
    put64(header + hdr::kTime, uint64_t(now));
    std::memcpy(header + hdr::kOverrides, P(kNgm + NGM_OVERRIDES), hdr::kOverridesSize);
    const std::string path = d + "/" + new_name(d, now);
    if (!writer.open(path)) {
        log("cinema: cannot write %s, match not recorded", path.c_str());
        return;
    }
    G<uint8_t>(kCinema + C_READING) = 0;
    G<uint8_t>(kCinema + C_EOF) = 0;
    G<int32_t>(kCinema + C_FILE) = 1;
    recording_now = true;
    record_start = std::chrono::steady_clock::now();
    log("cinema: recording to %s", path.c_str());
}

// ---- playback -------------------------------------------------------------------------------
void close_read() {
    if (!reader.is_open()) return;
    reader.close();
    G<int32_t>(kCinema + C_FILE) = 0;
    G<uint8_t>(kCinema + C_READING) = 0;
}

// PS3 0x4638cc(cinema, 1), which RePlay::Enter called before waiting for the first bytes.
bool open_for_read() {
    ensure_stream();
    ensure_lists();
    close_read();
    Stream &s = stream();
    std::memset(buffer, 0, sizeof buffer);
    s.bits = 0;
    s.bytes = 0;
    const int32_t i = selected();
    if (i >= 0 && i < count()) {
        const std::string path = dir() + "/" + entry_name(i);
        if (reader.open(path, P(kCinema + C_HEADER))) {
            G<uint8_t>(kCinema + C_READING) = 1;
            G<uint8_t>(kCinema + C_EOF) = 0;
            G<int32_t>(kCinema + C_FILE) = 1;
            reader.fill(s);
            log("cinema: playing %s", path.c_str());
            return true;
        }
        log("cinema: cannot read %s", path.c_str());
    }
    // RePlay::Enter (0x5e9630) waits for bytes, then leaves the task when the first word is not 5.
    s.bytes = 4;
    G<uint8_t>(kCinema + C_EOF) = 1;
    return false;
}

// ---- hooks ----------------------------------------------------------------------------------
U2MP_GUEST uint64_t pump(void *) {
    Stream &s = stream();
    if (s.data != buffer) return 0;
    if (recording_now) writer.flush(s);
    else if (reader.is_open()) {
        reader.fill(s);
        G<uint8_t>(kCinema + C_EOF) = reader.eof() ? 1 : 0;
    }
    return 0;
}

U2MP_GUEST void receive_hook(void *event, float) { record(event); }
U2MP_GUEST uint64_t send_hook(void *ngm, void *event, uint64_t mask) {
    record(event);
    return orig_send(ngm, event, mask);
}
U2MP_GUEST uint64_t session_send_hook(void *session, void *event, uint64_t mask) {
    record(event);
    return orig_session_send(session, event, mask);
}

U2MP_GUEST void waitready_exit(void *state) {
    ensure_stream();   // the original writes the stream start
    orig_waitready_exit(state);
    begin_recording();
}
U2MP_GUEST void show_results_enter(void *) { finish_recording(); }
U2MP_GUEST void online_leave_enter(void *state) {
    finish_recording();
    orig_online_leave_enter(state);
}
U2MP_GUEST void replaying_exit(void *state) {
    orig_replaying_exit(state);
    close_read();
}
U2MP_GUEST void replay_leave_enter(void *state) {
    close_read();
    orig_replay_leave_enter(state);
}

// PAGE_PROGRESS (PS3 0x4336e4). PS3 ran the refresh on a thread and this page waited for it;
// ours is synchronous. Events 2..4 refresh the control flags, 6 is the per-frame update.
U2MP_GUEST void progress_page(int32_t *event) {
    ensure_lists();
    const int32_t e = *event;
    if (e >= 2 && e <= 4) {
        update_flags();
        return;
    }
    if (e != 6) return;
    Request r = request;
    request = Request{};
    if (!r.pending) {
        // BUTTON_MULTIPLAYER_CINEMA: the mode dispatcher (0x5ecb80, PS3 0x443248) set mode 3,
        // SetNetGame(-1) and showed this page; PS3 then refreshed, loaded file 0 and went on to
        // the list (0x432d0c).
        if (G<int32_t>(kNetInfo + NI_MODE) != kModeCinema) return;
        r = Request{true, true, kPageList};
    }
    refresh();
    if (r.load_first) load(0);
    pop_page();
    if (r.next) show_page(r.next);
    if (count() == 0) message_no_files();
}

// PAGE_CINEMA (PS3 0x43367c popped it when sceNpManagerGetStatus was not "online"; PS4 inlined
// that as an unconditional pop). We are always online.
U2MP_GUEST void cinema_page(int32_t *) {}

U2MP_GUEST void list_cinema(int32_t *control) {
    ensure_lists();
    const int32_t row = control[1];
    G<int32_t>(kListRow) = row;
    if (row < 0 || row >= count()) return;
    if (row != selected()) load(row);
    show_page(kPageCinema);
}

U2MP_GUEST void refresh_to_list(void *) {
    request = Request{true, false, kPageList};
    show_page(kPageProgress);
}
U2MP_GUEST void toggle_list(void *) {   // local/server switch: there is no server
    request = Request{true, false, 0};
    show_page(kPageProgress);
}

void protect(bool on) {
    ensure_lists();
    const int32_t i = selected();
    if (i < 0 || i >= count()) return;
    if (!set_protected(dir(), entry_name(i), on)) {
        log("cinema: cannot change protection of %s", entry_name(i).c_str());
        return;
    }
    put32(entry_at(i) + entry::kMode, on ? kModeProtected : kModeWritable);
    update_flags();
}
U2MP_GUEST void protect_button(void *) { protect(true); }
U2MP_GUEST void unprotect_button(void *) { protect(false); }

U2MP_GUEST void delete_confirmed(void *) {
    ensure_lists();
    const int32_t i = selected();
    if (i >= 0 && i < count()) {
        if (!cinema::remove(dir(), entry_name(i))) log("cinema: cannot delete %s", entry_name(i).c_str());
        else if (i > 0) G<int32_t>(kCinema + C_SELECTED) = i - 1;
    }
    refresh();
    if (count() > 0) load(selected());
    pop_page();
    if (count() == 0) message_no_files();
}

U2MP_GUEST void upload_button(void *) { log("cinema: no server to upload to"); }

U2MP_GUEST void start_cinema(void *) {
    finish_recording();
    if (!open_for_read()) return;   // nothing to play: stay on the page
    guest<void(U2MP_GUEST *)(void *)>(fMenuReset)(P(addr::menu_manager));
    void *task = guest<void *(U2MP_GUEST *)(uint32_t, uint32_t)>(fFindTask)(kTaskSet, kTaskReplay);
    guest<void(U2MP_GUEST *)(void *)>(fPlayTask)(task);
}

// cond 69 query, op 9: PS3 loaded the chosen file (0x4bb170 -> 0x463738); PS4 dropped that.
U2MP_GUEST void query_set(int32_t value, uint32_t local) {
    if (!local) {
        G<int32_t>(kRemoteRow) = value;
        return;
    }
    if (value != selected()) load(value);
}

// ---- patching helpers -----------------------------------------------------------------------
unsigned char *near_code(size_t bytes) {
    static unsigned char *pool = nullptr;
    static size_t used = 0;
    constexpr size_t kPool = 0x1000;
    bytes = (bytes + 15) & ~size_t(15);
    if (!pool || used + bytes > kPool) {
        pool = runtime_near_image_alloc(kPool, 1);
        used = 0;
        if (!pool) return nullptr;
    }
    unsigned char *p = pool + used;
    used += bytes;
    return p;
}

bool rel32(const unsigned char *from_next, const void *to, int32_t &out) {
    const int64_t d = int64_t(uintptr_t(to)) - int64_t(uintptr_t(from_next));
    if (d < INT32_MIN || d > INT32_MAX) return false;
    out = int32_t(d);
    return true;
}

// Hooks a function whose first `length` bytes are whole position-independent instructions:
// they move to a trampoline (then jmp back), the entry jumps to `host`. `original` = trampoline.
bool detour(uint64_t offset, const unsigned char *expect, unsigned length, void *host, void **original,
            const char *name) {
    if (length < 5 || !image.executable(offset, length) || !image.matches(offset, expect, length)) {
        log("cinema: %s at 0x%llx differs, not hooked", name, static_cast<unsigned long long>(offset));
        return false;
    }
    unsigned char *tramp = near_code(length + 14), *stub = near_code(12);
    if (!tramp || !stub) return false;
    unsigned char *code = image.base + offset;
    std::memcpy(tramp, code, length);
    const uint64_t back = uint64_t(uintptr_t(code + length)), target = uint64_t(uintptr_t(host));
    tramp[length] = 0xff;   // jmp [rip+0]; dq back
    tramp[length + 1] = 0x25;
    std::memset(tramp + length + 2, 0, 4);
    std::memcpy(tramp + length + 6, &back, 8);
    stub[0] = 0x48;   // mov rax, host; jmp rax
    stub[1] = 0xb8;
    std::memcpy(stub + 2, &target, 8);
    stub[10] = 0xff;
    stub[11] = 0xe0;
    int32_t rel;
    if (!rel32(code + 5, stub, rel)) return false;
    *original = tramp;
    code[0] = 0xe9;
    std::memcpy(code + 1, &rel, 4);
    std::memset(code + 5, 0xcc, length - 5);
    log("cinema: %s at 0x%llx hooked", name, static_cast<unsigned long long>(offset));
    return true;
}

template <typename F> bool swap_slot(uint64_t slot, uint64_t expected, F host, F &original, const char *name) {
    uint64_t &value = G<uint64_t>(slot);
    if (slot + 8 > image.size || value != uint64_t(uintptr_t(image.base)) + expected) {
        log("cinema: %s slot 0x%llx differs, not hooked", name, static_cast<unsigned long long>(slot));
        return false;
    }
    original = reinterpret_cast<F>(uintptr_t(value));
    value = uint64_t(uintptr_t(host));
    log("cinema: %s (slot 0x%llx) -> C++", name, static_cast<unsigned long long>(slot));
    return true;
}

// The cond 69 query's op-9 case (test cl,cl; jne done; mov [kRemoteRow], r8d; jmp done) becomes
// a call to query_set(value = r8d, local = cl) and the jump to the common exit.
bool patch_query_set() {
    static const unsigned char expect[] = {0x84, 0xc9, 0x0f, 0x85, 0x68, 0xfd, 0xff, 0xff, 0x44, 0x89,
                                           0x05, 0x27, 0x80, 0xe7, 0x00, 0xe9, 0x5c, 0xfd, 0xff, 0xff};
    if (!image.executable(hQuerySet, sizeof expect) || !image.matches(hQuerySet, expect, sizeof expect)) {
        log("cinema: list query at 0x%llx differs, not patched", static_cast<unsigned long long>(hQuerySet));
        return false;
    }
    unsigned char *stub = near_code(32);
    if (!stub) return false;
    const uint64_t host = uint64_t(uintptr_t(&query_set));
    unsigned char *s = stub;
    const unsigned char head[] = {0x44, 0x89, 0xc7, 0x0f, 0xb6, 0xf1, 0x48, 0xb8};   // mov edi,r8d; movzx esi,cl; mov rax,
    std::memcpy(s, head, sizeof head);
    s += sizeof head;
    std::memcpy(s, &host, 8);
    s += 8;
    *s++ = 0xff;   // call rax (rsp is 16-aligned in the query's body)
    *s++ = 0xd0;
    *s++ = 0xe9;   // jmp kQueryDone
    int32_t rel;
    if (!rel32(s + 4, image.base + kQueryDone, rel)) return false;
    std::memcpy(s, &rel, 4);
    unsigned char *code = image.base + hQuerySet;
    if (!rel32(code + 5, stub, rel)) return false;
    code[0] = 0xe9;
    std::memcpy(code + 1, &rel, 4);
    std::memset(code + 5, 0xcc, sizeof expect - 5);
    log("cinema: list query set -> C++");
    return true;
}

template <size_t N> bool replace_checked(uint64_t at, const unsigned char (&expect)[N], void *host, const char *name) {
    if (!image.matches(at, expect, N)) {
        log("cinema: %s at 0x%llx differs, not replaced", name, static_cast<unsigned long long>(at));
        return false;
    }
    return replace_function(at, N, host, name);
}

bool replace_stub(uint64_t at, void *host, const char *name) {
    if (!is_ret_stub(at)) {
        log("cinema: %s at 0x%llx is not a ret stub, not replaced", name, static_cast<unsigned long long>(at));
        return false;
    }
    return replace_function(at, 16, host, name);
}

}  // namespace

void record_event(void *event) { record(event); }
bool recording() { return recording_now; }

}  // namespace u2mp::cinema

namespace u2mp {

void install_cinema(int &installed) {
    using namespace cinema;
    const char *env = std::getenv("UNCHARTED_U2MP_CINEMA");
    record_enabled = !(env && env[0] == '0');
    auto count_if = [&](bool ok) {
        if (ok) ++installed;
        return ok;
    };

    // Shared: the stream pump (file I/O for both directions).
    static const unsigned char pump_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                               0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xe4};
    if (!count_if(replace_checked(hPump, pump_bytes, reinterpret_cast<void *>(&pump), "cinema pump"))) {
        log("cinema: not installed (the stream pump differs)");
        return;
    }

    // Recording.
    static const unsigned char send_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                               0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x58};
    void *orig = nullptr;
    const bool send_ok = count_if(detour(hSend, send_bytes, sizeof send_bytes, reinterpret_cast<void *>(&send_hook),
                                         &orig, "SendNetEvent"));
    if (send_ok) orig_send = reinterpret_cast<SendFn>(orig);
    if (count_if(detour(hSessionSend, send_bytes, sizeof send_bytes, reinterpret_cast<void *>(&session_send_hook),
                        &orig, "NetSession::SendNetEvent")))
        orig_session_send = reinterpret_cast<SendFn>(orig);
    const bool recv_ok = count_if(replace_stub(hReceiveHook, reinterpret_cast<void *>(&receive_hook), "cinema receive hook"));
    const bool start_ok = count_if(swap_slot(sWaitReadyExit, fWaitReadyExit, static_cast<StateFn>(&waitready_exit),
                                             orig_waitready_exit, "WaitReady::Exit"));
    count_if(replace_stub(hShowResultsEnter, reinterpret_cast<void *>(&show_results_enter), "ShowResults::Enter"));
    count_if(swap_slot(sOnlineLeaveEnter, fOnlineLeaveEnter, static_cast<StateFn>(&online_leave_enter),
                       orig_online_leave_enter, "online Leave::Enter"));
    can_record = send_ok && recv_ok && start_ok;
    if (!can_record) log("cinema: recording off (a recorder hook is missing)");
    if (!start_ok) log("cinema: WARNING the replay stream has no buffer; WaitReady::Exit writes to NULL");

    // Playback and the cinema menus.
    count_if(swap_slot(sRePlayingExit, fRePlayingExit, static_cast<StateFn>(&replaying_exit), orig_replaying_exit,
                       "RePlaying::Exit"));
    count_if(swap_slot(sReplayLeaveEnter, fReplayLeaveEnter, static_cast<StateFn>(&replay_leave_enter),
                       orig_replay_leave_enter, "replay Leave::Enter"));
    static const unsigned char progress_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                                   0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x8b, 0x07};
    count_if(replace_checked(hProgressPage, progress_bytes, reinterpret_cast<void *>(&progress_page), "PAGE_PROGRESS"));
    static const unsigned char cinema_page_bytes[] = {0x83, 0x3f, 0x06, 0x75, 0x0c, 0x48, 0x8d, 0x3d, 0xd4,
                                                      0x25, 0xb5, 0x00, 0xe9, 0x7f, 0x86, 0xfe, 0xff, 0xc3};
    count_if(replace_checked(hCinemaPage, cinema_page_bytes, reinterpret_cast<void *>(&cinema_page), "PAGE_CINEMA"));
    static const unsigned char list_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                               0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec};
    count_if(replace_checked(hListCinema, list_bytes, reinterpret_cast<void *>(&list_cinema), "LIST_CINEMA"));
    static const unsigned char refresh_bytes[] = {0x48, 0x8d, 0x3d, 0xa9, 0x21, 0xb5, 0x00, 0xbe,
                                                  0x1e, 0xd1, 0x84, 0x1d, 0x31, 0xd2, 0xe9, 0xed};
    count_if(replace_checked(hRefreshToList, refresh_bytes, reinterpret_cast<void *>(&refresh_to_list),
                             "cinema refresh"));
    static const unsigned char toggle_bytes[] = {0x48, 0x8d, 0x05, 0xe2, 0xab, 0xa6, 0x00, 0x80,
                                                 0x30, 0x01, 0x48, 0x8d, 0x3d, 0x5f, 0x20, 0xb5};
    count_if(replace_checked(hToggleList, toggle_bytes, reinterpret_cast<void *>(&toggle_list), "cinema list toggle"));
    count_if(replace_stub(hProtect, reinterpret_cast<void *>(&protect_button), "BUTTON_CINEMA_PROTECT"));
    count_if(replace_stub(hUnprotect, reinterpret_cast<void *>(&unprotect_button), "BUTTON_CINEMA_UNPROTECT"));
    static const unsigned char delete_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d,
                                                 0x1d, 0x43, 0x24, 0xb5, 0x00, 0x48, 0x89, 0xdf};
    count_if(replace_checked(hDeleteConfirmed, delete_bytes, reinterpret_cast<void *>(&delete_confirmed),
                             "cinema delete"));
    static const unsigned char upload_bytes[] = {0x48, 0x8d, 0x05, 0xb9, 0x66, 0xb5, 0x00, 0x8b,
                                                 0x80, 0xb8, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x0d};
    count_if(replace_checked(hUpload, upload_bytes, reinterpret_cast<void *>(&upload_button), "BUTTON_CINEMA_UPLOAD"));
    static const unsigned char start_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x3d, 0x95,
                                                0x20, 0xb5, 0x00, 0xe8, 0x30, 0x8c, 0xfe, 0xff};
    count_if(replace_checked(hStartCinema, start_bytes, reinterpret_cast<void *>(&start_cinema),
                             "BUTTON_MULTIPLAYER_STARTCINEMA"));
    count_if(patch_query_set());
    log("cinema: recording %s; files in <user dir>/u2mp/cinema", can_record && record_enabled ? "on" : "off");
}

}  // namespace u2mp
