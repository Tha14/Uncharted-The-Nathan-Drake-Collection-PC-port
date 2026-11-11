// apply.cpp builds apply_update.exe, the program that swaps in a downloaded launcher version (it replaced
// tools/apply_update.py). It is started from the unpacked copy of the NEW version, as
// update\staged\tools\apply_update.exe, once the running launcher is on its way out, and goes through these stages:
//   1. Wait until the launcher process given by --wait-pid has ended.
//   2. For every top-level program item in the package (anything but analysis\), move the installed one aside into
//      update\old and copy the new one into the root. Inside analysis\uncharted-user only the runtime's defaults file
//      is overwritten; the shader cache, logs, settings and saves are left alone.
//   3. If anything fails, move every item that was set aside back again so the previous version still runs.
//   4. Bring across, into the fresh folders, whatever the old folders held that the package does not ship. That covers
//      the player's own additions: ReShade (ReShade.ini, presets, reshade-shaders\ beside Uncharted.exe), a game folder
//      placed under gow3_pc\, personal notes and so on. The only things left out are files that the previous package
//      itself shipped (recorded in installed-files.txt) and that the new package has dropped (#134); this holds in
//      folders that disappeared from the package too. Program folders that the new package no longer contains at all
//      (an old python\, say) lose those files and are deleted once empty.
//   5. Launch "Uncharted Launcher.exe"; that new launcher deletes update\ on startup.
// usage: apply_update.exe --root <package folder> --wait-pid <launcher pid> [--no-restart] [--quiet]
// The exe is statically linked, so it does not depend on anything in the folder it is about to replace.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

const wchar_t *const TITLE = L"Uncharted: The Nathan Drake Collection";
const wchar_t *const DEFAULTS[] = {L"analysis/uncharted-user/disabled-optimizations.txt"};
// Written at the end of every update: one line per file the package shipped, relative to the root with "/" between
// folders. When the following update runs, files listed here that its package lacks are left behind, and all other
// files are kept.
const wchar_t *const MANIFEST = L"installed-files.txt";

struct Error { std::wstring message; };

std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
void write_std(DWORD which, const std::wstring &text) {
    std::string o;
    for (char c : utf8(text + L"\n")) { if (c == '\n') o += "\r\n"; else o += c; }
    HANDLE h = GetStdHandle(which);
    DWORD put = 0;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, o.data(), (DWORD)o.size(), &put, nullptr);
}
void print(const std::wstring &text) { write_std(STD_OUTPUT_HANDLE, text); }

// Formats a system error the way Python prints an OSError: "[WinError n] text: 'a' -> 'b'"
std::wstring os_error(DWORD code, const std::wstring &a = L"", const std::wstring &b = L"") {
    wchar_t *text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                   0, (LPWSTR)&text, 0, nullptr);
    std::wstring t = text ? text : L"error";
    if (text) LocalFree(text);
    while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ' || t.back() == L'.')) t.pop_back();
    std::wstring o = L"[WinError " + std::to_wstring(code) + L"] " + t;
    if (!a.empty()) o += L": '" + a + L"'";
    if (!b.empty()) o += L" -> '" + b + L"'";
    return o;
}

// ------------------------------------------------------------------------------------------------ file system helpers

