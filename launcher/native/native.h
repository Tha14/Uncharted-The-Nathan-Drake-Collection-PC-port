// native.h - uncharted_native.dll: the launcher's own native code (C++, this project's work), called from Python with
// ctypes. One function per feature; text in and out is UTF-8, one record per line, fields separated by tabs.
// A function that fills `out` returns 0 when it fits, or -needed (buffer size in bytes) to be called again.
#pragma once

#include <wchar.h>

#define UN_API __declspec(dllexport)
#define UN_VERSION 1

#define UN_ERR_NO_STEAM 1   /* no Steam account folder on this PC */
#define UN_ERR_BAD_FILE 2   /* a shortcuts.vdf that can't be read: out = "<file>: <reason>"; nothing was written */
#define UN_ERR_OS 3         /* a file couldn't be written: out = the reason */

#ifdef __cplusplus
extern "C" {
#endif

UN_API int un_version(void);

/* Steam ("Add to Steam", launcher/steam_library.py) */
UN_API int un_steam_running(void);
/* request lines: root\t<port folder>, starter\t<Uncharted Launcher.exe>, python\t<python.exe>, script\t<launcher script>,
 * per_game, game\t<key>\t<name> (one per game). out lines: <name>\t<exe>\t<start folder>\t<options> */
UN_API int un_steam_targets(const char *request, char *out, int cap);
/* request lines: target\t<name>\t<exe>\t<start>\t<options>, name\t<a name of ours>, icon\t<file>, game_dir\t<folder>,
 * account\t<userdata\id folder>\t<name> (optional: these accounts instead of every one found).
 * out lines: account\t<folder>\t<name>\t<shortcuts.vdf>\t<backup or empty>\t<added>\t<updated>\t<removed>\t<pictures>\t<ids> */
UN_API int un_steam_add(const char *request, char *out, int cap);
UN_API int un_steam_roundtrip(const char *path);

/* Freeze reports (the game starter's hang watchdog, tools/hang_report.py).
 * un_thread_report out: text lines, threads grouped by identical stacks (native frames as module+offset, game-code
 * frames as offsets from guest_base). un_minidump: 1 written, 0 failed. */
UN_API int un_thread_report(unsigned long pid, unsigned long long guest_base, unsigned long long guest_size, char *out,
                            int cap);
UN_API int un_minidump(unsigned long pid, const wchar_t *path);

/* Resource tracking for long sessions (leaks): a background thread writes <folder>\memory-<time>.csv every `seconds`
 * until the game ends; the newest 10 such files stay. out: the file's path. 0 started, 1 not started. */
UN_API int un_memory_monitor(unsigned long pid, const wchar_t *folder, double seconds, char *out, int cap);

#ifdef __cplusplus
}
#endif
