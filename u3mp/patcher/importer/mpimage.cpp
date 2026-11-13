// importer/mpimage.cpp: the Uncharted 3 multiplayer image, made on the player's PC from the player's own game. No game
// bytes are shipped; every stage is checked by sha256.
//   1. base         the launcher's own prepare + module link (launcher/native/prepare.cpp, link.cpp) on the player's
//                   retail big3-ps4_Shipping.elf (CUSA02320 / CUSA02343 1.02): the PC runtime's prepared image (G3PROBE5)
//   2. menu shell   build/<version>/menu-shell.json applied to it: the port's code patches and menu-flow stubs (bytes we
//                   wrote; the bytes they replace are only checked by hash) and the menu-flow segment of our helpers
//   3. MP image     build/<version>/link.json applied to the menu shell: the multiplayer helper's link (our code, its
//                   hooks and segments; the bytes it replaces are only checked by hash), made by the helper build
//   4. keys         the helper's online profile key block, filled from the player's decrypted PS3 1.19 EBOOT.elf (or
//                   carried over from the previous image)
// Also the recipe's PS4 ELF (ps4_elf), made from the base image, never shipped.
#include "mpimage.hpp"

#include "native.h"   // launcher/native: un_prepare (prepare.cpp)

// launcher/native/link.cpp (not declared in native.h)
extern "C" UN_API int un_link_modules(const wchar_t *game, const wchar_t *out_dir, const char *executable, const char *modules, char *out, int cap);

#include <windows.h>

#include <cstring>

namespace u3imp {

namespace {

const char *EXECUTABLE = "big3-ps4_Shipping.elf";

uint64_t q64(const Bytes &b, size_t o) {
    if (o + 8 > b.size()) throw ImageError("invalid prepared image layout");
    uint64_t v;
    std::memcpy(&v, &b[o], 8);
    return v;
}
void put64(Bytes &b, size_t o, uint64_t v) { std::memcpy(&b[o], &v, 8); }

Bytes unhex(const std::string &h) {
    Bytes b;
    for (size_t i = 0; i + 1 < h.size(); i += 2) b.push_back(uint8_t(std::stoul(h.substr(i, 2), nullptr, 16)));
    return b;
}
std::string b64decode(const std::string &s) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue;
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((acc >> bits) & 0xff);
        }
    }
    return out;
}
uint64_t hexnum(const std::string &s) { return std::stoull(s, nullptr, 16); }
std::string text_of(const Value &o, const char *k) {
    const Value *v = o.get(k);
    return v && v->kind == Value::String ? v->s : "";
}

// the native code's call convention: 0 = fits, -n = call again with n bytes, 1 = failed with the reason in out
template <class F>
std::pair<int, std::string> call_native(F f) {
    int cap = 1 << 16;
    while (true) {
        std::string buf(size_t(cap), '\0');
        const int st = f(buf.data(), cap);
        if (st < 0) {
            cap = -st;
            continue;
        }
        buf.resize(std::strlen(buf.c_str()));
        return {st, buf};
    }
}

void log_tail(const std::string &text, size_t n, const std::function<void(const std::string &)> &lg) {
    std::vector<std::string> lines;
    for (const std::string &l : split(strip(text), '\n'))
        if (!strip(l).empty() || !lines.empty()) lines.push_back(l.empty() || l.back() != '\r' ? l : l.substr(0, l.size() - 1));
    for (size_t i = lines.size() > n ? lines.size() - n : 0; i < lines.size(); i++) lg("  " + lines[i]);
}

}  // namespace

// ------------------------------------------------------------------------------------------------ builds
fs::path builds_dir(const fs::path &patcher) { return patcher / "build"; }

std::string latest_version(const fs::path &patcher) {
    std::vector<std::vector<int>> vs;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(builds_dir(patcher), ec)) {
        const std::string n = S(e.path().filename());
        if (n.size() < 2 || n[0] != 'v' || !fs::is_regular_file(e.path() / "build.json", ec)) continue;
        std::vector<int> v;
        try {
            for (const std::string &x : split(n.substr(1), '.')) v.push_back(std::stoi(x));
        } catch (const std::exception &) {
            continue;
        }
        vs.push_back(v);
    }
    if (vs.empty()) throw ImageError("no multiplayer build in " + S(builds_dir(patcher)));
    std::sort(vs.begin(), vs.end());
    std::string out;
    for (int x : vs.back()) out += (out.empty() ? "" : ".") + std::to_string(x);
    return out;
}

