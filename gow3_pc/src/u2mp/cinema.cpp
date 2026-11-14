// u2mp cinema (theater): the game side of match recording and playback.
//
// The PS4 build still has the replay task, the stream reader, the code that writes the stream
// start and the cinema list query, but the parts around them are gone. This file puts them back:
// it gives the replay stream a buffer, records every net event sent or received during an online
// match into it, streams it to a file (cinema_store.cpp), drives the cinema menus over those files
// and feeds a chosen file back into the stream for the game's replay task to play.
//
// Reimplemented from the behaviour description u2mp/docs/spec/cinema.md.
#include "cinema.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "bitstream.h"
#include "cinema_store.h"
#include "u2mp_internal.h"

extern "C" const char *runtime_file_user_dir(void);

namespace u2mp {

namespace {

using cinema::get32;
using cinema::get64;
using cinema::put32;
using cinema::put64;
using cinema::Stream;

// ---------------------------------------------------------------------------------------------
// Where things live in the game image (image offsets).

constexpr uint64_t kCinema = 0x113a990;        // cinema manager
constexpr uint64_t kStream = 0x113a258;        // the replay stream
constexpr uint64_t kClockPtr = 0x113a040;      // pointer to the net clock object
constexpr uint64_t kNetGameManager = 0x113b760;
constexpr uint64_t kNetMode = 0x104f1c8 + 0x2c4;
constexpr uint64_t kShowsLocal = 0x104f1b9;    // 1 = the list shows local files
constexpr uint64_t kRemoteRow = 0x14cfeb0;
constexpr uint64_t kPressedRow = 0x14ce098;

// Offsets inside the cinema manager.
namespace cm {
constexpr uint64_t kOpen = 0x00;
constexpr uint64_t kReading = 0x94;
constexpr uint64_t kEof = 0x95;
constexpr uint64_t kFlags = 0x9c;               // protect, delete, unprotect, upload (+ 4 spare)
constexpr uint64_t kSelected = 0x1a4;
constexpr uint64_t kCount = 0x1a8;
constexpr uint64_t kEntries = 0x1b0;
constexpr uint64_t kServerCount = 0x1b8;
constexpr uint64_t kServerEntries = 0x1c0;
constexpr uint64_t kHeader = 0x1c8;
}  // namespace cm

constexpr uint64_t kNgmGame = 0x1ca0;           // s16 net game index
constexpr uint64_t kNgmOverrides = 0x21b4;
constexpr uint64_t kMenuMessageBox = 0x17d0;

// Game functions this file calls.
constexpr uint64_t fPopPage = 0x5cc6f0;
constexpr uint64_t fSetNetGame = 0x63dc30;
constexpr uint64_t fMenuReset = 0x5cd1e0;
constexpr uint64_t fFindTask = 0xa572d0;
constexpr uint64_t fPlayTask = 0xa5be10;

// Page, task and message ids.
constexpr uint32_t kPageProgress = 0x1d84d11e;
constexpr uint32_t kPageCinemaList = 0x735c4dbe;
constexpr uint32_t kPageCinema = 0xca9509a0;
constexpr uint32_t kPageMessage = 0x719e9fec;
constexpr uint32_t kTaskSet = 0x9f2aeafb;
constexpr uint32_t kTaskReplay = 0xad76460e;
static_assert(sid("replay") == kTaskReplay, "replay task id");
constexpr uint32_t kNoFilesMessage = 0x2760b1fd;
constexpr uint32_t kNoFilesParams[7] = {0, 0xc6467f2d, 0x11d8e78a, 0, 0x1519fa3d, 0, 0};

constexpr uint32_t kStreamBits = 0x100000;     // the buffer's 128 KB in bits
constexpr uint32_t kRecordLimit = 0x1fc00;     // bytes; leaves room for the end marker and payload
constexpr size_t kSpare = 64;                  // the game's readers peek one word past the end

// ---------------------------------------------------------------------------------------------
// Host state.

alignas(64) uint8_t g_buffer[cinema::kBufferBytes + kSpare];
alignas(16) uint8_t g_local[cinema::kMaxFiles * cinema::kEntrySize];
alignas(16) uint8_t g_server[cinema::kMaxFiles * cinema::kEntrySize];   // the server list stays empty

cinema::Writer g_writer;
cinema::Reader g_reader;
bool g_recording = false;
bool g_record_enabled = true;
bool g_can_record = false;
std::chrono::steady_clock::time_point g_started;

// What PAGE_PROGRESS should do once it reaches its "done" event: set by the menu actions that
// open it, consumed once.
struct PageRequest {
    bool pending = false;
    bool load_first = false;
    uint32_t next_page = 0;
};
PageRequest g_request;

// Originals reached through trampolines or saved vtable slots.
using SendFn = uint64_t(U2MP_GUEST *)(void *, void *, uint64_t);
using StateFn = void(U2MP_GUEST *)(void *);
SendFn g_send_original = nullptr;
SendFn g_session_send_original = nullptr;
StateFn g_wait_ready_exit = nullptr;
StateFn g_online_leave_enter = nullptr;
StateFn g_replaying_exit = nullptr;
StateFn g_replay_leave_enter = nullptr;

// ---------------------------------------------------------------------------------------------
// Small accessors.

uint8_t *game(uint64_t offset) { return image.base + offset; }
uint8_t *cinema_mgr(uint64_t field) { return game(kCinema + field); }
Stream *stream() { return reinterpret_cast<Stream *>(game(kStream)); }
void *menu_manager() { return game(addr::menu_manager); }
uint32_t *buffer_words() { return reinterpret_cast<uint32_t *>(g_buffer); }

int32_t list_count() { return int32_t(get32(cinema_mgr(cm::kCount))); }
int32_t selected() { return int32_t(get32(cinema_mgr(cm::kSelected))); }
void set_selected(int32_t v) { put32(cinema_mgr(cm::kSelected), uint32_t(v)); }
bool in_range(int32_t i) { return i >= 0 && i < list_count(); }
uint8_t *local_entry(int32_t i) { return g_local + size_t(i) * cinema::kEntrySize; }
std::string entry_name(int32_t i) { return reinterpret_cast<const char *>(local_entry(i) + cinema::entry::kName); }

std::string store_dir() {
    const char *user = runtime_file_user_dir();
    return std::string(user && *user ? user : ".") + "/u2mp/cinema";
}

void clear_buffer() { std::memset(g_buffer, 0, sizeof g_buffer); }

// ---------------------------------------------------------------------------------------------
// Steps shared by the hooks.

// Gives the replay stream this module's buffer, unless it already has it.
void attach_stream() {
    Stream *s = stream();
    if (s->data == buffer_words() && s->max_bits == kStreamBits) return;
    clear_buffer();
    s->data = buffer_words();
    s->bits = 0;
    s->max_bits = kStreamBits;
    s->bytes = 0;
}

// Points the cinema manager at the host list storage. A fresh attach starts with an empty list.
void attach_lists() {
    if (get64(cinema_mgr(cm::kEntries)) != uint64_t(uintptr_t(g_local))) {
        put64(cinema_mgr(cm::kEntries), uint64_t(uintptr_t(g_local)));
        put32(cinema_mgr(cm::kCount), 0);
        put32(cinema_mgr(cm::kSelected), 0);
    }
    put64(cinema_mgr(cm::kServerEntries), uint64_t(uintptr_t(g_server)));
    put32(cinema_mgr(cm::kServerCount), 0);
}

// Enables the buttons that make sense for the selected file.
void update_flags() {
    uint8_t *flags = cinema_mgr(cm::kFlags);
    std::memset(flags, 0, 8);
    const int32_t i = selected();
    if (!in_range(i)) return;
    const uint32_t mode = get32(local_entry(i) + cinema::entry::kMode);
    if ((mode & cinema::kModeWritable) == cinema::kModeWritable) {
        flags[0] = 1;   // protect
        flags[1] = 1;   // delete
    } else {
        flags[2] = 1;   // unprotect
    }
    flags[3] = 1;       // upload
}

// Rebuilds the local list from the store directory and keeps the selection on a valid row.
void refresh() {
    attach_lists();
    const std::string dir = store_dir();
    cinema::ensure_dir(dir);
    const std::vector<cinema::File> files = cinema::list(dir);
    std::memset(g_local, 0, sizeof g_local);
    for (size_t i = 0; i < files.size(); ++i) cinema::fill_entry(files[i], local_entry(int32_t(i)));
    const int32_t n = int32_t(files.size());
    put32(cinema_mgr(cm::kCount), uint32_t(n));
    const int32_t sel = selected();
    if (sel >= n || sel < 0) set_selected(n > 0 ? n - 1 : 0);
    *game(kShowsLocal) = 1;
    update_flags();
}

// Makes row `index` the current recording: its header becomes the cinema header and its net
// game is selected so the menu shows the right map and mode.
bool load(int32_t index) {
    attach_lists();
    if (!in_range(index)) return false;
    set_selected(index);
    uint8_t *header = cinema_mgr(cm::kHeader);
    std::memcpy(header, local_entry(index) + cinema::entry::kHeader, cinema::kHeaderSize);
    const int32_t game_index = int16_t(uint16_t(get32(header + cinema::hdr::kGame) & 0xffff));
    using SetNetGameFn = void(U2MP_GUEST *)(void *, int32_t, void *, int32_t);
    reinterpret_cast<SetNetGameFn>(game(fSetNetGame))(game(kNetGameManager), game_index, nullptr, 10);
    update_flags();
    return true;
}

void pop_page() { reinterpret_cast<void(U2MP_GUEST *)(void *)>(game(fPopPage))(menu_manager()); }

// "No recordings" message box, shown only when the menu has a message box to fill.
void no_files_message() {
    const uint64_t box = get64(static_cast<uint8_t *>(menu_manager()) + kMenuMessageBox);
    if (!box) return;
    auto *b = reinterpret_cast<uint8_t *>(uintptr_t(box));
    put32(b + 0x08, kNoFilesMessage);
    for (int i = 0; i < 7; ++i) put32(b + 0x40 + 4 * i, kNoFilesParams[i]);
    show_page(kPageMessage);
}

uint32_t frame_clock() {
    const uint64_t clock = get64(game(kClockPtr));
    if (!clock) return 0;
    return get32(reinterpret_cast<const uint8_t *>(uintptr_t(clock)) + 0xc);
}

void close_playback() {
    if (!g_reader.is_open()) return;
    g_reader.close();
    put32(cinema_mgr(cm::kOpen), 0);
    *cinema_mgr(cm::kReading) = 0;
}

// ---------------------------------------------------------------------------------------------
// Recording.

void record(void *event) {
    if (!g_recording || !event) return;
    Stream *s = stream();
    if (s->data != buffer_words()) return;
    if ((s->bits + 7u) / 8u > kRecordLimit) return;
    BitStream b{s->data, s->bits, s->max_bits, 0};
    const uint32_t clock = frame_clock();
    write_bits(b, clock ? clock : 1, 32);   // time 0 marks the end of the stream
    write_bits(b, static_cast<const uint8_t *>(event)[8], 8);
    s->bits = b.bits;
    // The event serializes its own payload into the same stream.
    const uint64_t vtable = get64(static_cast<const uint8_t *>(event));
    const uint64_t write = get64(reinterpret_cast<const uint8_t *>(uintptr_t(vtable)) + 0x18);
    reinterpret_cast<void(U2MP_GUEST *)(void *, void *)>(uintptr_t(write))(event, s);
}

void finish_recording() {
    if (!g_recording) return;
    g_recording = false;
    Stream *s = stream();
    BitStream b{s->data, s->bits, s->max_bits, 0};
    write_bits(b, 0, 32);   // end marker
    s->bits = b.bits;
    const long long seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_started).count();
    uint8_t *header = cinema_mgr(cm::kHeader);
    put32(header + cinema::hdr::kDuration, uint32_t(seconds));
    const bool ok = g_writer.finish(*s, header);
    s->bits = 0;
    s->bytes = 0;
    clear_buffer();
    put32(cinema_mgr(cm::kOpen), 0);
    log("cinema: recording %s (%lld s)", ok ? "saved" : "failed, removed", seconds);
}

void start_recording() {
    if (!g_record_enabled || !g_can_record) return;
    finish_recording();
    g_reader.close();
    const std::string dir = store_dir();
    if (!cinema::ensure_dir(dir)) {
        log("cinema: cannot create %s, match not recorded", dir.c_str());
        return;
    }
    if (!cinema::make_room(dir)) {
        log("cinema: all %d recordings are protected, match not recorded", cinema::kMaxFiles);
        return;
    }
    uint8_t *header = cinema_mgr(cm::kHeader);
    const uint8_t *ngm = game(kNetGameManager);
    int16_t game_index;
    std::memcpy(&game_index, ngm + kNgmGame, sizeof game_index);
    const std::time_t now = std::time(nullptr);
    put32(header + cinema::hdr::kMagicOff, cinema::kMagic);
    put32(header + cinema::hdr::kGame, uint32_t(int32_t(game_index)));
    put32(header + cinema::hdr::kDuration, 0);
    put64(header + cinema::hdr::kTime, uint64_t(now));
    std::memcpy(header + cinema::hdr::kOverrides, ngm + kNgmOverrides, cinema::hdr::kOverridesSize);
    const std::string path = dir + "/" + cinema::new_name(dir, now);
    if (!g_writer.open(path)) {
        log("cinema: cannot write %s, match not recorded", path.c_str());
        return;
    }
    *cinema_mgr(cm::kReading) = 0;
    *cinema_mgr(cm::kEof) = 0;
    put32(cinema_mgr(cm::kOpen), 1);
    g_recording = true;
    g_started = std::chrono::steady_clock::now();
    log("cinema: recording to %s", path.c_str());
}

// ---------------------------------------------------------------------------------------------
// Playback.

bool open_for_reading() {
    attach_stream();
    attach_lists();
    close_playback();
    clear_buffer();
    Stream *s = stream();
    s->bits = 0;
    s->bytes = 0;
    const int32_t i = selected();
    if (in_range(i)) {
        const std::string path = store_dir() + "/" + entry_name(i);
        if (g_reader.open(path, cinema_mgr(cm::kHeader))) {
            *cinema_mgr(cm::kReading) = 1;
            *cinema_mgr(cm::kEof) = 0;
            put32(cinema_mgr(cm::kOpen), 1);
            g_reader.fill(*s);
            log("cinema: playing %s", path.c_str());
            return true;
        }
        log("cinema: cannot read %s", path.c_str());
    }
    // Give the reader a stream that ends at once, so the replay task leaves cleanly.
    s->bytes = 4;
    *cinema_mgr(cm::kEof) = 1;
    return false;
}

// ---------------------------------------------------------------------------------------------
// Entry points the game reaches.

U2MP_GUEST uint64_t pump(void *) {
    Stream *s = stream();
    if (s->data != buffer_words()) return 0;
    if (g_recording) {
        g_writer.flush(*s);
    } else if (g_reader.is_open()) {
        g_reader.fill(*s);
        *cinema_mgr(cm::kEof) = g_reader.eof() ? 1 : 0;
    }
    return 0;
}

U2MP_GUEST uint64_t send_net_event(void *ngm, void *event, uint64_t mask) {
    record(event);
    return g_send_original(ngm, event, mask);
}

U2MP_GUEST uint64_t session_send_net_event(void *session, void *event, uint64_t mask) {
    record(event);
    return g_session_send_original(session, event, mask);
}

U2MP_GUEST void receive_hook(void *event, float) { record(event); }

U2MP_GUEST void wait_ready_exit(void *state) {
    attach_stream();   // the original writes the stream start into it
    g_wait_ready_exit(state);
    start_recording();
}

U2MP_GUEST void show_results_enter(void *) { finish_recording(); }

U2MP_GUEST void online_leave_enter(void *state) {
    finish_recording();
    g_online_leave_enter(state);
}

U2MP_GUEST void replaying_exit(void *state) {
    g_replaying_exit(state);
    close_playback();
}

U2MP_GUEST void replay_leave_enter(void *state) {
    close_playback();
    g_replay_leave_enter(state);
}

U2MP_GUEST void page_progress(int32_t *event) {
    attach_lists();
    const int32_t ev = *event;
    if (ev == 2 || ev == 3 || ev == 4) {
        update_flags();
        return;
    }
    if (ev != 6) return;
    PageRequest req = g_request;
    g_request = PageRequest{};
    if (!req.pending) {
        // Entering the cinema list from elsewhere: load the newest file and show the list.
        if (int32_t(get32(game(kNetMode))) != 3) return;
        req = PageRequest{true, true, kPageCinemaList};
    }
    refresh();
    if (req.load_first) load(0);
    pop_page();
    if (req.next_page) show_page(req.next_page);
    if (list_count() == 0) no_files_message();
}

U2MP_GUEST void page_cinema(int32_t *, uint64_t) {}

U2MP_GUEST void list_cinema(int32_t *control) {
    attach_lists();
    const int32_t row = int32_t(get32(reinterpret_cast<const uint8_t *>(control) + 4));
    put32(game(kPressedRow), uint32_t(row));
    if (!in_range(row)) return;
    if (row != selected()) load(row);
    show_page(kPageCinema);
}

U2MP_GUEST void cinema_refresh(void *) {
    g_request = PageRequest{true, false, kPageCinemaList};
    show_page(kPageProgress);
}

U2MP_GUEST void list_toggle(void *) {
    g_request = PageRequest{true, false, 0};
    show_page(kPageProgress);
}

void change_protection(bool protect) {
    attach_lists();
    const int32_t i = selected();
    if (!in_range(i)) return;
    const std::string name = entry_name(i);
    if (!cinema::set_protected(store_dir(), name, protect)) {
        log("cinema: cannot change protection of %s", name.c_str());
        return;
    }
    put32(local_entry(i) + cinema::entry::kMode, protect ? cinema::kModeProtected : cinema::kModeWritable);
    update_flags();
}

U2MP_GUEST void protect_button(void *) { change_protection(true); }
U2MP_GUEST void unprotect_button(void *) { change_protection(false); }

U2MP_GUEST void delete_confirmed(void *) {
    attach_lists();
    const int32_t i = selected();
    if (in_range(i)) {
        const std::string name = entry_name(i);
        if (!cinema::remove(store_dir(), name)) log("cinema: cannot delete %s", name.c_str());
        else if (i > 0) set_selected(i - 1);
    }
    refresh();
    if (list_count() > 0) load(selected());
    pop_page();
    if (list_count() == 0) no_files_message();
}

U2MP_GUEST void upload_button(void *) { cinema_pick_friend(); }

U2MP_GUEST void start_cinema(void *) {
    finish_recording();
    if (!open_for_reading()) return;
    reinterpret_cast<void(U2MP_GUEST *)(void *)>(game(fMenuReset))(menu_manager());
    void *task = reinterpret_cast<void *(U2MP_GUEST *)(uint32_t, uint32_t)>(game(fFindTask))(kTaskSet, kTaskReplay);
    reinterpret_cast<void(U2MP_GUEST *)(void *)>(game(fPlayTask))(task);
}

// Called from the patched list query with the row value and whether the local list is shown.
U2MP_GUEST void list_query_set(int32_t value, uint32_t local) {
    if (local == 0) {
        put32(game(kRemoteRow), uint32_t(value));
        return;
    }
    if (value != selected()) load(value);
}

// ---------------------------------------------------------------------------------------------
// Install helpers. Each returns true when its hook is in place.

template <typename F> void *host(F f) { return reinterpret_cast<void *>(f); }

bool checked_replace(uint64_t offset, const std::vector<uint8_t> &expected, void *fn, const char *name) {
    if (!image.matches(offset, expected.data(), expected.size())) {
        log("cinema: %s at 0x%llx differs, not replaced", name, (unsigned long long)offset);
        return false;
    }
    return replace_function(offset, unsigned(expected.size()), fn, name);
}

bool ret_stub_replace(uint64_t offset, void *fn, const char *name) {
    if (!is_ret_stub(offset)) {
        log("cinema: %s at 0x%llx is not a ret stub, not replaced", name, (unsigned long long)offset);
        return false;
    }
    return replace_function(offset, 16, fn, name);
}

bool fits_rel32(int64_t delta) { return delta >= INT32_MIN && delta <= INT32_MAX; }

void put_rel32(uint8_t *at, int64_t delta) {
    const int32_t rel = int32_t(delta);
    std::memcpy(at, &rel, 4);
}

int64_t distance(const void *to, const void *from) { return int64_t(uintptr_t(to)) - int64_t(uintptr_t(from)); }

// Both SendNetEvent functions start with the same 17-byte prologue; it is moved into a
// trampoline so the original stays callable after the hook.
bool detour(uint64_t offset, void *fn, SendFn &original, const char *name) {
    static const uint8_t prologue[17] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                         0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x58};
    if (!image.executable(offset, sizeof prologue) || !image.matches(offset, prologue, sizeof prologue)) {
        log("cinema: %s at 0x%llx differs, not hooked", name, (unsigned long long)offset);
        return false;
    }
    uint8_t *trampoline = near_stub(31);
    uint8_t *jump = near_stub(12);
    if (!trampoline || !jump) return false;
    uint8_t *code = game(offset);
    std::memcpy(trampoline, prologue, sizeof prologue);
    const uint8_t indirect[6] = {0xff, 0x25, 0, 0, 0, 0};   // jmp [rip+0]
    std::memcpy(trampoline + 17, indirect, 6);
    put64(trampoline + 23, uint64_t(uintptr_t(code + 17)));
    jump[0] = 0x48;   // mov rax, imm64
    jump[1] = 0xb8;
    put64(jump + 2, uint64_t(uintptr_t(fn)));
    jump[10] = 0xff;  // jmp rax
    jump[11] = 0xe0;
    const int64_t delta = distance(jump, code + 5);
    if (!fits_rel32(delta)) return false;
    original = reinterpret_cast<SendFn>(trampoline);
    code[0] = 0xe9;
    put_rel32(code + 1, delta);
    std::memset(code + 5, 0xcc, sizeof prologue - 5);
    log("cinema: %s at 0x%llx hooked", name, (unsigned long long)offset);
    return true;
}

