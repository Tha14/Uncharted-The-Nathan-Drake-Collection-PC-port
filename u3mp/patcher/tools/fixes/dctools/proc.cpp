// dctools/proc.cpp: see proc.hpp
#include "proc.hpp"

#include <algorithm>
#include <thread>
#include <windows.h>

namespace u3 {

std::wstring wide(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string P(const fs::path &p) { return win_path(utf8(p)); }

// subprocess.list2cmdline
std::wstring quote_arg(const std::wstring &a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += c; bs = 0; continue; }
        o.append(bs, L'\\');
        bs = 0;
        o += c;
    }
    o.append(bs * 2, L'\\');
    return o + L"\"";
}


namespace {
std::wstring command_line(const std::vector<std::string> &cmd) {
    std::wstring line;
    for (const std::string &a : cmd) line += (line.empty() ? L"" : L" ") + quote_arg(wide(a));
    return line;
}
// this process's environment with the overrides (an empty value removes the variable), as CreateProcessW wants it
std::wstring env_block(const Env &env) {
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    if (wchar_t *blk = GetEnvironmentStringsW()) {
        for (const wchar_t *p = blk; *p; p += wcslen(p) + 1) {
            std::wstring kv = p;
            const size_t eq = kv.find(L'=', 1);
            if (eq != std::wstring::npos) vars.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
        }
        FreeEnvironmentStringsW(blk);
    }
    auto up = [](std::wstring s) { for (wchar_t &c : s) c = towupper(c); return s; };
    for (const auto &[k, v] : env) {
        const std::wstring wk = wide(k);
        bool set = false;
        for (auto &kv : vars)
            if (up(kv.first) == up(wk)) { kv.second = wide(v); set = true; }
        if (!set) vars.push_back({wk, wide(v)});
    }
    std::sort(vars.begin(), vars.end(), [&](const auto &x, const auto &y) { return up(x.first) < up(y.first); });
    std::wstring block;
    for (const auto &[k, v] : vars)
        if (!v.empty()) block += k + L"=" + v + L'\0';
    block += L'\0';
    return block;
}
}  // namespace

// a child process; RuntimeError text as levels.py's run() on a non-zero exit
std::string run(const std::vector<std::string> &cmd, const Env &env, const std::string &cwd) {
    std::wstring line = command_line(cmd), block = env_block(env);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out_r, out_w, err_r, err_w;
    CreatePipe(&out_r, &out_w, &sa, 0);
    CreatePipe(&err_r, &err_w, &sa, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    PROCESS_INFORMATION pi{};
    const std::wstring wcwd = wide(cwd);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, block.data(),
                        cwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        CloseHandle(out_r); CloseHandle(out_w); CloseHandle(err_r); CloseHandle(err_w);
        throw Stop("cannot start " + cmd[0]);
    }
    CloseHandle(out_w);
    CloseHandle(err_w);
    std::string out, err;
    auto pump = [](HANDLE h, std::string *s) {
        char buf[65536];
        DWORD n;
        while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) s->append(buf, n);
    };
    std::thread te(pump, err_r, &err);
    pump(out_r, &out);
    te.join();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(out_r); CloseHandle(err_r);
    for (std::string *s : {&out, &err})   // text mode
        for (size_t k; (k = s->find("\r\n")) != std::string::npos;) s->replace(k, 2, "\n");
    if (code) {
        const std::string script = cmd.size() > 1 ? cmd[1].substr(cmd[1].find_last_of("/\\") == std::string::npos ? 0 : cmd[1].find_last_of("/\\") + 1) : cmd[0];
        throw Stop(fmt("%s failed (%lu): %s %s", script.c_str(), (unsigned long)code, out.substr(out.size() > 800 ? out.size() - 800 : 0).c_str(),
                       err.substr(err.size() > 1500 ? err.size() - 1500 : 0).c_str()));
    }
    return out;
}


