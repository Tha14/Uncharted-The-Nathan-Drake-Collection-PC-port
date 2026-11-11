// starter.cpp - "Uncharted Launcher.exe", the program players start. It runs the launcher window
// (launcher-qt\UnchartedLauncher.exe, beside it) with the same command line - --play u1 | u2 | u3 included, which the
// per-game shortcuts and Steam entries use - and keeps running until that window has closed, so Steam counts the
// entry's play time. Its exit code is the window's. Built by tools/package_windows.sh with starter.rc's icon.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace {

const wchar_t *const CAPTION = L"Uncharted: The Nathan Drake Collection";

std::wstring own_folder() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, &path[0], (DWORD)path.size());
    if (n == 0 || n >= path.size()) return L"";
    path.resize(n);
    return path.substr(0, path.find_last_of(L"\\/"));
}

// the command line's arguments after the program's own name, exactly as they were given
std::wstring arguments_given() {
    const wchar_t *line = GetCommandLineW();
    bool quoted = false;
    while (*line && (quoted || (*line != L' ' && *line != L'\t'))) {
        if (*line == L'"') quoted = !quoted;
        ++line;
    }
    while (*line == L' ' || *line == L'\t') ++line;
    return line;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::wstring folder = own_folder();
    const std::wstring window = folder + L"\\launcher-qt\\UnchartedLauncher.exe";
    if (folder.empty() || GetFileAttributesW(window.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr, L"The launcher window (launcher-qt\\UnchartedLauncher.exe) is not next to this program. "
                             L"Unpack the whole package into one folder and start the launcher from there.",
                    CAPTION, MB_OK | MB_ICONERROR);
        return 1;
    }
    const std::wstring given = arguments_given();
    std::wstring line = L"\"" + window + L"\"" + (given.empty() ? L"" : L" " + given);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(window.c_str(), &line[0], nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup, &child)) {
        MessageBoxW(nullptr, L"The launcher window could not be started. Reinstall the package, or check that your "
                             L"antivirus did not quarantine launcher-qt\\UnchartedLauncher.exe.", CAPTION, MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(child.hThread);
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hProcess);
    return (int)code;
}
