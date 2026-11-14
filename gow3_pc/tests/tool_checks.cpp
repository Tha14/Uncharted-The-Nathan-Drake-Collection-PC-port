// Checks that run the test executables as separate processes: the loader (out/gow3-probe) on tiny
// synthetic x86-64 images, and the runtime and semaphore tests (out/runtime-test, out/sema-test)
// in their failure and concurrency modes. Each check looks at the exit code and at text in stdout
// or stderr. A group whose executable is not built is skipped. build.sh --test runs it on Linux; on
// Windows (out/*.exe) it checks the probe too, except the Linux signal exit code.
// Usage: out/tool-checks [gow3_pc folder] (default: the current folder, as build.sh --test runs it)
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using Bytes = std::string;

#ifdef _WIN32
static const char* const kExe = ".exe";
#else
static const char* const kExe = "";
#endif

struct Result {
    int code = -1;  // exit code; minus the signal number when a signal ended it
    std::string out, err;
    bool timed_out = false;
};

static std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

static void write_file(const fs::path& path, const Bytes& data) {
    std::ofstream(path, std::ios::binary) << data;
}

// A fresh folder for one check, removed again when the check ends.
struct TempDir {
    fs::path path;
    TempDir() {
        static unsigned counter;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (;;) {
            path = fs::temp_directory_path() /
                   ("tool-checks-" + std::to_string(stamp) + "-" + std::to_string(++counter));
            if (fs::create_directory(path)) break;
        }
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

// Runs exe with args; stdout and stderr go to files in a temporary folder and are read back.
static Result run(const fs::path& exe, const std::vector<std::string>& args, int timeout_seconds) {
    TempDir dir;
    const fs::path out = dir.path / "stdout", err = dir.path / "stderr";
    Result r;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
#ifdef _WIN32
    std::string line = '"' + exe.string() + '"';
    for (const std::string& arg : args) line += " \"" + arg + '"';
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE out_file = CreateFileW(out.c_str(), GENERIC_WRITE, 0, &inherit, CREATE_ALWAYS, 0, nullptr);
    HANDLE err_file = CreateFileW(err.c_str(), GENERIC_WRITE, 0, &inherit, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_file;
    si.hStdError = err_file;
    PROCESS_INFORMATION pi{};
    const bool started = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(out_file);
    CloseHandle(err_file);
    if (!started) {
        r.err = "cannot start " + exe.string();
        return r;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (WaitForSingleObject(pi.hProcess, DWORD(left.count() > 0 ? left.count() : 0)) == WAIT_TIMEOUT) {
        r.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.code = int(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    const pid_t pid = fork();
    if (pid < 0) {
        r.err = "cannot start " + exe.string();
        return r;
    }
    if (pid == 0) {
        const int out_fd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int err_fd = open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0 || err_fd < 0 || dup2(out_fd, 1) < 0 || dup2(err_fd, 2) < 0) _exit(127);
        std::vector<char*> argv{const_cast<char*>(exe.c_str())};
        for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            r.timed_out = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? -WTERMSIG(status) : -1;
#endif
    r.out = read_file(out);
    r.err = read_file(err);
    return r;
}

static int failures, passes, skips;

// One check: expected exit code, text that must (or must not) appear in stdout or stderr.
struct Expect {
    using Texts = std::vector<std::string>;
    int code;
    Texts out_has, err_has, out_lacks;
    Expect(int code, Texts out_has = {}, Texts err_has = {}, Texts out_lacks = {})
        : code(code), out_has(std::move(out_has)), err_has(std::move(err_has)), out_lacks(std::move(out_lacks)) {}
};

static void check(const std::string& name, const Result& r, const Expect& e) {
    std::string problem;
    if (r.timed_out) problem = "timed out";
    else if (r.code != e.code) problem = "exit code " + std::to_string(r.code) + ", expected " + std::to_string(e.code);
    for (const std::string& text : e.out_has)
        if (problem.empty() && r.out.find(text) == std::string::npos) problem = "stdout lacks \"" + text + '"';
    for (const std::string& text : e.err_has)
        if (problem.empty() && r.err.find(text) == std::string::npos) problem = "stderr lacks \"" + text + '"';
    for (const std::string& text : e.out_lacks)
        if (problem.empty() && r.out.find(text) != std::string::npos) problem = "stdout has \"" + text + '"';
    if (problem.empty()) {
        ++passes;
        std::printf("PASS %s\n", name.c_str());
        return;
    }
    ++failures;
    std::printf("FAIL %s: %s\n--- stdout\n%s--- stderr\n%s---\n", name.c_str(), problem.c_str(), r.out.c_str(),
                r.err.c_str());
}

static bool built(const fs::path& exe, const char* group, const char* hint) {
    if (fs::exists(exe)) return true;
    ++skips;
    std::printf("SKIP %s: %s (%s not built)\n", group, hint, exe.string().c_str());
    return false;
}

// Little-endian packing of the probe's image formats.
static void put(Bytes& b, uint64_t value) {
    for (int i = 0; i < 8; ++i) b += char(value >> (8 * i));
}
static void put32(Bytes& b, uint32_t value) {
    for (int i = 0; i < 4; ++i) b += char(value >> (8 * i));
}
static Bytes padded(Bytes data, size_t size) {
    if (data.size() < size) data.resize(size, '\0');
    return data;
}

struct Reloc {
    uint64_t offset, kind;
    int64_t addend, symbol;
};

// G3PROBE1 (G3PROBE2 with capabilities): one RX segment of at least 4096 bytes, relocations and import names.
static Bytes package(const Bytes& code, const std::vector<Reloc>& relocs = {}, const std::vector<std::string>& names = {},
                     int64_t capabilities = -1) {
    const Bytes image = padded(code, 4096);
    Bytes b = capabilities < 0 ? "G3PROBE1" : "G3PROBE2";
    for (uint64_t v : {uint64_t(image.size()), uint64_t(0), uint64_t(1), uint64_t(relocs.size()), uint64_t(names.size())})
        put(b, v);
    if (capabilities >= 0) put(b, uint64_t(capabilities));
    for (uint64_t v : {uint64_t(0), uint64_t(image.size()), uint64_t(5)}) put(b, v);
    for (const std::string& name : names) b += padded(name, 128);
    for (const Reloc& r : relocs) {
        put(b, r.offset);
        put(b, r.kind);
        put(b, uint64_t(r.addend));
        put(b, uint64_t(r.symbol));
    }
    return b + image;
}

struct Native {
    std::string name = "fixture-native";
    uint64_t binding_address = 4112, binding_kind = 1, lib_flags = 5;
    Bytes init = Bytes("\x31\xc0\xc3", 3), native = "\xc3";
    std::vector<uint64_t> metadata = {4096, 8192, 4096, 8192, 32, 4, 1, 256};
};

// Main calls an import, then tail-jumps to the terminal diagnostic import.
static const Bytes kMain("\x48\x83\xec\x08\xff\x15\x16\0\0\0\x48\x83\xc4\x08\xff\x25\x14\0\0\0", 20);

// G3PROBE3: the game image plus a linked native module (initializer at 4096, export at 4112).
static Bytes native_package(const Native& n) {
    const Bytes image = padded(padded(kMain, 4096) + padded(n.init, 16) + n.native, 12288);
    Bytes b = "G3PROBE3";
    for (uint64_t v : {uint64_t(image.size()), uint64_t(0), uint64_t(3), uint64_t(2), uint64_t(2), uint64_t(1)}) put(b, v);
    for (uint64_t v : n.metadata) put(b, v);
    for (uint64_t v : {uint64_t(0), n.binding_address, n.binding_kind}) put(b, v);
    for (uint64_t v : {uint64_t(0), uint64_t(4096), uint64_t(5), uint64_t(4096), uint64_t(4096), n.lib_flags,
                       uint64_t(8192), uint64_t(4096), uint64_t(6)})
        put(b, v);
    b += padded(n.name, 128) + padded("after-native", 128);
    for (uint64_t v : {32, 1, 0, 0, 40, 1, 1, 0}) put(b, v);
    return b + image;
}

static fs::path probe;

// Writes the image to a temporary boot.bin and runs the loader on it without the GPU.
static Result run_image(const Bytes& image, const std::vector<std::string>& options = {}) {
    TempDir dir;
    const fs::path boot = dir.path / "boot.bin";
    write_file(boot, image);
    std::vector<std::string> args{boot.string(), "--cpu-only"};
    args.insert(args.end(), options.begin(), options.end());
    return run(probe, args, 5);
}

static void probe_checks() {
    const Bytes ret = "\xc3", ud2 = "\x0f\x0b", jump_import("\xff\x25\x02\0\0\0\x90\x90", 8);
    const std::string init_env = "bzQExy189ZI#libc";

    const Bytes header("G3CONT01", 8);
    Bytes v2 = header, v3 = header;
    for (uint32_t v : {2, 0, 0, 0, 0}) put32(v2, v);
    for (uint32_t v : {3, 0, 0, 0, 0}) put32(v3, v);
    int profile = 0;
    for (const Bytes& data : {header, v2, v3 + "extra"}) {
        TempDir dir;
        const fs::path path = dir.path / "content.bin";
        write_file(path, data);
        check("probe: invalid content profile is rejected #" + std::to_string(++profile),
              run_image(package(ret), {"--content-profile", path.string()}), {1, {}, {"content profile"}});
    }

    check("probe: original instruction reaches named import",
          run_image(package(jump_import, {{8, 1, 0, 0}}, {"fixture-import"})),
          {20, {"first unsupported PS4 import: fixture-import"}});

    check("probe: native initializer and export return", run_image(native_package({})),
          {20, {"Module 0 initializer returned 0", "first unsupported PS4 import: after-native"}});

    Native host_contract;
    host_contract.name = init_env;
    host_contract.native = ud2;
    check("probe: host contract takes priority over native export", run_image(native_package(host_contract)),
          {20, {"_init_env returned", "first unsupported PS4 import: after-native"}});

    Native faulting_init;
    faulting_init.init = ud2;
    check("probe: strict mode skips native initialization and binding",
          run_image(native_package(faulting_init), {"--strict-imports"}),
          {20, {"first unsupported PS4 import: fixture-native"}, {}, {"Starting native libc"}});

    Native bad_address, bad_kind, not_executable, bad_metadata, bad_procparam;
    bad_address.binding_address = 12288;
    bad_kind.binding_kind = 2;
    not_executable.lib_flags = 4;
    bad_metadata.metadata = {4096, 8192, 4096, 8192, 32, 33, 1, 256};
    bad_procparam.metadata = {4096, 8192, 4096, 8192, 32, 4, 1, 12280};
    for (const auto& [native, message] : std::initializer_list<std::pair<Native, std::string>>{
             {bad_address, "invalid native binding"},
             {bad_kind, "native export kind/range mismatch"},
             {not_executable, "native function is not executable"},
             {bad_metadata, "invalid linked module metadata"},
             {bad_procparam, "unmapped procparam"}})
        check("probe: native metadata and bindings are validated (" + message + ")", run_image(native_package(native)),
              {1, {}, {message}});

    Native failing_init;
    failing_init.init = Bytes("\xb8\x01\0\0\0\xc3", 6);
    check("probe: failed native initializer does not enter game", run_image(native_package(failing_init)),
          {1, {}, {"module initializer failed"}, {"Entering original"}});

    // Jump via a relative relocation to code at +16, then to an import at +24.
    check("probe: base-relative address is relocated",
          run_image(package(jump_import + Bytes(8, '\0') + jump_import, {{8, 0, 16, 0}, {24, 1, 0, 0}},
                            {"after-relative-jump"})),
          {20, {"after-relative-jump"}});

    check("probe: out-of-bounds relocation is rejected", run_image(package(ret, {{4092, 0, 0, 0}})),
          {1, {}, {"bad relocation"}});

    const Bytes whole = package(ret);
    check("probe: truncated image is rejected", run_image(whole.substr(0, whole.size() - 1)),
          {1, {}, {"incorrect memory image size"}});

#ifdef _WIN32
    ++skips;  // 132 = 128 + SIGILL: the Linux signal path; Windows reports the exception differently
    std::printf("SKIP probe: illegal instruction exit code 132 is checked on Linux only\n");
#else
    check("probe: illegal instruction reports guest offset", run_image(package(ud2)),
          {132, {}, {"at guest offset 0x0,"}});
#endif

    // Align the stack, call _init_env through +32, then tail-jump to the unknown import at +40.
    const Bytes init_then_unknown = package(kMain, {{32, 1, 0, 0}, {40, 1, 1, 0}}, {init_env, "after-init"}, 1);
    check("probe: runtime returns from verified _init_env", run_image(init_then_unknown),
          {20, {"first unsupported PS4 import: after-init", "_init_env=1"}});
    check("probe: strict imports stop at _init_env", run_image(init_then_unknown, {"--strict-imports"}),
          {20, {"first unsupported PS4 import: " + init_env}});

    // The code would fault if the game ran.
    check("probe: check-imports lists missing imports without running",
          run_image(package(ud2, {{32, 1, 0, 0}, {40, 1, 1, 0}}, {init_env, "missing#libSceFixture"}, 1),
                    {"--check-imports"}),
          {1, {"MISSING: missing#libSceFixture"}, {}, {"bzQExy189ZI"}});
    check("probe: check-imports passes when nothing is missing",
          run_image(package(ud2, {{32, 1, 0, 0}}, {init_env}, 1), {"--check-imports"}), {0, {"Imports: 0 missing"}});

    check("probe: unverified runtime stays disabled", run_image(package(jump_import, {{8, 1, 0, 0}}, {init_env}, 0)),
          {20, {"_init_env=0"}});

    check("probe: unknown capabilities are rejected", run_image(package(ret, {}, {}, 2)),
          {1, {}, {"unknown runtime capabilities"}});
}

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? argv[1] : ".";
    probe = fs::absolute(root / (std::string("out/gow3-probe") + kExe)).lexically_normal();
    if (built(probe, "probe", "build native probe first")) probe_checks();

    const fs::path runtime = fs::absolute(root / (std::string("out/runtime-test") + kExe)).lexically_normal();
    if (built(runtime, "runtime", "run bash build.sh --test first")) {
        check("runtime: contracts", run(runtime, {}, 10), {0, {"PASS: callback lifecycle"}});
        check("runtime: recursive guard fails explicitly", run(runtime, {"--guard-recursion"}, 10),
              {21, {}, {"recursive/concurrent static initialization"}});
        check("runtime: stack corruption is not ignored", run(runtime, {"--stack-failure"}, 10),
              {22, {}, {"stack protector detected corruption"}});
        check("runtime: TLS bounds are checked", run(runtime, {"--bad-tls"}, 10),
              {21, {}, {"unsupported TLS module/offset"}});
        check("runtime: rwlock lifecycle and ownership", run(runtime, {"--rwlock-lifecycle"}, 10), {0});
        check("runtime: rwlock readers exclude the writer", run(runtime, {"--rwlock-concurrency"}, 10), {0});
        check("runtime: rwlock timeouts use Orbis errors", run(runtime, {"--rwlock-timeouts"}, 10), {0});
    }

    const fs::path sema = fs::absolute(root / (std::string("out/sema-test") + kExe)).lexically_normal();
    if (built(sema, "sema", "run bash build.sh --test first")) {
        check("sema: counts, errors, timeout and stale ids", run(sema, {}, 10), {0});
        check("sema: FIFO wakeup and token reservation", run(sema, {"--concurrency"}, 10), {0});
        check("sema: cancel and delete wake all waiters", run(sema, {"--cancel-delete"}, 10), {0});
        check("sema: priority wait blocks until signal", run(sema, {"--priority"}, 10), {0});
    }

    std::printf("tool-checks: %d passed, %d failed, %d skipped\n", passes, failures, skips);
    return failures ? 1 : 0;
}