// a child fed `input` on stdin, its stdout / stderr captured as bytes (subprocess.run(input=..., capture_output=True))
int run_pipe(const std::vector<std::string> &cmd, const Bytes &input, Bytes &out, std::string &err) {
    std::wstring line = command_line(cmd), block = env_block({});
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r, in_w, out_r, out_w, err_r, err_w;
    CreatePipe(&in_r, &in_w, &sa, 0);
    CreatePipe(&out_r, &out_w, &sa, 0);
    CreatePipe(&err_r, &err_w, &sa, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, block.data(), nullptr, &si, &pi)) {
        for (HANDLE h : {in_r, in_w, out_r, out_w, err_r, err_w}) CloseHandle(h);
        throw Stop("cannot start " + cmd[0]);
    }
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    std::thread tw([&] {
        size_t o = 0;
        DWORD n;
        while (o < input.size() && WriteFile(in_w, input.data() + o, DWORD(std::min<size_t>(input.size() - o, 1 << 20)), &n, nullptr) && n) o += n;
        CloseHandle(in_w);
    });
    std::string es;
    std::thread te([&] {
        char buf[65536];
        DWORD n;
        while (ReadFile(err_r, buf, sizeof buf, &n, nullptr) && n) es.append(buf, n);
    });
    out.clear();
    {
        uint8_t buf[65536];
        DWORD n;
        while (ReadFile(out_r, buf, sizeof buf, &n, nullptr) && n) out.insert(out.end(), buf, buf + n);
    }
    tw.join();
    te.join();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(out_r); CloseHandle(err_r);
    err = es;
    return int(code);
}

std::string text_file(const fs::path &p) {   // read in text mode
    const Bytes b = read_file(p);
    std::string s(b.begin(), b.end());
    for (size_t k; (k = s.find("\r\n")) != std::string::npos;) s.replace(k, 2, "\n");
    return s;
}
void write_text(const fs::path &p, const std::string &s) {   // written in text mode: CRLF
    Bytes b;
    for (char c : s) {
        if (c == '\n') b.push_back('\r');
        b.push_back(uint8_t(c));
    }
    write_file_replace(p, b);
}
std::string sha_file(const fs::path &p) { return sha256_hex(read_file(p)); }

// json.dump(v, f): one line, ', ' and ': ' separators
void dump_compact(std::string &o, const json::Value &v) {
    switch (v.kind) {
    case json::Value::Null: o += "null"; break;
    case json::Value::Bool: o += v.b ? "true" : "false"; break;
    case json::Value::Int: o += std::to_string(v.i); break;
    case json::Value::Float: o += json::real(v.f); break;
    case json::Value::String: json::quote(o, v.s); break;
    case json::Value::Array:
        o += "[";
        for (size_t k = 0; k < v.items.size(); ++k) { if (k) o += ", "; dump_compact(o, v.items[k]); }
        o += "]";
        break;
    case json::Value::Object:
        o += "{";
        for (size_t k = 0; k < v.fields.size(); ++k) {
            if (k) o += ", ";
            json::quote(o, v.fields[k].first);
            o += ": ";
            dump_compact(o, v.fields[k].second);
        }
        o += "}";
        break;
    }
}
std::string env_text(const json::Value &v) {   // an environment value from the overrides file
    if (v.kind == json::Value::String) return v.s;
    std::string o;
    dump_compact(o, v);
    return o;
}

Worker::Worker(const std::vector<std::string> &cmd, const Env &env) {
    std::wstring line = command_line(cmd), block = env_block(env);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r, in_w, out_r, out_w;
    CreatePipe(&in_r, &in_w, &sa, 0);
    CreatePipe(&out_r, &out_w, &sa, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);   // as Popen without stderr=: the worker's tracebacks reach the log
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, block.data(), nullptr, &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        throw Stop("cannot start " + cmd[0]);
    }
    CloseHandle(pi.hThread);
    proc = pi.hProcess;
    in = in_w;
    out = out_r;
}

Worker::~Worker() {
    if (in) CloseHandle(in);   // the worker's stdin ends: it exits
    if (proc) {
        if (WaitForSingleObject(proc, 10000) != WAIT_OBJECT_0) TerminateProcess(proc, 1);
        CloseHandle(proc);
    }
    if (out) CloseHandle(out);
}

bool Worker::request(const std::string &line, std::string &answer) {
    const std::string l = line + "\n";
    DWORD n;
    if (!WriteFile(in, l.data(), DWORD(l.size()), &n, nullptr) || n != l.size()) return false;
    for (;;) {
        const size_t nl = buf.find('\n');
        if (nl != std::string::npos) {
            answer = buf.substr(0, nl);
            if (!answer.empty() && answer.back() == '\r') answer.pop_back();
            buf.erase(0, nl + 1);
            return true;
        }
        char tmp[65536];
        if (!ReadFile(out, tmp, sizeof tmp, &n, nullptr) || !n) return false;
        buf.append(tmp, n);
    }
}

}  // namespace u3
