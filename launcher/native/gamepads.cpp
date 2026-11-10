// gamepads.cpp - controllers for the launcher's Controls page, part of uncharted_native.dll: through SDL3, the game's
// own library (gow3_pc/out in a release, MSYS2 in the source tree), which buttons are held and whether the first
// controller is a PlayStation one. The launcher has no SDL window: controller input arrives anyway (background events).

#include "native.h"

#include <cstdint>
#include <cstdlib>
#include <windows.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

typedef bool(__cdecl *SetHint)(const char *, const char *);
typedef bool(__cdecl *Init)(uint32_t);
typedef uint32_t *(__cdecl *GetGamepads)(int *);
typedef void *(__cdecl *OpenGamepad)(uint32_t);
typedef void(__cdecl *CloseGamepad)(void *);
typedef bool(__cdecl *GetButton)(void *, int);
typedef int16_t(__cdecl *GetAxis)(void *, int);
typedef int(__cdecl *GetType)(void *);
typedef void(__cdecl *Free)(void *);
typedef void(__cdecl *Void)(void);

struct Sdl {
    bool ready = false;
    GetGamepads get_gamepads = nullptr;
    OpenGamepad open = nullptr;
    CloseGamepad close = nullptr;
    GetButton button = nullptr;
    GetAxis axis = nullptr;
    GetType type = nullptr;
    Free free = nullptr;
    Void pump = nullptr, update = nullptr;
    std::map<uint32_t, void *> pads;   // joystick id -> open gamepad, in id order
    std::vector<uint32_t> order;       // ids in the order SDL listed them
} sdl;
bool tried = false;

// SDL_GamepadButton order (SDL3)
const char *const BUTTONS[] = {"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
                               "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2",
                               "paddle3", "paddle4", "touchpad"};

bool is_file(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring env(const wchar_t *name) {
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return (n && n < 2048) ? std::wstring(buf, n) : std::wstring();
}

std::wstring parent(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t cut = p.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return L".";
    return cut <= 2 ? p.substr(0, cut + 1) : p.substr(0, cut);
}

template <class F> F proc(HMODULE m, const char *name) { return (F)(void *)GetProcAddress(m, name); }

void load(const std::wstring &root) {
    std::wstring msys = env(L"MSYS2_ROOT");
    if (msys.empty()) msys = L"C:/msys64";
    for (const std::wstring &path : {root + L"\\gow3_pc\\out\\SDL3.dll", msys + L"\\clang64\\bin\\SDL3.dll",
                                     parent(root) + L"\\msys64\\clang64\\bin\\SDL3.dll"}) {
        if (!is_file(path)) continue;
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m) continue;
        SetHint hint = proc<SetHint>(m, "SDL_SetHint");
        Init init = proc<Init>(m, "SDL_Init");
        sdl.get_gamepads = proc<GetGamepads>(m, "SDL_GetGamepads");
        sdl.open = proc<OpenGamepad>(m, "SDL_OpenGamepad");
        sdl.close = proc<CloseGamepad>(m, "SDL_CloseGamepad");
        sdl.button = proc<GetButton>(m, "SDL_GetGamepadButton");
        sdl.axis = proc<GetAxis>(m, "SDL_GetGamepadAxis");
        sdl.type = proc<GetType>(m, "SDL_GetGamepadType");
        sdl.free = proc<Free>(m, "SDL_free");
        sdl.pump = proc<Void>(m, "SDL_PumpEvents");
        sdl.update = proc<Void>(m, "SDL_UpdateGamepads");
        bool all = hint && init && sdl.get_gamepads && sdl.open && sdl.close && sdl.button && sdl.axis && sdl.type &&
                   sdl.free && sdl.pump && sdl.update;
        if (all) {
            hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
            sdl.ready = init(0x2000);   // SDL_INIT_GAMEPAD
        }
        break;
    }
}

// the open controllers, in the order they were first seen
std::vector<void *> refresh() {
    std::vector<void *> out;
    if (!sdl.ready) return out;
    sdl.pump();
    int count = 0;
    uint32_t *ids = sdl.get_gamepads(&count);
    std::vector<uint32_t> current(ids ? ids : nullptr, ids ? ids + count : nullptr);
    if (ids) sdl.free(ids);
    for (uint32_t id : current)
        if (!sdl.pads.count(id)) {
            void *pad = sdl.open(id);
            if (pad) { sdl.pads[id] = pad; sdl.order.push_back(id); }
        }
    for (size_t i = 0; i < sdl.order.size();) {
        uint32_t id = sdl.order[i];
        bool still = false;
        for (uint32_t c : current) still = still || c == id;
        if (still) { ++i; continue; }
        sdl.close(sdl.pads[id]);
        sdl.pads.erase(id);
        sdl.order.erase(sdl.order.begin() + i);
    }
    sdl.update();
    for (uint32_t id : sdl.order) out.push_back(sdl.pads[id]);
    return out;
}

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

}  // namespace

extern "C" {

// loads SDL3 once (root: the launcher's folder); 1 when controllers can be read
UN_API int un_pad_init(const wchar_t *root) {
    if (!tried) {
        tried = true;
        load(root ? root : L".");
    }
    return sdl.ready ? 1 : 0;
}

// out: three lines: the number of controllers, the first one's style (playstation, xbox or empty), the buttons held
// on any of them (SDL order, then lefttrigger / righttrigger), space-separated
UN_API int un_pad_state(char *out, int cap) {
    std::vector<void *> pads = refresh();
    std::string style, held;
    if (!pads.empty()) {
        int t = sdl.type(pads[0]);
        style = (t == 4 || t == 5 || t == 6) ? "playstation" : "xbox";   // SDL_GAMEPAD_TYPE_PS3, PS4, PS5
    }
    std::vector<std::string> down;
    for (void *pad : pads) {
        for (int i = 0; i < (int)(sizeof BUTTONS / sizeof *BUTTONS); ++i)
            if (sdl.button(pad, i)) down.push_back(BUTTONS[i]);
        if (sdl.axis(pad, 4) > 16000) down.push_back("lefttrigger");
        if (sdl.axis(pad, 5) > 16000) down.push_back("righttrigger");
    }
    for (auto &b : down) held += (held.empty() ? "" : " ") + b;
    return give(std::to_string(pads.size()) + "\n" + style + "\n" + held, out, cap);
}

}  // extern "C"