std::wstring join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}
std::wstring native_rel(std::wstring rel) {
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    return rel;
}
DWORD attrs(const std::wstring &p) { return GetFileAttributesW(p.c_str()); }
bool exists(const std::wstring &p) {   // like Path.exists: a link counts only if its target is there
    const DWORD a = attrs(p);
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}
bool is_link(const std::wstring &p) {
    const DWORD a = attrs(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool is_dir(const std::wstring &p) { return exists(p) && (attrs(p) & FILE_ATTRIBUTE_DIRECTORY); }
bool is_file(const std::wstring &p) { return exists(p) && !(attrs(p) & FILE_ATTRIBUTE_DIRECTORY); }

// Names inside a folder in the order sorted(Path.iterdir()) would produce
std::vector<std::wstring> entries(const std::wstring &folder) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(folder, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) throw Error{os_error(GetLastError(), folder)};
    do {
        const std::wstring n = fd.cFileName;
        if (n != L"." && n != L"..") names.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // case-insensitive, because pathlib lower-cases Windows paths before it compares them
    std::stable_sort(names.begin(), names.end(), [](const std::wstring &a, const std::wstring &b) {
        std::wstring x = a, y = b;
        CharLowerBuffW(&x[0], (DWORD)x.size());
        CharLowerBuffW(&y[0], (DWORD)y.size());
        return x < y;
    });
    return names;
}

std::wstring full_path(const std::wstring &p) {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetFullPathNameW(p.c_str(), 32768, buf.data(), nullptr);
    std::wstring f = n && n < 32768 ? std::wstring(buf.data(), n) : p;
    while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/')) f.pop_back();
    return f;
}

// Repeats `action` every half second until it works or `seconds` run out. Right after a process ends its files may
// still be locked briefly, and a virus scanner can hold them as well.
template <typename F> void retry(F action, int seconds = 15) {
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)seconds * 1000;
    while (true) {
        try {
            action();
            return;
        } catch (const Error &) {
            if (GetTickCount64() > deadline) throw;
            Sleep(500);
        }
    }
}

void rename(const std::wstring &from, const std::wstring &to) {
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) throw Error{os_error(GetLastError(), from, to)};
}
void make_dirs(const std::wstring &p) {
    if (is_dir(p)) return;
    const size_t cut = p.find_last_of(L'\\');
    if (cut != std::wstring::npos && cut > 2) make_dirs(p.substr(0, cut));
    if (!CreateDirectoryW(p.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw Error{os_error(GetLastError(), p)};
}

// Recursive copy that keeps file times, as shutil.copytree / copy2 do
void copy_item(const std::wstring &source, const std::wstring &target) {
    if (is_dir(source)) {
        if (!CreateDirectoryW(target.c_str(), nullptr)) throw Error{os_error(GetLastError(), target)};
        for (const std::wstring &n : entries(source)) copy_item(join(source, n), join(target, n));
    } else if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
        throw Error{os_error(GetLastError(), source, target)};
    }
}

// Deletes a file or a whole tree (shutil.rmtree / unlink); a link is removed without touching what it points to
void remove_item(const std::wstring &path) {
    const DWORD a = attrs(path);
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT))
        for (const std::wstring &n : entries(path)) remove_item(join(path, n));
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    const BOOL ok = (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str());
    if (!ok) throw Error{os_error(GetLastError(), path)};
}

// ------------------------------------------------------------------------------------------------ installing

std::wstring g_root, g_staged;
HWND g_window = nullptr, g_label = nullptr;
HFONT g_font = nullptr;
HBRUSH g_brush = nullptr;

void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void status(const std::wstring &text) {
    print(text);
    if (!g_label) return;
    SetWindowTextW(g_label, text.c_str());
    // resize the window around the text, padded 28 x 22 like the old Tk label
    HDC dc = GetDC(g_label);
    HGDIOBJ previous = SelectObject(dc, g_font);
    RECT r{0, 0, 0, 0};
    DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, previous);
    ReleaseDC(g_label, dc);
    const UINT dpi = GetDpiForWindow(g_window);
    const int padx = MulDiv(28, (int)dpi, 96), pady = MulDiv(22, (int)dpi, 96);
    RECT frame{0, 0, r.right + 2 * padx, r.bottom + 2 * pady};
    AdjustWindowRectExForDpi(&frame, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, dpi);
    SetWindowPos(g_window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER);
    MoveWindow(g_label, padx, pady, r.right, r.bottom, TRUE);
    InvalidateRect(g_window, nullptr, TRUE);
    UpdateWindow(g_window);
    pump();
}

LRESULT CALLBACK window_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CTLCOLORSTATIC) {
        SetTextColor((HDC)wp, RGB(0xef, 0xe6, 0xd8));
        SetBkColor((HDC)wp, RGB(0x14, 0x11, 0x0d));
        return (LRESULT)g_brush;
    }
    if (m == WM_CLOSE) return 0;   // closing is ignored: the install has to finish
    return DefWindowProcW(w, m, wp, lp);
}