BuildInfo build_info(const fs::path &patcher, const std::string &version) {
    BuildInfo b;
    b.folder = builds_dir(patcher) / P("v" + version);
    b.info = load_json(b.folder / "build.json");
    return b;
}

G3Layout parse_g3(const Bytes &raw) {
    if (raw.size() < 96 || std::memcmp(raw.data(), "G3PROBE5", 8) != 0) throw ImageError("not a G3PROBE5 prepared image");
    G3Layout p;
    p.size = q64(raw, 8);
    p.entry = q64(raw, 16);
    p.ns = q64(raw, 24);
    p.nr = q64(raw, 32);
    p.ni = q64(raw, 40);
    size_t pos = 96;
    const uint64_t nm = q64(raw, pos);
    pos += 8 + nm * 56;
    const uint64_t nb = q64(raw, pos);
    pos += 8 + nb * 24;
    p.segpos = pos;
    p.namespos = pos + p.ns * 24;
    p.imagepos = p.namespos + p.ni * 128 + p.nr * 32;
    if (p.imagepos + p.size != raw.size()) throw ImageError("invalid prepared image layout");
    return p;
}

// ------------------------------------------------------------------------------------------------ 1. base image
fs::path find_retail_self(const fs::path &game, const BuildInfo &b, const std::optional<fs::path> &update) {
    const std::string want = text_of(b.info, "self_sha256");
    std::vector<std::string> seen;
    std::vector<fs::path> folders;
    if (update) folders = {*update, *update / "backup"};
    for (const fs::path &f : {game, game / "backup", game / "uroot"}) folders.push_back(f);
    std::error_code ec;
    for (const fs::path &f : folders) {
        const fs::path c = f / EXECUTABLE;
        if (!fs::is_regular_file(c, ec)) continue;
        const std::string d = sha256_file(c);
        if (d == want) return c;
        seen.push_back(fmt("%s (sha256 %s..., %llu bytes)", S(c).c_str(), d.substr(0, 12).c_str(), (unsigned long long)fs::file_size(c, ec)));
    }
    std::string list;
    for (const auto &s : seen) list += (list.empty() ? "" : "; ") + s;
    const std::string g = text_of(b.info, "game");
    throw ImageError(fmt("The Uncharted 3 executable of this PS4 game folder is not the retail %s (sha256 %s...) the multiplayer build "
                         "is made for: %s.  Supported: %s.  If you replaced it (for example with a patched PKG build), put the "
                         "original back or keep it as backup/%s.",
                         g.c_str(), want.substr(0, 12).c_str(), list.empty() ? (std::string("no ") + EXECUTABLE).c_str() : list.c_str(), g.c_str(),
                         EXECUTABLE));
}

namespace {
void copytree(const fs::path &src, const fs::path &dst) {
    fs::create_directories(dst);
    for (const auto &e : fs::recursive_directory_iterator(src)) {
        const fs::path rel = fs::relative(e.path(), src), d = dst / rel;
        if (e.is_directory()) fs::create_directories(d);
        else fs::copy_file(e.path(), d, fs::copy_options::overwrite_existing);
    }
}
}  // namespace

