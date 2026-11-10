/* "Uncharted Launcher.exe", the program players open. It opens the launcher's window, launcher-qt\UnchartedLauncher.exe,
 * from the folder it sits in. Started with --play (a game started directly, as the per-game Steam entries do), or in
 * a package without the Qt window, it opens python\pythonw.exe launcher\uncharted_launcher.pyw with its own arguments
 * instead; and when the Qt window ends with an error within 15 s of opening (no graphics it can use, a file missing),
 * that one opens after it. It stays until what it opened has ended, so Steam counts the play time of the entry that
 * started it, and ends with the same exit code. Built by tools/package_windows.sh (clang, with starter.rc's icon). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#define TITLE L"Uncharted: The Nathan Drake Collection"
#define LONG_PATH 32768

static wchar_t folder[LONG_PATH], qt_window[LONG_PATH], python[LONG_PATH], command[LONG_PATH];

/* Runs `program` with `line` from the package folder until it ends; returns its exit code, or -1 when it could not
 * start. */
static long long run(const wchar_t *program, wchar_t *line)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION child;
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    if (!CreateProcessW(program, line, NULL, NULL, FALSE, 0, NULL, folder, &startup, &child))
        return -1;
    CloseHandle(child.hThread);
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hProcess);
    return code;
}

/* python\pythonw.exe launcher\uncharted_launcher.pyw with these arguments */
static long long run_python_launcher(const wchar_t *arguments)
{
    _snwprintf(command, LONG_PATH, L"\"%ls\" \"%ls\\launcher\\uncharted_launcher.pyw\"%ls%ls", python, folder,
               arguments[0] ? L" " : L"", arguments);
    command[LONG_PATH - 1] = 0;
    long long code = run(python, command);
    if (code < 0)
        MessageBoxW(NULL, L"The launcher's files were not found next to this program. Extract the whole package "
                          L"into one folder and start it from there.", TITLE, MB_OK | MB_ICONERROR);
    return code < 0 ? 1 : code;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE unused, PWSTR arguments, int show)
{
    (void)instance, (void)unused, (void)show;
    DWORD size = GetModuleFileNameW(NULL, folder, LONG_PATH);
    if (size == 0 || size >= LONG_PATH)
        return 1;
    wchar_t *name = wcsrchr(folder, L'\\');
    if (name)
        *name = 0;
    _snwprintf(qt_window, LONG_PATH, L"%ls\\launcher-qt\\UnchartedLauncher.exe", folder);
    _snwprintf(python, LONG_PATH, L"%ls\\python\\pythonw.exe", folder);
    const wchar_t *given = arguments ? arguments : L"";
    BOOL play = wcsstr(given, L"--play") != NULL;
    if (play || GetFileAttributesW(qt_window) == INVALID_FILE_ATTRIBUTES)
        return (int)run_python_launcher(given);
    _snwprintf(command, LONG_PATH, L"\"%ls\"", qt_window);
    ULONGLONG opened = GetTickCount64();
    long long code = run(qt_window, command);
    if (code != 0 && GetTickCount64() - opened < 15000)
        return (int)run_python_launcher(L"");
    return (int)code;
}
