// dctools/u3fix.cpp: u3fix.exe, the C++ recipe steps of the Uncharted 3 import.
//
//   u3fix <step> [options]
//     remove-lab        --project P [--actors lab_actors.txt] [--apply]       (tools/fixes/no-lab)
//     remove-rpcn-page  --project P [--apply]                                 (tools/fixes/no-rpcn-menu)
//     netgame-order     --project P --net18 F [--apply | --verify] [...]      (tools/fixes/netgame-order)
//
// The package builds it into u3mp/bin (tools/package_windows.sh), which the importer puts on PATH; a recipe step runs
// it as ["u3fix.exe", "<step>", ...]. Every step is a dry run without --apply and exits 0 'already applied' when
// there is nothing to do; a problem prints its reason and exits 1 with nothing written.
#include "u3fix.hpp"

#include <cstdio>
#include <windows.h>

namespace u3 {

fs::path upath(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

Args::Args(const std::vector<std::string> &argv, const std::set<std::string> &flags, const std::set<std::string> &values) {
    for (size_t i = 0; i < argv.size(); i++) {
        const std::string &a = argv[i];
        if (flags.count(a)) { set.insert(a); continue; }
        if (values.count(a) && i + 1 < argv.size()) { val[a] = argv[++i]; continue; }
        throw Stop("unknown or incomplete argument: " + a);
    }
}

std::string Args::need(const std::string &k) const {
    auto it = val.find(k);
    if (it == val.end() || it->second.empty()) throw Stop("missing " + k);
    return it->second;
}

std::string Args::get(const std::string &k, const std::string &def) const {
    auto it = val.find(k);
    return it == val.end() ? def : it->second;
}

}  // namespace u3

namespace {
std::string narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
}  // namespace

int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> a;
    for (int i = 2; i < argc; i++) a.push_back(narrow(argv[i]));
    std::string step = argc > 1 ? narrow(argv[1]) : "";
    try {
        if (step == "remove-lab") return u3::remove_lab(a);
        if (step == "remove-rpcn-page") return u3::remove_rpcn_page(a);
        if (step == "netgame-order") return u3::netgame_order(a);
        std::fprintf(stderr, "usage: u3fix remove-lab | remove-rpcn-page | netgame-order [options]\n");
        return 2;
    } catch (const u3::Stop &e) {
        std::fflush(stdout);
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    } catch (const std::exception &e) {
        std::fflush(stdout);
        std::fprintf(stderr, "%s: %s\n", step.c_str(), e.what());
        return 1;
    }
}