Bytes prepare_base(const fs::path &game, const fs::path &work, const BuildInfo &b, const LogFn &lg, const std::optional<fs::path> &update) {
    const fs::path source = find_retail_self(game, b, update);
    const fs::path view = work / "game-view";
    std::error_code ec;
    if (fs::exists(view, ec)) rmtree(view);
    fs::create_directories(view / "sce_sys");
    fs::copy_file(source, view / EXECUTABLE, fs::copy_options::overwrite_existing);
    std::optional<fs::path> modules;
    for (const std::optional<fs::path> &f : {std::optional<fs::path>(source.parent_path()), update, std::optional<fs::path>(game)})
        if (f && fs::is_directory(*f / "sce_module", ec)) {
            modules = *f / "sce_module";
            break;
        }
    if (!modules) throw ImageError("no sce_module folder in the PS4 game folder " + S(game));
    copytree(*modules, view / "sce_module");
    if (fs::is_regular_file(game / "sce_sys" / "param.sfo", ec))
        fs::copy_file(game / "sce_sys" / "param.sfo", view / "sce_sys" / "param.sfo", fs::copy_options::overwrite_existing);
    const fs::path out = work / "prepared";
    fs::create_directories(out);
    const std::wstring wv = view.wstring(), wo = out.wstring();
    auto [st1, msg1] = call_native([&](char *o, int cap) { return un_prepare(wv.c_str(), wo.c_str(), EXECUTABLE, o, cap); });
    log_tail(st1 ? "prepare failed: " + msg1 : msg1, 3, lg);
    if (st1) throw ImageError(fmt("the port's prepare step failed (exit %d)", st1));
    auto [st2, msg2] = call_native([&](char *o, int cap) { return un_link_modules(wv.c_str(), wo.c_str(), EXECUTABLE, "", o, cap); });
    log_tail(msg2, 3, lg);
    if (st2) throw ImageError(fmt("the port's module link failed (exit %d)", st2));
    Bytes raw = read_bytes(out / "boot-linked.bin");
    rmtree(view);
    const std::string d = sha256_hex(raw);
    if (d != text_of(b.info, "base_sha256"))
        throw ImageError(fmt("The port prepared a different Uncharted 3 image (sha256 %s..., expected %s...). The multiplayer build %s was made "
                             "with the image of port 0.8.x; this port version lays the game out differently, so the multiplayer patch needs "
                             "an update for it.",
                             d.substr(0, 12).c_str(), text_of(b.info, "base_sha256").substr(0, 12).c_str(), text_of(b.info, "version").c_str()));
    return raw;
}

// ------------------------------------------------------------------------------------------------ 2. menu shell
Bytes apply_menu_shell(const Bytes &raw, const Value &delta) {
    const G3Layout p = parse_g3(raw);
    if (sha256_hex(raw) != text_of(delta, "base_sha256")) throw ImageError("menu shell: unexpected base image");
    Bytes image(raw.begin() + std::ptrdiff_t(p.imagepos), raw.end());
    if (const Value *ps = delta.get("patches"))
        for (const Value &patch : ps->items) {
            const uint64_t at = hexnum(text_of(patch, "at"));
            const Bytes nw = unhex(text_of(patch, "new"));
            if (at + nw.size() > image.size() || sha256_hex(&image[at], nw.size()) != text_of(patch, "old_sha256"))
                throw ImageError(fmt("menu shell: unexpected bytes at image offset %#llx", (unsigned long long)at));
            std::copy(nw.begin(), nw.end(), image.begin() + std::ptrdiff_t(at));
        }
    const Value *seg = delta.get("segment");
    if (!seg) throw ImageError("menu shell: no segment");
    const uint64_t va = hexnum(text_of(*seg, "va")), length = uint64_t(seg->get("size")->i), prot = uint64_t(seg->get("prot")->i);
    if (va != ((p.size + 0x3fff) & ~uint64_t(0x3fff))) throw ImageError("menu shell: segment address mismatch");
    const std::string data = zlib_inflate(b64decode(text_of(*seg, "zlib_b64")));
    if (data.size() != length || sha256_hex(data) != text_of(*seg, "sha256")) throw ImageError("menu shell: segment data damaged");
    image.resize(size_t(va), 0);
    image.insert(image.end(), data.begin(), data.end());
    Bytes result(raw.begin(), raw.begin() + std::ptrdiff_t(p.namespos));
    for (uint64_t v : {va, length, prot})
        for (int k = 0; k < 8; k++) result.push_back(uint8_t(v >> (8 * k)));
    result.insert(result.end(), raw.begin() + std::ptrdiff_t(p.namespos), raw.begin() + std::ptrdiff_t(p.imagepos));
    put64(result, 8, image.size());
    put64(result, 24, p.ns + 1);
    result.insert(result.end(), image.begin(), image.end());
    parse_g3(result);
    if (sha256_hex(result) != text_of(delta, "output_sha256")) throw ImageError("menu shell: result differs from the reference (" + sha256_hex(result).substr(0, 12) + "...)");
    return result;
}

