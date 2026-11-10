// bridge.cpp - pythonw_bridge.exe: shipped as python\pythonw.exe once the launcher needs no Python, so that every older
// launcher can still update. Since 0.1.0 a launcher installs an update by starting the NEW package's
//   python\pythonw.exe -I tools\apply_update.py --root <folder> --wait-pid <pid>
// (launcher/updater.py start_apply). This program takes that command and runs the package's own installer,
// tools\apply_update.exe, with the same arguments, and ends with its exit code. Anything else it is asked to run is
// refused with a message: there is no Python here.

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
    if (i >= argc || !(ends_with_ci(argv[i], L"\\apply_update.py") || ends_with_ci(argv[i], L"/apply_update.py") ||
                       CompareStringOrdinal(argv[i], -1, L"apply_update.py", -1, TRUE) == CSTR_EQUAL)) {
        MessageBoxW(nullptr,
                    L"This launcher no longer uses Python: python\\pythonw.exe only installs launcher updates.\n\n"
                    L"Start \"Uncharted Launcher.exe\" instead.",
                    L"Uncharted: The Nathan Drake Collection", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    // tools\apply_update.exe next to the script it was asked to run
    std::wstring script = argv[i];
    std::vector<wchar_t> full(32768);
    DWORD n = GetFullPathNameW(script.c_str(), 32768, full.data(), nullptr);
    if (n && n < 32768) script.assign(full.data(), n);
    const size_t cut = script.find_last_of(L"\\/");
    const std::wstring installer = (cut == std::wstring::npos ? std::wstring(L".") : script.substr(0, cut)) + L"\\apply_update.exe";
    std::wstring cmd = quote(installer);
    for (int k = i + 1; k < argc; ++k) cmd += L" " + quote(argv[k]);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(installer.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        const std::wstring message = L"The update's installer could not be started:\n" + installer;
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
