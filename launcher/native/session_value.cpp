// session_value.cpp - Uncharted 3 multiplayer: the helper's session value, in uncharted_native.dll.
//
// To talk to a session server the multiplayer helper needs 16 bytes that are part of the PS3 Uncharted 3 1.19
// executable. No release of this project carries them. They are taken here, on the player's PC, from the player's own
// decrypted EBOOT.elf, and kept as 32 hex digits in <user dir>\u3-session.key, which the helper reads when the game
// starts. This file holds only where the bytes are in that EBOOT and their SHA-256, never the bytes.
//
// Players who set the multiplayer up with an earlier release and no longer have the EBOOT where the launcher knows it
// still have the bytes on their PC: the multiplayer image that release linked for them contains them. So when no EBOOT
// can be read, the image file is searched for 16 bytes with that SHA-256, before an update replaces it.

#include "native.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

const long long EBOOT_AT = 0xdac720;   // file offset in the decrypted 1.19 EBOOT.elf
const uint8_t WANT[32] = {0xc2, 0x44, 0x00, 0xdc, 0x4b, 0x15, 0xfb, 0x66, 0x56, 0xf4, 0x17, 0x5f, 0x4d, 0x5e, 0x02, 0x34,
                          0x4f, 0x48, 0xd8, 0xa1, 0xc1, 0x11, 0x7b, 0x7f, 0xf2, 0xfb, 0x2e, 0x70, 0xd6, 0xed, 0x86, 0xba};
const long long IMAGE_TAIL = 3ll << 20;   // the helper sits in the last 1.3 MiB of the image: only this much is searched

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t v, int by) { return (v >> by) | (v << (32 - by)); }

// SHA-256 of exactly 16 bytes (one block), compared with WANT
bool is_the_value(const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 4; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    w[4] = 0x80000000u;
    for (int i = 5; i < 15; ++i) w[i] = 0;
    w[15] = 128;   // the message length in bits
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint32_t v[8];
    std::memcpy(v, h, sizeof v);
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
        const uint32_t t1 = v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + K[i] + w[i];
        const uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
        const uint32_t t2 = s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; ++i) {
        const uint32_t word = h[i] + v[i];
        for (int b = 0; b < 4; ++b)
            if (WANT[4 * i + b] != (uint8_t)(word >> (24 - 8 * b))) return false;
    }
    return true;
}

// up to `count` bytes of a file from `at` (a negative `at` counts from the end)
std::vector<uint8_t> read_part(const wchar_t *path, long long at, long long count) {
    std::vector<uint8_t> data;
    if (!path || !*path) return data;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size, pos;
    if (GetFileSizeEx(f, &size)) {
        if (at < 0) at = size.QuadPart + at < 0 ? 0 : size.QuadPart + at;
        if (at < size.QuadPart) {
            if (count > size.QuadPart - at) count = size.QuadPart - at;
            pos.QuadPart = at;
            data.resize((size_t)count);
            DWORD got = 0;
            size_t have = 0;
            if (SetFilePointerEx(f, pos, nullptr, FILE_BEGIN))
                while (have < data.size() &&
                       ReadFile(f, data.data() + have, (DWORD)std::min<size_t>(data.size() - have, 1 << 20), &got, nullptr) && got)
                    have += got;
            data.resize(have);
        }
    }
    CloseHandle(f);
    return data;
}

bool from_text(const std::vector<uint8_t> &text, uint8_t *value) {
    if (text.size() < 32) return false;
    for (int i = 0; i < 32; ++i) {
        const uint8_t c = text[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return false;
        value[i / 2] = (uint8_t)((i & 1) ? (value[i / 2] | digit) : (digit << 4));
    }
    return true;
}

bool file_has_it(const std::wstring &file) {
    uint8_t value[16];
    return from_text(read_part(file.c_str(), 0, 64), value) && is_the_value(value);
}

bool write_value(const std::wstring &file, const uint8_t *value) {
    char text[34];
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        text[2 * i] = digits[value[i] >> 4];
        text[2 * i + 1] = digits[value[i] & 15];
    }
    text[32] = '\n';
    const std::wstring tmp = file + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(f, text, 33, &put, nullptr) && put == 33;
    CloseHandle(f);
    SecureZeroMemory(text, sizeof text);
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace

extern "C" {

// Makes sure <user_dir>\u3-session.key holds the value. `eboots`: paths of decrypted EBOOT.elf candidates, one per
// line; `image`: the player's current multiplayer image (may be empty). Returns 1 the file was already right, 2 written
// from an EBOOT, 3 written from the image, 0 the value was found nowhere, -1 it was found and the file can't be written.
UN_API int un_u3_session_value(const wchar_t *user_dir, const wchar_t *eboots, const wchar_t *image) {
    if (!user_dir || !*user_dir) return 0;
    std::wstring file = user_dir;
    if (file.back() != L'\\' && file.back() != L'/') file += L'\\';
    file += L"u3-session.key";
    if (file_has_it(file)) return 1;
    uint8_t value[16];
    int from = 0;
    std::wstring list = eboots ? eboots : L"";
    for (size_t at = 0; at <= list.size() && !from;) {
        size_t end = list.find(L'\n', at);
        if (end == std::wstring::npos) end = list.size();
        const std::wstring path = list.substr(at, end - at);
        at = end + 1;
        const std::vector<uint8_t> bytes = read_part(path.c_str(), EBOOT_AT, 16);
        if (bytes.size() == 16 && is_the_value(bytes.data())) {
            std::memcpy(value, bytes.data(), 16);
            from = 2;
        }
    }
    if (!from) {
        const std::vector<uint8_t> tail = read_part(image, -IMAGE_TAIL, IMAGE_TAIL);
        for (size_t at = tail.size() >= 16 ? tail.size() - 16 + 1 : 0; at-- > 0 && !from;)
            if (is_the_value(tail.data() + at)) {
                std::memcpy(value, tail.data() + at, 16);
                from = 3;
            }
    }
    if (!from) return 0;
    const bool written = write_value(file, value);
    SecureZeroMemory(value, sizeof value);
    return written ? from : -1;
}

}  // extern "C"