void open_window() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_brush = CreateSolidBrush(RGB(0x14, 0x11, 0x0d));
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = g_brush;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"UnchartedUpdate";
    RegisterClassW(&wc);
    g_window = CreateWindowExW(0, wc.lpszClassName, TITLE, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                               CW_USEDEFAULT, 400, 120, nullptr, nullptr, wc.hInstance, nullptr);
    const UINT dpi = GetDpiForWindow(g_window);
    g_font = CreateFontW(-MulDiv(11, (int)dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, g_window, nullptr,
                              wc.hInstance, nullptr);
    SendMessageW(g_label, WM_SETFONT, (WPARAM)g_font, TRUE);
    ShowWindow(g_window, SW_SHOW);
    status(L"The launcher is being updated...");
}

bool wait_for_exit(DWORD pid, int seconds) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return true;   // the process has ended already
    // keep pumping messages so the status window does not freeze during the wait
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)seconds * 1000;
    bool gone = false;
    while (!gone) {
        const ULONGLONG now = GetTickCount64();
        if (now > deadline) break;
        const DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, (DWORD)std::min<ULONGLONG>(deadline - now, 100), QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) gone = true;
        else pump();
    }
    CloseHandle(h);
    return gone;
}

// Collects every program file the new package will put in the root, as root-relative paths with "/" separators.
void package_files(const std::wstring &folder, const std::wstring &rel, std::vector<std::wstring> &out) {
    for (const std::wstring &n : entries(folder)) {
        const std::wstring path = join(folder, n), r = rel.empty() ? n : rel + L"/" + n;
        if (rel.empty() && (n == L"analysis" || n == L"update")) continue;
        if (is_dir(path)) package_files(path, r, out);
        else if (is_file(path)) out.push_back(r);
    }
}

std::set<std::wstring> read_manifest() {
    std::set<std::wstring> lines;
    HANDLE h = CreateFileW(join(g_root, MANIFEST).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return lines;   // versions up to 0.5.0 never wrote one: keep every file the package lacks
    std::string data;
    char buf[1 << 16];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) data.append(buf, got);
    CloseHandle(h);
    const std::wstring text = widen(data);
    size_t a = 0;
    while (a <= text.size()) {
        size_t b = text.find_first_of(L"\r\n", a);
        if (b == std::wstring::npos) b = text.size();
        std::wstring line = text.substr(a, b - a);
        while (!line.empty() && iswspace(line.back())) line.pop_back();
        while (!line.empty() && iswspace(line.front())) line.erase(line.begin());
        if (!line.empty()) lines.insert(line);
        a = b + 1;
    }
    return lines;
}

// True if some file below `rel` came with the previous package and is absent from the new one.
bool holds_dropped(const std::set<std::wstring> &dropped, const std::wstring &rel) {
    const std::wstring prefix = rel + L"/";
    auto it = dropped.lower_bound(prefix);
    return it != dropped.end() && it->compare(0, prefix.size(), prefix) == 0;
}

// Detects a __pycache__ folder. Python creates these beside its scripts on its own; they are neither part of any
// package nor something the player made, so they are not kept and end up in update\old instead.
bool python_cache(const std::wstring &name) {
    return CompareStringOrdinal(name.c_str(), (int)name.size(), L"__pycache__", -1, TRUE) == CSTR_EQUAL;
}

// Brings into `fresh` everything that `old`, the set-aside copy of the same program folder, has and `fresh` lacks.
// A subfolder that `fresh` lacks moves over in one rename (same drive, so it is instant even for a whole game folder)
// unless it contains files the new package dropped. Folders found on both sides, and folders with dropped files, are
// walked entry by entry instead, and the previous package's files that the new package no longer ships are skipped.
void carry_over(const std::wstring &old, const std::wstring &fresh, const std::wstring &relative,
                const std::set<std::wstring> &dropped, std::vector<std::wstring> &kept) {
    for (const std::wstring &n : entries(old)) {
        const std::wstring item = join(old, n), target = join(fresh, n);
        const std::wstring rel = relative.empty() ? n : relative + L"/" + n;
        const bool real_dir = is_dir(item) && !is_link(item);
        if (real_dir && python_cache(n)) {
            continue;   // __pycache__ is not kept
        } else if (real_dir && is_dir(target)) {
            carry_over(item, target, rel, dropped, kept);
        } else if (exists(target) || is_link(target)) {
            continue;   // the new package supplies this one
        } else if (is_file(item) && dropped.count(rel)) {
            continue;   // shipped by the previous package, no longer by this one
        } else if (real_dir && holds_dropped(dropped, rel)) {
            // folder missing from the new package: recreate it for the player's files, without the dropped ones
            make_dirs(target);
            carry_over(item, target, rel, dropped, kept);
            if (entries(target).empty()) RemoveDirectoryW(target.c_str());
        } else {
            rename(item, target);
            kept.push_back(rel);
        }
    }
}

