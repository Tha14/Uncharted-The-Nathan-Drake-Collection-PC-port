// shortcuts.cpp - TOOLS' desktop shortcuts, part of uncharted_native.dll: the launcher's, and one per game that starts it
// directly ("Uncharted Launcher.exe --play u1"). Made with Windows' shell link object (IShellLinkW), as the launcher's
// PowerShell script made them through WScript.Shell before.
//
//   un_shortcuts(root, request_json, out, cap)
//     request: {"which": "launcher" | "games", "games": [{"key", "name"}] (for "games"), "python": the Python that runs
//     the launcher and "script" its .pyw (used when the package has no "Uncharted Launcher.exe"), "folder": where the
//     shortcuts go (default: the Desktop, SHGetKnownFolderPath as system.cpp finds it)}
//     out: {"kind": "info" | "error", "text": what to tell the player}
#include "native.h"
#include "json.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using json::Value;

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

std::wstring wide(const std::string &s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string utf8(const std::wstring &w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool is_file(const std::wstring &p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string why(HRESULT hr) {
    wchar_t *text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   (DWORD)hr, 0, (LPWSTR)&text, 0, nullptr);
    std::wstring w = text ? text : L"";
    if (text) LocalFree(text);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
    char code[16];
    wsprintfA(code, "0x%08X", (unsigned)hr);
    return w.empty() ? std::string("error ") + code : utf8(w) + " (" + code + ")";
}

// one .lnk; "" when saved, else why not
std::string save_link(const std::wstring &link, const std::wstring &target, const std::wstring &args, const std::wstring &dir,
                      const std::wstring &icon, const std::wstring &description) {
    IShellLinkW *shell = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&shell);
    if (FAILED(hr)) return why(hr);
    shell->SetPath(target.c_str());
    shell->SetArguments(args.c_str());
    shell->SetWorkingDirectory(dir.c_str());
    shell->SetIconLocation(icon.c_str(), 0);
    shell->SetDescription(description.c_str());
    IPersistFile *file = nullptr;
    hr = shell->QueryInterface(IID_IPersistFile, (void **)&file);
    if (SUCCEEDED(hr)) {
        hr = file->Save(link.c_str(), TRUE);
        file->Release();
    }
    shell->Release();
    return SUCCEEDED(hr) ? "" : why(hr);
}

std::string desktop() {
    std::string out(1 << 12, '\0');
    if (un_system("desktop", L"", &out[0], (int)out.size()) != 0) return "";
    out.resize(std::strlen(out.c_str()));
    return out;
}

Value shortcuts(const std::wstring &root, const Value &request) {
    auto text = [&](const char *key) { const Value *v = request.get(key); return v && v->kind == Value::String ? v->s : ""; };
    const std::string which = text("which");
    std::string folder = text("folder");
    if (folder.empty()) folder = desktop();
    const std::wstring starter = root + L"\\Uncharted Launcher.exe";
    // the target: the packaged build's starter, else the launcher's Python with its script
    std::wstring target = starter, base_args;
    if (!is_file(starter)) {
        const std::wstring python = wide(text("python"));
        const std::wstring pythonw = python.substr(0, python.find_last_of(L"\\/") + 1) + L"pythonw.exe";
        target = is_file(pythonw) ? pythonw : python;
        base_args = L"\"" + wide(text("script")) + L"\"";
    }
    const std::wstring icon = root + L"\\launcher\\assets\\uncharted.ico";
    struct Link { std::string name, description, play; };
    std::vector<Link> links;
    if (which == "games") {
        if (const Value *games = request.get("games"))
            for (auto &g : games->items) {
                const Value *k = g.get("key"), *n = g.get("name");
                if (!k || !n) continue;
                std::string name = n->s;
                for (size_t at; (at = name.find(':')) != std::string::npos;) name.replace(at, 1, " -");
                links.push_back({name, n->s + " (opens directly)", k->s});
            }
    } else {
        links.push_back({"Uncharted", "Uncharted: The Nathan Drake Collection", ""});
    }
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<std::string> made, failed;
    std::string first_link;
    for (auto &l : links) {
        const std::wstring link = wide(folder) + L"\\" + wide(l.name) + L".lnk";
        std::wstring args = base_args;
        if (!l.play.empty()) args = (args.empty() ? L"" : args + L" ") + L"--play " + wide(l.play);
        const std::string error = save_link(link, target, args, root, icon, wide(l.description));
        if (error.empty()) {
            made.push_back(l.name + ".lnk");
            if (first_link.empty()) first_link = utf8(link);
        } else {
            failed.push_back(l.name + ".lnk: " + error);
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    Value out = Value::object();
    auto lines = [](const std::vector<std::string> &v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) s += (i ? "\n" : "") + v[i];
        return s;
    };
    if (!failed.empty()) {
        out.set("kind", Value::text("error"));
        out.set("text", Value::text((which == "games" ? "The shortcuts couldn't be created:\n" : "The shortcut couldn't be created:\n") +
                                    lines(failed)));
    } else if (which == "games") {
        out.set("kind", Value::text("info"));
        out.set("text", Value::text("These shortcuts are now on your desktop:\n" + lines(made) +
                                    "\n\nEach one opens its game right away with the launcher's settings. A command "
                                    "prompt does the same with \"Uncharted Launcher.exe\" --play u1 (or u2, u3)."));
    } else {
        out.set("kind", Value::text("info"));
        out.set("text", Value::text("The shortcut is on your desktop:\n" + first_link));
    }
    return out;
}

}  // namespace

extern "C" {

UN_API int un_shortcuts(const wchar_t *root, const char *request_json, char *out, int cap) {
    Value request;
    if (!json::parse(request_json ? request_json : "{}", request) || !request.is_object()) return 3;
    return give(json::dump(shortcuts(root ? root : L".", request), false), out, cap);
}

}  // extern "C"
