// memstat.cpp - resource tracking for a running game, part of uncharted_native.dll. un_memory_monitor starts a
// background thread that writes one CSV row of the game's memory, handles, threads and GUI objects (and the PC's free
// memory) every few seconds to memory-<time>.csv in a folder, until the game ends, so a slow leak shows as a column
// that keeps rising over a long session. Everything is read from outside the game process.

#include "native.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

const char *const HEADER =
    "time,seconds,private_mib,working_set_mib,peak_working_set_mib,commit_mib,peak_commit_mib,handles,threads,"
    "gdi_objects,user_objects,pc_free_ram_mib,pc_free_commit_mib,pc_memory_load_pct\n";
const int KEEP_FILES = 10;

// one row (without time and seconds); false when the process has ended or can't be read
bool sample(HANDLE process, DWORD pid, char *row, size_t cap) {
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process, &exit_code) || exit_code != STILL_ACTIVE) return false;
    PROCESS_MEMORY_COUNTERS_EX mem{};
    mem.cb = sizeof mem;
    if (!K32GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&mem, sizeof mem)) return false;
    DWORD handles = 0;
    GetProcessHandleCount(process, &handles);
    DWORD gdi = GetGuiResources(process, GR_GDIOBJECTS), user = GetGuiResources(process, GR_USEROBJECTS);
    DWORD threads = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    for (BOOL more = snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == pid) { threads = pe.cntThreads; break; }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    MEMORYSTATUSEX sys{};
    sys.dwLength = sizeof sys;
    GlobalMemoryStatusEx(&sys);
    const double MiB = 1024.0 * 1024.0;
    std::snprintf(row, cap, "%.1f,%.1f,%.1f,%.1f,%.1f,%lu,%lu,%lu,%lu,%.1f,%.1f,%lu", mem.PrivateUsage / MiB,
                  mem.WorkingSetSize / MiB, mem.PeakWorkingSetSize / MiB, mem.PagefileUsage / MiB,
                  mem.PeakPagefileUsage / MiB, (unsigned long)handles, (unsigned long)threads, (unsigned long)gdi,
                  (unsigned long)user, sys.ullAvailPhys / MiB, sys.ullAvailPageFile / MiB, (unsigned long)sys.dwMemoryLoad);
    return true;
}

// keeps the newest KEEP_FILES - 1 memory-*.csv in the folder (the new one makes KEEP_FILES)
void trim_old(const std::wstring &folder) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\memory-*.csv").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do names.push_back(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end());   // memory-YYYYmmdd-HHMMSS.csv: name order is time order
    for (size_t i = 0; i + (KEEP_FILES - 1) < names.size(); ++i) DeleteFileW((folder + L"\\" + names[i]).c_str());
}

struct Job {
    HANDLE process;
    DWORD pid;
    HANDLE file;
    DWORD interval_ms;
};

DWORD WINAPI run(void *arg) {
    Job *job = (Job *)arg;
    DWORD written;
    WriteFile(job->file, HEADER, (DWORD)std::strlen(HEADER), &written, nullptr);
    ULONGLONG start = GetTickCount64();
    char row[400], line[512];
    for (;;) {
        if (!sample(job->process, job->pid, row, sizeof row)) break;
        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char clock[16];
        std::strftime(clock, sizeof clock, "%H:%M:%S", &local);
        int n = std::snprintf(line, sizeof line, "%s,%llu,%s\n", clock,
                              (unsigned long long)((GetTickCount64() - start) / 1000), row);
        WriteFile(job->file, line, (DWORD)n, &written, nullptr);
        FlushFileBuffers(job->file);
        if (WaitForSingleObject(job->process, job->interval_ms) != WAIT_TIMEOUT) break;   // the game ended
    }
    CloseHandle(job->file);
    CloseHandle(job->process);
    delete job;
    return 0;
}

}  // namespace

extern "C" {

// Starts tracking the process: writes <folder>\memory-<YYYYmmdd-HHMMSS>.csv every `seconds` until it ends (the newest
// 10 such files stay in the folder). out receives the file's path (UTF-8). Returns 0 started, 1 not started,
// -needed when out is too small.
UN_API int un_memory_monitor(unsigned long pid, const wchar_t *folder, double seconds, char *out, int cap) {
    if (seconds <= 0 || !folder) return 1;
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &local);
    std::wstring dir = folder, path = dir + L"\\memory-" + stamp + L".csv";
    int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > cap) return -n;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!process) return 1;
    trim_old(dir);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) { CloseHandle(process); return 1; }
    Job *job = new Job{process, pid, file, (DWORD)std::max(250.0, seconds * 1000.0)};
    HANDLE thread = CreateThread(nullptr, 0, run, job, 0, nullptr);
    if (!thread) { CloseHandle(file); CloseHandle(process); delete job; return 1; }
    CloseHandle(thread);
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, out, cap, nullptr, nullptr);
    return 0;
}

}  // extern "C"