// ------------------------------------------------------------------------------------------------ 3. link
// link.json: {shell_sha256, output_sha256, output_size, ops_zlib_b64}: the ops (zlib, base64) build the linked image from
// start to end: 'C' u64 offset, u64 n = copy n bytes of the menu shell; 'I' u64 n, bytes = insert the helper build's bytes.
// Everything the shell holds (game code included) is copied on the player's PC; only the helper build's bytes ship.
Bytes apply_link(const Bytes &shell, const Value &link) {
    if (sha256_hex(shell) != text_of(link, "shell_sha256")) throw ImageError("helper link: unexpected menu shell");
    const std::string ops = zlib_inflate(b64decode(text_of(link, "ops_zlib_b64")));
    Bytes out;
    out.reserve(size_t(link.get("output_size")->i));
    auto u64 = [&](size_t p) {
        if (p + 8 > ops.size()) throw ImageError("helper link: data damaged");
        uint64_t v;
        std::memcpy(&v, &ops[p], 8);
        return v;
    };
    for (size_t p = 0; p < ops.size();) {
        const char t = ops[p++];
        if (t == 'C') {   // copy shell[off, off + n)
            const uint64_t off = u64(p), n = u64(p + 8);
            p += 16;
            if (off > shell.size() || n > shell.size() - off) throw ImageError("helper link: copy out of range");
            out.insert(out.end(), shell.begin() + std::ptrdiff_t(off), shell.begin() + std::ptrdiff_t(off + n));
        } else if (t == 'I') {   // n bytes of the helper build
            const uint64_t n = u64(p);
            p += 8;
            if (n > ops.size() - p) throw ImageError("helper link: data damaged");
            out.insert(out.end(), ops.begin() + std::ptrdiff_t(p), ops.begin() + std::ptrdiff_t(p + n));
            p += n;
        } else
            throw ImageError("helper link: data damaged");
    }
    if (out.size() != size_t(link.get("output_size")->i)) throw ImageError("helper link: wrong size");
    return out;
}

// ------------------------------------------------------------------------------------------------ 4. keys
namespace keys {
const char MAGIC[] = "U3MP-EBOOT-KEYS1";   // 16 bytes, right before the block in the helper
const size_t BLOCK = 0x10c8;
const std::string PS3_ELF_SHA256 = "aac4faa63ab1387e8aaa0b8e9e80ee87ac4ae61e0b40292f7aa7401df7b3f50a";
// (file offset, length) in the decrypted 1.19 EBOOT.elf; block offsets of each value
const std::pair<size_t, size_t> BF_KEY{0xdde3d0, 20}, HMAC_KEY{0xdde388, 64}, STORE_ACCESS{0xdddb10, 20}, STORE_SECRET{0xdddb28, 40};
const size_t OFF_BF = 0, OFF_HMAC = 0x1048, OFF_ACCESS = 0x1089, OFF_SECRET = 0x109e;

ptrdiff_t find(const Bytes &b, size_t from = 0) {
    auto it = std::search(b.begin() + std::ptrdiff_t(from), b.end(), MAGIC, MAGIC + 16);
    return it == b.end() ? -1 : (it - b.begin()) + 16;
}
uint32_t be(const Bytes &b, size_t o) { return uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3]; }
ptrdiff_t find_words(const Bytes &b, uint32_t w0, uint32_t w1) {
    const uint8_t pat[8] = {uint8_t(w0 >> 24), uint8_t(w0 >> 16), uint8_t(w0 >> 8), uint8_t(w0), uint8_t(w1 >> 24), uint8_t(w1 >> 16), uint8_t(w1 >> 8), uint8_t(w1)};
    auto it = std::search(b.begin(), b.end(), pat, pat + 8);
    return it == b.end() ? -1 : it - b.begin();
}

