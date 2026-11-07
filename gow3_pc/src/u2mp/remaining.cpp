// u2mp: the last multiplayer functions the PS4 1.02 build left as bare `ret` (or cut short), ported
// from PS3 1.10. Gameplay: NetEventNpcDealDamage::Apply and NetEventKickGriefer::Apply. Frontend: the
// party / player / friends menu callbacks, and the PSN-only buttons (store, invites, clan tag,
// Twitter, cinema sharing), which get the closest PC behaviour or a message box saying so.
//
// Addresses are Uncharted 2 PS4 1.02 image offsets, PS3 1.10 originals in the comments (pairs from
// u2mp/docs/functions-*.tsv). Every function replaced here is checked first: a ret stub, or for the
// cut-short NpcDealDamage Apply its prologue.
//
// Stubs left alone because the PS3 function is empty too (nothing to port): menu callbacks
// 0x5eb7c0 / 0x5eba90 / 0x5ec470 (PS3 0x4384e8 / 0x4384ec / 0x4385d8: blr), NetEventEarnMedalClient
// Apply 0x61d250 (PS3 0x468dc8: blr), 0x5f8240 (PS3 0x456a40: blr) and 0x63c740 (the type-39 event
// destructor; PS3 0x4862b4 only resets the vtable pointer, every PS4 event destructor is a ret).
// 0x5fb7a0, 0x604e80 and 0x673900 are not stubs: they return constants like their PS3 pairs.
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "session_api.h"
#include "voice.h"
#include "u2mp_internal.h"

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);

namespace u2mp {
namespace {

// ---- guest data -----------------------------------------------------------------------------
constexpr uint64_t kNetState = 0x14ce97c;     // PS3 NET_SM 0xf11764
constexpr uint64_t kParty = 0x105efb8;        // party session (PS3 0xf2c8a0)
constexpr uint64_t kMatch = 0x1065b08;        // g_matchSession (PS3 0xf20c48)
constexpr uint64_t kNetInfo = 0x104f1c8;      // PS3 0xf1fa48
constexpr uint64_t kNgm = 0x113b760;          // g_netGameManager (PS3 0xf1dc80)
constexpr uint64_t S_CTX = 0x10, S_ME = 0xb00, S_HOST = 0xb04;
constexpr uint64_t NI_MODE = 0x2c4;
// NetGameManager player trackers (PS3 +16 + i*624, net id +280, handle +16)
constexpr uint64_t NGM_TRACKERS = 0x10, NGM_TRACKER_SIZE = 0x2c0, T_HANDLE = 0x10, T_HANDLE_ID = 0x18,
                   T_NETID = 0x120;
constexpr int kTrackers = 10;
// NetGameManager npc trackers: hash map id -> tracker (PS3 lookup 0x4935d0, inlined on PS4 in the
// NpcKill Apply 0x621f80): bucket array +0x1ba0, bucket count +0x1bdc, FNV-1a over the id's bytes;
// node {+0 next, +0x10 id, +0x18 tracker}; tracker {+0 process slot, +8 process unique id}.
constexpr uint64_t NGM_NPC_BUCKETS = 0x1ba0, NGM_NPC_BUCKET_COUNT = 0x1bdc;
constexpr uint64_t P_UNIQUE_ID = 0x44;        // process unique id (PS3 +56)
// Npc (vtable layout of e.g. 0xff5dd0, same slot numbers as PS3): IsDead +0x250 (PS3 slot 74,
// 0x226c40 = [npc+0x1f0]->IsDead), TakeDamage +0x370 (PS3 slot 110, 0x2603f0).
constexpr uint64_t V_NPC_IS_DEAD = 0x250, V_NPC_TAKE_DAMAGE = 0x370;
constexpr uint64_t NPC_FLAGS_KILLED_BYTE = 0x262a;   // PS3 [npc+6264] |= 0x4000 << 32
// 45 attack-type ids (PS3 0xb3f4e0, lookup 0x4916f0); [2] is a sid the PS4 code also compares with.
constexpr uint64_t kAttackTypes = 0xdef290;
constexpr int kAttackTypeCount = 45;
constexpr uint32_t kAttackType2 = 0xdf95da88;
constexpr float kClockRate = 300.0f;          // PS3 0xdb1914: the frame clock runs at 300 Hz

// ---- guest functions ------------------------------------------------------------------------
constexpr uint64_t fPopPage = 0x5cc6f0;       // PopPage(menu)
constexpr uint64_t fShowPopup = 0x5cc480;     // ShowPopup(menu, desc, only-with-options) (PS3 0x429e10)
constexpr uint64_t fKillInfo = 0x60eb90;      // build + send NetEventKillInfo (PS3 0x48a3c0 + 0x472ce0)
constexpr uint64_t fTextFind = 0xcddc00;      // TextDb::Find(db, id) -> text or NULL (textdb.cpp)

// ---- replaced functions ---------------------------------------------------------------------
constexpr uint64_t hNpcDealDamageApply = 0x61fa70;   // PS3 0x48a620 (PS4 kept only the npc lookup)
constexpr uint64_t hKickGrieferApply = 0x61b900;     // PS3 0x4711f4

// ids
constexpr uint32_t kMessageBoxPage = 0x719e9fec;
constexpr uint32_t kPopupPage = 0x99ab57c2;          // PS3 0x422028 default, PS4 0x5ebdf0
constexpr uint32_t kPageLobby = sid("PAGE_LOBBY");
constexpr uint32_t kTextOk = 0xc6467f2d, kTextMute = 0xffbbac12, kTextKickFromParty = 0x1e2955af;
constexpr uint32_t kCbMute = 0x6547d63c, kCbKick = 0x7cc46b94;   // popup option callbacks (PS3)
static_assert(kPageLobby == 0xb99a3235u, "string id");

// Our texts, served by the TextDb::Find hook.
constexpr uint32_t kTextPlayer = sid("U2MP_PC_TEXT_PLAYER"), kTextStore = sid("U2MP_PC_TEXT_STORE"),
                   kTextVoice = sid("U2MP_PC_TEXT_VOICE"), kTextLeader = sid("U2MP_PC_TEXT_LEADER"),
                   kTextInvite = sid("U2MP_PC_TEXT_INVITE"), kTextClanTag = sid("U2MP_PC_TEXT_CLANTAG"),
                   kTextTwitter = sid("U2MP_PC_TEXT_TWITTER"), kTextCinemaSend = sid("U2MP_PC_TEXT_CINEMA_SEND"),
                   kTextCinemaReceive = sid("U2MP_PC_TEXT_CINEMA_RECEIVE");
char player_text[96];
const struct { uint32_t id; const char *text; } kTexts[] = {
    {kTextPlayer, player_text},
    {kTextStore, "The PlayStation Store is not available on PC."},
    {kTextVoice, "Voice chat is turned off (UNCHARTED_U2MP_VOICE=off)."},
    {kTextLeader, "On PC the player who hosts the party leads it. The leader can't be changed."},
    {kTextInvite, "Invites are not used on PC. To join the party, a friend starts the game with "
                  "UNCHARTED_U2MP_JOIN set to the party leader's address and selects Multiplayer."},
    {kTextClanTag, "Clan tags are not available on PC."},
    {kTextTwitter, "Twitter is not available on PC."},
    {kTextCinemaSend, "Sending cinema files to friends is not available on PC."},
    {kTextCinemaReceive, "Receiving cinema files from friends is not available on PC."},
};

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }
template <typename T> T field(const void *p, uint64_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t *>(p) + off, sizeof v);
    return v;
}
template <typename F> F vcall(void *object, uint64_t slot) {
    return reinterpret_cast<F>(*reinterpret_cast<uint64_t *>(*static_cast<uint64_t *>(object) + slot));
}