// Swaps a state's vtable slot; the plain host address goes in so later modules can chain it.
bool swap_slot(uint64_t slot, uint64_t expected, void *fn, StateFn &original, const char *name) {
    if (slot + 8 > image.size || get64(game(slot)) != uint64_t(uintptr_t(image.base)) + expected) {
        log("cinema: %s slot 0x%llx differs, not hooked", name, (unsigned long long)slot);
        return false;
    }
    original = reinterpret_cast<StateFn>(uintptr_t(get64(game(slot))));
    put64(game(slot), uint64_t(uintptr_t(fn)));
    log("cinema: %s (slot 0x%llx) -> C++", name, (unsigned long long)slot);
    return true;
}

// The list query's "op 9 set" case lost its local-list branch; route it through list_query_set
// and continue where the game's case ends.
bool patch_list_query() {
    constexpr uint64_t site = 0x657e7a, resume = 0x657bea;
    static const uint8_t expected[20] = {0x84, 0xc9, 0x0f, 0x85, 0x68, 0xfd, 0xff, 0xff, 0x44, 0x89,
                                         0x05, 0x27, 0x80, 0xe7, 0x00, 0xe9, 0x5c, 0xfd, 0xff, 0xff};
    if (!image.executable(site, sizeof expected) || !image.matches(site, expected, sizeof expected)) {
        log("cinema: list query at 0x%llx differs, not patched", (unsigned long long)site);
        return false;
    }
    uint8_t *stub = near_stub(32);
    if (!stub) return false;
    const uint8_t args[6] = {0x44, 0x89, 0xc7, 0x0f, 0xb6, 0xf1};   // mov edi, r8d; movzx esi, cl
    std::memcpy(stub, args, 6);
    stub[6] = 0x48;   // mov rax, imm64
    stub[7] = 0xb8;
    put64(stub + 8, uint64_t(uintptr_t(host(list_query_set))));
    stub[16] = 0xff;  // call rax
    stub[17] = 0xd0;
    stub[18] = 0xe9;  // jmp back into the query
    const int64_t back = distance(game(resume), stub + 23);
    uint8_t *code = game(site);
    const int64_t there = distance(stub, code + 5);
    if (!fits_rel32(back) || !fits_rel32(there)) return false;
    put_rel32(stub + 19, back);
    code[0] = 0xe9;
    put_rel32(code + 1, there);
    std::memset(code + 5, 0xcc, sizeof expected - 5);
    log("cinema: list query set -> C++");
    return true;
}