// The helper's Blowfish state: the EBOOT's own start tables keyed with the EBOOT's key (the standard Blowfish schedule)
std::vector<uint32_t> blowfish_state(const Bytes &elf, const Bytes &key) {
    const ptrdiff_t p0 = find_words(elf, 0x243f6a88, 0x85a308d3), s0 = find_words(elf, 0xd1310ba6, 0x98dfb5ac);
    if (p0 < 0 || s0 < 0 || size_t(s0) + 4096 > elf.size() || size_t(p0) + 72 > elf.size()) throw KeyError("the Blowfish tables are not in this EBOOT");
    uint32_t Pa[18], Sb[4][256];
    for (int i = 0; i < 18; i++) Pa[i] = be(elf, size_t(p0) + 4 * size_t(i));
    for (int b = 0; b < 4; b++)
        for (int i = 0; i < 256; i++) Sb[b][i] = be(elf, size_t(s0) + 1024 * size_t(b) + 4 * size_t(i));
    for (size_t i = 0; i < 18; i++) {
        uint32_t k = 0;
        for (size_t j = 0; j < 4; j++) k = k << 8 | key[(4 * i + j) % key.size()];
        Pa[i] ^= k;
    }
    auto f = [&](uint32_t x) { return ((Sb[0][x >> 24] + Sb[1][(x >> 16) & 255]) ^ Sb[2][(x >> 8) & 255]) + Sb[3][x & 255]; };
    auto enc = [&](uint32_t &l, uint32_t &r) {
        for (int i = 0; i < 16; i++) {
            l ^= Pa[i];
            r ^= f(l);
            std::swap(l, r);
        }
        std::swap(l, r);
        r ^= Pa[16];
        l ^= Pa[17];
    };
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2) {
        enc(l, r);
        Pa[i] = l;
        Pa[i + 1] = r;
    }
    for (int b = 0; b < 4; b++)
        for (int i = 0; i < 256; i += 2) {
            enc(l, r);
            Sb[b][i] = l;
            Sb[b][i + 1] = r;
        }
    std::vector<uint32_t> out(Pa, Pa + 18);
    for (int b = 0; b < 4; b++) out.insert(out.end(), Sb[b], Sb[b] + 256);
    return out;
}

Bytes take(const Bytes &elf, std::pair<size_t, size_t> at) {
    if (at.first + at.second > elf.size()) throw KeyError("a value is not where it should be in the EBOOT");
    Bytes v(elf.begin() + std::ptrdiff_t(at.first), elf.begin() + std::ptrdiff_t(at.first + at.second));
    if (std::find(v.begin(), v.end(), 0) != v.end()) throw KeyError("a value is not where it should be in the EBOOT");
    return v;
}

Bytes block_from_eboot(const Bytes &elf) {
    if (sha256_hex(elf) != PS3_ELF_SHA256) throw KeyError("not the tested PS3 1.19 EBOOT.elf");
    Bytes out(BLOCK, 0);
    const std::vector<uint32_t> st = blowfish_state(elf, take(elf, BF_KEY));
    for (size_t i = 0; i < st.size(); i++) std::memcpy(&out[OFF_BF + 4 * i], &st[i], 4);
    for (const auto &[off, at] : {std::pair{OFF_HMAC, HMAC_KEY}, std::pair{OFF_ACCESS, STORE_ACCESS}, std::pair{OFF_SECRET, STORE_SECRET}}) {
        const Bytes v = take(elf, at);
        std::copy(v.begin(), v.end(), out.begin() + std::ptrdiff_t(off));
    }
    return out;
}

