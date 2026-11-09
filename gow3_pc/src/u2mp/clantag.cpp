// u2mp clan tag: the profile page's BUTTON_CLANTAG (PS3 0x441440 opened the on-screen keyboard; the profile page
// 0x4443f8 stored the tag after the censor check). PC: a small Windows text box over the game. The tag is
// NetPlayerData +0x1a8 (8 bytes, shown by the label query 0x6609e0 with "%s"), saved and published with the
// rest of the profile (progression.cpp).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "progression.h"
#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t kNpd = 0x11474f8, NPD_CLAN_TAG = 0x1a8;
constexpr int kMaxTag = 4;   // PS3 clan tags: up to 4 letters or digits
constexpr int kIdEdit = 100;

struct Ask {
    std::wstring text;
    bool ok = false;
};

INT_PTR CALLBACK proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    Ask *a = reinterpret_cast<Ask *>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        a = reinterpret_cast<Ask *>(lp);
        HWND edit = GetDlgItem(dlg, kIdEdit);
        SendMessageW(edit, EM_LIMITTEXT, kMaxTag, 0);
        SetWindowTextW(edit, a->text.c_str());
        SendMessageW(edit, EM_SETSEL, 0, -1);
        SetWindowPos(dlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(dlg);
        SetFocus(edit);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && a) {
            wchar_t buf[16] = {};
            GetDlgItemTextW(dlg, kIdEdit, buf, 16);
            a->text = buf;
            a->ok = true;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

// An in-memory dialog template: DLGTEMPLATE, then each DLGITEMTEMPLATE on a DWORD boundary.
struct Builder {
    std::vector<WORD> w;
    void align() {
        if (w.size() % 2) w.push_back(0);
    }
    void str(const wchar_t *s) {
        for (; *s; ++s) w.push_back(WORD(*s));
        w.push_back(0);
    }
    void dword(DWORD v) {
        w.push_back(LOWORD(v));
        w.push_back(HIWORD(v));
    }
    void item(DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t *text) {
        align();
        dword(style | WS_CHILD | WS_VISIBLE);
        dword(0);
        w.push_back(WORD(x));
        w.push_back(WORD(y));
        w.push_back(WORD(cx));
        w.push_back(WORD(cy));
        w.push_back(id);
        w.push_back(0xffff);
        w.push_back(cls);
        str(text);
        w.push_back(0);   // no creation data
    }
};

HWND game_window() {
    struct Find { DWORD pid; HWND found; } f{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto *f = reinterpret_cast<Find *>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
                f->found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}

bool ask_tag(std::wstring &tag) {
    Builder b;
    b.dword(DS_SETFONT | DS_MODALFRAME | DS_CENTER | DS_SETFOREGROUND | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    b.dword(WS_EX_TOPMOST);
    b.w.push_back(4);        // controls
    b.w.push_back(0);
    b.w.push_back(0);
    b.w.push_back(180);
    b.w.push_back(70);
    b.w.push_back(0);        // no menu
    b.w.push_back(0);        // default class
    b.str(L"Clan Tag");
    b.w.push_back(10);       // font size
    b.str(L"Segoe UI");
    b.item(SS_LEFT, 8, 8, 164, 10, 0xffff, 0x0082, L"Clan tag (up to 4 letters or digits, empty = none):");
    b.item(ES_AUTOHSCROLL | ES_UPPERCASE | WS_BORDER | WS_TABSTOP, 8, 22, 164, 13, kIdEdit, 0x0081, L"");
    b.item(BS_DEFPUSHBUTTON | WS_TABSTOP, 68, 46, 50, 14, IDOK, 0x0080, L"OK");
    b.item(BS_PUSHBUTTON | WS_TABSTOP, 122, 46, 50, 14, IDCANCEL, 0x0080, L"Cancel");
    Ask a;
    a.text = tag;
    std::vector<DWORD> mem((b.w.size() * 2 + 3) / 4 + 1, 0);
    std::memcpy(mem.data(), b.w.data(), b.w.size() * 2);
    const INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(mem.data()),
                                              game_window(), proc, reinterpret_cast<LPARAM>(&a));
    if (r != IDOK || !a.ok) return false;
    tag = a.text;
    return true;
}

}  // namespace

// BUTTON_CLANTAG: edit the clan tag. False when the box could not be shown.
bool edit_clan_tag() {
    char *slot = image.at<char>(kNpd + NPD_CLAN_TAG);
    std::wstring tag;
    for (int i = 0; i < 8 && slot[i]; ++i) tag.push_back(wchar_t(static_cast<unsigned char>(slot[i])));
    if (!ask_tag(tag)) {
        log("clan tag: unchanged");
        return true;
    }
    char out[8] = {};
    int n = 0;
    for (wchar_t c : tag) {
        if (n >= kMaxTag) break;
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) out[n++] = char(c);
        else if (c >= L'a' && c <= L'z') out[n++] = char(c - L'a' + L'A');
    }
    std::memcpy(slot, out, sizeof out);
    log("clan tag: [%.4s]", out);
    progression_publish();   // saved with the profile, sent to the party
    return true;
}

}  // namespace u2mp
