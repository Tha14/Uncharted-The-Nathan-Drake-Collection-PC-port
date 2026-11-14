// bridge.cpp - pythonw_bridge.exe: shipped as python\pythonw.exe, so that every older launcher can still update and
// old shortcuts still open the launcher, with no Python involved. Two commands are understood:
//   python\pythonw.exe -I tools\apply_update.py --root <folder> --wait-pid <pid>
//     how launchers up to 0.10.63 install an update (their updater started the NEW package's pythonw). This runs the
//     package's own installer, tools\apply_update.exe, with the same arguments, and ends with its exit code.
//   python\pythonw.exe launcher\uncharted_launcher.pyw [arguments]
//     what old Steam and desktop shortcuts start. This runs "Uncharted Launcher.exe" in the package folder with the
//     same arguments and ends with its exit code.
// The package still carries tools\apply_update.py and launcher\uncharted_launcher.pyw, but only as placeholder text
// files (older launchers check that they exist); nothing reads them. Anything else is refused with a message.

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

// subprocess.list2cmdline
std::wstring quote(const std::wstring &arg) {
    std::wstring result, bs;
    const bool q = arg.find(L' ') != std::wstring::npos || arg.find(L'\t') != std::wstring::npos || arg.empty();
    if (q) result += L'"';
    for (wchar_t c : arg) {
        if (c == L'\\') bs += c;
        else if (c == L'"') { result += std::wstring(bs.size() * 2, L'\\'); bs.clear(); result += L"\\\""; }
        else { result += bs; bs.clear(); result += c; }
    }
    result += bs;
    if (q) { result += bs; result += L'"'; }
    return result;
}

bool ends_with_ci(const std::wstring &s, const std::wstring &tail) {
    return s.size() >= tail.size() &&
           CompareStringOrdinal(s.c_str() + s.size() - tail.size(), (int)tail.size(), tail.c_str(), (int)tail.size(), TRUE) == CSTR_EQUAL;
}

// is `path` the file `name` (alone, or after a \ or /)?
bool named(const std::wstring &path, const std::wstring &name) {
    return CompareStringOrdinal(path.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL || ends_with_ci(path, L"\\" + name) ||
           ends_with_ci(path, L"/" + name);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // Python's own options come first (-I, -u, -E, -s, -B, ...; -X and -W take a value); the script follows.
    int i = 1;
    while (i < argc && argv[i][0] == L'-' && argv[i][1]) {
        const std::wstring o = argv[i];
        if (o == L"-c" || o == L"-m") { i = argc; break; }   // code or a module: nothing this bridge runs
        if ((o == L"-X" || o == L"-W") && i + 1 < argc) ++i;
        ++i;
    }
    const bool update = i < argc && named(argv[i], L"apply_update.py");
    const bool open = i < argc && named(argv[i], L"uncharted_launcher.pyw");
    if (!update && !open) {
        MessageBoxW(nullptr,
                    L"This launcher does not use Python: python\\pythonw.exe only installs launcher updates.\n\n"
                    L"Start \"Uncharted Launcher.exe\" instead.",
                    L"Uncharted: The Nathan Drake Collection", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    // the folder of the script it was asked to run: tools\ (the installer is beside it) or launcher\ (the package
    // folder, with "Uncharted Launcher.exe", is one up)
    std::wstring script = argv[i];
    std::vector<wchar_t> full(32768);
    DWORD n = GetFullPathNameW(script.c_str(), 32768, full.data(), nullptr);
    if (n && n < 32768) script.assign(full.data(), n);
    size_t cut = script.find_last_of(L"\\/");
    std::wstring folder = cut == std::wstring::npos ? std::wstring(L".") : script.substr(0, cut);
    if (open) {
        cut = folder.find_last_of(L"\\/");
        folder = cut == std::wstring::npos ? std::wstring(L".") : folder.substr(0, cut);
    }
    const std::wstring program = folder + (update ? L"\\apply_update.exe" : L"\\Uncharted Launcher.exe");
    std::wstring cmd = quote(program);
    for (int k = i + 1; k < argc; ++k) cmd += L" " + quote(argv[k]);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(program.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        const std::wstring message = (update ? L"The update's installer could not be started:\n" : L"The launcher could not be started:\n") + program;
        MessageBoxW(nullptr, message.c_str(), L"Uncharted: The Nathan Drake Collection", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