// Cleans up a program item that the new package does not contain any more (for example a leftover python\ folder).
// Files the previous package shipped there are moved into update\old, files the player added stay, and any folder that
// ends up empty is deleted. Returns true when the item no longer exists afterwards.
bool prune(const std::wstring &path, const std::wstring &rel, const std::wstring &old, const std::set<std::wstring> &dropped,
           std::vector<std::wstring> &problems) {
    if (is_link(path)) return false;
    const std::wstring name = rel.substr(rel.find_last_of(L'/') + 1);
    if (is_dir(path) && python_cache(name)) {   // a __pycache__ folder is moved away in one piece
        try {
            const std::wstring target = join(old, native_rel(rel));
            make_dirs(target.substr(0, target.find_last_of(L'\\')));
            retry([&] { rename(path, target); }, 5);
            return true;
        } catch (const Error &e) {
            problems.push_back(rel + L" (" + e.message + L")");
            return false;
        }
    }
    if (is_dir(path)) {
        bool empty = true;
        for (const std::wstring &n : entries(path)) {
            if (!prune(join(path, n), rel + L"/" + n, old, dropped, problems)) empty = false;
        }
        if (empty && !RemoveDirectoryW(path.c_str())) {
            problems.push_back(rel + L" (" + os_error(GetLastError(), path) + L")");
            return false;
        }
        return empty;
    }
    if (!dropped.count(rel)) return false;   // not from the previous package, so it belongs to the player
    try {
        const std::wstring target = join(old, native_rel(rel));
        make_dirs(target.substr(0, target.find_last_of(L'\\')));
        retry([&] { rename(path, target); }, 5);
        return true;
    } catch (const Error &e) {
        problems.push_back(rel + L" (" + e.message + L")");
        return false;
    }
}