int32_t state() { return G<int32_t>(kNetState); }
// PS3 IsHost 0x4d2394, inlined ~120 times on PS4.
bool is_host(uint64_t session) {
    return G<uint64_t>(session + S_CTX) == 0 || G<uint32_t>(session + S_ME) == G<uint32_t>(session + S_HOST);
}
void pop_page() { guest<void(U2MP_GUEST *)(void *)>(fPopPage)(image.at<void>(addr::menu_manager)); }

// The one-button message box, as 0x5e41f0 shows it (PS3 0x42a3e8).
void message_box(uint32_t text) {
    uint8_t *box = *image.at<uint8_t *>(addr::menu_manager + 0x17d0);
    if (!box) return;
    std::memcpy(box + 0x08, &text, 4);
    const uint32_t words[] = {0, kTextOk, 0x11d8e78a, 0, 0x1519fa3d, 0, 0};   // +0x40..+0x58
    std::memcpy(box + 0x40, words, sizeof words);
    show_page(kMessageBoxPage);
}

// Popup descriptor (PS3 0x422028 builds it, 0x429e10 shows it; PS4 0x5cc480 copies 0x13 dwords).
struct Popup {
    uint32_t title, page, count;
    struct { uint32_t text, callback; } option[8];
    void add(uint32_t text, uint32_t callback) {
        if (count < 8) option[count++] = {text, callback};
    }
};
static_assert(sizeof(Popup) == 0x13 * 4, "popup descriptor");

// ---- TextDb::Find hook: our texts by id, everything else from the game ------------------------
using TextFindFn = const char *(U2MP_GUEST *)(void *db, uint32_t id);
TextFindFn orig_text_find = nullptr;

