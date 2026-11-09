// u2mp: the game's texts for the multiplayer menus, in every language (texts.cpp, u2mp/docs/TEXTS.md).
#pragma once

#include <cstdint>

#include "u2mp_internal.h"

namespace u2mp {

// TextDb::Find(db, id) (0xcddc00), as remaining.cpp hooks it: the game's text, else ours. NULL only when
// no text exists at all (the game then shows its "UNTRANSLATED STRING!!!"); each such id is logged once.
using TextFindFn = const char *(U2MP_GUEST *)(void *db, uint32_t id);
U2MP_GUEST const char *text_find(void *db, uint32_t id);
void text_find_original(TextFindFn original);   // the hook's trampoline (the original function)

// Our own text for `id` in the game's current language (NULL for an id we have no text for).
const char *own_text(uint32_t id);
// The text of U2MP_PC_TEXT_PLAYER (a player name and state; remaining.cpp's message box / popup title).
void text_set_player(const char *text);

// PC message box texts (own_text / text_find serve them).
constexpr uint32_t kTextPlayer = sid("U2MP_PC_TEXT_PLAYER"), kTextStore = sid("U2MP_PC_TEXT_STORE"),
                   kTextVoice = sid("U2MP_PC_TEXT_VOICE"), kTextLeader = sid("U2MP_PC_TEXT_LEADER"),
                   kTextInvite = sid("U2MP_PC_TEXT_INVITE"), kTextClanTag = sid("U2MP_PC_TEXT_CLANTAG"),
                   kTextTwitter = sid("U2MP_PC_TEXT_TWITTER"), kTextCinemaSend = sid("U2MP_PC_TEXT_CINEMA_SEND"),
                   kTextCinemaReceive = sid("U2MP_PC_TEXT_CINEMA_RECEIVE"), kTextYou = sid("U2MP_PC_TEXT_YOU"),
                   kTextMuted = sid("U2MP_PC_TEXT_MUTED"), kTextUnmuted = sid("U2MP_PC_TEXT_UNMUTED");

}  // namespace u2mp