// Images from before v7.97 carry the same values at fixed places (every player's image of one build is the same file):
// release image sha256 -> offsets of (Blowfish state, HMAC key, access key, secret). Offsets only.
struct Legacy {
    const char *sha;
    size_t bf, hk, ak, sk;
};
const Legacy LEGACY[] = {
    {"052fed4ee4f28d3c074225314a4e910d20ab6fb285b04b50492c985cf7eb4457", 0x22b8bd8, 0x16dc9f2, 0x17013ba, 0x235c378},
    {"223914ded5f276f3e9571ca2dccbd7efa877ef766a76d1fb2e0a9003aeaeda63", 0x22b8bd8, 0x16dc9f2, 0x17013ba, 0x235c5f8},
    {"9a2250af821c119d9379d12213213eb692d9dcbd58a3e76b7ad7d248fd389472", 0x22b8bd8, 0x16dc9f2, 0x17013ba, 0x235c8a8},
    {"6c081ca6d540552dccf99f75c111914960e68973846a2a92d07794e1631c5dca", 0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235c7d8},
    {"e9b0053f57e32cd40718227c96a55e7e367a824f185b3fc739636955376b10fa", 0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235cb48},
    {"bbe25b75cc2f19288c4a8123ea1f070c0fa73b0c5bbd0925ee904fcea78c556f", 0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235cba8},
    {"f8a820317a758a5e6e84e62fbf33de94125ffa0ba99e68bb8dfafc38ec4d6924", 0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235cc28},
    {"824c00bdae8deeba25149fcc73cc5a8d72b44928f68c79f7c58041732ef75de1", 0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235cc38},
    {"82966c1af8785b34ce73c71db8353e009bfc1db4254f009995b67598938f9a9b", 0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235d248},
    {"277a8393b7c3f319a3420440d4a84a7486a5ee8a795da5764dc10a95bfb95e9c", 0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235d248},
    {"ab51fde046b0eccc6b08a922cde02c8d024e6881461e02588784a2fb17a3b8f5", 0x22b8c08, 0x16dc9f2, 0x17013ba, 0x235d4a8},
    {"e0e3fdcc5abbfd3e4eda848bf88bccf7cdd2b1208af00aa6fc62e5c8a7c761e7", 0x22b8c18, 0x16dc9f2, 0x17013ba, 0x235eee8},
    {"7f516e80358449b698b1ff2f99f9bfb4b4d26aab375eeed06f4025825a63d710", 0x22b8c18, 0x16dc9f2, 0x17013ba, 0x235f778},
    {"2f370f6c08091ad7baa779d1f2397e04ba58bfdbcc2a74c20b42ff8755482b40", 0x22b8c38, 0x16dc9f2, 0x17013ba, 0x235fba8},
    {"ad151cc9ab8cf36cf993921cae11126507baf06e458bbbb6f87bbab2d621913a", 0x22b8c98, 0x16dc9f2, 0x17013ba, 0x2360988},
    {"6a7e730f8a3bfaca21fe5a946704b612238e21eec4311e40b318e5087c435411", 0x22b8d18, 0x16dc9f2, 0x17013ba, 0x2360c18},
    {"65bcd756f45be71c26e7cfd26ba6c27ab9ac811b0cf71a54ad9e8943ccad2eb2", 0x22b8d88, 0x16dc9f2, 0x17013ba, 0x2360cd8},
    {"3541a5c7d4f7d17fb675282ff496acbcbd9db846f5985eab8a4792c8d9932f1b", 0x22b8d88, 0x16dc9f2, 0x17013ba, 0x2361198},
    {"b1f73839dee6df1a35499ee813a83c0876f91d153e2a2d76211809233829c96d", 0x22b8d18, 0x16dc9f2, 0x17013ba, 0x23610d8},
    {"2c2f475befb5f3ada934698358127ba03f8ffbec5224cf69bde947192af02b9f", 0x22b8d18, 0x16dc9f2, 0x17013ba, 0x2361288},
    {"ec1f1b1ee2c1602ce342a1b0bba8f5448fa8efdcdf2295bd544ef4f82f0fc10c", 0x22b8d18, 0x16dc9f2, 0x17013ba, 0x23615f8},
    {"53f952d33a118cb38a8a9ecb11c03fc5cfcbb3d8c5567953195229381e77eb47", 0x22b8d78, 0x16dc9f2, 0x17013ba, 0x23619d8},
    {"6232dc503438c0448f8b63eb15a894346313beb079602562a074fbe8a9f7bd44", 0x22b8d78, 0x16dc9f2, 0x17013ba, 0x2362228},
    {"03377d79488440618ebd7968332e7cbab0aa07e27ae9c786a1dd2db7c2aa5def", 0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2362c28},
    {"4fc26ebbc8a7219bf3f24aa4e3cb3212131d26d2bca928e958f4207ab735e829", 0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2362e88},
    {"ce2961cd73ff0483907987eca50a0dd08af72f23aa335855cc4ad57b238b5e1c", 0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2362ee8},
    {"1a9d9e55dac78437ec1e54bca7ecbacf0bb55280997149d5cfdb46370dcfddc0", 0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2362ff8},
    {"3cace10b439372d5f4a4b7fe9ed8f59e5100371e04b01742b6b10bf9a76e8d4a", 0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2363008},
    {"fb1e1fc76554ded21d7fc18fd5d84f70ce503ceffa991f902c128fab78ecaa4b", 0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2363148},
    {"5abed2917cd43c8f201c4f6540c3144c20b39e9ea7504239e3f2cf74c462cd7d", 0x22b8d98, 0x16dc9f2, 0x17013ba, 0x2362e88},
};

