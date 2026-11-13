// importer/mpimage.hpp: the multiplayer image (see mpimage.cpp).
#pragma once
#include "importer.hpp"

#include <stdexcept>

namespace u3imp {

struct ImageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// the new image would have no online profile keys (no usable EBOOT.elf, no previous image): the previous image stays
struct KeysMissing : ImageError {
    using ImageError::ImageError;
};
struct KeyError : std::runtime_error {   // the EBOOT is not the tested 1.19 one, or a value is not where it should be
    using std::runtime_error::runtime_error;
};

using LogFn = std::function<void(const std::string &)>;

struct BuildInfo {
    fs::path folder;
    Value info;   // build.json
};
struct G3Layout {
    uint64_t size = 0, entry = 0, ns = 0, nr = 0, ni = 0;
    size_t segpos = 0, namespos = 0, imagepos = 0;
};

std::string latest_version(const fs::path &patcher);
BuildInfo build_info(const fs::path &patcher, const std::string &version);
G3Layout parse_g3(const Bytes &raw);
Bytes prepare_base(const fs::path &game, const fs::path &work, const BuildInfo &b, const LogFn &lg, const std::optional<fs::path> &update);
Bytes apply_menu_shell(const Bytes &raw, const Value &delta);
Bytes apply_link(const Bytes &shell, const Value &link);
Bytes ps4_elf(const Bytes &raw);
// fills the helper's key block: 'eboot' | 'carried' | 'none' | 'not needed'
std::string with_keys(Bytes &image, const std::string &ps3_elf, const fs::path &previous, const LogFn &lg);
// steps 1-4: writes <out>/boot-linked.bin; returns {version, sha256 (the release image's), path, base_sha256, file_sha256, keys}
Value build_image(const fs::path &patcher, const fs::path &game, const fs::path &out, const LogFn &lg, const std::optional<fs::path> &elf_out,
                  const std::optional<fs::path> &update, const std::string &ps3_elf, std::string version = "");

}  // namespace u3imp
