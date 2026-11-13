// importer/ps3keep.hpp: "keep only the PS3 files updates need" (see ps3keep.cpp).
#pragma once
#include "importer.hpp"

namespace u3imp {

using LogFn = std::function<void(const std::string &)>;
using EventFn = std::function<void(const Value &)>;

Value ps3keep_manifest(const fs::path &data);
std::vector<fs::path> ps3keep_roots(const fs::path &data);          // the kept folders to add after the player's own
void ps3keep_record(const std::vector<fs::path> &roots, const fs::path &data);   // record the PS3 files this import reads
Value ps3keep_keep(const fs::path &data, const std::vector<fs::path> &roots, const std::string &elf, const LogFn &lg, const EventFn &ev);
void ps3keep_drop(const fs::path &data, const LogFn &lg);

}  // namespace u3imp
