// importer/u3import.cpp: u3import.exe, Uncharted 3 multiplayer prepared on this PC from the player's own game copies.
// Run by the launcher (MULTIPLAYER tab -> Import) in the background; can also be run by hand:
//
//   u3import --ps4 <PS4 Collection folder> --ps3-u3 <folder> [--ps3-u3 <folder> ...] [--ps3-u2 <folder>]
//            --data <multiplayer data folder> --port <port root> [--ps3-elf <EBOOT.elf>] [--image-only] [--dns <IPv4>]
//
//   0. PS3 game data (BCES01670DATA2): the player's own, or with --dns downloaded by the online program
//      (online/uncharted_online.exe --fetch-game-data)                  -> <data>/ps3-data2/BCES01670DATA2/USRDIR
//   1. multiplayer image (mpimage.cpp; every stage checked by sha256)   -> <data>/image/boot-linked.bin
//   2. overlays: the overlay recipe (recipe.cpp, steps in recipe/steps/ORDER.txt) on the PS3 / PS4 copies, continuing
//      after a failing step                                             -> <data>/recipe/work/overlay-{r3,r2,port,mp}
//   3. <data>/state.json: what was made, which steps failed; the launcher starts Uncharted 3 with it.
//
// Updates: when the shipped recipe differs from the one an import was made with (state.json recipe_id), the recipe runs
// again into <data>/recipe-next, which starts as a copy of the current files and step records (the caches in
// recipe/scratch move along): only the steps whose fingerprint changed run again, and the steps whose files they change.
// When it ends no worse than the import it replaces, the two folders are swapped (if the game holds the files open, the
// swap waits for the next run: <data>/recipe-next/.ready). The replaced folder is deleted after the swap (a part locked
// then is deleted at the next run), and an update that is not applied deletes <data>/recipe-next again (its caches go
// back to <data>/recipe/scratch). Without room for a second work folder, or with --in-place, the recipe runs on
// <data>/recipe itself.
//
// Progress for the launcher: lines "@U3MP <json>". Nothing is written outside --data.
#include "mpimage.hpp"
#include "ps3keep.hpp"
#include "recipe.hpp"

#include <windows.h>
#include <winioctl.h>

#include <chrono>
#include <cstdio>
#include <regex>
#include <thread>

using namespace u3imp;