// ---- the strings only PS3 title updates carried (u2mp/docs/MENUS.md section 4) -----------------
// The multiplayer menus use about 25 text ids that are in no PS4 text bank: the stat labels of the
// profile panel (f67d9ad2 "Competitive Wins" ...), the leaderboard footer, "Time Played", "[A] daily
// players", "Clan Tag" ... PS3 1.10 has them in the dlc-patch7 networking bank. The import writes
// text1/ps4/<lang>.<category> = the PS4 bank plus the PS3 strings it lacks (u2text.py), but the game
// reads its banks from core.psarc, so a loose bank is never opened. Here: a TextDb::Find miss is
// answered from those imported banks (the player's own PS3 copy). Nothing is answered without them.
// Bank object (loader 0xcdd960): +0x20 category name, +0x28 loaded language index (-1 none),
// +0x58 next bank; language table 0x1450d18 (0x38 per language, +0x14 the file number).
constexpr uint64_t kLanguageTable = 0x1450d18, TB_LANGUAGE = 0x28, TB_NEXT = 0x58;
constexpr const char *kTextDir = "/app0/u2data/build/main/text1/ps4/";
std::mutex import_text_lock;
int import_text_language = -2;   // -2: not loaded yet
std::unordered_map<uint32_t, std::string> import_texts;

int bank_language(const uint8_t *bank) {
    for (int guard = 0; bank && guard < 8; ++guard, bank = *reinterpret_cast<uint8_t *const *>(bank + TB_NEXT)) {
        const int32_t index = field<int32_t>(bank, TB_LANGUAGE);
        if (index >= 0 && index < 64) return G<int32_t>(kLanguageTable + uint64_t(index) * 0x38 + 0x14);
    }
    return -1;
}

uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// A bank file (big-endian: u32 count; count x {u32 id, u32 offset}; NUL-terminated strings).
bool load_bank(const std::string &guest) {
    char host[1024];
    if (runtime_file_translate(guest.c_str(), host, sizeof host) != 0) return false;
    std::FILE *f = std::fopen(host, "rb");
    if (!f) return false;
    std::string d;
    char buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) d.append(buffer, n);
    std::fclose(f);
    const auto *p = reinterpret_cast<const uint8_t *>(d.data());
    if (d.size() < 4) return false;
    const uint32_t count = be32(p);
    const uint64_t blob = 4 + 8 * uint64_t(count);
    if (blob > d.size()) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = be32(p + 4 + 8 * i), off = be32(p + 8 + 8 * i);
        if (blob + off >= d.size()) continue;
        const char *s = d.data() + blob + off;
        import_texts[id].assign(s, strnlen(s, d.size() - size_t(blob + off)));
    }
    return true;
}

const char *import_text(const uint8_t *db, uint32_t id) {
    const int language = bank_language(db);
    std::lock_guard<std::mutex> lock(import_text_lock);
    if (language != import_text_language) {
        import_text_language = language;
        import_texts.clear();
        int files = 0;
        // common first: the networking bank (dlc-patch7, the newest) wins for ids both have.
        for (const char *category : {"common", "networking"}) {
            const std::string number = std::to_string(language >= 0 ? language : 2);
            if (load_bank(kTextDir + number + "." + category) ||
                (number != "2" && load_bank(std::string(kTextDir) + "2." + category)))
                ++files;
        }
        log("texts: %zu strings from %d imported bank(s) for language %d", import_texts.size(), files, language);
    }
    const auto it = import_texts.find(id);
    return it == import_texts.end() ? nullptr : it->second.c_str();
}

U2MP_GUEST const char *text_find(void *db, uint32_t id) {
    for (const auto &t : kTexts)
        if (t.id == id) return t.text;
    if (const char *text = extra_text(id)) return text;
    if (const char *text = orig_text_find(db, id)) return text;
    return import_text(static_cast<const uint8_t *>(db), id);
}

