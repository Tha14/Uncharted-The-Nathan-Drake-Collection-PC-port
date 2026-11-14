// u2mp: editing the clan tag on the multiplayer profile page.
//
// On PS3 BUTTON_CLANTAG opened the on-screen keyboard and the page polled it every frame. On PC the
// runtime's in-window text input takes its place (Enter or Cross accepts, Esc or Circle cancels):
// remaining.cpp's button callback opens it with edit_clan_tag(), and the page's per-frame event calls
// clan_tag_frame() until it closes. The tag is kept in NetPlayerData, where the game's label query
// shows it and progression.cpp saves and publishes it.
//
// Reimplemented from the behaviour description u2mp/docs/spec/remaining.md.
#include "progression.h"
#include "u2mp_internal.h"

#include <cstdint>
#include <cstring>
#include <string>

extern "C" int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
extern "C" int gow3gpu_text_input_poll(char *out_utf8, uint64_t size);

namespace u2mp {
namespace {

constexpr uint64_t kClanTag = 0x11474f8 + 0x1a8;   // NetPlayerData: 8 bytes, NUL-padded
constexpr size_t kTagBytes = 8;
constexpr size_t kTagChars = 4;

bool g_editing = false;

// Keeps only what a tag may hold: letters (upper-cased) and digits, at most four of them.
void store_tag(const char *text, size_t size) {
    char tag[kTagBytes] = {};
    size_t taken = 0;
    for (size_t i = 0; i < size && text[i] && taken < kTagChars; ++i) {
        const char c = text[i];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            tag[taken++] = c;
        else if (c >= 'a' && c <= 'z')
            tag[taken++] = char(c - 'a' + 'A');
    }
    std::memcpy(image.at<char>(kClanTag), tag, kTagBytes);
    log("clan tag: [%.4s]", tag);
    progression_publish();
}

}  // namespace

bool edit_clan_tag() {
    if (g_editing) return true;
    const char *stored = image.at<const char>(kClanTag);
    const std::string current(stored, strnlen(stored, kTagBytes));
    if (!gow3gpu_text_input_begin(current.c_str(), "Clan Tag (up to 4 letters or digits; empty = none)")) {
        log("clan tag: no text input available");
        return false;
    }
    g_editing = true;
    log("clan tag: text input open");
    return true;
}

void clan_tag_frame() {
    if (!g_editing) return;
    char text[64] = {};
    const int result = gow3gpu_text_input_poll(text, sizeof text);
    if (result == 0) return;   // still open
    g_editing = false;
    if (result == 1)
        store_tag(text, sizeof text);
    else
        log("clan tag: unchanged");
}

}  // namespace u2mp