namespace {

const std::vector<std::string> OVERLAYS = {"overlay-r3", "overlay-r2", "overlay-port", "overlay-mp"};   // first wins
// the decrypted PS3 1.19 EBOOT the reference tables were made from
const std::string PS3_ELF_SHA256 = "aac4faa63ab1387e8aaa0b8e9e80ee87ac4ae61e0b40292f7aa7401df7b3f50a";
const char *DATA2_ID = "BCES01670DATA2";   // <data>/ps3-data2/BCES01670DATA2/USRDIR
// steps without which the overlays crash Uncharted 3 at boot (dc1 modules and their reserved sizes in modules.bin)
const std::vector<std::string> CRITICAL_STEPS = {"convert-dc1", "dc-recipes"};

bool present(const fs::path &p) {
    std::error_code ec;
    return fs::exists(p, ec);
}
bool is_dir(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool is_file(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
void ev(const Value &v) { event(v); }
void lg(const std::string &s) { log(s); }
std::string join(const std::vector<std::string> &v, const std::string &sep) {
    std::string o;
    for (const auto &x : v) o += (o.empty() ? "" : sep) + x;
    return o;
}
std::string cut(const std::string &s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }

struct Args {
    std::string ps4, ps4_update, ps3_u2, ps3_elf, speech_langs = "english", only, dns;
    std::vector<std::string> ps3_u3;
    fs::path data, port;
    bool image_only = false, in_place = false, keep_ps3 = false, keep_ps3_only = false, keep_ps3_off = false;
};

// ------------------------------------------------------------------------------------------------ programs
std::vector<fs::path> bin_dirs(const fs::path &port) {
    // folders with the import's helper programs (ffmpeg, objdump): the launcher package's u3mp/bin first
    std::vector<fs::path> dirs;
    for (const fs::path &d : {port / "u3mp" / "bin", patcher_dir() / "bin"}) {
        if (!is_dir(d)) continue;
        dirs.push_back(d);
        std::vector<fs::path> sub;
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(d, ec)) {
            std::error_code e2;
            if (e.is_directory(e2)) sub.push_back(e.path());
        }
        std::sort(sub.begin(), sub.end());
        dirs.insert(dirs.end(), sub.begin(), sub.end());
    }
    return dirs;
}

// The decrypted PS3 1.19 EBOOT.elf: the one given, else one in the PS3 folders, 4 levels deep: an .elf with the tested
// 1.19 hash (the EU, UK, US and Asia Game of the Year 1.19 EBOOTs decrypt to the same file), else an EBOOT.elf
std::string find_ps3_elf(const std::vector<std::string> &folders, const std::string &given) {
    if (!given.empty() && is_file(P(given))) return given;
    std::string best;
    for (const std::string &rs : folders) {
        const fs::path root = P(rs);
        if (!is_dir(root)) continue;
        for (int depth = 0; depth < 5; depth++) {
            std::function<void(const fs::path &, int)> walk;
            std::string found;
            walk = [&](const fs::path &d, int left) {
                std::error_code ec;
                for (const auto &e : fs::directory_iterator(d, ec)) {
                    if (!found.empty()) return;
                    std::error_code e2;
                    if (left > 0) {
                        if (e.is_directory(e2)) walk(e.path(), left - 1);
                        continue;
                    }
                    const std::string n = S(e.path().filename());
                    if (!ends(lower(n), ".elf") || !e.is_regular_file(e2)) continue;
                    FILE *f = _wfopen(e.path().c_str(), L"rb");
                    if (!f) continue;
                    char m[4] = {};
                    const size_t got = std::fread(m, 1, 4, f);
                    std::fclose(f);
                    if (got != 4 || std::memcmp(m, "\x7f" "ELF", 4) != 0) continue;
                    if (sha256_file(e.path()) == PS3_ELF_SHA256) {
                        found = S(e.path());
                        return;
                    }
                    if (lower(n) == "eboot.elf" && best.empty()) best = S(e.path());
                }
            };
            walk(root, depth);
            if (!found.empty()) return found;
        }
    }
    return best;
}

// ------------------------------------------------------------------------------------------------ game data
std::vector<std::string> data2_problems(const std::vector<std::string> &folders) {
    Inputs inp;
    std::vector<fs::path> f;
    for (const auto &x : folders) f.push_back(P(x));
    inp.add_ps3_u3(f);
    std::vector<std::string> out;
    for (const auto &p : full_import_problems(inp))
        if (p.find("DLC map packs") == std::string::npos) out.push_back(p);
    return out;
}

// The online program's game-data download: `uncharted_online.exe --fetch-game-data <dns> <target> <EBOOT.elf>` writes
// <target>/USRDIR/... as Uncharted 3 would when it goes online (where the files come from is the online program's
// business). Its stdout lines: start, get <path> <got> <total>, file <path> <bytes>, note <path> <text>, fail <path>
// <text>, error <text>, done <files> <bytes> <failed>; exit 0 = complete. Returns "" or what went wrong (no addresses:
// the program prints none, and the DNS value is not logged).
std::string fetch_data2(const std::string &dns, const std::string &ps3_elf, const fs::path &target) {
    const fs::path exe = patcher_dir() / "online" / "uncharted_online.exe";
    if (!is_file(exe)) return "the online program is missing (" + S(exe) + ")";
    std::vector<std::string> fails;
    long long files = 0, done = 0;
    int rc;
    try {
        rc = run({S(exe), "--fetch-game-data", dns, S(target), ps3_elf}, fs::path(), {}, [&](const std::string &raw) {
            const std::string line = strip(raw);
            const size_t sp = line.find(' ');
            const std::string kind = line.substr(0, sp), rest = sp == std::string::npos ? "" : line.substr(sp + 1);
            auto digits = [](const std::string &s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); };
            if (kind == "get") {
                const std::vector<std::string> parts = split(rest, ' ');
                if (parts.size() == 3 && digits(parts[1]) && digits(parts[2])) {
                    const long long total = std::stoll(parts[2]) >> 20;
                    const std::string nm = parts[0].substr(parts[0].rfind('/') == std::string::npos ? 0 : parts[0].rfind('/') + 1);
                    ev(obj({{"phase", str("data2")},
                            {"status", str("running")},
                            {"note", str(fmt("Downloading the game data: %s, %lld%s MB", nm.c_str(), std::stoll(parts[1]) >> 20,
                                             total ? fmt(" of %lld", total).c_str() : ""))}}));
                }
            } else if (kind == "file") {
                const size_t k = rest.rfind(' ');
                const std::string path = k == std::string::npos ? "" : rest.substr(0, k), size = k == std::string::npos ? rest : rest.substr(k + 1);
                files++;
                done += digits(size) ? std::stoll(size) : 0;
                log("game data: downloaded " + path + " (" + size + " bytes)");
                ev(obj({{"phase", str("data2")}, {"status", str("running")}, {"note", str(fmt("Downloading the game data: %lld files, %lld MB", files, done >> 20))}}));
            } else if (kind == "fail" || kind == "error") {
                fails.push_back(rest);
                log("game data: " + line);
            } else if (kind == "note" || kind == "start" || kind == "done")
                log("game data: " + line);
        });
    } catch (const std::exception &e) {
        return std::string("the online program could not start (") + e.what() + ")";
    }
    if (rc == 0) return "";
    const std::string f = cut(join(fails, "; "), 400);
    return f.empty() ? fmt("the online program ended with code %d", rc) : f;
}

// The game-data folder (net18.bin, the Lab maps, text, EULA pages: what Uncharted 3 writes to
// dev_hdd0/game/BCES01670DATA2 when it goes online in RPCS3): the player's RPCS3 folder, or the copy in
// <data>/ps3-data2. When neither is complete and current and a community server DNS is set (--dns: the "Set up
// multiplayer" DNS field, shared with the RPCN section), the online program downloads it into <data>/ps3-data2.
// Without a DNS a missing or incomplete folder is reported by the recipe's input check, which tells the player what to
// do. Returns "" (the import goes on), or why the download did not give a usable folder.
std::string ensure_data2(Args &a, const fs::path &data, const std::string &ps3_elf) {
    const fs::path own = data / "ps3-data2", usrdir = own / DATA2_ID / "USRDIR";
    if (present(usrdir / "net18.bin") && data2_problems({S(own)}).empty()) {
        a.ps3_u3.insert(a.ps3_u3.begin(), S(own));   // downloaded earlier (or left by an earlier launcher version)
        log("game data: using the copy already on this PC (" + S(usrdir) + ")");
        return "";
    }
    const std::vector<std::string> problems = data2_problems(a.ps3_u3);
    if (problems.empty()) return "";
    log("game data: " + join(problems, " / "));
    if (strip(a.dns).empty()) return "";
    log("game data: downloading it through the community server DNS, as Uncharted 3 does when it goes online");
    ev(obj({{"phase", str("data2")}, {"status", str("running")}, {"note", str("Downloading the game data...")}}));
    const std::string why = fetch_data2(strip(a.dns), ps3_elf, own / DATA2_ID);
    const std::vector<std::string> left = present(usrdir / "net18.bin") ? data2_problems({S(own)}) : std::vector<std::string>{};
    if (present(usrdir / "net18.bin") && left.empty()) {
        a.ps3_u3.insert(a.ps3_u3.begin(), S(own));
        ev(obj({{"phase", str("data2")}, {"status", str("ok")}, {"note", str("Game data downloaded.")}}));
        log("game data: downloaded to " + S(usrdir) + (why.empty() ? "" : " (but: " + why + ")"));
        return "";
    }
    std::vector<std::string> parts;
    if (!why.empty()) parts.push_back(why);
    parts.insert(parts.end(), left.begin(), left.end());
    std::string w = join(parts, " / ");
    if (w.empty()) w = "nothing was downloaded";
    ev(obj({{"phase", str("data2")}, {"status", str("failed")}, {"note", str(cut(w, 300))}}));
    return "the game data download through the community server DNS did not give a usable folder (" + cut(w, 500) +
           "). Check the DNS in \"Set up multiplayer\" and import again";
}

// ------------------------------------------------------------------------------------------------ staged updates
void copytree(const fs::path &src, const fs::path &dst) {   // shutil.copytree: files with their times
    fs::create_directories(dst);
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(src, ec)) {
        const fs::path d = dst / e.path().filename();
        std::error_code e2;
        if (e.is_directory(e2)) copytree(e.path(), d);
        else if (!CopyFileW(e.path().c_str(), d.c_str(), FALSE)) throw u3::Stop("cannot copy " + S(e.path()));
    }
}