// Hooks a function whose first `length` bytes are whole position-independent instructions: they
// move to a trampoline (then jmp back) and the entry jumps to `host`. `original` = trampoline.
bool detour(uint64_t offset, const unsigned char *expect, unsigned length, void *host, void **original,
            const char *name) {
    if (length < 5 || !image.executable(offset, length) || !image.matches(offset, expect, length)) {
        log("%s at 0x%llx differs, not hooked", name, (unsigned long long)offset);
        return false;
    }
    unsigned char *code = image.base + offset;
    unsigned char *tramp = near_stub(0x100);
    if (!tramp) return false;
    unsigned char *stub = tramp + 0x80;
    std::memcpy(tramp, code, length);
    const uint64_t back = uint64_t(uintptr_t(code + length)), target = uint64_t(uintptr_t(host));
    tramp[length] = 0xff;   // jmp [rip+0]; dq back
    tramp[length + 1] = 0x25;
    std::memset(tramp + length + 2, 0, 4);
    std::memcpy(tramp + length + 6, &back, 8);
    stub[0] = 0x48;         // mov rax, host; jmp rax
    stub[1] = 0xb8;
    std::memcpy(stub + 2, &target, 8);
    stub[10] = 0xff;
    stub[11] = 0xe0;
    const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(code + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    const int32_t rel = int32_t(delta);
    *original = tramp;
    code[0] = 0xe9;
    std::memcpy(code + 1, &rel, 4);
    std::memset(code + 5, 0xcc, length - 5);
    log("%s: 0x%llx hooked", name, (unsigned long long)offset);
    return true;
}

// ---- 44 NetEventNpcDealDamage::Apply (PS3 0x48a620) -------------------------------------------
// Payload (Read in events.cpp): +0x0c attacker net id (12 bits), +0x10 npc id, +0x14 S32 damage,
// +0x18 time (s), +0x1c attack type index (6 bits).
void *process_of(const void *slot_holder, uint64_t slot_off, uint64_t id_off) {
    const uint64_t slot = field<uint64_t>(slot_holder, slot_off);
    const uint32_t id = field<uint32_t>(slot_holder, id_off);
    if (!slot || !id) return nullptr;
    void *process = *reinterpret_cast<void **>(uintptr_t(slot));
    return process && field<uint32_t>(process, P_UNIQUE_ID) == id ? process : nullptr;
}

// PS3 0x4918e8: the player tracker with this net id.
void *player_tracker(uint32_t net_id) {
    for (int i = 0; i < kTrackers; ++i) {
        uint8_t *t = image.at<uint8_t>(kNgm + NGM_TRACKERS + uint64_t(i) * NGM_TRACKER_SIZE);
        if (field<uint32_t>(t, T_NETID) == net_id) return t;
    }
    return nullptr;
}

// PS3 0x4935d0 (PS4 inlined in 0x61fa70 / 0x621f80): the npc tracker with this id.
void *npc_tracker(uint32_t id) {
    const uint32_t count = G<uint32_t>(kNgm + NGM_NPC_BUCKET_COUNT);
    const uint64_t buckets = G<uint64_t>(kNgm + NGM_NPC_BUCKETS);
    if (!count || !buckets) return nullptr;
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 4; ++i) h = (h ^ ((id >> (8 * i)) & 0xff)) * 0x1000193u;
    const uint64_t *table = reinterpret_cast<const uint64_t *>(uintptr_t(buckets));
    uint64_t node = table[h % count];
    while (node && field<uint32_t>(reinterpret_cast<void *>(uintptr_t(node)), 0x10) != id)
        node = *reinterpret_cast<const uint64_t *>(uintptr_t(node));
    if (!node || node == table[count]) return nullptr;   // table[count] is the end node
    return reinterpret_cast<void *>(uintptr_t(node + 0x18));
}

int32_t cvtt32(float f) { return (f >= -2147483648.0f && f < 2147483648.0f) ? int32_t(f) : INT32_MIN; }

U2MP_GUEST void npc_deal_damage_apply(void *ev, void *) {
    void *player = player_tracker(field<uint32_t>(ev, 0x0c));
    void *npc_t = npc_tracker(field<uint32_t>(ev, 0x10));
    if (!player || !npc_t) return;
    void *attacker = process_of(player, T_HANDLE, T_HANDLE_ID);
    void *npc = process_of(npc_t, 0, 8);
    if (!attacker || !npc) return;
    using IsDead = uint8_t(U2MP_GUEST *)(void *);
    using TakeDamage = void(U2MP_GUEST *)(void *, int32_t);
    if (vcall<IsDead>(npc, V_NPC_IS_DEAD)(npc)) return;
    vcall<TakeDamage>(npc, V_NPC_TAKE_DAMAGE)(npc, field<int32_t>(ev, 0x14));
    if (!vcall<IsDead>(npc, V_NPC_IS_DEAD)(npc)) return;
    if (!is_host(kMatch)) return;   // PS3 NetGameManager::IsHost 0x494ea8
    // the kill: credit the attacker (PS4 callers of 0x60eb90 do the same, e.g. 0x25ead0)
    const uint32_t index = field<uint32_t>(ev, 0x1c);
    const uint32_t type = index < uint32_t(kAttackTypeCount) ? G<uint32_t>(kAttackTypes + 4 * index) : 0;
    int32_t when = cvtt32(field<float>(ev, 0x18) * kClockRate);
    using KillInfo = void(U2MP_GUEST *)(uint32_t, void *, void *, int32_t *, uint32_t, uint32_t, uint32_t);
    guest<KillInfo>(fKillInfo)(type, attacker, npc, &when, 0, 0, 0);
    static_cast<uint8_t *>(npc)[NPC_FLAGS_KILLED_BYTE] |= 2;
}

// ---- 69 NetEventKickGriefer::Apply (PS3 0x4711f4) ---------------------------------------------
// The host kicks the member (+0x0c, 12-bit net id) out of the match: PS3 KickoutRoomMember 0x4d8a98
// on g_matchSession. Here party and match are one session, so it is the session layer's kick.
U2MP_GUEST void kick_griefer_apply(void *ev, void *) {
    if (!is_host(kMatch)) return;
    const uint32_t net_id = field<uint32_t>(ev, 0x0c);
    if (!session::kick(net_id)) log("NetEventKickGriefer: can't kick net id %u", net_id);
}