std::optional<Bytes> carry(const Bytes &old) {
    const ptrdiff_t i = find(old);
    if (i >= 0) {
        if (size_t(i) + BLOCK > old.size()) return std::nullopt;
        Bytes b(old.begin() + i, old.begin() + i + std::ptrdiff_t(BLOCK));
        return std::any_of(b.begin(), b.end(), [](uint8_t c) { return c != 0; }) ? std::optional<Bytes>(b) : std::nullopt;
    }
    const std::string h = sha256_hex(old);
    for (const Legacy &L : LEGACY) {
        if (h != L.sha) continue;
        Bytes out(BLOCK, 0);
        if (L.bf + 4168 > old.size()) return std::nullopt;
        std::copy(old.begin() + std::ptrdiff_t(L.bf), old.begin() + std::ptrdiff_t(L.bf + 4168), out.begin() + std::ptrdiff_t(OFF_BF));
        for (const auto &[off, at, n] : {std::tuple{OFF_HMAC, L.hk, size_t(64)}, std::tuple{OFF_ACCESS, L.ak, size_t(20)}, std::tuple{OFF_SECRET, L.sk, size_t(40)}}) {
            if (at + n > old.size()) return std::nullopt;
            const Bytes v(old.begin() + std::ptrdiff_t(at), old.begin() + std::ptrdiff_t(at + n));
            if (std::find(v.begin(), v.end(), 0) != v.end()) return std::nullopt;
            std::copy(v.begin(), v.end(), out.begin() + std::ptrdiff_t(off));
        }
        return out;
    }
    return std::nullopt;
}

void fill(Bytes &image, const Bytes &block) {
    const ptrdiff_t i = find(image);
    if (i < 0) throw KeyError("this multiplayer build has no key block");
    if (find(image, size_t(i)) != -1) throw KeyError("two key blocks in one image");
    std::copy(block.begin(), block.end(), image.begin() + i);
}
}  // namespace keys

std::string with_keys(Bytes &image, const std::string &ps3_elf, const fs::path &previous, const LogFn &lg) {
    if (keys::find(image) < 0) return "not needed";   // a build from before v7.97: its helper carries no key block
    if (!ps3_elf.empty()) {
        try {
            const Bytes block = keys::block_from_eboot(read_bytes(P(ps3_elf)));
            lg("  online profile keys: read from your PS3 EBOOT.elf");
            keys::fill(image, block);
            return "eboot";
        } catch (const KeyError &e) {
            lg("  online profile keys: not from " + ps3_elf + " (" + e.what() + ")");
        } catch (const u3::Stop &e) {
            lg("  online profile keys: not from " + ps3_elf + " (" + e.what() + ")");
        }
    }
    std::optional<Bytes> block;
    std::error_code ec;
    if (fs::is_regular_file(previous, ec)) block = keys::carry(read_bytes(previous));
    if (block) {
        lg("  online profile keys: kept from your previous multiplayer image");
        keys::fill(image, *block);
        return "carried";
    }
    lg("  online profile keys: missing (choose your decrypted 1.19 EBOOT.elf on the Multiplayer tab)");
    return "none";
}

