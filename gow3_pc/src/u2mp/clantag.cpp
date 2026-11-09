// u2mp clan tag: the profile page's BUTTON_CLANTAG. PS3 0x441440 opened the on-screen keyboard and the profile
// page 0x4443f8 polled it every frame (event 6 stored the tag after the censor check). PC: the runtime's text
// input in the game window (the one sceImeDialog uses: keyboard Enter or Cross accepts, Esc or Circle cancels),
// started by the button and polled by the same page event. The tag is NetPlayerData +0x1a8 (8 bytes, shown by
// the label query 0x6609e0), saved and published with the rest of the profile (progression.cpp).
#include <cstdint>
#include <cstring>
#include <string>

#include "progression.h"
#include "u2mp_internal.h"

extern "C" {
int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int gow3gpu_text_input_poll(char *out_utf8, uint64_t size);
}

namespace u2mp {
namespace {

constexpr uint64_t kNpd = 0x11474f8, NPD_CLAN_TAG = 0x1a8;
constexpr int kMaxTag = 4;   // PS3 clan tags: up to 4 letters or digits
bool editing = false;

void store_tag(const char *text) {
    char out[8] = {};
    int n = 0;
    for (const char *p = text; *p && n < kMaxTag; ++p) {
        const char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[n++] = c;
        else if (c >= 'a' && c <= 'z') out[n++] = char(c - 'a' + 'A');
    }
    std::memcpy(image.at<char>(kNpd + NPD_CLAN_TAG), out, sizeof out);
    log("clan tag: [%.4s]", out);
    progression_publish();   // saved with the profile, sent to the party
}

}  // namespace

bool edit_clan_tag() {
    if (editing) return true;
    const char *slot = image.at<char>(kNpd + NPD_CLAN_TAG);
    const std::string current(slot, strnlen(slot, 8));
    if (!gow3gpu_text_input_begin(current.c_str(), "Clan Tag (up to 4 letters or digits; empty = none)")) {
        log("clan tag: no text input available");
        return false;
    }
    editing = true;
    log("clan tag: text input open");
    return true;
}

void clan_tag_frame() {
    if (!editing) return;
    char text[64] = {};
    const int state = gow3gpu_text_input_poll(text, sizeof text);
    if (!state) return;
    editing = false;
    if (state == 1) store_tag(text);
    else log("clan tag: unchanged");
}

}  // namespace u2mp