// ---- party / player options -------------------------------------------------------------------
// PS3 kept the selected player's NpId (0xf11a40); here its net id.
uint32_t selected_id = 0;

void set_player_text(const std::string &text) { std::snprintf(player_text, sizeof player_text, "%s", text.c_str()); }

// PARTY_PLAYERn (PS3 0x4417f0) / PLAYERn (PS3 0x440620) / LIST_FRIENDS rows (PS3 0x43f830): the
// options popup of one player, titled with the name. PS3 offered mute / unmute, and to the party
// leader "Promote To Party Leader" and "Kick From Party". On PC: mute / unmute (voice.cpp) and,
// for the leader, kick. Your own row (PS3: no popup) shows your name.
void player_options(int row) {
    uint32_t id = 0;
    std::string name;
    if (!session::member_at(row, id, name)) {
        log("player options: no player in row %d", row);
        return;
    }
    selected_id = id;
    if (id == session::local_id()) {
        set_player_text(name + " (you)");
        message_box(kTextPlayer);
        return;
    }
    set_player_text(name);
    Popup p{kTextPlayer, kPopupPage, 0, {}};
    p.add(kTextMute, kCbMute);
    if (session::is_host()) p.add(kTextKickFromParty, kCbKick);
    guest<void(U2MP_GUEST *)(void *, Popup *, uint32_t)>(fShowPopup)(image.at<void>(addr::menu_manager), &p, 1);
}

uint32_t arg32(const void *arg, uint64_t off) { return arg ? field<uint32_t>(arg, off) : 0; }

// PS3 arg+8 = n; PAGE_LOBBY's rows count on from 10 (cond 26..35), PS3 takes n % 10 (0x4417f0).
U2MP_GUEST void party_player(void *arg, void *, void *) { player_options(int(arg32(arg, 8) % 10)); }
U2MP_GUEST void match_player(void *arg, void *, void *) { player_options(int(arg32(arg, 8))); }
U2MP_GUEST void friends_row(void *arg, void *, void *) { player_options(int(arg32(arg, 4))); }   // list row

// 7cc46b94 (PS3 0x439cf8): KickoutRoomMember(party, selected member).
U2MP_GUEST void kick_selected(void *, void *, void *) {
    if (!selected_id || !session::kick(selected_id)) log("Kick From Party: net id %u not kicked", selected_id);
}
// 6547d63c (PS3 0x439bf0) and 7273924c (PS3 0x439d60): toggle the voice mute flag (cellVoice; here
// voice.cpp), then say which way it went.
U2MP_GUEST void toggle_mute(void *, void *, void *) {
    std::string name;
    if (!voice::enabled()) {
        message_box(kTextVoice);
        return;
    }
    if (!session::member_name(selected_id, name)) return;   // left meanwhile
    set_player_text(name + (voice::toggle_mute(selected_id) ? ": muted" : ": unmuted"));
    message_box(kTextPlayer);
}
// 2d32c8b0 (PS3 0x439c7c): GrantRoomOwner(party, selected), then PAGE_LOBBY.
U2MP_GUEST void promote_leader(void *, void *, void *) { message_box(kTextLeader); }
// 56126d00 (PS3 0x438638): IsHost(party session).
U2MP_GUEST uint8_t party_is_host(void *, void *, void *) { return is_host(kParty) ? 1 : 0; }
// 37c9cb76 (PS3 0x438a10): only a debug print.
U2MP_GUEST void save_print(void *, void *, void *) { log("Save"); }

// BUTTON_MULTIPLAYER_INVITES (PS3 0x440948): the PSN invite inbox, then PAGE_LOBBY when a mode is
// chosen. 0ace8463 (PS3 0x43ff60): invite the selected friend to the party (NP message 0x10b).
U2MP_GUEST void invites(void *, void *, void *) {
    if (G<int32_t>(kNetInfo + NI_MODE) != 0) show_page(kPageLobby);
    message_box(kTextInvite);
}
U2MP_GUEST void invite_friend(void *, void *, void *) { message_box(kTextInvite); }

// BUTTON_CLANTAG (PS3 0x441440): the on-screen keyboard for the clan tag; PAGE_MULTIPLAYER_PLAYER_OPTIONS
// (PS3 0x4443f8) polled it (events 2/3 free it, 6 stores the tag) and closed itself when signed out.
U2MP_GUEST void clan_tag(void *, void *, void *) { message_box(kTextClanTag); }
U2MP_GUEST void player_options_page(void *arg, void *, void *) {
    if (arg32(arg, 0) == 6 && state() < 2) pop_page();
}

// sceNpCommerce2ExecuteStoreBrowse (PS3 0x4399a0).
U2MP_GUEST void store_browse(void *, void *, void *) { message_box(kTextStore); }