double secs_since(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

// A staged update starts from a copy of the current import (work files, backups and the step records in
// _recipe/state.json), so the recipe reruns only the steps whose fingerprint changed and those whose files they change.
// The copy goes to work.seed first and is renamed when complete; the step records come last: an interrupted copy is
// started again, never used half.
bool seed_next(const fs::path &cur, const fs::path &nxt) {
    if (present(nxt / "_recipe") || !is_dir(cur / "work") || !present(cur / "_recipe" / "state.json")) return false;
    const fs::path seed = nxt / "work.seed";
    if (present(nxt / "work")) rmtree(nxt / "work");   // a copy interrupted after the rename, before the records
    if (present(seed)) rmtree(seed);
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t size = tree_size(cur / "work");
    log(fmt("update: copying the current files to %s (%.1f GB) so only the changed steps run again ...", S(nxt).c_str(), size / 1e9));
    copytree(cur / "work", seed);
    fs::rename(seed, nxt / "work");
    copytree(cur / "_recipe", nxt / "_recipe");
    log(fmt("update: copied in %.0f s", secs_since(t0)));
    return true;
}

void remove_leftovers(const fs::path &data) {
    // old copies of the import that an earlier swap could not delete (files in use then): recipe-old-*
    std::vector<fs::path> olds;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(data, ec)) {
        std::error_code e2;
        if (starts(S(e.path().filename()), "recipe-old-") && e.is_directory(e2)) olds.push_back(e.path());
    }
    std::sort(olds.begin(), olds.end());
    for (const fs::path &old : olds) {
        const uint64_t size = tree_size(old);
        if (rmtree_retry(old)) log(fmt("removed an old copy of the import: %s (%.1f GB)", S(old.filename()).c_str(), size / 1e9));
        else log("could not remove the old copy " + S(old.filename()) + " yet (files in use); it is removed at the next run");
    }
}

bool try_swap(const fs::path &data) {
    // put a finished <data>/recipe-next in place of <data>/recipe; false while the files are in use (game running)
    const fs::path nxt = data / "recipe-next", cur = data / "recipe";
    if (!present(nxt / ".ready")) return false;
    const fs::path old = data / P(fmt("recipe-old-%lld", (long long)std::time(nullptr)));
    if (present(cur) && !MoveFileExW(cur.c_str(), old.c_str(), 0)) {
        log(fmt("update ready, waiting to swap it in (files in use: [WinError %lu])", GetLastError()));
        return false;
    }
    if (!MoveFileExW(nxt.c_str(), cur.c_str(), 0)) {
        const DWORD e = GetLastError();
        MoveFileExW(old.c_str(), cur.c_str(), 0);   // put the playable import back
        log(fmt("update ready, waiting to swap it in ([WinError %lu])", e));
        return false;
    }
    std::error_code ec;
    fs::remove(cur / ".ready", ec);
    log("update swapped in: " + S(cur));
    remove_leftovers(data);   // the replaced copy (and any older one a locked file kept)
    return true;
}

void discard_next(const fs::path &data) {
    // an update that is not applied: delete <data>/recipe-next (a full copy of the import), keeping its caches
    const fs::path nxt = data / "recipe-next", cur = data / "recipe";
    if (!is_dir(nxt)) return;
    if (is_dir(nxt / "scratch") && !present(cur / "scratch") && is_dir(cur)) MoveFileExW((nxt / "scratch").c_str(), (cur / "scratch").c_str(), 0);
    const uint64_t size = tree_size(nxt);
    if (rmtree_retry(nxt)) log(fmt("update not applied: its copy %s was deleted (%.1f GB)", S(nxt.filename()).c_str(), size / 1e9));
    else log("update not applied: " + S(nxt.filename()) + " could not be deleted yet (files in use); it is reused or removed later");
}

// ------------------------------------------------------------------------------------------------ the PS4 view
// a junction at `link` to the folder `target` (the reparse data a mount point needs)
bool make_junction(const fs::path &target, const fs::path &link) {
    std::wstring t = fs::absolute(target).wstring();
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link.c_str());
        return false;
    }
    const std::wstring sub = L"\\??\\" + t;
    const USHORT sub_b = USHORT(sub.size() * 2), print_b = USHORT(t.size() * 2);
    std::vector<uint8_t> buf(8 + 8 + sub_b + 2 + print_b + 2, 0);
    auto w16 = [&](size_t o, uint16_t v) { std::memcpy(&buf[o], &v, 2); };
    const uint32_t tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(&buf[0], &tag, 4);
    w16(4, uint16_t(buf.size() - 8));
    w16(8, 0);
    w16(10, sub_b);
    w16(12, USHORT(sub_b + 2));
    w16(14, print_b);
    std::memcpy(&buf[16], sub.data(), sub_b);
    std::memcpy(&buf[16 + sub_b + 2], t.data(), print_b);
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), DWORD(buf.size()), nullptr, 0, &got, nullptr);
    CloseHandle(h);
    if (!ok) RemoveDirectoryW(link.c_str());
    return ok != 0;
}

void link_file(const fs::path &src, const fs::path &dst, uint64_t &copied, uint64_t budget) {
    // dst as a hard link to src (same drive), else a copy
    if (CreateHardLinkW(dst.c_str(), src.c_str(), nullptr)) return;
    std::error_code ec;
    copied += fs::file_size(src, ec);
    if (copied > budget)
        throw u3::Stop(fmt("the game folder and its update are on another drive than the multiplayer data, and the files to combine exceed %llu MB",
                           (unsigned long long)(budget >> 20)));
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) throw u3::Stop("cannot copy " + S(src));
}