// ------------------------------------------------------------------------------------------------ the recipe's PS4 ELF
// A plain ELF of the prepared image for the recipe's tools (RECIPE_PS4_ELF: Havok reflection tables): every image
// segment as a PT_LOAD at its address, base-relative pointers relocated to load address 0. Made on the player's PC.
Bytes ps4_elf(const Bytes &raw) {
    const G3Layout p = parse_g3(raw);
    Bytes image(raw.begin() + std::ptrdiff_t(p.imagepos), raw.end());
    const size_t relpos = p.namespos + p.ni * 128;
    for (uint64_t i = 0; i < p.nr; i++) {
        const uint64_t target = q64(raw, relpos + i * 32), kind = q64(raw, relpos + i * 32 + 8), value = q64(raw, relpos + i * 32 + 16);
        if (kind == 0 && target + 8 <= image.size()) put64(image, target, value);
    }
    struct Seg {
        uint64_t va, len, prot;
    };
    std::vector<Seg> segs;
    for (uint64_t i = 0; i < p.ns; i++) segs.push_back({q64(raw, p.segpos + i * 24), q64(raw, p.segpos + i * 24 + 8), q64(raw, p.segpos + i * 24 + 16)});
    const uint64_t phoff = 0x40, align = 0x4000;
    const uint64_t offset = (phoff + 56 * segs.size() + align - 1) & ~(align - 1);
    Bytes headers, body;
    auto p32 = [](Bytes &b, uint32_t v) { for (int k = 0; k < 4; k++) b.push_back(uint8_t(v >> (8 * k))); };
    auto p64 = [](Bytes &b, uint64_t v) { for (int k = 0; k < 8; k++) b.push_back(uint8_t(v >> (8 * k))); };
    auto p16 = [](Bytes &b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };
    for (const Seg &s : segs) {
        const uint32_t fl = (s.prot & 4 ? 1 : 0) | (s.prot & 2 ? 2 : 0) | (s.prot & 1 ? 4 : 0);
        p32(headers, 1);
        p32(headers, fl);
        for (uint64_t v : {offset + body.size(), s.va, s.va, s.len, s.len, align}) p64(headers, v);
        const uint64_t a = std::min<uint64_t>(s.va, image.size()), e = std::min<uint64_t>(s.va + s.len, image.size());
        body.insert(body.end(), image.begin() + std::ptrdiff_t(a), image.begin() + std::ptrdiff_t(e));
        body.resize(body.size() + size_t((align - body.size() % align) % align), 0);
    }
    Bytes out = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    out.resize(16, 0);
    p16(out, 2);
    p16(out, 62);
    p32(out, 1);
    p64(out, p.entry);
    p64(out, phoff);
    p64(out, 0);
    p32(out, 0);
    for (uint16_t v : {uint16_t(64), uint16_t(56), uint16_t(segs.size()), uint16_t(64), uint16_t(0), uint16_t(0)}) p16(out, v);
    out.insert(out.end(), headers.begin(), headers.end());
    out.resize(size_t(offset), 0);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// ------------------------------------------------------------------------------------------------ steps 1-4
Value build_image(const fs::path &patcher, const fs::path &game, const fs::path &out, const LogFn &lg, const std::optional<fs::path> &elf_out,
                  const std::optional<fs::path> &update, const std::string &ps3_elf, std::string version) {
    if (version.empty()) version = latest_version(patcher);
    const BuildInfo b = build_info(patcher, version);
    const Value delta = load_json(b.folder / "menu-shell.json");
    fs::create_directories(out);
    const fs::path work = out / "_work";
    fs::create_directories(work);
    lg("Preparing the retail Uncharted 3 image with the port's own code ...");
    const Bytes raw = prepare_base(game, work, b, lg, update);
    lg("  base image ok (sha256 " + text_of(b.info, "base_sha256").substr(0, 12) + "...)");
    if (elf_out) {
        fs::create_directories(elf_out->parent_path());
        write_bytes(*elf_out, ps4_elf(raw));
    }
    const Bytes shell = apply_menu_shell(raw, delta);
    lg("  menu shell ok (sha256 " + text_of(delta, "output_sha256").substr(0, 12) + "...)");
    lg("Linking multiplayer helper v" + version + " ...");
    std::error_code ec0;
    if (!fs::is_regular_file(b.folder / "link.json", ec0)) throw ImageError("link.json of build " + version + " is missing (private files out of step?)");
    const Bytes link_raw = read_bytes(b.folder / "link.json");
    if (const Value *files = b.info.get("files"))
        if (const Value *want = files->get("link.json"); want && want->s != sha256_hex(link_raw)) throw ImageError("link.json is damaged (sha256 mismatch)");
    Value link;
    if (!json::parse(std::string(link_raw.begin(), link_raw.end()), link)) throw ImageError("link.json is damaged (not JSON)");
    Bytes image = apply_link(shell, link);
    const std::string digest = sha256_hex(image);
    if (digest != text_of(b.info, "image_sha256"))
        throw ImageError(fmt("the linked image differs from the v%s release (sha256 %s..., expected %s...)", version.c_str(), digest.substr(0, 12).c_str(),
                             text_of(b.info, "image_sha256").substr(0, 12).c_str()));
    const std::string k = with_keys(image, ps3_elf, out / "boot-linked.bin", lg);
    std::error_code ec;
    if (k == "none" && fs::is_regular_file(out / "boot-linked.bin", ec)) {
        rmtree(work);
        throw KeysMissing("no decrypted PS3 1.19 EBOOT.elf of your own copy of the game for the online profile keys; the current multiplayer image stays");
    }
    write_bytes(out / "boot-linked.bin.tmp", image);
    fs::rename(out / "boot-linked.bin.tmp", out / "boot-linked.bin");
    rmtree(work);
    lg("  multiplayer image v" + version + " ok (sha256 " + digest + ")");
    return obj({{"version", str(version)},
                {"sha256", str(digest)},
                {"path", str(S(out / "boot-linked.bin"))},
                {"base_sha256", str(text_of(b.info, "base_sha256"))},
                {"file_sha256", str(sha256_hex(image))},
                {"keys", str(k)}});
}

}  // namespace u3imp
