// dctools/u3fix.hpp: the steps of u3fix.exe (see u3fix.cpp)
#pragma once
#include "dc.hpp"

namespace u3 {

fs::path upath(const std::string &utf8_path);

// --flag and --name value arguments
struct Args {
    Args(const std::vector<std::string> &argv, const std::set<std::string> &flags, const std::set<std::string> &values);
    bool has(const std::string &f) const { return set.count(f) != 0; }
    std::string need(const std::string &k) const;
    std::string get(const std::string &k, const std::string &def) const;
    std::set<std::string> set;
    std::map<std::string, std::string> val;
};

int remove_lab(const std::vector<std::string> &argv);         // tools/fixes/no-lab/remove_lab.cpp
int remove_rpcn_page(const std::vector<std::string> &argv);   // tools/fixes/no-rpcn-menu/remove_rpcn_page.cpp
int netgame_order(const std::vector<std::string> &argv);      // tools/fixes/netgame-order/netgame_order.cpp

}  // namespace u3