std::wstring timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf(b, 32, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

// The launcher wipes update\ when it starts, so this moves update\old up into the root under a dated name. Returns
// the folder's final location (unchanged if the move failed).
std::wstring preserve(const std::wstring &old) {
    const std::wstring target = join(g_root, L"update-kept-" + timestamp() + L"-old");
    try {
        retry([&] { rename(old, target); });
        return target;
    } catch (const Error &) {
        return old;
    }
}

void apply() {
    const std::wstring old = join(join(g_root, L"update"), L"old");
    retry([&] { if (exists(old)) remove_item(old); });
    make_dirs(old);
    std::vector<std::wstring> new_files;
    package_files(g_staged, L"", new_files);
    std::sort(new_files.begin(), new_files.end());
    std::set<std::wstring> dropped = read_manifest();
    for (const std::wstring &f : new_files) dropped.erase(f);
    std::vector<std::wstring> items;
    for (const std::wstring &n : entries(g_staged))
        if (n != L"analysis" && n != L"update") items.push_back(n);
    std::sort(items.begin(), items.end());   // plain string order, so upper and lower case differ
    std::vector<std::wstring> moved, added;
    try {
        for (const std::wstring &name : items) {
            status(L"Copying " + name + L"...");
            const std::wstring current = join(g_root, name);
            if (exists(current)) {
                retry([&] { rename(current, join(old, name)); });
                moved.push_back(name);
            } else {
                added.push_back(name);
            }
            copy_item(join(g_staged, name), current);
        }
        for (const wchar_t *relative : DEFAULTS) {
            const std::wstring rel = native_rel(relative), source = join(g_staged, rel);
            if (!is_file(source)) continue;
            const std::wstring target = join(g_root, rel);
            make_dirs(target.substr(0, target.find_last_of(L'\\')));
            const std::wstring saved = join(old, rel.substr(rel.find_last_of(L'\\') + 1));
            if (exists(target)) {
                if (!CopyFileW(target.c_str(), saved.c_str(), FALSE)) throw Error{os_error(GetLastError(), target, saved)};
            }
            if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) throw Error{os_error(GetLastError(), source, target)};
        }
    } catch (const Error &) {
        status(L"Update failed, putting the previous version back...");
        // Each undo step runs even if one before it failed (a virus scanner may be holding a file). Giving up partway
        // would strand folders in update\old and leave an install that does not start.
        std::vector<std::wstring> stuck;
        std::vector<std::wstring> both = moved;
        both.insert(both.end(), added.begin(), added.end());
        for (const std::wstring &name : both) {
            const bool was_added = std::find(added.begin(), added.end(), name) != added.end();
            if ((exists(join(g_root, name)) && exists(join(old, name))) || was_added) {
                try {
                    retry([&] { remove_item(join(g_root, name)); });
                } catch (const Error &e) {
                    stuck.push_back(name + L" (could not remove: " + e.message + L")");
                }
            }
        }
        for (const std::wstring &name : moved) {
            try {
                retry([&] { rename(join(old, name), join(g_root, name)); });
            } catch (const Error &e) {
                stuck.push_back(name + L" (could not restore: " + e.message + L")");
            }
        }
        for (const wchar_t *relative : DEFAULTS) {
            const std::wstring rel = native_rel(relative), saved = join(old, rel.substr(rel.find_last_of(L'\\') + 1));
            if (is_file(saved) && !CopyFileW(saved.c_str(), join(g_root, rel).c_str(), FALSE))
                stuck.push_back(std::wstring(relative) + L" (" + os_error(GetLastError(), saved, join(g_root, rel)) + L")");
        }
        if (!stuck.empty()) {
            // The next launcher start wipes update\old, so get the leftovers out of there first.
            const std::wstring kept_at = preserve(old);
            std::wstring list;
            for (size_t k = 0; k < stuck.size(); ++k) list += (k ? L", " : L"") + stuck[k];
            throw Error{L"Not everything of the previous version could be put back; move these from " + kept_at +
                        L" back yourself: " + list};
        }
        throw;
    }
    // The new version is in place. Now the player's own files return to the new folders. Anything that refuses to move
    // goes to a separate dated folder next to them, since update\ is wiped on the launcher's next start.
    std::vector<std::wstring> kept, left_in_old;
    status(L"Carrying over your own files...");
    for (const std::wstring &name : moved) {
        if (!(is_dir(join(old, name)) && is_dir(join(g_root, name)))) continue;
        try {
            carry_over(join(old, name), join(g_root, name), name, dropped, kept);
        } catch (const Error &e) {
            const std::wstring rescue = join(g_root, L"update-kept-" + timestamp());
            status(L"Files of yours in " + name + L" could not be returned (" + e.message + L"); you can find them in " +
                   rescue.substr(rescue.find_last_of(L'\\') + 1) + L".");
            try {
                if (!CreateDirectoryW(rescue.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                    throw Error{os_error(GetLastError(), rescue)};
                retry([&] { rename(join(old, name), join(rescue, name)); });
            } catch (const Error &rescue_error) {
                // Only mention it: the update itself succeeded and must not be reported as failed.
                left_in_old.push_back(name + L" (" + rescue_error.message + L")");
            }
        }
    }
    if (!left_in_old.empty()) {
        std::wstring list;
        for (size_t k = 0; k < left_in_old.size(); ++k) list += (k ? L", " : L"") + left_in_old[k];
        status(L"A few of your old files are left in " + preserve(old) + L": " + list);
    }
    // Clean up top-level items that are not in the new package any more (e.g. python\ after the launcher stopped
    // needing Python): remove what the previous package shipped there, keep what the player put there.
    std::vector<std::wstring> problems;
    for (const std::wstring &name : entries(g_root)) {
        if (name == L"analysis" || name == L"update" || name.rfind(L"update-kept-", 0) == 0) continue;
        if (std::find(items.begin(), items.end(), name) != items.end()) continue;
        if (!dropped.count(name) && !holds_dropped(dropped, name)) continue;
        status(L"Removing " + name + L" (no longer part of the launcher)...");
        prune(join(g_root, name), name, old, dropped, problems);
    }
    for (const std::wstring &p : problems) print(L"Not removed: " + p);
    for (const std::wstring &rel : kept) print(L"Kept: " + rel);
    std::string manifest;
    for (const std::wstring &f : new_files) manifest += utf8(f) + "\r\n";
    HANDLE h = CreateFileW(join(g_root, MANIFEST).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD put = 0;
    if (h == INVALID_HANDLE_VALUE || !WriteFile(h, manifest.data(), (DWORD)manifest.size(), &put, nullptr))
        write_std(STD_ERROR_HANDLE, std::wstring(L"Failed to write ") + MANIFEST + L": " + os_error(GetLastError()));
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

const wchar_t *const USAGE = L"usage: apply_update.exe [-h] --root ROOT [--wait-pid WAIT_PID] [--no-restart] [--quiet]";

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring root;
    DWORD wait_pid = 0;
    bool restart = true, quiet = false, have_root = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i], value;
        const size_t eq = a.find(L'=');
        bool has_value = false;
        if (a.rfind(L"--", 0) == 0 && eq != std::wstring::npos) {
            value = a.substr(eq + 1);
            a = a.substr(0, eq);
            has_value = true;
        }
        auto take = [&]() -> std::wstring {
            if (has_value) return value;
            if (i + 1 < argc) return argv[++i];
            write_std(STD_ERROR_HANDLE, std::wstring(USAGE) + L"\napply_update.exe: error: argument " + a + L": expected one argument");
            ExitProcess(2);
        };
        if (a == L"--root") { root = take(); have_root = true; }
        else if (a == L"--wait-pid") wait_pid = (DWORD)_wtoi(take().c_str());
        else if (a == L"--no-restart") restart = false;
        else if (a == L"--quiet") quiet = true;
        else if (a == L"-h" || a == L"--help") { print(USAGE); return 0; }
        else {
            write_std(STD_ERROR_HANDLE, std::wstring(USAGE) + L"\napply_update.exe: error: unrecognized arguments: " + argv[i]);
            return 2;
        }
    }
    if (!have_root) {
        write_std(STD_ERROR_HANDLE, std::wstring(USAGE) + L"\napply_update.exe: error: the following arguments are required: --root");
        return 2;
    }
    g_root = full_path(root);
    {
        std::vector<wchar_t> self(32768);
        GetModuleFileNameW(nullptr, self.data(), 32768);
        std::wstring s = self.data();
        s = s.substr(0, s.find_last_of(L'\\'));   // the exe's own folder, tools; its parent is the staged package
        g_staged = s.substr(0, s.find_last_of(L'\\'));
    }
    if (!quiet) open_window();
    bool ok = true;
    try {
        if (wait_pid && !wait_for_exit(wait_pid, 60)) throw Error{L"The launcher did not exit in time."};
        apply();
        status(L"The update is installed.");
    } catch (const Error &e) {
        ok = false;
        const std::wstring message = L"The update failed to install; the previous version stays in place.\n\n" + e.message +
                                     L"\n\nIf the game is running, close it and try once more, or get the new version from the "
                                     L"releases page.";
        write_std(STD_ERROR_HANDLE, message);
        if (g_window) MessageBoxW(g_window, message.c_str(), TITLE, MB_OK | MB_ICONERROR);
    }
    if (g_window) DestroyWindow(g_window);
    if (restart) {
        const std::wstring starter = join(g_root, L"Uncharted Launcher.exe");
        std::wstring cmd = L"\"" + starter + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(starter.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_root.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            write_std(STD_ERROR_HANDLE, L"Failed to start " + starter + L": " + os_error(GetLastError()));
        }
    }
    return ok ? 0 : 1;
}
