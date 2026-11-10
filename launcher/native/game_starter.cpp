// game_starter.cpp - launcher\native\game_starter.exe: the game starter PLAY runs (launcher/native/play.cpp
// un_play_start), with the same command line tools/probe_uncharted.py takes:
//   game_starter.exe --boot --seconds 0 --executable <eboot.bin | big2-ps4_Shipping.elf | big3-ps4_Shipping.elf>
//                    --game <game folder>
// All of the work is un_start_game in uncharted_native.dll (start.cpp), next to this program; this only hands it the
// launcher's folder (two folders up) and the command line, and ends with its exit code. Without Python.
#include <windows.h>

#include <string>

namespace {

std::string utf8(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// a JSON string (the DLL's command line is a JSON array of texts)
std::string quoted(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) {
            char b[8];
            wsprintfA(b, "\\u%04x", c);
            out += b;
        } else out += (char)c;
    }
    return out + "\"";
}

void say(const char *text) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, (DWORD)lstrlenA(text), &written, nullptr);
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    wchar_t self[32768];
    const DWORD n = GetModuleFileNameW(nullptr, self, 32768);
    if (!n || n >= 32768) { say("game_starter: its own path is unknown\n"); return 1; }
    std::wstring folder(self, n);
    folder = folder.substr(0, folder.find_last_of(L"\\/"));          // launcher\native
    const std::wstring dll = folder + L"\\uncharted_native.dll";
    const std::wstring launcher = folder.substr(0, folder.find_last_of(L"\\/"));    // launcher
    const std::wstring root = launcher.substr(0, launcher.find_last_of(L"\\/"));    // the launcher's folder
    HMODULE library = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library) { say("game_starter: launcher\\native\\uncharted_native.dll could not be loaded\n"); return 1; }
    using StartFn = long long (*)(const wchar_t *, const char *);
    auto start = reinterpret_cast<StartFn>(reinterpret_cast<void *>(GetProcAddress(library, "un_start_game")));
    if (!start) { say("game_starter: uncharted_native.dll has no un_start_game\n"); return 1; }
    std::string args = "[";
    for (int i = 1; i < argc; ++i) args += (i > 1 ? "," : "") + quoted(utf8(argv[i]));
    args += "]";
    return (int)start(root.c_str(), args.c_str());
}