void merge(const std::optional<fs::path> &base, const std::optional<fs::path> &update, const fs::path &view, uint64_t &copied, uint64_t budget) {
    // view = base with update laid over it: folders only one side has are junctions to it, files hard links (or copies),
    // folders both sides have are merged one level further down
    if (!CreateDirectoryW(view.c_str(), nullptr)) throw u3::Stop("cannot create " + S(view));
    std::map<std::string, fs::path> names, over;
    std::error_code ec;
    if (base && is_dir(*base))
        for (const auto &e : fs::directory_iterator(*base, ec)) names[S(e.path().filename())] = e.path();
    if (update && is_dir(*update))
        for (const auto &e : fs::directory_iterator(*update, ec)) over[S(e.path().filename())] = e.path();
    std::set<std::string> all;
    for (const auto &[n, p] : names) all.insert(n);
    for (const auto &[n, p] : over) all.insert(n);
    for (const std::string &n : all) {
        const auto b = names.find(n), u = over.find(n);
        const fs::path v = view / P(n);
        if (u != over.end() && b != names.end() && is_dir(u->second) && is_dir(b->second)) merge(b->second, u->second, v, copied, budget);
        else {
            const fs::path src = u != over.end() ? u->second : b->second;
            if (is_dir(src)) {
                if (!make_junction(src, v)) throw u3::Stop("cannot create a junction to " + S(src));
            } else
                link_file(src, v, copied, budget);
        }
    }
}

void remove_view(const fs::path &view) { rmtree(view); }   // rmtree never follows junctions

std::string ps4_view(const Args &a, const fs::path &data) {
    // The PS4 game as the game sees it: with update 1.02 in its own folder (--ps4-update) the recipe must read the
    // updated files, so it gets a merged view of the two folders (<data>/ps4-view, rebuilt when either folder changes)
    if (a.ps4_update.empty()) return a.ps4;
    const fs::path base = P(a.ps4), update = P(a.ps4_update), view = data / "ps4-view", sf = data / "ps4-view.txt";
    auto mt = [](const fs::path &p) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d);
        const int64_t t = int64_t(uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32 | d.ftLastWriteTime.dwLowDateTime);
        return (long long)((t - 116444736000000000LL) / 10000000);
    };
    const std::string stamp = S(base) + "|" + S(update) + "|" + std::to_string(mt(base)) + "|" + std::to_string(mt(update)) + "|v1";
    if (is_dir(view) && present(sf) && read_text(sf) == stamp) return S(view);
    if (present(view)) remove_view(view);
    std::error_code ec;
    fs::remove(sf, ec);
    try {
        uint64_t copied = 0;
        merge(base, update, view, copied, uint64_t(2) << 30);
    } catch (const std::exception &e) {
        log(std::string("note: the PS4 update could not be combined with the game folder (") + e.what() +
            "); the recipe reads the game folder alone, so steps that need update 1.02 files may fail");
        if (present(view)) remove_view(view);
        return a.ps4;
    }
    write_text(sf, stamp);
    log("PS4 game for the recipe: " + S(base) + " with update " + S(update) + " laid over it (" + S(view) + ")");
    return S(view);
}

// ------------------------------------------------------------------------------------------------ the recipe run
struct StepInfo {
    long long n = 0;
    std::string title, status;
    std::optional<std::string> note;   // set by a result line, or when a step failed or found an input missing
};
struct RecipeResult {
    int exit = 0;
    std::vector<std::pair<std::string, StepInfo>> steps;
    std::vector<std::string> overlays, problems;
    std::string recipe_version;
    StepInfo *get(const std::string &id) {
        for (auto &[k, s] : steps)
            if (k == id) return &s;
        return nullptr;
    }
    StepInfo &put(const std::string &id, StepInfo s) {
        if (StepInfo *x = get(id)) return *x = s;
        steps.push_back({id, s});
        return steps.back().second;
    }
};

RecipeResult run_recipe(const Args &a, const fs::path &data, const fs::path &ps4_elf, const std::string &ps3_elf, std::optional<fs::path> out_opt) {
    const fs::path out = out_opt ? *out_opt : data / "recipe";
    fs::create_directories(out);
    RecipeArgs ra;
    ra.ps4 = ps4_view(a, data);
    ra.out = S(out);
    ra.continue_on_error = true;
    ra.speech_langs = a.speech_langs;
    ra.ps3_u3 = a.ps3_u3;
    ra.only = a.only;
    if (!a.ps3_u2.empty()) ra.ps3_u2 = {a.ps3_u2};
    else ra.allow_missing_u2 = true;   // no step reads Uncharted 2 (recipe/RECIPE.md): optional here
    std::vector<fs::path> bins = bin_dirs(a.port);
    std::string path;
    for (const auto &d : bins) path += S(d) + ";";
    if (const char *p = std::getenv("PATH")) path += p;
    ra.env.push_back({"PATH", path});
    if (!ps4_elf.empty()) ra.env.push_back({"RECIPE_PS4_ELF", S(ps4_elf)});
    if (!ps3_elf.empty()) {
        ra.env.push_back({"RECIPE_PS3_ELF", ps3_elf});
        ra.env.push_back({"U3_PS3_ELF", ps3_elf});
    }
    const std::string ffmpeg = find_program(bins, "ffmpeg", {"C:\\msys64\\clang64\\bin\\ffmpeg.exe", "C:\\msys64\\ucrt64\\bin\\ffmpeg.exe"});
    if (!ffmpeg.empty()) ra.env.push_back({"RECIPE_FFMPEG", ffmpeg});
    const std::string objdump = find_program(bins, "objdump");
    log("ffmpeg: " + (ffmpeg.empty() ? std::string("not found (EULA pages are written without it; banks with MP3 samples are skipped)") : ffmpeg));
    log("objdump: " + (objdump.empty() ? std::string("not found (the Havok tables step will report it)") : objdump));
    RecipeResult r;
    // the recipe's step lines: "[ n/N] id - title ..." (starts) or "[ n/N] id - title: <result>" (did not run)
    static const std::regex step_re(R"(^\[\s*(\d+)/(\d+)\] (\S+) - (.*)$)");
    static const std::regex result_re(R"(^(.*): ((?:skipped \(selection\)|SKIP, |NOT REPRODUCIBLE HERE|already ).*)$)");
    std::string current;
    const auto t0 = std::chrono::steady_clock::now();
    auto on_line = [&](const std::string &line) {
        log(line);
        if (starts(line, "  PROBLEM: ")) r.problems.push_back(line.substr(11));
        std::smatch m;
        if (std::regex_match(line, m, step_re)) {
            const long long n = std::stoll(m[1]), total = std::stoll(m[2]);
            const std::string sid = m[3], rest = m[4];
            std::smatch rm;
            const bool running = ends(rest, " ...");
            if (running || !std::regex_match(rest, rm, result_re)) {
                const std::string title = running ? rest.substr(0, rest.size() - 4) : rest;
                current = sid;
                r.put(sid, StepInfo{n, title, "running", std::nullopt});
                ev(obj({{"phase", str("recipe")}, {"step", num(n)}, {"total", num(total)}, {"id", str(sid)}, {"title", str(title)}, {"status", str("running")}}));
            } else {
                current.clear();
                const std::string title = rm[1], result = rm[2];
                const std::string status = starts(result, "already") ? "already" : starts(result, "skipped") ? "skipped" : "unavailable";
                r.put(sid, StepInfo{n, title, status, cut(result, 300)});
                ev(obj({{"phase", str("recipe")},
                        {"step", num(n)},
                        {"total", num(total)},
                        {"id", str(sid)},
                        {"title", str(title)},
                        {"status", str(status)},
                        {"note", str(cut(result, 300))}}));
            }
            return;
        }
        const std::string s = strip(line);
        if (!current.empty() && starts(s, "ok ")) {
            r.get(current)->status = "ok";
            ev(obj({{"phase", str("recipe")}, {"id", str(current)}, {"status", str("ok")}, {"note", str(s)}}));
            current.clear();
        } else if (!current.empty() && starts(s, "SKIP, script found an input missing")) {
            // the step's program found an input missing (e.g. no decrypted EBOOT.elf): it runs again at the next import
            r.get(current)->status = "unavailable";
            r.get(current)->note = cut(s, 300);
            ev(obj({{"phase", str("recipe")}, {"id", str(current)}, {"status", str("unavailable")}, {"note", str(cut(s, 300))}}));
            current.clear();
        } else if (!current.empty() && starts(s, "FAILED")) {
            r.get(current)->status = "failed";
            r.get(current)->note = cut(s, 500);
            ev(obj({{"phase", str("recipe")}, {"id", str(current)}, {"status", str("failed")}, {"note", str(cut(s, 500))}}));
            current.clear();
        }
    };
    try {
        r.exit = rebuild_overlays(ra, on_line);
    } catch (const std::exception &e) {
        on_line(std::string("Error: ") + e.what());
        r.exit = 1;
    }
    log(fmt("recipe finished in %.0f s (exit %d)", secs_since(t0), r.exit));
    for (const auto &o : OVERLAYS)
        if (is_dir(out / "work" / P(o))) r.overlays.push_back(S(out / "work" / P(o)));
    r.recipe_version = recipe_version();
    return r;
}

