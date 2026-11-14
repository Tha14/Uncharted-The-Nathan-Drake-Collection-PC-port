// u2mp: the last multiplayer functions the PS4 1.02 build left empty or cut short, and four places
// that read address 0 because a PS4 accessor was folded to NULL.
//
// - two net event Applies (NpcDealDamage, KickGriefer);
// - the frontend menu callbacks of the party, player, friends, players-met, invites, clan tag,
//   store, Twitter and cinema-sharing buttons, answered with PC popups, message boxes and the
//   session / online layers;
// - the NULL-read sites (level record package, tracker values, empty tag table search, NetStatGame
//   host name);
// - the TextDb::Find hook that lets texts.cpp serve PC texts;
// - show_notice() and cinema_pick_friend() for other files.
//
// Reimplemented from the behaviour description u2mp/docs/spec/remaining.md.
#include "cinema.h"
#include "online.h"
#include "progression.h"
#include "session_api.h"
#include "texts.h"
#include "u2mp_internal.h"
#include "voice.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace u2mp {
namespace {

// ---- guest memory --------------------------------------------------------------------------------
// Guest data lives at real host addresses; these read and write it without alignment assumptions.

template <typename T> T load(const void *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void store(void *p, T v) { std::memcpy(p, &v, sizeof v); }
template <typename T> T load_at(const void *base, uint64_t off) {
    return load<T>(static_cast<const uint8_t *>(base) + off);
}
template <typename T> void store_at(void *base, uint64_t off, T v) { store(static_cast<uint8_t *>(base) + off, v); }
uint8_t *image_ptr(uint64_t off) { return image.base + off; }
template <typename T> T image_load(uint64_t off) { return load<T>(image_ptr(off)); }

// Menu callback fields (page event, list row, button / answer); a NULL arg reads as zeros.
uint32_t arg_word(const void *arg, uint64_t off) { return arg ? load_at<uint32_t>(arg, off) : 0; }

// ---- game addresses (image offsets of Uncharted 2 PS4 1.02) ----------------------------------------
constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kPartySession = 0x105efb8;
constexpr uint64_t kMatchSession = 0x1065b08;
constexpr uint64_t kNetInfo = 0x104f1c8;
constexpr uint64_t kNetGameManager = 0x113b760;
constexpr uint64_t kAttackTypes = 0xdef290;
constexpr uint64_t fPopPage = 0x5cc6f0;
constexpr uint64_t fShowPopup = 0x5cc480;
constexpr uint64_t fSendKillInfo = 0x60eb90;
constexpr uint64_t fDataLookup = 0xa15e70;
constexpr uint64_t fStrlen = 0xde6490;
constexpr uint64_t fMemcpy = 0xde6410;
constexpr uint64_t fTextFind = 0xcddc00;

constexpr uint32_t kAttackTypeCount = 45;
constexpr int kTrackers = 10;
constexpr uint32_t kMaxCallbacks = 128;

// Page and text ids the menus use.
constexpr uint32_t kPageMessageBox = 0x719e9fec;
constexpr uint32_t kPagePopup = 0x99ab57c2;
constexpr uint32_t kPageLobby = 0xb99a3235;
constexpr uint32_t kTextMute = 0xffbbac12, kCbMute = 0x6547d63c;
constexpr uint32_t kTextPromote = 0x9fdf8887, kCbPromote = 0x2d32c8b0;
constexpr uint32_t kTextKick = 0x1e2955af, kCbKick = 0x7cc46b94;
constexpr uint32_t kTextJoin = 0xb1600ce3, kCbJoin = 0x0ace8463;
constexpr uint32_t kTextInviteParty = 0x4a555973, kCbInviteParty = 0x45a9c65b;
constexpr uint32_t kTextViewProfile = 0x4040b706, kCbFriendProfile = 0x3fe6f70e;
constexpr uint32_t kTextAddFriend = 0x910fb4f9;

// Our own callback ids (registered in the game's menu callback table on demand).
constexpr uint32_t kCbFriendAccept = sid("U2MP_PC_CB_FRIEND_ACCEPT");
constexpr uint32_t kCbFriendRemove = sid("U2MP_PC_CB_FRIEND_REMOVE");
constexpr uint32_t kCbMetAdd = sid("U2MP_PC_CB_MET_ADD");
constexpr uint32_t kCbMetProfile = sid("U2MP_PC_CB_MET_PROFILE");
constexpr uint32_t kCbPick[kTextSlots] = {sid("U2MP_PC_CB_PICK0"), sid("U2MP_PC_CB_PICK1"), sid("U2MP_PC_CB_PICK2"),
                                          sid("U2MP_PC_CB_PICK3"), sid("U2MP_PC_CB_PICK4"), sid("U2MP_PC_CB_PICK5"),
                                          sid("U2MP_PC_CB_PICK6"), sid("U2MP_PC_CB_PICK7")};
static_assert(kCbFriendAccept == 0x2026899du && kCbPick[7] == 0xc15a485eu, "callback ids");

using MenuCallback = void(U2MP_GUEST *)(void *, void *, void *);

// ---- state that lives as long as the process ---------------------------------------------------------
uint32_t g_selected_member = 0;       // net id picked in a party / player row
int g_selected_friend = -1;           // friends list row picked (online rows, header excluded)
std::string g_selected_met;           // players-met name picked
std::vector<std::string> g_pick_names;   // cinema picker: the friends offered, by option
std::string g_last_level_name;        // level package lookups log once per name
bool g_matchmaking_answered = false;  // kept for parity; nothing reads it
char g_invite_count_text[16];         // BUTTON_MULTIPLAYER_INVITES column text

// ---- shared operations ----------------------------------------------------------------------------

uint8_t *menu_manager() { return image_ptr(addr::menu_manager); }

// A session is host when it has no room yet, or when its own member id is the host's.
bool session_is_host(uint64_t session) {
    const uint8_t *s = image_ptr(session);
    return load_at<uint64_t>(s, 0x10) == 0 || load_at<uint32_t>(s, 0xb00) == load_at<uint32_t>(s, 0xb04);
}

// The game's one-button message box with text `text`; it needs the descriptor the menu manager owns.
void message_box(uint32_t text) {
    uint8_t *box = load_at<uint8_t *>(menu_manager(), 0x17d0);
    if (!box) return;
    store_at<uint32_t>(box, 0x08, text);
    const uint32_t words[7] = {0, 0xc6467f2d, 0x11d8e78a, 0, 0x1519fa3d, 0, 0};
    std::memcpy(box + 0x40, words, sizeof words);
    show_page(kPageMessageBox);
}

void pop_page() { reinterpret_cast<void(U2MP_GUEST *)(void *)>(image_ptr(fPopPage))(menu_manager()); }

// The game's popup: a title, a page and up to eight {text, callback} options, laid out as the
// game reads them (19 words). Kept in static storage so it stays valid however long the game
// holds on to the pointer.
struct PopupDescriptor {
    uint32_t title;
    uint32_t page;
    uint32_t count;
    uint32_t option[8][2];
};
static_assert(sizeof(PopupDescriptor) == 76, "popup descriptor layout");

class Popup {
public:
    Popup() {
        desc_ = PopupDescriptor{};
        desc_.title = kTextPlayer;
        desc_.page = kPagePopup;
    }
    void add(uint32_t text, uint32_t callback) {
        if (desc_.count >= 8) return;
        desc_.option[desc_.count][0] = text;
        desc_.option[desc_.count][1] = callback;
        ++desc_.count;
    }
    bool empty() const { return desc_.count == 0; }
    void show() {
        reinterpret_cast<void(U2MP_GUEST *)(void *, void *, uint32_t)>(image_ptr(fShowPopup))(menu_manager(), &desc_, 1);
    }

private:
    static PopupDescriptor desc_;
};
PopupDescriptor Popup::desc_;

// Puts our callback id into the game's menu callback table so a popup option can name it: an
// existing entry with that id gets the new function, otherwise a new entry is appended.
bool register_callback(uint32_t id, MenuCallback fn) {
    uint8_t *menu = menu_manager();
    const uint32_t count = load_at<uint32_t>(menu, 0x18ec);
    const uint32_t scan = count < kMaxCallbacks ? count : kMaxCallbacks;
    for (uint32_t i = 0; i < scan; ++i) {
        uint8_t *entry = menu + 0x18f0 + uint64_t(i) * 0x18;
        if (load_at<uint32_t>(entry, 0) == id) {
            store_at<uint64_t>(entry, 8, uint64_t(uintptr_t(fn)));
            return true;
        }
    }
    if (count >= kMaxCallbacks) return false;
    uint8_t *entry = menu + 0x18f0 + uint64_t(count) * 0x18;
    store_at<uint32_t>(entry, 0, id);
    store_at<uint64_t>(entry, 8, uint64_t(uintptr_t(fn)));
    store_at<uint64_t>(entry, 0x10, 0);
    store_at<uint32_t>(menu, 0x18ec, count + 1);
    return true;
}

// Appends one of our own texts; a missing text must not crash, so it then adds nothing.
std::string with_own_text(std::string base, uint32_t id) {
    if (const char *t = own_text(id)) base += t;
    return base;
}

// ---- B1: level record package (0x644130) ---------------------------------------------------------------

struct DataTable {
    int32_t count;
    uint32_t pad;
    const uint8_t *records;
};

const DataTable *data_lookup(uint32_t id) {
    return reinterpret_cast<const DataTable *(U2MP_GUEST *)(uint32_t)>(image_ptr(fDataLookup))(id);
}

// The level's package lookup lost the room accessor; the level comes from the current net game's
// record instead. When the levels table names a package that is not loaded, its "-pak" name is
// tried as the in-game packages that do carry the spawner table.
U2MP_GUEST const uint8_t *level_record_package(void *) {
    const DataTable *games = data_lookup(0x3b9a067d);
    const DataTable *levels = data_lookup(0x0dec97a4);
    const int16_t index = image_load<int16_t>(kNetGameManager + 0x1ca0);
    if (!games || !levels || !games->records || !levels->records || index < 0 || index >= games->count)
        return nullptr;
    const uint32_t level_id = load_at<uint32_t>(games->records, 0x90 * uint64_t(index) + 4);

    const uint8_t *record = nullptr;
    for (int32_t i = 0; i < levels->count; ++i) {
        const uint8_t *r = levels->records + 0x38 * uint64_t(i);
        if (load_at<uint32_t>(r, 0) == level_id) {
            record = r;
            break;
        }
    }
    if (!record) return nullptr;
    if (const uint8_t *pkg = package_by_id(load_at<uint32_t>(record, 0x20))) return pkg;

    const char *name = load_at<const char *>(record, 0x18);
    if (!name) return nullptr;
    const size_t len = std::strlen(name);
    if (len <= 4 || std::strcmp(name + len - 4, "-pak") != 0) return nullptr;
    const std::string map(name, len - 4);
    const bool verbose = g_last_level_name != name;
    g_last_level_name = name;

    const std::string candidates[3] = {map + "-ingame10", map + "-ingame", map};
    const uint8_t *fallback = nullptr;
    const char *fallback_name = nullptr;
    for (const std::string &c : candidates) {
        const uint8_t *pkg = package_by_id(sid(c.c_str()));
        const uint64_t spawners = pkg ? load_at<uint64_t>(pkg, 0x9a0) : 0;
        if (verbose)
            log("level package: %s candidate %s: %s, spawner table %s", name, c.c_str(), pkg ? "loaded" : "missing",
                spawners ? "present" : "none");
        if (pkg && spawners) {
            if (verbose) log("level package: %s is defined nowhere, using %s", name, c.c_str());
            return pkg;
        }
        if (pkg && !fallback) {
            fallback = pkg;
            fallback_name = c.c_str();
        }
    }
    if (verbose)
        log("level package: %s is defined nowhere, no candidate has spawners; using %s", name,
            fallback_name ? fallback_name : "nothing");
    return fallback;
}

// ---- B4: NetStatGame host name (strlen / memcpy calls at 0x66c75f / 0x66c76d) -----------------------

// The match host's name as the roster knows it, else ours; the game's field holds 63 bytes + NUL.
std::string match_host_name() {
    std::string name;
    if (!session::member_name(image_load<uint32_t>(kMatchSession + 0xb04), name)) name = session::local_name();
    if (name.size() >= 64) name.resize(63);
    return name;
}

U2MP_GUEST uint64_t host_name_length(const char *s) {
    if (s) return std::strlen(s);
    return match_host_name().size();
}

U2MP_GUEST void *host_name_copy(void *dst, const void *src, uint64_t n) {
    if (src) {
        std::memcpy(dst, src, n);
    } else {
        const std::string name = match_host_name();
        std::memcpy(dst, name.c_str(), name.size() + 1);
    }
    return dst;
}

// ---- B6: NetEventNpcDealDamage::Apply (0x61fa70) ---------------------------------------------------

// A tracker names a process by slot address and unique id; the process is valid only while the
// slot still holds a process with that id.
const uint8_t *tracked_process(uint64_t slot, uint32_t id) {
    if (!slot || !id) return nullptr;
    const uint8_t *process = load<const uint8_t *>(reinterpret_cast<const void *>(uintptr_t(slot)));
    if (!process || load_at<uint32_t>(process, 0x44) != id) return nullptr;
    return process;
}

const uint8_t *find_npc_tracker(uint32_t npc_id) {
    const uint8_t *ngm = image_ptr(kNetGameManager);
    const uint32_t count = load_at<uint32_t>(ngm, 0x1bdc);
    const uint8_t *buckets = load_at<const uint8_t *>(ngm, 0x1ba0);
    if (!count || !buckets) return nullptr;
    uint32_t hash = 0x811c9dc5u;
    for (int i = 0; i < 4; ++i) {
        hash ^= (npc_id >> (8 * i)) & 0xff;
        hash *= 0x01000193u;
    }
    const uint8_t *node = load_at<const uint8_t *>(buckets, 8 * uint64_t(hash % count));
    // The walk goes through the end node like any other; only the result is checked against it.
    while (node && load_at<uint32_t>(node, 0x10) != npc_id) node = load_at<const uint8_t *>(node, 0);
    if (!node || node == load_at<const uint8_t *>(buckets, 8 * uint64_t(count))) return nullptr;
    return node + 0x18;
}

U2MP_GUEST void npc_deal_damage_apply(void *event, void *) {
    const uint8_t *ev = static_cast<const uint8_t *>(event);
    const uint8_t *ngm = image_ptr(kNetGameManager);

    const uint32_t attacker_id = load_at<uint32_t>(ev, 0x0c);
    const uint8_t *player = nullptr;
    for (int i = 0; i < kTrackers && !player; ++i) {
        const uint8_t *t = ngm + 0x10 + uint64_t(i) * 0x2c0;
        if (load_at<uint32_t>(t, 0x120) == attacker_id) player = t;
    }
    const uint8_t *npc_tracker = find_npc_tracker(load_at<uint32_t>(ev, 0x10));
    if (!player || !npc_tracker) return;

    const uint8_t *attacker = tracked_process(load_at<uint64_t>(player, 0x10), load_at<uint32_t>(player, 0x18));
    uint8_t *npc = const_cast<uint8_t *>(
        tracked_process(load_at<uint64_t>(npc_tracker, 0), load_at<uint32_t>(npc_tracker, 8)));
    if (!attacker || !npc) return;

    using IsDeadFn = uint8_t(U2MP_GUEST *)(void *);
    using TakeDamageFn = void(U2MP_GUEST *)(void *, int32_t);
    auto vtable = [npc] { return load<const uint8_t *>(npc); };
    if (load_at<IsDeadFn>(vtable(), 0x250)(npc)) return;
    load_at<TakeDamageFn>(vtable(), 0x370)(npc, load_at<int32_t>(ev, 0x14));
    if (!load_at<IsDeadFn>(vtable(), 0x250)(npc)) return;
    // Only the match host announces the kill.
    if (!session_is_host(kMatchSession)) return;

    const uint32_t index = load_at<uint32_t>(ev, 0x1c);
    const uint32_t type = index < kAttackTypeCount ? image_load<uint32_t>(kAttackTypes + 4 * uint64_t(index)) : 0;
    // Seconds to frame clock ticks, truncated the way the x86 conversion does (out of range or NaN
    // gives the "integer indefinite" value).
    const float ticks = load_at<float>(ev, 0x18) * 300.0f;
    int32_t time = INT32_MIN;
    if (ticks >= -2147483648.0f && ticks < 2147483648.0f) time = int32_t(ticks);

    using KillInfoFn = void(U2MP_GUEST *)(uint32_t, const void *, void *, int32_t *, uint64_t, uint64_t, uint64_t);
    reinterpret_cast<KillInfoFn>(image_ptr(fSendKillInfo))(type, attacker, npc, &time, 0, 0, 0);
    npc[0x262a] |= 2;
}

// ---- B7: NetEventKickGriefer::Apply (0x61b900) -----------------------------------------------------

U2MP_GUEST void kick_griefer_apply(void *event, void *) {
    if (!session_is_host(kMatchSession)) return;
    const uint32_t net_id = load_at<uint32_t>(event, 0x0c);
    if (!session::kick(net_id)) log("NetEventKickGriefer: can't kick net id %u", net_id);
}

// ---- B8: party and match player rows -----------------------------------------------------------------

void player_options(int row) {
    uint32_t id = 0;
    std::string name;
    if (!session::member_at(row, id, name)) {
        log("player options: no player in row %d", row);
        return;
    }
    g_selected_member = id;
    if (id == session::local_id()) {
        text_set_player(with_own_text(name + " ", kTextYou).c_str());
        message_box(kTextPlayer);
        return;
    }
    text_set_player(name.c_str());
    Popup popup;
    popup.add(kTextMute, kCbMute);
    if (session::is_host() && online::active()) popup.add(kTextPromote, kCbPromote);
    if (session::is_host()) popup.add(kTextKick, kCbKick);
    popup.show();
}

U2MP_GUEST void party_player_row(void *arg, void *, void *) { player_options(int(arg_word(arg, 8) % 10)); }
U2MP_GUEST void match_player_row(void *arg, void *, void *) { player_options(int32_t(arg_word(arg, 8))); }

// ---- B9 / B10: friends list ---------------------------------------------------------------------------

U2MP_GUEST void friend_accept(void *, void *, void *) { online::accept_request(g_selected_friend); }
U2MP_GUEST void friend_remove(void *, void *, void *) { online::remove_friend(g_selected_friend); }
U2MP_GUEST void friend_profile(void *, void *, void *) {
    text_set_player(online::profile_text(g_selected_friend).c_str());
    message_box(kTextPlayer);
}

void friend_options(int r) {
    if (r < 0 || r >= online::friend_count()) return;
    g_selected_friend = r;
    text_set_player(online::friend_row(r));
    const bool cbs = register_callback(kCbFriendAccept, friend_accept) &&
                     register_callback(kCbFriendRemove, friend_remove) &&
                     register_callback(kCbFriendProfile, friend_profile);
    Popup popup;
    switch (online::friend_kind(r)) {
    case online::kRowRequestIn:
        if (cbs) {
            popup.add(kTextAddFriend, kCbFriendAccept);
            popup.add(kTextDeclineRequest, kCbFriendRemove);
        }
        break;
    case online::kRowRequestOut:
        if (cbs) popup.add(kTextCancelRequest, kCbFriendRemove);
        break;
    default:
        if (online::friend_joinable(r)) popup.add(kTextJoin, kCbJoin);
        if (online::friend_online(r)) popup.add(kTextInviteParty, kCbInviteParty);
        if (cbs) {
            popup.add(kTextViewProfile, kCbFriendProfile);
            popup.add(kTextRemoveFriend, kCbFriendRemove);
        }
        break;
    }
    if (popup.empty())
        message_box(kTextPlayer);
    else
        popup.show();
}

// Offline the list shows the party; online its first row is "join a game" and the rest are friends.
U2MP_GUEST void friends_row(void *arg, void *, void *) {
    const int row = int32_t(arg_word(arg, 4));
    if (!online::active())
        player_options(row);
    else if (row == 0)
        online::open_join_list();
    else
        friend_options(row - 1);
}

// ---- B11 / B12: players met ----------------------------------------------------------------------------

void notice_text(const std::string &text) { show_notice(text.c_str()); }

U2MP_GUEST void met_add(void *, void *, void *) {
    if (online::friend_add(g_selected_met))
        notice_text("Friend request sent to " + g_selected_met + ".");
    else
        notice_text("The friend request could not be sent.");
}
U2MP_GUEST void met_profile(void *, void *, void *) { notice_text(progression_player_summary(g_selected_met)); }

U2MP_GUEST void players_met_row(void *arg, void *, void *) {
    const int row = int32_t(arg_word(arg, 4));
    const std::vector<std::string> names = progression_recent_players(50);
    if (row < 0 || size_t(row) >= names.size()) return;
    g_selected_met = names[size_t(row)];
    text_set_player(g_selected_met.c_str());
    if (!register_callback(kCbMetAdd, met_add) || !register_callback(kCbMetProfile, met_profile)) {
        message_box(kTextPlayer);
        return;
    }
    Popup popup;
    if (online::active()) popup.add(kTextAddFriend, kCbMetAdd);
    popup.add(kTextViewProfile, kCbMetProfile);
    popup.show();
}

// ---- B13 .. B21: party actions, invites ---------------------------------------------------------------

U2MP_GUEST void kick_from_party(void *, void *, void *) {
    if (g_selected_member == 0 || !session::kick(g_selected_member))
        log("Kick From Party: net id %u not kicked", g_selected_member);
}

U2MP_GUEST void mute_member(void *, void *, void *) {
    if (!voice::enabled()) {
        message_box(kTextVoice);
        return;
    }
    std::string name;
    if (!session::member_name(g_selected_member, name)) return;
    const bool muted = voice::toggle_mute(g_selected_member);
    text_set_player(with_own_text(name + ": ", muted ? kTextMuted : kTextUnmuted).c_str());
    message_box(kTextPlayer);
}

U2MP_GUEST void promote_leader(void *, void *, void *) {
    if (g_selected_member != 0 && online::promote(g_selected_member)) return;
    message_box(kTextLeader);
}

U2MP_GUEST bool party_is_host(void *, void *, void *) { return session_is_host(kPartySession); }

U2MP_GUEST void save_button(void *, void *, void *) { log("Save"); }

U2MP_GUEST void invites_button(void *, void *, void *) {
    if (online::open_join_list()) return;
    if (image_load<int32_t>(kNetInfo + 0x2c4) != 0) show_page(kPageLobby);
    message_box(kTextInvite);
}

U2MP_GUEST void join_party(void *, void *, void *) {
    if (online::active() && g_selected_friend >= 0)
        online::join_friend(g_selected_friend);
    else
        message_box(kTextInvite);
}

U2MP_GUEST void invite_to_party(void *, void *, void *) {
    if (online::active() && g_selected_friend >= 0)
        online::invite_friend(g_selected_friend);
    else
        message_box(kTextInvite);
}

// The invites button's count: op 1 hides the text when nothing waits, op 2 column 1 shows the count.
U2MP_GUEST void invites_query(void *query) {
    uint8_t *q = static_cast<uint8_t *>(query);
    static const char empty[] = "";
    const uint32_t op = load_at<uint32_t>(q, 0);
    if (op == 1) {
        if (online::invite_count() == 0) store_at<const char *>(q, 0x40, empty);
    } else if (op == 2 && load_at<uint32_t>(q, 0x54) == 1) {
        const int n = online::invite_count();
        if (n > 0) {
            std::snprintf(g_invite_count_text, sizeof g_invite_count_text, "%d", n);
            store_at<const char *>(q, 0x40, g_invite_count_text);
        }
    }
}

// ---- B22 .. B27: clan tag, player options page, store, matchmaking box, Twitter -----------------------

U2MP_GUEST void clan_tag_button(void *, void *, void *) {
    if (!edit_clan_tag()) message_box(kTextClanTag);
}

// Page event 6 is the page's per-frame update: it polls the clan tag box, and leaves the page once
// the player is no longer in Multiplayer.
U2MP_GUEST void player_options_page(void *arg, void *, void *) {
    if (arg_word(arg, 0) != 6) return;
    clan_tag_frame();
    if (image_load<int32_t>(kNetState) < 2) pop_page();
}

U2MP_GUEST void store_browse(void *, void *, void *) { message_box(kTextStore); }

U2MP_GUEST void matchmaking_box(void *arg, void *, void *) {
    const uint32_t answer = arg_word(arg, 8);
    if (answer) g_matchmaking_answered = true;
    log("matchmaking message box answered (%u)", answer);
}

U2MP_GUEST void twitter_page(void *arg, void *, void *) {
    if (arg_word(arg, 0) != 6) return;
    pop_page();
    message_box(kTextTwitter);
}

U2MP_GUEST void twitter_button(void *, void *, void *) { message_box(kTextTwitter); }

// ---- B28 / B29: cinema files to and from friends --------------------------------------------------------

template <int I> U2MP_GUEST void cinema_pick(void *, void *, void *) {
    if (size_t(I) < g_pick_names.size()) online::send_cinema(g_pick_names[I]);
}
constexpr MenuCallback kPickCallbacks[kTextSlots] = {cinema_pick<0>, cinema_pick<1>, cinema_pick<2>, cinema_pick<3>,
                                                     cinema_pick<4>, cinema_pick<5>, cinema_pick<6>, cinema_pick<7>};

void pick_friend_for_cinema() {
    if (!online::linked()) {
        message_box(kTextCinemaSend);
        return;
    }
    g_pick_names.clear();
    for (int i = 0; i < online::friend_count() && g_pick_names.size() < size_t(kTextSlots); ++i)
        if (online::friend_kind(i) == online::kRowFriend && online::friend_online(i))
            g_pick_names.push_back(online::friend_name(i));
    if (g_pick_names.empty()) {
        show_notice("None of your RPCN friends is online to receive the cinema file.");
        return;
    }
    text_set_player("Send the cinema file to");
    Popup popup;
    for (size_t i = 0; i < g_pick_names.size(); ++i) {
        text_set_slot(int(i), g_pick_names[i].c_str());
        if (register_callback(kCbPick[i], kPickCallbacks[i])) popup.add(kTextSlot[i], kCbPick[i]);
    }
    popup.show();
}

U2MP_GUEST void cinema_send(void *, void *, void *) { pick_friend_for_cinema(); }

U2MP_GUEST void cinema_receive(void *, void *, void *) {
    if (!online::linked()) {
        message_box(kTextCinemaReceive);
        return;
    }
    const int n = online::cinema_received();
    // Clears the "cinema messages waiting" flag the L1 Messages entry shows.
    store<int32_t>(image_ptr(kNetInfo + 0x18), 0);
    if (n != 0)
        notice_text(std::to_string(n) + " cinema file(s) from friends are in your cinema list.");
    else
        show_notice("No cinema files from friends yet. Friends send them from a cinema's page.");
}

// ---- install --------------------------------------------------------------------------------------------

template <size_t N> bool bytes_at(uint64_t offset, const unsigned char (&bytes)[N]) {
    return image.matches(offset, bytes, N);
}

bool fits_rel32(const void *target, const void *next_instruction) {
    const int64_t d = int64_t(uintptr_t(target)) - int64_t(uintptr_t(next_instruction));
    return d >= INT32_MIN && d <= INT32_MAX;
}

// `jmp [rip+0]` followed by the 8-byte target: reaches anything, needs no register.
unsigned char *emit_abs_jump(unsigned char *p, const void *target) {
    static const unsigned char op[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(p, op, sizeof op);
    const uint64_t t = uint64_t(uintptr_t(target));
    std::memcpy(p + 6, &t, 8);
    return p + 14;
}

void write_rel32_jump(unsigned char *at, const void *target) {
    const int32_t rel = int32_t(int64_t(uintptr_t(target)) - int64_t(uintptr_t(at + 5)));
    at[0] = 0xe9;
    std::memcpy(at + 1, &rel, 4);
}

// The tag table search reads past an empty table; the stub sends empty tables to "not found".
bool install_tag_search() {
    static const unsigned char site[] = {0x48, 0x8b, 0x02, 0x8b, 0x4a, 0x08, 0x31, 0xd2, 0xeb, 0x0a};
    if (!bytes_at(0x4bf44c, site)) return false;
    unsigned char *stub = near_stub(64);
    if (!stub || !fits_rel32(stub, image_ptr(0x4bf451))) return false;
    static const unsigned char head[] = {
        0x48, 0x8b, 0x02,         // mov rax, [rdx]
        0x8b, 0x4a, 0x08,         // mov ecx, [rdx+8]
        0x48, 0x85, 0xc0,         // test rax, rax
        0x74, 0x14,               // je not_found
        0x85, 0xc9,               // test ecx, ecx
        0x7e, 0x10,               // jle not_found
        0x31, 0xd2,               // xor edx, edx
    };
    std::memcpy(stub, head, sizeof head);
    unsigned char *p = emit_abs_jump(stub + sizeof head, image_ptr(0x4bf460));   // the search loop
    emit_abs_jump(p, image_ptr(0x4bf510));                                       // not_found: "tag not found"
    unsigned char *code = image_ptr(0x4bf44c);
    write_rel32_jump(code, stub);
    std::memset(code + 5, 0x90, 5);
    return true;
}

bool install_text_find() {
    static const unsigned char head[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x89, 0xf3, 0x49, 0x89, 0xfe};
    if (!image.executable(fTextFind, sizeof head) || !bytes_at(fTextFind, head)) {
        log("%s at 0x%llx differs, not hooked", "TextDb::Find", (unsigned long long)fTextFind);
        return false;
    }
    unsigned char *block = near_stub(0x100);
    if (!block) return false;
    unsigned char *trampoline = block;
    unsigned char *entry_stub = block + 0x80;
    if (!fits_rel32(entry_stub, image_ptr(fTextFind + 5))) return false;
    // Trampoline: the moved prologue (no rip-relative parts), then back into the original body.
    std::memcpy(trampoline, head, sizeof head);
    emit_abs_jump(trampoline + sizeof head, image_ptr(fTextFind + sizeof head));
    emit_abs_jump(entry_stub, reinterpret_cast<const void *>(&text_find));
    unsigned char *code = image_ptr(fTextFind);
    write_rel32_jump(code, entry_stub);
    std::memset(code + 5, 0xcc, sizeof head - 5);
    log("%s: 0x%llx hooked", "TextDb::Find", (unsigned long long)fTextFind);
    text_find_original(reinterpret_cast<TextFindFn>(trampoline));
    return true;
}

bool kill_info_ok() {
    static const unsigned char sender[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                           0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};
    return bytes_at(fSendKillInfo, sender) && kAttackTypes + 4 * kAttackTypeCount <= image.size &&
           image_load<uint32_t>(kAttackTypes) == 0 && image_load<uint32_t>(kAttackTypes + 8) == 0xdf95da88u;
}

struct StubSite {
    uint64_t address;
    const char *name;
    void *fn;
    bool needs_popup;
};

}  // namespace

// ---- interface for other files ---------------------------------------------------------------------------

void show_notice(const char *text) {
    if (!text || !*text) return;
    text_set_player(text);
    message_box(kTextPlayer);
}

void cinema_pick_friend() { pick_friend_for_cinema(); }

void install_remaining(int &installed) {
    // 1. level record package
    static const unsigned char level_pkg[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53,
                                              0x48, 0x83, 0xec, 0x10, 0x48, 0x89, 0xfb};
    if (bytes_at(0x644130, level_pkg) &&
        replace_function(0x644130, 14, reinterpret_cast<void *>(&level_record_package), "level record package (644130)"))
        ++installed;

    // 2. tracker values: read the match session's own block instead of the NULL room accessor
    static const unsigned char tracker_old[] = {0xbf, 0x00, 0x00, 0x00, 0x00, 0x48, 0x0f, 0x45, 0xfe};
    static const unsigned char tracker_new[] = {0x48, 0x89, 0xf7, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    if (bytes_at(0x640598, tracker_old)) {
        std::memcpy(image_ptr(0x640598), tracker_new, sizeof tracker_new);
        log("tracker values (640598): read from the session block, not the stripped room accessor");
        ++installed;
    } else {
        log("tracker values (640598): bytes differ, left alone");
    }

    // 3. empty tag table search
    if (install_tag_search()) {
        log("empty tag table search (4bf44c): checked -> C++ stub");
        ++installed;
    } else {
        log("empty tag table search (4bf44c): bytes differ, left alone");
    }

    // 4. NetStatGame host name
    static const unsigned char len_site[] = {0x31, 0xff, 0xe8};
    static const unsigned char copy_site[] = {0x48, 0x8d, 0x50, 0x01, 0x31, 0xf6, 0x48, 0x89, 0xdf, 0xe8};
    if (bytes_at(0x66c75d, len_site) && bytes_at(0x66c764, copy_site) &&
        retarget_call(0x66c75f, fStrlen, reinterpret_cast<void *>(&host_name_length), "NetStatGame host name length") &&
        retarget_call(0x66c76d, fMemcpy, reinterpret_cast<void *>(&host_name_copy), "NetStatGame host name copy"))
        ++installed;
    else
        log("NetStatGame host name: call sites differ, left alone");

    // 5. TextDb::Find
    if (install_text_find())
        ++installed;
    else
        log("PC texts unavailable: the PC message boxes will show the game's fallback text");

    // 6. guest functions the replacements call
    const bool kill_info = kill_info_ok();
    static const unsigned char show_popup[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xf0};
    const bool popup_ok = bytes_at(fShowPopup, show_popup);

    // 7. NetEventNpcDealDamage::Apply
    static const unsigned char deal_damage[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x10, 0x4c, 0x8b, 0x05};
    if (!kill_info)
        log("NetEventNpcDealDamage::Apply: the kill-info sender or attack types differ, left alone");
    else if (!bytes_at(0x61fa70, deal_damage))
        log("NetEventNpcDealDamage::Apply at 0x%llx: not the cut-short function this port expects, left alone",
            0x61fa70ull);
    else if (replace_function(0x61fa70, 5, reinterpret_cast<void *>(&npc_deal_damage_apply),
                              "NetEventNpcDealDamage::Apply"))
        ++installed;

    // 8. Invite To Party
    static const unsigned char invite[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x05, 0xcb,
                                           0x30, 0xa7, 0x00, 0x80, 0xb8, 0xc8, 0x00, 0x00, 0x00, 0x00};
    if (bytes_at(0x5ebee0, invite)) {
        if (replace_function(0x5ebee0, 5, reinterpret_cast<void *>(&invite_to_party), "Invite To Party (45a9c65b)"))
            ++installed;
    } else {
        log("Invite To Party at 0x%llx differs, left alone", 0x5ebee0ull);
    }

    // 9. BUTTON_MULTIPLAYER_INVITES query (cond 48)
    static const unsigned char query[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48,
                                          0x89, 0xfb, 0x8b, 0x03, 0x83, 0xf8, 0x02};
    if (bytes_at(0x659b50, query)) {
        if (replace_function(0x659b50, 5, reinterpret_cast<void *>(&invites_query),
                             "BUTTON_MULTIPLAYER_INVITES query (cond 48)"))
            ++installed;
    } else {
        log("BUTTON_MULTIPLAYER_INVITES query at 0x%llx differs, left alone", 0x659b50ull);
    }

    // 10. LIST_PLAYERSMET row
    static const unsigned char met[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x58, 0x48, 0x8b, 0x1d};
    if (popup_ok && bytes_at(0x5ebdf0, met)) {
        if (replace_function(0x5ebdf0, 5, reinterpret_cast<void *>(&players_met_row), "LIST_PLAYERSMET row (369314b2)"))
            ++installed;
    } else {
        log("LIST_PLAYERSMET row at 0x5ebdf0 differs, left alone");
    }

    // 11. the stripped callbacks (a bare ret)
    const StubSite stubs[] = {
        {0x61b900, "NetEventKickGriefer::Apply", reinterpret_cast<void *>(&kick_griefer_apply), false},
        {0x5ec490, "BUTTON_MULTIPLAYER_PARTY_PLAYERn", reinterpret_cast<void *>(&party_player_row), true},
        {0x5ec460, "BUTTON_MULTIPLAYER_PLAYERn", reinterpret_cast<void *>(&match_player_row), true},
        {0x5ebde0, "LIST_FRIENDS", reinterpret_cast<void *>(&friends_row), true},
        {0x5ec4a0, "Kick From Party (7cc46b94)", reinterpret_cast<void *>(&kick_from_party), false},
        {0x5ec4b0, "Mute (6547d63c)", reinterpret_cast<void *>(&mute_member), false},
        {0x5ec480, "Mute (7273924c)", reinterpret_cast<void *>(&mute_member), false},
        {0x5ec4c0, "Promote To Party Leader (2d32c8b0)", reinterpret_cast<void *>(&promote_leader), false},
        {0x5ecb70, "party IsHost (56126d00)", reinterpret_cast<void *>(&party_is_host), false},
        {0x5ebdd0, "Save (37c9cb76)", reinterpret_cast<void *>(&save_button), false},
        {0x5ebea0, "BUTTON_MULTIPLAYER_INVITES", reinterpret_cast<void *>(&invites_button), false},
        {0x5ec000, "Join Party (0ace8463)", reinterpret_cast<void *>(&join_party), false},
        {0x5ed8a0, "BUTTON_CLANTAG", reinterpret_cast<void *>(&clan_tag_button), false},
        {0x5ed8b0, "PAGE_MULTIPLAYER_PLAYER_OPTIONS", reinterpret_cast<void *>(&player_options_page), false},
        {0x5ed8c0, "store browse", reinterpret_cast<void *>(&store_browse), false},
        {0x64e900, "matchmaking box (24df6961)", reinterpret_cast<void *>(&matchmaking_box), false},
        {0x6769d0, "PAGE_TWITTER", reinterpret_cast<void *>(&twitter_page), false},
        {0x6769e0, "BUTTON_TWITTER_*", reinterpret_cast<void *>(&twitter_button), false},
        {0x5e4510, "cinema send to friend (fe0e7de7)", reinterpret_cast<void *>(&cinema_send), false},
        {0x5e4520, "cinema receive from friend (6b2970e2)", reinterpret_cast<void *>(&cinema_receive), false},
    };
    int done = 0;
    for (const StubSite &s : stubs) {
        if (s.needs_popup && !popup_ok) {
            log("%s: ShowPopup 0x%llx differs, left alone", s.name, (unsigned long long)fShowPopup);
            continue;
        }
        if (!is_ret_stub(s.address)) {
            log("%s at 0x%llx: not the stripped function this port expects, left alone", s.name,
                (unsigned long long)s.address);
            continue;
        }
        if (replace_function(s.address, 5, s.fn, s.name)) ++done;
    }
    installed += done;
}

}  // namespace u2mp
