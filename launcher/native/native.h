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

/* Settings core (config.cpp): tables, and the per-game runtime settings file <user_dir>\probe-<game>.ini.
 * un_settings_table name: "ini_defaults" (key\tvalue), "fps_limits" (value\tlabel), "presets"
 *   (name\ttitle\tsummary\tdetail\tresolution\tupscale\tkey=value;...).
 * un_ini_read out: key\tvalue lines (defaults, then the file's keys). un_ini_write updates: key\tvalue lines; 0 ok. */
UN_API int un_settings_table(const char *name, char *out, int cap);
UN_API int un_ini_read(const wchar_t *user_dir, const char *game, char *out, int cap);
UN_API int un_ini_write(const wchar_t *user_dir, const char *game, const char *updates);
/* The launcher's settings file (launcher.json): read with its defaults, the Wine merge with the prefix's own file and
 * the version migrations (which also update the runtime settings files); out: the settings as JSON.
 * un_config_write: writes the JSON given (any layout) in the file's layout, safely; 0 ok, 1 not written, 2 bad JSON. */
UN_API int un_config_read(const wchar_t *config_file, const wchar_t *prefix_file, const wchar_t *user_dir, int wine,
                          char *out, int cap);
UN_API int un_config_write(const wchar_t *config_file, const char *config_json);

/* Game detection and system helpers (system.cpp). un_system_tables: GAMES, LANGUAGES, CHEATS as JSON.
 * un_system what: unix_path, windows_path, shared_config, slow_drive, is_game_dir, update_dir, app_version, voices,
 * launcher_version, recipe_version, u3mp_release, desktop, u3mp_default_data, sfo (param.sfo as JSON),
 * starter_version and find_update (the game starter: update path, then a message); 0 ok ("" = none), 1 unknown what,
 * 2 no answer (app_version: no readable APP_VER; an empty one is ""). */
UN_API int un_system_tables(char *out, int cap);
UN_API int un_wine(void);
UN_API int un_system(const char *what, const wchar_t *arg, char *out, int cap);
UN_API int un_pid_alive(unsigned long pid);

/* Controllers through SDL3 (gamepads.cpp). un_pad_init: loads SDL3 once (root: the launcher's folder); 1 when
 * controllers can be read. un_pad_state out: the number of controllers, the first one's style (playstation, xbox or
 * empty) and the buttons held (space-separated), one per line. */
UN_API int un_pad_init(const wchar_t *root);
UN_API int un_pad_state(char *out, int cap);

/* The game starter's code patches (patcher.cpp). un_apply_patches: the patch list as JSON (tools/game_patcher.py);
 * out: the log; 0 applied and written, 1 not written (last line: why), negative: out too small (nothing written).
 * un_pattern_find out: match offsets without overlaps (at most limit; 0 all), one per line; 1 unreadable. */
UN_API int un_apply_patches(const wchar_t *image, const char *patches_json, char *out, int cap);
UN_API int un_pattern_find(const wchar_t *file, const char *pattern, int limit, char *out, int cap);

#ifdef __cplusplus
}
#endif