Value steps_value(const RecipeResult &r) {
    Value v = Value::object();
    for (const auto &[id, s] : r.steps) {
        Value x = obj({{"n", num(s.n)}, {"title", str(s.title)}, {"status", str(s.status)}});
        if (s.note) x.set("note", str(*s.note));
        v.set(id, x);
    }
    return v;
}
Value strings(const std::vector<std::string> &v) {
    Value a;
    a.kind = Value::Array;
    for (const auto &x : v) a.items.push_back(str(x));
    return a;
}

std::string recipe_fingerprint();

// the recipe fingerprint of the installed files: recipe/CONTENT_ID (the release build writes it, so the launcher does
// not hash 1 500 files at every start), else computed
std::string shipped_id() {
    const fs::path cid = recipe_dir() / "CONTENT_ID";
    if (is_file(cid)) {
        const std::string v = strip(read_text(cid));
        if (v.size() == 64) return v;
    }
    return recipe_fingerprint();
}

// sha256 over recipe/ and tools/: relative path + NUL + sha256 of the content (text files with \n line ends, so a
// checkout with CRLF gives the release zip's value), folders and files sorted
std::string recipe_fingerprint() {
    Sha256 h;
    const fs::path pat = patcher_dir();
    for (const char *part : {"recipe", "tools"}) {
        std::function<void(const fs::path &)> walk = [&](const fs::path &dp) {
            std::vector<std::string> dirs, files;
            std::error_code ec;
            for (const auto &e : fs::directory_iterator(dp, ec)) {
                const std::string n = S(e.path().filename());
                std::error_code e2;
                if (e.is_directory(e2)) {
                    if (n != "__pycache__") dirs.push_back(n);
                } else
                    files.push_back(n);
            }
            std::sort(dirs.begin(), dirs.end());
            std::sort(files.begin(), files.end());
            for (const auto &f : files) {
                if (ends(f, ".pyc") || ends(f, ".orig") || f == "CONTENT_ID") continue;
                std::string b;
                try {
                    b = read_text(dp / P(f));
                } catch (const std::exception &) {
                    continue;
                }
                for (const char *x : {".py", ".sh", ".txt", ".json", ".tsv", ".md", ".cmd", ".c"})
                    if (ends(f, x)) {
                        std::string o;
                        for (size_t i = 0; i < b.size(); i++)
                            if (!(b[i] == '\r' && i + 1 < b.size() && b[i + 1] == '\n')) o += b[i];
                        b.swap(o);
                        break;
                    }
                std::string rel = S((dp / P(f)).lexically_relative(pat));
                for (char &c : rel)
                    if (c == '\\') c = '/';
                h.update(rel.data(), rel.size());
                h.update("", 1);
                const Bytes d = sha256_raw(b.data(), b.size());
                h.update(d.data(), d.size());
            }
            for (const auto &d : dirs) walk(dp / P(d));
        };
        walk(pat / part);
    }
    return h.hex();
}