// 24df6961 (PS3 0x4af4fc): the answer of a matchmaking message box (arg+8 != 0), read only by the
// matchmaking state machine (PS3 0x4b0a58), which PS4 lost (0x64e910 = ret; the session layer starts
// matchmaking games from the party instead).
bool matchmaking_answer = false;
U2MP_GUEST void matchmaking_box(void *arg, void *, void *) {
    if (arg32(arg, 8)) matchmaking_answer = true;
    log("matchmaking message box answered (%u)", arg32(arg, 8));
}

// PAGE_TWITTER (PS3 0x4f1950 -> 0x4f1744: login thread, password keyboard) and BUTTON_TWITTER_*
// (PS3 0x4f1c38). On PC the page closes itself and says why.
U2MP_GUEST void twitter_page(void *arg, void *, void *) {
    if (arg32(arg, 0) == 6) {
        pop_page();
        message_box(kTextTwitter);
    }
}
U2MP_GUEST void twitter_button(void *, void *, void *) { message_box(kTextTwitter); }

// Cinema send to friend fe0e7de7 (PS3 0x43402c, sceNpBasicSendMessageGui) and receive 6b2970e2
// (PS3 0x433554, sceNpBasicRecvMessageCustom).
U2MP_GUEST void cinema_send(void *, void *, void *) { message_box(kTextCinemaSend); }
U2MP_GUEST void cinema_receive(void *, void *, void *) { message_box(kTextCinemaReceive); }

// NetStatGame match-start init (0x66c600, test run 16 crash): when the match host is a session member
// it copies the host's name to NetStatGame+0xe (0x40 bytes, up to +0x4e) with
// memcpy(dst, name, strlen(name) + 1). PS4 stripped the accessor that returned the name and the
// compiler folded it to NULL: `xor edi,edi; call strlen` (0x66c75d) and `xor esi,esi; ... call memcpy`
// (0x66c76d) read address 0. Both calls now get the host's name from the session.
constexpr uint64_t kMatchSession = 0x1065b08, MS_HOST = 0xb04;
constexpr uint64_t cStatHostLen = 0x66c75f, cStatHostCopy = 0x66c76d, fStrlen = 0xde6490, fMemcpy = 0xde6410;
constexpr size_t kStatNameSize = 0x40;
std::string stat_host_name() {
    std::string n;
    if (!session::member_name(*image.at<uint32_t>(kMatchSession + MS_HOST), n)) n = session::local_name();
    if (n.size() >= kStatNameSize) n.resize(kStatNameSize - 1);
    return n;
}
U2MP_GUEST uint64_t stat_host_len(const char *s) { return s ? std::strlen(s) : stat_host_name().size(); }
U2MP_GUEST void *stat_host_copy(void *dst, const void *src, uint64_t n) {
    if (src) return std::memcpy(dst, src, n);
    const std::string name = stat_host_name();
    std::memcpy(dst, name.c_str(), name.size() + 1);
    return dst;
}

}  // namespace

// 0x644130 (one caller, 0x662fd0, at match start): the package of the current net game's level record
// (*levels* 0dec97a4, the record whose +0 is the net game's +4; package id at +0x20 = sid of the
// name at +0x18, "<map>-pak"). No PS3 or PS4 registry, file or code defines a "<map>-pak" package
// (test runs 21-28), so it returned NULL and the caller read NULL+0x9a0 (test run 28). Here the
// same lookup, and when "<map>-pak" does not exist, the map's own package "<map>".
U2MP_GUEST const uint8_t *level_record_package(void *) {
    struct Table { int32_t count; int32_t pad; uint8_t *records; };
    auto lookup = reinterpret_cast<Table *(U2MP_GUEST *)(uint32_t)>(image.base + 0xa15e70);
    Table *games = lookup(0x3b9a067d), *levels = lookup(0x0dec97a4);
    const int16_t game = *image.at<int16_t>(0x113b760 + 0x1ca0);
    if (!games || !levels || !games->records || !levels->records || game < 0 || game >= games->count) return nullptr;
    const uint32_t level_id = *reinterpret_cast<const uint32_t *>(games->records + 0x90 * game + 4);
    for (int32_t i = 0; i < levels->count; ++i) {
        const uint8_t *rec = levels->records + 0x38 * i;
        if (*reinterpret_cast<const uint32_t *>(rec) != level_id) continue;
        if (const uint8_t *pkg = package_by_id(*reinterpret_cast<const uint32_t *>(rec + 0x20))) return pkg;
        const char *name = *reinterpret_cast<const char *const *>(rec + 0x18);
        const size_t n = name ? std::strlen(name) : 0;
        if (n > 4 && std::strcmp(name + n - 4, "-pak") == 0) {
            const std::string map(name, n - 4);
            const uint8_t *pkg = package_by_id(sid(map.c_str()));
            static bool said = false;
            if (!said) {
                said = true;
                log("level package: %s is defined nowhere, using %s (%s)", name, map.c_str(), pkg ? "loaded" : "not loaded");
            }
            return pkg;
        }
        return nullptr;
    }
    return nullptr;
}