// The 16-byte frame-pointer prologue several of the replaced functions share.
const std::vector<uint8_t> kFramePrologue = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                             0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Public functions.

namespace cinema {

void record_event(void *event) { record(event); }

bool recording() { return g_recording; }

bool read_selected(std::vector<uint8_t> &bytes, std::string &name) {
    attach_lists();
    const int32_t i = selected();
    if (!in_range(i)) return false;
    name = entry_name(i);
    std::FILE *f = std::fopen((store_dir() + "/" + name).c_str(), "rb");
    if (!f) return false;
    long size = -1;
    if (std::fseek(f, 0, SEEK_END) == 0) size = std::ftell(f);
    const bool acceptable = size >= long(kHeaderSize) && size < 0x4000000L;
    bool complete = false;
    if (acceptable) {
        bytes.resize(size_t(size));
        complete = std::fseek(f, 0, SEEK_SET) == 0 && std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    }
    std::fclose(f);
    return acceptable && complete && valid_header(bytes.data());
}

int add_received(const uint8_t *data, size_t size, const std::string &wanted) {
    if (!data || size < kHeaderSize || !valid_header(data)) return -1;
    const std::string dir = store_dir();
    if (!ensure_dir(dir)) return -1;
    if (!make_room(dir)) return 0;
    // Only a plain, unused file name from the sender is kept; anything else gets a fresh name.
    bool safe = !wanted.empty() && wanted.size() < 64 && wanted.find_first_of("/\\:") == std::string::npos &&
                wanted[0] != '.';
    if (safe) {
        if (std::FILE *existing = std::fopen((dir + "/" + wanted).c_str(), "rb")) {
            std::fclose(existing);
            safe = false;
        }
    }
    const std::string ext = kExtension;
    const bool has_ext = wanted.size() >= ext.size() && wanted.compare(wanted.size() - ext.size(), ext.size(), ext) == 0;
    const std::string name = (safe && wanted.size() >= 9 && has_ext) ? wanted : new_name(dir, std::time(nullptr));
    const std::string path = dir + "/" + name;
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return -1;
    const bool written = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    if (!written) {
        std::remove(path.c_str());
        return -1;
    }
    refresh();
    log("cinema: received file saved as %s (%zu bytes)", name.c_str(), size);
    return 1;
}

}  // namespace cinema