// ------------------------------------------------------------------------------------------------ main
int import_main(Args a) {
    std::error_code ec;
    const fs::path data = fs::absolute(a.data).lexically_normal();
    fs::create_directories(data);
    const fs::path state_file = data / "state.json";
    Value old = Value::object();
    if (auto v = try_json(state_file); v && v->is_object()) old = *v;
    std::vector<std::string> own_ps3;   // the player's PS3 folders that still exist
    for (const auto &f : a.ps3_u3)
        if (is_dir(P(f))) own_ps3.push_back(f);
    std::vector<fs::path> own_paths;
    for (const auto &f : own_ps3) own_paths.push_back(P(f));
    auto save = [&](const Value &v) { write_text(state_file, dump_indent1(v)); };
    if (a.keep_ps3_off || a.keep_ps3_only) {
        if (a.keep_ps3_off) {
            ps3keep_drop(data, lg);
            old.remove("ps3_kept");
        } else
            old.set("ps3_kept", ps3keep_keep(data, own_paths, find_ps3_elf(own_ps3, a.ps3_elf), lg, ev));
        save(old);
        return a.keep_ps3_off || (old.get("ps3_kept")->get("complete") && old.get("ps3_kept")->get("complete")->truthy()) ? 0 : 1;
    }
    // the kept PS3 files come after the player's own folders: those win while they exist; once they are deleted the
    // import reads the kept copy
    a.ps3_u3 = own_ps3;
    for (const fs::path &k : ps3keep_roots(data))
        if (std::find(own_ps3.begin(), own_ps3.end(), S(k)) == own_ps3.end()) a.ps3_u3.push_back(S(k));
    if (a.ps3_u3.size() > own_ps3.size()) log("PS3 files: using the kept copy in " + S(data / "ps3-kept") + " after your PS3 folders");
    {
        std::vector<fs::path> rec = own_paths;
        if (!a.ps3_elf.empty() && is_file(P(a.ps3_elf))) rec.push_back(fs::absolute(P(a.ps3_elf)).parent_path());
        ps3keep_record(rec, data);
    }
    Value state = old;
    state.set("status", str("running"));
    state.set("pid", num((long long)GetCurrentProcessId()));
    state.set("started", str(now_text()));
    state.set("inputs", obj({{"ps4", str(a.ps4)}, {"ps3_u3", strings(a.ps3_u3)}, {"ps3_u2", str(a.ps3_u2)}}));
    state.remove("error");
    save(state);
    remove_leftovers(data);
    auto old_s = [&](const char *k) {
        const Value *v = old.get(k);
        return v && v->kind == Value::String ? v->s : std::string();
    };
    auto finish = [&](const std::string &status, std::initializer_list<std::pair<std::string, Value>> kw = {}) {
        if (a.keep_ps3 && (status == "imported" || status == "partial") && a.only.empty() && !a.image_only) {
            try {
                state.set("ps3_kept", ps3keep_keep(data, own_paths, find_ps3_elf(own_ps3, a.ps3_elf), lg, ev));
            } catch (const std::exception &e) {   // the import itself is done; reported
                Value st = ps3keep_manifest(data).get("state") ? *ps3keep_manifest(data).get("state") : Value::object();
                st.set("error", str(std::string("Error: ") + e.what()));
                state.set("ps3_kept", st);
                log("PS3 files: could not keep them: Error: " + std::string(e.what()));
            }
        }
        for (const auto &[k, v] : kw) state.set(k, v);
        state.set("status", str(status));
        state.set("finished", str(now_text()));
        save(state);
        const Value *e = state.get("error");
        ev(obj({{"phase", str("done")}, {"status", str(status)}, {"error", str(e && e->kind == Value::String ? e->s : "")}}));
        return status == "imported" || status == "partial" ? 0 : 1;
    };
    const bool old_ok = old_s("status") == "imported" || old_s("status") == "partial";

    // 1. the multiplayer image (seconds): a mismatching game or port version is reported before the long part
    ev(obj({{"phase", str("image")}, {"status", str("running")}}));
    const fs::path ps4_elf = data / "tools" / "u3_ps4.elf";
    Value image;
    try {
        // the helper's online profile keys come from the player's own decrypted 1.19 EBOOT.elf
        image = build_image(patcher_dir(), P(a.ps4), data / "image", lg, ps4_elf, a.ps4_update.empty() ? std::nullopt : std::optional<fs::path>(P(a.ps4_update)),
                            find_ps3_elf(a.ps3_u3, a.ps3_elf));
    } catch (const KeysMissing &e) {
        log(std::string("ERROR: ") + e.what());
        if (old_ok)   // an update of a working import: not applied, the current image stays
            return finish(old_s("status"), {{"error", str(std::string("Update not applied: ") + e.what())}, {"failed_at", str("image")}});
        return finish("failed", {{"error", str(std::string("Multiplayer image: ") + e.what())}, {"failed_at", str("image")}});
    } catch (const std::exception &e) {
        log(std::string("ERROR: ") + e.what());
        return finish("failed", {{"error", str(std::string("Multiplayer image: ") + e.what())}, {"failed_at", str("image")}});
    }
    state.set("image", image);
    ev(obj({{"phase", str("image")}, {"status", str("ok")}, {"sha256", *image.get("sha256")}, {"version", *image.get("version")}}));
    if (a.image_only) return finish(old_ok ? old_s("status") : "image-only");

    // 2. overlays
    if (a.ps3_u3.empty()) return finish("failed", {{"error", str("No PS3 Uncharted 3 folder given")}, {"failed_at", str("inputs")}});
    const std::string ps3_elf = find_ps3_elf(a.ps3_u3, a.ps3_elf);
    if (!ps3_elf.empty())
        log("PS3 1.19 EBOOT.elf: " + ps3_elf + (sha256_file(P(ps3_elf)) == PS3_ELF_SHA256 ? "" : " (differs from the tested 1.19 EBOOT.elf)"));
    else {
        // required: no import without the decrypted EBOOT.elf
        const std::string why = "No decrypted PS3 1.19 EBOOT.elf of your own copy of the game: choose it on the Multiplayer tab, then import again";
        log("ERROR: " + why);
        if (old_ok) return finish(old_s("status"), {{"error", str("Update not applied: " + why)}, {"failed_at", str("inputs")}});
        return finish("failed", {{"error", str(why)}, {"failed_at", str("inputs")}});
    }
    const std::string data2_pending = a.only.empty() ? ensure_data2(a, data, ps3_elf) : "";
    if (!data2_pending.empty()) log("game data: " + data2_pending);
    state.set("data2_pending", str(data2_pending));   // why the game-data download failed ("" = no download or it worked)
    ev(obj({{"phase", str("recipe")}, {"status", str("running")}}));
    const std::string recipe_id = shipped_id();   // the recipe this run applies (the launcher updates when it changes)
    const fs::path nxt = data / "recipe-next";
    if (present(nxt / ".stopped")) {
        // the launcher stopped this update to install its own update first: the half-built copy may hold a file a step
        // was rewriting in place, so it is not reused; the caches go back to the import
        log("update: the last update was stopped for a launcher update; it starts again from the current files");
        discard_next(data);
        state.remove("next");
        state.remove("staging");
    }
    std::optional<Value> pending;
    if (const Value *p = state.get("next")) pending = *p;
    state.remove("next");
    if (pending && pending->is_object() && present(nxt / ".ready")) {
        // an update finished earlier but could not be swapped in (the game had the files open)
        const Value *pr = pending->get("recipe_id");
        const std::string prid = pr && pr->kind == Value::String ? pr->s : "";
        if (prid == recipe_id && try_swap(data)) {
            for (const auto &[k, v] : pending->fields) state.set(k, v);
            const Value *st = pending->get("status");
            return finish(st && st->kind == Value::String ? st->s : "imported");
        }
        if (prid != recipe_id) fs::remove(nxt / ".ready", ec);   // a newer recipe: build again
    }
    bool staged = a.only.empty() && !a.in_place && old_ok && !old_s("recipe_id").empty() && old_s("recipe_id") != recipe_id && is_dir(data / "recipe" / "work");
    if (staged) {
        const uint64_t free = fs::space(data, ec).available;
        const double need = double(tree_size(data / "recipe" / "work")) * 1.1 + 2e9;
        if (!present(nxt) && double(free) < need) {
            log(fmt("update: %.1f GB free, %.1f GB needed for a second work folder: updating in place", free / 1e9, need / 1e9));
            staged = false;
        }
    }
    if (staged) {
        log("update: the recipe changed since this import (" + old_s("recipe_id").substr(0, 12) + " -> " + recipe_id.substr(0, 12) + "): rebuilding into " + S(nxt) +
            "; the current files stay playable until it is swapped in");
        fs::create_directories(nxt);
        if (is_dir(data / "recipe" / "scratch") && !present(nxt / "scratch"))
            MoveFileExW((data / "recipe" / "scratch").c_str(), (nxt / "scratch").c_str(), 0);   // caches: PS3 extracts, converter trees
        seed_next(data / "recipe", nxt);
        state.set("staging", str(recipe_id));   // the launcher lets the game run on the current files meanwhile
        save(state);
    }
    state.remove("update_failed_for");
    RecipeResult r = run_recipe(a, data, ps4_elf, ps3_elf, staged ? std::optional<fs::path>(nxt) : std::nullopt);
    std::vector<std::string> failed, unavailable;
    for (const auto &[sid, s] : r.steps)
        if (s.status == "failed") failed.push_back(sid);
    // steps that could not run here (an input missing, or not reproducible yet): their files are missing too
    for (const auto &[sid, s] : r.steps)
        if (s.status == "unavailable") unavailable.push_back(sid);
    // "NOT REPRODUCIBLE HERE" steps can run on no PC yet: listed, but an import that has only those is complete; a step
    // whose own program found an input missing is not counted (the steps that need its result fail and are counted)
    if (a.only.empty())
        for (const auto &sid : unavailable) {
            const std::string note = r.get(sid)->note.value_or("");
            if (!starts(note, "NOT REPRODUCIBLE") && !starts(note, "SKIP, script found")) failed.push_back(sid);
        }
    auto first_failed = [&]() {
        std::string best;
        long long bn = 0;
        for (const auto &f : failed)
            if (best.empty() || r.get(f)->n < bn) {
                best = f;
                bn = r.get(f)->n;
            }
        return best;
    };
    const Value *of = old.get("failed");
    const size_t old_failed = of ? of->items.size() : 0;
    if (staged) {
        state.remove("staging");
        bool crit = false;
        for (const auto &c : CRITICAL_STEPS) crit = crit || std::find(failed.begin(), failed.end(), c) != failed.end();
        if (r.exit == 2 || r.overlays.empty() || failed.size() > old_failed || crit) {
            state.set("update_failed_for", str(recipe_id));   // the launcher does not retry it at every start
            if (r.exit == 2 || r.steps.empty()) {
                std::string why = join(r.problems, " / ");
                if (why.empty()) why = "the input folders (see the log)";
                log("update: not started: " + why + "; the current files are kept");
                discard_next(data);
                return finish(old_s("status"), {{"error", str("Update not applied: " + why)}, {"failed_at", str("inputs")}});
            }
            log(fmt("update: the new build has more failed steps (%zu) than the current import (%zu); the current files are kept", failed.size(), old_failed));
            discard_next(data);
            return finish(old_s("status"), {{"error", str(fmt("Update not applied: %zu step(s) failed", failed.size()))},
                                            {"failed_at", str(failed.empty() ? "recipe" : first_failed())}});
        }
        std::vector<std::string> ov;
        for (const auto &o : OVERLAYS)
            if (is_dir(nxt / "work" / P(o))) ov.push_back(S(data / "recipe" / "work" / P(o)));
        r.overlays = ov;
        write_text(nxt / ".ready", recipe_id);
        Value done = obj({{"recipe_version", str(r.recipe_version)},
                          {"steps", steps_value(r)},
                          {"overlays", strings(r.overlays)},
                          {"failed", strings(failed)},
                          {"unavailable", strings(unavailable)},
                          {"ps3_elf", str(ps3_elf)},
                          {"recipe_id", str(recipe_id)},
                          {"status", str(failed.empty() ? "imported" : "partial")}});
        if (!try_swap(data)) {
            state.set("next", done);
            return finish(old_s("status"), {{"error", str("Update ready: it is applied the next time the launcher starts")}});
        }
        for (const auto &[k, v] : done.fields) state.set(k, v);
        return finish(done.get("status")->s);
    }
    state.set("recipe_version", str(r.recipe_version));
    state.set("steps", steps_value(r));
    state.set("overlays", strings(r.overlays));
    state.set("failed", strings(failed));
    state.set("unavailable", strings(unavailable));
    state.set("ps3_elf", str(ps3_elf));
    if (a.only.empty()) state.set("recipe_id", str(recipe_id));
    if (r.exit == 2 || r.steps.empty()) {
        if (!r.problems.empty()) {
            std::vector<std::string> problems = r.problems;
            // no game data and its download through the community DNS failed: say that, not the general advice
            bool d2 = false;
            for (const auto &p : problems) d2 = d2 || p.find("BCES01670DATA2") != std::string::npos;
            if (!data2_pending.empty() && d2) {
                std::vector<std::string> o = {data2_pending};
                for (const auto &p : problems)
                    if (p.find("BCES01670DATA2") == std::string::npos) o.push_back(p);
                problems = o;
            }
            return finish("failed", {{"error", str("Nothing was imported: " + join(problems, " / "))}, {"failed_at", str("inputs")}});
        }
        return finish("failed", {{"error", str("The overlay recipe could not start (see the log: input folders)")}, {"failed_at", str("inputs")}});
    }
    if (r.overlays.empty()) return finish("failed", {{"error", str("The overlay recipe produced no overlay")}, {"failed_at", str("recipe")}});
    // dc1/modules.bin holds the reserved size of every dc1 module: a converted module (convert-dc1) without the matching
    // modules.bin (dc-recipes) stops Uncharted 3 at boot. Such an import is not playable: the launcher then starts
    // Uncharted 3 without multiplayer.
    std::vector<std::string> critical;
    for (const auto &c : CRITICAL_STEPS)
        if (std::find(failed.begin(), failed.end(), c) != failed.end()) critical.push_back(c);
    if (!critical.empty())
        return finish("failed", {{"error", str("Step " + join(critical, ", ") +
                                                " failed: the multiplayer files would not start. Open the import log, check the PS3 folders (Uncharted 3 Game of "
                                                "the Year BCES01670, BCES01692, BCUS99086 or BCAS25014 with update 1.19) and press RE-IMPORT / UPDATE")},
                                 {"failed_at", str(critical[0])}});
    if (!failed.empty())
        return finish("partial", {{"error", str(fmt("%zu step(s) failed, first: ", failed.size()) + first_failed())}, {"failed_at", str(first_failed())}});
    return finish("imported");
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> v;
    for (int i = 1; i < argc; i++) v.push_back(S(fs::path(argv[i])));
    Args a;
    try {
        // the launcher: u3import --shipped-id (the installed recipe's fingerprint, compared with state.json recipe_id)
        // maintainers: u3import --recipe-fingerprint (computed, for recipe/CONTENT_ID); u3import --verify-output
        // <recipe out folder> [--report <tsv>] (the files against recipe/manifest.tsv); u3import --recipe <options>:
        // the overlay recipe alone (--ps4, --ps3-u3 ..., --ps3-u2 ..., --out, --list, --only A,B, --from ID,
        // --skip A,B, --force, --speech-langs LIST|all, --dry-run, --continue-on-error, --all-files, --jobs N,
        // --allow-missing-u2, --allow-partial-inputs); exit 0, 1 = a step failed, 2 = inputs missing
        if (!v.empty() && v[0] == "--recipe") {
            RecipeArgs ra;
            bool list = false;
            for (size_t i = 1; i < v.size(); i++) {
                const std::string &k = v[i];
                auto val = [&]() -> std::string {
                    if (i + 1 >= v.size()) throw u3::Stop("argument " + k + ": expected one argument");
                    return v[++i];
                };
                if (k == "--ps4") ra.ps4 = val();
                else if (k == "--ps3-u3") ra.ps3_u3.push_back(val());
                else if (k == "--ps3-u2") ra.ps3_u2.push_back(val());
                else if (k == "--out") ra.out = val();
                else if (k == "--only") ra.only = val();
                else if (k == "--from") ra.from = val();
                else if (k == "--skip") ra.skip = val();
                else if (k == "--speech-langs") ra.speech_langs = val();
                else if (k == "--jobs") ra.jobs = std::stoi(val());
                else if (k == "--list") list = true;
                else if (k == "--force") ra.force = true;
                else if (k == "--dry-run") ra.dry_run = true;
                else if (k == "--continue-on-error") ra.continue_on_error = true;
                else if (k == "--all-files") ra.all_files = true;
                else if (k == "--allow-missing-u2") ra.allow_missing_u2 = true;
                else if (k == "--allow-partial-inputs") ra.allow_partial_inputs = true;
                else throw u3::Stop("unrecognized argument: " + k);
            }
            if (list) {
                recipe_list();
                return 0;
            }
            // the steps find u3fix.exe next to u3import.exe, ffmpeg and objdump on PATH (RECIPE_FFMPEG when set)
            return rebuild_overlays(ra, lg);
        }
        if (!v.empty() && v[0] == "--shipped-id") {
            log(shipped_id());
            return 0;
        }
        if (!v.empty() && v[0] == "--recipe-fingerprint") {
            log(recipe_fingerprint());
            return 0;
        }
        if (!v.empty() && v[0] == "--verify-output" && v.size() >= 2)
            return verify_output(P(v[1]), v.size() >= 4 && v[2] == "--report" ? P(v[3]) : fs::path());
        for (size_t i = 0; i < v.size(); i++) {
            const std::string &k = v[i];
            auto val = [&]() -> std::string {
                if (i + 1 >= v.size()) throw u3::Stop("argument " + k + ": expected one argument");
                return v[++i];
            };
            if (k == "--ps4") a.ps4 = val();
            else if (k == "--ps4-update") a.ps4_update = val();
            else if (k == "--ps3-u3") a.ps3_u3.push_back(val());
            else if (k == "--ps3-u2") a.ps3_u2 = val();
            else if (k == "--ps3-elf") a.ps3_elf = val();
            else if (k == "--data") a.data = P(val());
            else if (k == "--port") a.port = P(val());
            else if (k == "--speech-langs") a.speech_langs = val();
            else if (k == "--only") a.only = val();
            else if (k == "--dns") a.dns = val();
            else if (k == "--image-only") a.image_only = true;
            else if (k == "--in-place") a.in_place = true;
            else if (k == "--keep-ps3") a.keep_ps3 = true;
            else if (k == "--keep-ps3-only") a.keep_ps3_only = true;
            else if (k == "--keep-ps3-off") a.keep_ps3_off = true;
            else throw u3::Stop("unrecognized argument: " + k);
        }
        if (a.ps4.empty() || a.data.empty() || a.port.empty()) throw u3::Stop("the following arguments are required: --ps4, --data, --port");
        return import_main(a);
    } catch (const std::exception &e) {
        log(std::string("u3import: error: ") + e.what());
        return 2;
    }
}