void install_remaining(int &installed) {
    {
        // 0x644130: push rbp; mov rbp,rsp; push r14; push rbx; sub rsp,0x10; mov rbx,rdi
        static const unsigned char expect[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x10, 0x48, 0x89, 0xfb};
        if (image.matches(0x644130, expect, sizeof expect) &&
            replace_function(0x644130, sizeof expect, reinterpret_cast<void *>(&level_record_package), "level record package (644130)"))
            ++installed;
    }
    {
        // Test run 25 (null+8 read at 0x6405af): for every player tracker the NetGameManager takes a
        // value from the match session's own block (+0xb10) when it has no room (+0x10 == 0) and
        // +0xb0c == 0x30, else from the room attributes, an accessor PS4 stripped and the compiler
        // folded to NULL (`mov edi,0; cmovne rdi,rsi`). Our session has a room, so: always the own block.
        constexpr uint64_t site = 0x640598;
        static const unsigned char expect[] = {0xbf, 0x00, 0x00, 0x00, 0x00, 0x48, 0x0f, 0x45, 0xfe};
        if (image.matches(site, expect, sizeof expect)) {
            static const unsigned char fix[] = {0x48, 0x89, 0xf7, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};   // mov rdi,rsi
            std::memcpy(image.base + site, fix, sizeof fix);
            log("tracker values (640598): read from the session block, not the stripped room accessor");
            ++installed;
        } else {
            log("tracker values (640598): bytes differ, left alone");
        }
    }
    {
        // Test run 18: online only (NET_SM >= 2), a level object's tag table ({data, count} at object
        // +0x68) is binary-searched for tag 2743c4b0 (0x4bf44c..0x4bf492) without a count check; an
        // empty table has data NULL and the search reads address 0 while LoadLevels runs. PS4 never ran
        // this online path. 0x4bf44c (mov rax,[rdx]; mov ecx,[rdx+8]; xor edx,edx; jmp 4bf460) jumps to
        // a copy that treats an empty table as "tag not found" (0x4bf510).
        constexpr uint64_t site = 0x4bf44c, loop = 0x4bf460, not_found = 0x4bf510;
        static const unsigned char expect[] = {0x48, 0x8b, 0x02, 0x8b, 0x4a, 0x08, 0x31, 0xd2, 0xeb, 0x0a};
        unsigned char *stub = image.matches(site, expect, sizeof expect) ? near_stub(64) : nullptr;
        const int64_t delta = stub ? int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + site + 5)) : 0;
        if (stub && delta >= INT32_MIN && delta <= INT32_MAX) {
            static const unsigned char code[] = {
                0x48, 0x8b, 0x02,               // mov rax,[rdx]
                0x8b, 0x4a, 0x08,               // mov ecx,[rdx+8]
                0x48, 0x85, 0xc0,               // test rax,rax
                0x74, 0x14,                     // jz not_found
                0x85, 0xc9,                     // test ecx,ecx
                0x7e, 0x10,                     // jle not_found
                0x31, 0xd2,                     // xor edx,edx
                0xff, 0x25, 0, 0, 0, 0,         // jmp [rip] -> loop
                0, 0, 0, 0, 0, 0, 0, 0,
                0xff, 0x25, 0, 0, 0, 0,         // not_found: jmp [rip] -> not_found
                0, 0, 0, 0, 0, 0, 0, 0};
            std::memcpy(stub, code, sizeof code);
            const uint64_t a = uint64_t(uintptr_t(image.base + loop)), b = uint64_t(uintptr_t(image.base + not_found));
            std::memcpy(stub + 23, &a, 8);
            std::memcpy(stub + 37, &b, 8);
            unsigned char *c = image.base + site;
            const int32_t rel = int32_t(delta);
            c[0] = 0xe9;
            std::memcpy(c + 1, &rel, 4);
            std::memset(c + 5, 0x90, sizeof expect - 5);
            log("empty tag table search (4bf44c): checked -> C++ stub");
            ++installed;
        } else {
            log("empty tag table search (4bf44c): bytes differ, left alone");
        }
    }
    {
        // 66c75d: xor edi,edi; call strlen; lea rdx,[rax+1]; xor esi,esi; mov rdi,rbx; call memcpy
        static const unsigned char site[] = {0x31, 0xff, 0xe8};
        static const unsigned char site2[] = {0x48, 0x8d, 0x50, 0x01, 0x31, 0xf6, 0x48, 0x89, 0xdf, 0xe8};
        if (image.matches(cStatHostLen - 2, site, sizeof site) && image.matches(cStatHostLen + 5, site2, sizeof site2) &&
            retarget_call(cStatHostLen, fStrlen, reinterpret_cast<void *>(&stat_host_len), "NetStatGame host name length") &&
            retarget_call(cStatHostCopy, fMemcpy, reinterpret_cast<void *>(&stat_host_copy), "NetStatGame host name copy"))
            ++installed;
        else
            log("NetStatGame host name: call sites differ, left alone");
    }
    // Texts first: the message boxes below need them.
    static const unsigned char find_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x89, 0xf3, 0x49, 0x89, 0xfe};
    void *orig = nullptr;
    if (detour(fTextFind, find_bytes, sizeof find_bytes, reinterpret_cast<void *>(&text_find), &orig, "TextDb::Find")) {
        orig_text_find = reinterpret_cast<TextFindFn>(orig);
        ++installed;
    } else {
        log("PC texts unavailable: the PC message boxes will show the game's fallback text");
    }

    // Guest functions we call.
    static const unsigned char kill_info_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                                    0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};
    static const unsigned char popup_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xf0};
    const bool kill_info_ok = image.matches(fKillInfo, kill_info_bytes, sizeof kill_info_bytes) &&
                              kAttackTypes + 4 * kAttackTypeCount <= image.size &&
                              G<uint32_t>(kAttackTypes) == 0 && G<uint32_t>(kAttackTypes + 8) == kAttackType2;
    const bool popup_ok = image.matches(fShowPopup, popup_bytes, sizeof popup_bytes);

    // NpcDealDamage Apply is not a stub: PS4 kept the npc lookup and dropped the rest.
    static const unsigned char deal_damage_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x10, 0x4c, 0x8b, 0x05};
    if (!kill_info_ok) log("NetEventNpcDealDamage::Apply: the kill-info sender or attack types differ, left alone");
    else if (!image.matches(hNpcDealDamageApply, deal_damage_bytes, sizeof deal_damage_bytes))
        log("NetEventNpcDealDamage::Apply at 0x%llx: not the cut-short function this port expects, left alone",
            (unsigned long long)hNpcDealDamageApply);
    else if (replace_function(hNpcDealDamageApply, 5, reinterpret_cast<void *>(&npc_deal_damage_apply),
                              "NetEventNpcDealDamage::Apply"))
        ++installed;

    struct { uint64_t at; void *fn; const char *name; bool needs_popup; } stubs[] = {
        {hKickGrieferApply, reinterpret_cast<void *>(&kick_griefer_apply), "NetEventKickGriefer::Apply", false},
        {0x5ec490, reinterpret_cast<void *>(&party_player), "BUTTON_MULTIPLAYER_PARTY_PLAYERn", true},
        {0x5ec460, reinterpret_cast<void *>(&match_player), "BUTTON_MULTIPLAYER_PLAYERn", true},
        {0x5ebde0, reinterpret_cast<void *>(&friends_row), "LIST_FRIENDS", true},
        {0x5ec4a0, reinterpret_cast<void *>(&kick_selected), "Kick From Party (7cc46b94)", false},
        {0x5ec4b0, reinterpret_cast<void *>(&toggle_mute), "Mute (6547d63c)", false},
        {0x5ec480, reinterpret_cast<void *>(&toggle_mute), "Mute (7273924c)", false},
        {0x5ec4c0, reinterpret_cast<void *>(&promote_leader), "Promote To Party Leader (2d32c8b0)", false},
        {0x5ecb70, reinterpret_cast<void *>(&party_is_host), "party IsHost (56126d00)", false},
        {0x5ebdd0, reinterpret_cast<void *>(&save_print), "Save (37c9cb76)", false},
        {0x5ebea0, reinterpret_cast<void *>(&invites), "BUTTON_MULTIPLAYER_INVITES", false},
        {0x5ec000, reinterpret_cast<void *>(&invite_friend), "invite friend (0ace8463)", false},
        {0x5ed8a0, reinterpret_cast<void *>(&clan_tag), "BUTTON_CLANTAG", false},
        {0x5ed8b0, reinterpret_cast<void *>(&player_options_page), "PAGE_MULTIPLAYER_PLAYER_OPTIONS", false},
        {0x5ed8c0, reinterpret_cast<void *>(&store_browse), "store browse", false},
        {0x64e900, reinterpret_cast<void *>(&matchmaking_box), "matchmaking box (24df6961)", false},
        {0x6769d0, reinterpret_cast<void *>(&twitter_page), "PAGE_TWITTER", false},
        {0x6769e0, reinterpret_cast<void *>(&twitter_button), "BUTTON_TWITTER_*", false},
        {0x5e4510, reinterpret_cast<void *>(&cinema_send), "cinema send to friend (fe0e7de7)", false},
        {0x5e4520, reinterpret_cast<void *>(&cinema_receive), "cinema receive from friend (6b2970e2)", false},
    };
    int done = 0;
    for (const auto &s : stubs) {
        if (s.needs_popup && !popup_ok) {
            log("%s: ShowPopup 0x%llx differs, left alone", s.name, (unsigned long long)fShowPopup);
            continue;
        }
        if (!is_ret_stub(s.at)) {
            log("%s at 0x%llx: not the stripped function this port expects, left alone", s.name,
                (unsigned long long)s.at);
            continue;
        }
        if (replace_function(s.at, 5, s.fn, s.name)) ++done;
    }
    installed += done;
}

}  // namespace u2mp