// ---------------------------------------------------------------------------------------------
// Install.

void install_cinema(int &installed) {
    const char *env = std::getenv("UNCHARTED_U2MP_CINEMA");
    g_record_enabled = !(env && env[0] == '0');

    if (!checked_replace(0x608c40,
                         {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xe4},
                         host(pump), "cinema pump")) {
        log("cinema: not installed (the stream pump differs)");
        return;
    }
    ++installed;

    const bool send_ok = detour(0x644ab0, host(send_net_event), g_send_original, "SendNetEvent");
    if (send_ok) ++installed;
    if (detour(0x66a3d0, host(session_send_net_event), g_session_send_original, "NetSession::SendNetEvent"))
        ++installed;
    const bool recv_ok = ret_stub_replace(0x5e9d80, host(receive_hook), "cinema receive hook");
    if (recv_ok) ++installed;
    const bool start_ok = swap_slot(0x100abd0, 0x5f2830, host(wait_ready_exit), g_wait_ready_exit, "WaitReady::Exit");
    if (start_ok) ++installed;
    if (ret_stub_replace(0x5f0ea0, host(show_results_enter), "ShowResults::Enter")) ++installed;
    if (swap_slot(0x100a9c8, 0x5f00d0, host(online_leave_enter), g_online_leave_enter, "online Leave::Enter"))
        ++installed;

    g_can_record = send_ok && recv_ok && start_ok;
    if (!g_can_record) log("cinema: recording off (a recorder hook is missing)");
    if (!start_ok) log("cinema: WARNING the replay stream has no buffer; WaitReady::Exit writes to NULL");

    if (swap_slot(0x100a7a0, 0x5e8740, host(replaying_exit), g_replaying_exit, "RePlaying::Exit")) ++installed;
    if (swap_slot(0x100a6d8, 0x5e5520, host(replay_leave_enter), g_replay_leave_enter, "replay Leave::Enter"))
        ++installed;

    struct Replacement {
        uint64_t offset;
        std::vector<uint8_t> bytes;   // empty: a stripped ret stub
        void *fn;
        const char *name;
    };
    const Replacement menus[] = {
        {0x5e3f60,
         {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x8b, 0x07},
         host(page_progress), "PAGE_PROGRESS"},
        {0x5e4060,
         {0x83, 0x3f, 0x06, 0x75, 0x0c, 0x48, 0x8d, 0x3d, 0xd4, 0x25, 0xb5, 0x00, 0xe9, 0x7f, 0x86, 0xfe, 0xff, 0xc3},
         host(page_cinema), "PAGE_CINEMA"},
        {0x5e4080, kFramePrologue, host(list_cinema), "LIST_CINEMA"},
        {0x5e4490,
         {0x48, 0x8d, 0x3d, 0xa9, 0x21, 0xb5, 0x00, 0xbe, 0x1e, 0xd1, 0x84, 0x1d, 0x31, 0xd2, 0xe9, 0xed},
         host(cinema_refresh), "cinema refresh"},
        {0x5e45d0,
         {0x48, 0x8d, 0x05, 0xe2, 0xab, 0xa6, 0x00, 0x80, 0x30, 0x01, 0x48, 0x8d, 0x3d, 0x5f, 0x20, 0xb5},
         host(list_toggle), "cinema list toggle"},
        {0x5e41d0, {}, host(protect_button), "BUTTON_CINEMA_PROTECT"},
        {0x5e41e0, {}, host(unprotect_button), "BUTTON_CINEMA_UNPROTECT"},
        {0x5e41f0,
         {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x1d, 0x43, 0x24, 0xb5, 0x00, 0x48, 0x89, 0xdf},
         host(delete_confirmed), "cinema delete"},
        {0x5e42d0,
         {0x48, 0x8d, 0x05, 0xb9, 0x66, 0xb5, 0x00, 0x8b, 0x80, 0xb8, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x0d},
         host(upload_button), "BUTTON_CINEMA_UPLOAD"},
        {0x5e45a0,
         {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x3d, 0x95, 0x20, 0xb5, 0x00, 0xe8, 0x30, 0x8c, 0xfe, 0xff},
         host(start_cinema), "BUTTON_MULTIPLAYER_STARTCINEMA"},
    };
    for (const Replacement &r : menus) {
        const bool ok = r.bytes.empty() ? ret_stub_replace(r.offset, r.fn, r.name)
                                        : checked_replace(r.offset, r.bytes, r.fn, r.name);
        if (ok) ++installed;
    }

    if (patch_list_query()) ++installed;

    log("cinema: recording %s; files in <user dir>/u2mp/cinema", g_can_record && g_record_enabled ? "on" : "off");
}

}  // namespace u2mp
