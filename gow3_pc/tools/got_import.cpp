// got_import.cpp - diagnostic: names the import bound to a GOT slot (guest offsets, hex) in out/boot.bin.
//   got_import <slot> [slot...]   prints "<slot> <nid> <symbol>" for each ("None" where boot.bin binds nothing there)
// The folders are found like the Python tool found them: out/boot.bin and src/import_names.inc of the folder above
// the one this program is in, so build it here in gow3_pc/tools:
//   clang++ -std=c++20 -O2 -static got_import.cpp -o got_import.exe
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

struct Fail {
    std::string message;
};

std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Fail{"cannot read " + p.string()};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint64_t u64(const std::string &d, uint64_t at) {   // struct.unpack_from('<Q'), checked
    if (at > d.size() || d.size() - at < 8) throw Fail{"struct.error: boot.bin is too short"};
    uint64_t v;
    std::memcpy(&v, d.data() + at, 8);
    return v;
}

// int(text, 16): optional sign, optional 0x, '_' between digits, surrounding whitespace
std::optional<long long> hex_int(const std::string &t) {
    size_t a = t.find_first_not_of(" \t\n\r\f\v"), b = t.find_last_not_of(" \t\n\r\f\v");
    if (a == std::string::npos) return std::nullopt;
    std::string s = t.substr(a, b - a + 1);
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') neg = s[0] == '-', s.erase(0, 1);
    size_t i = 0;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        i = 2;
        if (i < s.size() && s[i] == '_') i++;   // "0x_1f" is allowed
    }
    auto digit = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    if (i >= s.size()) return std::nullopt;
    unsigned long long v = 0;
    for (bool after_digit = false; i < s.size(); i++) {
        if (s[i] == '_') {   // one '_' between two digits
            if (!after_digit || i + 1 >= s.size() || digit(s[i + 1]) < 0) return std::nullopt;
            after_digit = false;
            continue;
        }
        after_digit = true;
        if (digit(s[i]) < 0) return std::nullopt;
        v = v * 16 + unsigned(digit(s[i]));
    }
    return neg ? -(long long)v : (long long)v;
}

fs::path program_folder(const char *argv0) {
#ifdef _WIN32
    (void)argv0;
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    return fs::path(std::wstring(buf, n)).parent_path();
#else
    return fs::canonical(fs::path(argv0)).parent_path();
#endif
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const fs::path here = program_folder(argv[0]).parent_path();   // gow3_pc
        const std::string data = read(here / "out" / "boot.bin");
        const uint64_t ns = u64(data, 24), nr = u64(data, 32), ni = u64(data, 40);
        u64(data, 48);   // size, entry, ns, nr, ni, caps at 8: the header is 56 bytes
        uint64_t pos = 56 + ns * 24;
        std::vector<std::string> names;
        for (uint64_t i = 0; i < ni; i++) {
            const uint64_t at = pos + i * 128;
            std::string n = at < data.size() ? data.substr(at, 128) : "";
            names.push_back(n.substr(0, n.find('\0')));
        }
        pos += ni * 128;
        // dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', text)): later pairs replace earlier ones
        std::string text;
        for (char c : read(here / "src" / "import_names.inc"))
            if (c != '\r') text += c;
        std::map<std::string, std::string> symbols;
        for (size_t i = 0; i + 1 < text.size();) {
            if (text[i] == '{' && text[i + 1] == '"') {
                const size_t j = text.find('"', i + 2);
                if (j != std::string::npos && j > i + 2 && text.compare(j, 3, "\",\"") == 0) {
                    const size_t k = text.find('"', j + 3);
                    if (k != std::string::npos && k > j + 3 && text.compare(k, 2, "\"}") == 0) {
                        symbols[text.substr(i + 2, j - i - 2)] = text.substr(j + 3, k - j - 3);
                        i = k + 2;
                        continue;
                    }
                }
            }
            i++;
        }
        std::map<uint64_t, std::string> slots;
        for (uint64_t i = 0; i < nr; i++) {
            const uint64_t at = pos + i * 32, target = u64(data, at), kind = u64(data, at + 8);
            const long long value = (long long)u64(data, at + 16);
            u64(data, at + 24);
            if (!kind) continue;
            const long long index = value < 0 ? (long long)names.size() + value : value;   // names[value]
            if (index < 0 || index >= (long long)names.size()) throw Fail{"IndexError: list index out of range"};
            slots[target] = names[size_t(index)];
        }
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            const auto v = hex_int(arg);
            if (!v) throw Fail{"ValueError: invalid literal for int() with base 16: '" + arg + "'"};
            const auto s = *v >= 0 ? slots.find(uint64_t(*v)) : slots.end();
            std::string line = arg + " " + (s != slots.end() ? s->second : "None") + " ";
            if (s != slots.end() && !s->second.empty()) {
                const auto sym = symbols.find(s->second);
                line += sym != symbols.end() ? sym->second : "None";
            }
            std::printf("%s\n", line.c_str());
        }
        return 0;
    } catch (const Fail &e) {
        std::fflush(stdout);
        std::fprintf(stderr, "%s\n", e.message.c_str());
        return 1;
    }
}
