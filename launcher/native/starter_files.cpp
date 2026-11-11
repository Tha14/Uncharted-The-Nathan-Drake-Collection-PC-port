// starter_files.cpp - file chores of the game starter (formerly in tools/probe_uncharted.py), built into
// uncharted_native.dll: putting the runtime in place under the name Uncharted.exe, setting up the user folder of an
// extra instance, copying the update's files for drives that can't hold links, making the game window's icon, and
// building the combined game-plus-update view.

#include "native.h"
#include "json.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

using json::Value;

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::wstring env(const wchar_t *name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return L"";
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(n);
    return v;
}

std::wstring join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}

std::wstring base_name(const std::wstring &p) {
    size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? p : p.substr(cut + 1);
}

// Formats a Windows error code the way Python prints an OSError for a path: "[WinError n] text: 'path'".
std::string os_error(const std::wstring &path, DWORD code = GetLastError()) {
    wchar_t *text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                   0, (LPWSTR)&text, 0, nullptr);
    std::wstring t = text ? text : L"error";
    if (text) LocalFree(text);
    while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ' || t.back() == L'.')) t.pop_back();
    std::string shown;   // the path with doubled backslashes, matching Python's repr()
    for (char c : utf8(path)) shown += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return "[WinError " + std::to_string(code) + "] " + utf8(t) + ": '" + shown + "'";
}

bool attributes(const std::wstring &p, WIN32_FILE_ATTRIBUTE_DATA &a) {
    return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) != 0;
}
bool is_file(const std::wstring &p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return attributes(p, a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}
bool is_dir(const std::wstring &p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return attributes(p, a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}
uint64_t size_of(const WIN32_FILE_ATTRIBUTE_DATA &a) { return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow; }
uint64_t ticks(const FILETIME &f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }   // in 100 ns units

bool make_dirs(const std::wstring &p) {
    if (is_dir(p)) return true;
    size_t cut = p.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 2) make_dirs(p.substr(0, cut));
    return CreateDirectoryW(p.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

struct Failure { std::string message; };

// Copies a folder and everything in it; file times come along with the files (like shutil.copytree).
void copy_tree(const std::wstring &from, const std::wstring &to) {
    if (!CreateDirectoryW(to.c_str(), nullptr)) throw Failure{os_error(to)};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(from, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring a = join(from, fd.cFileName), b = join(to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_tree(a, b);
        else if (!CopyFileW(a.c_str(), b.c_str(), FALSE)) { FindClose(h); throw Failure{os_error(a)}; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Which program to start as the runtime. The game runs as Uncharted.exe because that name is what Task Manager, the
// Windows graphics settings and overlays display; it is refreshed from the build output any time the two differ.
// UNCHARTED_TEST_PROBE_EXE=1 (used by tests) runs the build output under its own name. UNCHARTED_PROBE_EXE=<file> runs
// a different build as Uncharted.exe; the regular build is put back on the next normal start. The answer is two lines:
// the program's path, then an optional message.
std::string game_executable(const std::wstring &root) {
    std::wstring built = join(root, L"gow3_pc\\out\\gow3-probe.exe"), game_exe = join(root, L"gow3_pc\\out\\Uncharted.exe");
    if (env(L"UNCHARTED_TEST_PROBE_EXE") == L"1") return utf8(built) + "\n";
    std::wstring other = env(L"UNCHARTED_PROBE_EXE");
    if (!other.empty()) built = other;
    WIN32_FILE_ATTRIBUTE_DATA b, g;
    // If Uncharted.exe can't be refreshed (for example because an earlier start is still running), the build output is
    // started directly, here and in the copy below.
    if (!attributes(built, b))
        return utf8(built) + "\nFalling back to " + utf8(base_name(built)) + ": " + os_error(built);
    bool have = attributes(game_exe, g);
    if (!have || ticks(b.ftLastWriteTime) != ticks(g.ftLastWriteTime) || size_of(b) != size_of(g)) {
        if (!CopyFileW(built.c_str(), game_exe.c_str(), FALSE))
            return utf8(built) + "\nFalling back to " + utf8(base_name(built)) + ": " + os_error(game_exe);
    }
    return utf8(game_exe) + "\n";
}

// The folder where this instance keeps its saves, settings and caches. An extra instance (UNCHARTED_INSTANCE=<n>) gets
// its own folder, seeded on first use with copies of the main instance's settings, saves, trophies and shader caches;
// starting with empty caches would mean minutes of shader compiling.
std::string user_dir(const std::wstring &root, const std::string &suffix) {
    std::wstring main_user = join(root, L"analysis\\uncharted-user");
    if (suffix.empty()) return utf8(main_user) + "\n";
    std::wstring user = join(root, L"analysis\\uncharted-user" + widen(suffix));
    if (GetFileAttributesW(user.c_str()) != INVALID_FILE_ATTRIBUTES) return utf8(user) + "\n";
    if (!make_dirs(user)) throw Failure{os_error(user)};
    for (const wchar_t *name : {L"probe.ini", L"probe-u1.ini", L"probe-u2.ini", L"probe-u3.ini", L"disabled-optimizations.txt",
                                L"skip.txt", L"savedata", L"trophy", L"cache", L"shader"}) {
        std::wstring source = join(main_user, name), dest = join(user, name);
        if (is_dir(source)) copy_tree(source, dest);
        else if (is_file(source) && !CopyFileW(source.c_str(), dest.c_str(), FALSE)) throw Failure{os_error(source)};
    }
    return utf8(user) + "\nInstance " + suffix.substr(2) + ": created the user folder " + utf8(user) +
           " with copies of the main settings, saves and caches";
}

void files_under(const std::wstring &folder, const std::wstring &relative, std::vector<std::wstring> &out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(folder, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring rel = relative + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) files_under(join(folder, fd.cFileName), rel, out);
        else out.push_back(rel);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string sfo_text(const std::wstring &folder, const char *key) {
    std::vector<char> buf(1 << 16);
    if (un_system("sfo", folder.c_str(), buf.data(), (int)buf.size()) != 0) return "";
    Value v;
    if (!json::parse(buf.data(), v)) return "";
    const Value *f = v.get(key);
    if (!f) return "";
    return f->kind == Value::String ? f->s : f->kind == Value::Int ? std::to_string(f->i) : "";
}

// Used when the game's drive can't hold links (exFAT, FAT32). The preparation only reads the executables, sce_module
// and sce_sys, so just those are copied into analysis\views, with the update's version of a file taking priority over
// the game's. That is roughly 90 MB, and a file is copied again only after its source changes. Everything else the runtime
// reads from the game with the update laid on top by itself (--app0-overlay).
std::string boot_view(const std::wstring &root, const std::wstring &base, const std::wstring &update) {
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> chosen;   // lower-case path -> (path, source)
    std::vector<std::wstring> order;
    auto choose = [&](const std::wstring &relative, const std::wstring &source) {
        std::wstring key = lower(relative);
        if (!chosen.count(key)) order.push_back(key);
        chosen[key] = {relative, source};
    };
    for (const std::wstring &layer : {base, update}) {   // update second, so its entries overwrite the game's
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(join(layer, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) throw Failure{os_error(layer)};
        do {
            std::wstring name = fd.cFileName, low = lower(name);
            if (name == L"." || name == L"..") continue;
            bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!dir && (low == L"eboot.bin" || low == L"big2-ps4_shipping.elf" || low == L"big3-ps4_shipping.elf")) {
                choose(name, join(layer, name));
            } else if (dir && (low == L"sce_module" || low == L"sce_sys")) {
                std::vector<std::wstring> files;
                files_under(join(layer, name), name, files);
                for (const std::wstring &rel : files) choose(rel, join(layer, rel));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::string title = sfo_text(update, "TITLE_ID"), version = sfo_text(update, "APP_VER");
    std::wstring view = join(root, L"analysis\\views\\boot-" + widen(title.empty() ? "game" : title) + L"-" +
                                       widen(version.empty() ? "update" : version));
    for (const std::wstring &key : order) {
        const auto &entry = chosen[key];
        std::wstring dest = join(view, entry.first), source = entry.second;
        WIN32_FILE_ATTRIBUTE_DATA wanted, current;
        if (!attributes(source, wanted)) throw Failure{os_error(source)};
        // Same size and a write time within 2 s counts as unchanged: FAT32 only stores times to the even second.
        if (attributes(dest, current) && size_of(current) == size_of(wanted)) {
            uint64_t a = ticks(current.ftLastWriteTime), b = ticks(wanted.ftLastWriteTime);
            if ((a > b ? a - b : b - a) <= 20000000ULL) continue;
        }
        make_dirs(dest.substr(0, dest.find_last_of(L'\\')));
        // Only the contents and the times are copied, not the attributes: a copy marked read-only like its source
        // could not be overwritten the next time it is refreshed.
        HANDLE in = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
        if (in == INVALID_HANDLE_VALUE) throw Failure{os_error(source)};
        HANDLE outf = CreateFileW(dest.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (outf == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            CloseHandle(in);
            throw Failure{os_error(dest, code)};
        }
        std::vector<char> buf(1 << 20);
        DWORD got = 0, put = 0;
        bool ok = true;
        while (ok && ReadFile(in, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
            ok = WriteFile(outf, buf.data(), got, &put, nullptr) && put == got;
        ok = ok && SetFileTime(outf, nullptr, &wanted.ftLastAccessTime, &wanted.ftLastWriteTime);
        DWORD code = GetLastError();
        CloseHandle(in);
        CloseHandle(outf);
        if (!ok) throw Failure{os_error(dest, code)};
    }
    return utf8(view);
}

// ------------------------------------------------------------------------------------------------ window icon

// The window icon: the game's sce_sys\icon0.png scaled to a 64x64 24-bit BMP, since SDL only loads BMP files. It is
// created once, as launcher\assets\window_icon.bmp. The bytes are identical to what the older Python starter produced
// through Pillow (RGB conversion, bicubic resize, BMP save), which is why Pillow's fixed-point resampler and its BMP
// header are reproduced here.
struct Rgb {
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // three bytes (red, green, blue) per pixel, rows from the top down
};

bool load_rgb(const std::wstring &path, Rgb &out, std::string &error) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *factory = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom));
    UINT w = 0, h = 0;
    if (ok) ok = SUCCEEDED(conv->GetSize(&w, &h)) && w && h && w <= 16384 && h <= 16384;
    if (ok) {
        out.w = (int)w;
        out.h = (int)h;
        out.px.assign((size_t)w * h * 3, 0);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 3, (UINT)out.px.size(), out.px.data()));
    }
    if (!ok) error = "cannot read " + utf8(path);
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

// The resampler follows Pillow's libImaging/Resample.c with the bicubic kernel (a = -0.5). Each output pixel gets its
// own set of weights, normalized to sum to 1 and stored as 22-bit fixed point. One pass scales horizontally and one
// vertically, and each rounds its result to 8 bits.
const int PRECISION_BITS = 32 - 8 - 2;
double bicubic(double x) {
    const double a = -0.5;
    x = std::fabs(x);
    if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
    if (x < 2.0) return (((x - 5) * x + 8) * x - 4) * a;
    return 0.0;
}
struct Coeffs {
    int ksize = 0;
    std::vector<int> bounds;   // two ints per output pixel: index of its first input pixel and how many inputs it uses
    std::vector<int> k;        // ksize weights for each output pixel
};
Coeffs coefficients(int in_size, int out_size) {
    double scale = (double)in_size / out_size, filterscale = scale < 1.0 ? 1.0 : scale;
    double support = 2.0 * filterscale;
    Coeffs c;
    c.ksize = (int)std::ceil(support) * 2 + 1;
    c.bounds.assign((size_t)out_size * 2, 0);
    std::vector<double> pre((size_t)out_size * c.ksize, 0.0);
    for (int xx = 0; xx < out_size; ++xx) {
        double center = 0 + (xx + 0.5) * scale, ww = 0.0, ss = 1.0 / filterscale;
        int xmin = (int)(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = (int)(center + support + 0.5);
        if (xmax > in_size) xmax = in_size;
        xmax -= xmin;
        double *k = &pre[(size_t)xx * c.ksize];
        for (int x = 0; x < xmax; ++x) {
            double w = bicubic((x + xmin - center + 0.5) * ss);
            k[x] = w;
            ww += w;
        }
        for (int x = 0; x < xmax; ++x) if (ww != 0.0) k[x] /= ww;
        c.bounds[(size_t)xx * 2] = xmin;
        c.bounds[(size_t)xx * 2 + 1] = xmax;
    }
    c.k.resize(pre.size());
    for (size_t i = 0; i < pre.size(); ++i)
        c.k[i] = pre[i] < 0 ? (int)(-0.5 + pre[i] * (1 << PRECISION_BITS)) : (int)(0.5 + pre[i] * (1 << PRECISION_BITS));
    return c;
}
uint8_t clip8(long long v) {
    long long x = v >> PRECISION_BITS;
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}
Rgb horizontal(const Rgb &in, int w, const Coeffs &hz, int row0, int rows) {   // `rows` rows from row0, scaled to w
    Rgb mid;
    mid.w = w;
    mid.h = rows;
    mid.px.assign((size_t)w * rows * 3, 0);
    for (int y = 0; y < rows; ++y)
        for (int xx = 0; xx < w; ++xx) {
            int xmin = hz.bounds[(size_t)xx * 2], xmax = hz.bounds[(size_t)xx * 2 + 1];
            const int *k = &hz.k[(size_t)xx * hz.ksize];
            for (int ch = 0; ch < 3; ++ch) {
                long long ss = 1LL << (PRECISION_BITS - 1);
                for (int x = 0; x < xmax; ++x) ss += (long long)in.px[((size_t)(y + row0) * in.w + x + xmin) * 3 + ch] * k[x];
                mid.px[((size_t)y * w + xx) * 3 + ch] = clip8((int)ss);
            }
        }
    return mid;
}
Rgb vertical(const Rgb &in, int h, const Coeffs &vt, int row0) {   // scaled to h; `in` begins at source row row0
    Rgb out;
    out.w = in.w;
    out.h = h;
    out.px.assign((size_t)in.w * h * 3, 0);
    for (int yy = 0; yy < h; ++yy) {
        int ymin = vt.bounds[(size_t)yy * 2] - row0, ymax = vt.bounds[(size_t)yy * 2 + 1];
        const int *k = &vt.k[(size_t)yy * vt.ksize];
        for (int xx = 0; xx < in.w; ++xx)
            for (int ch = 0; ch < 3; ++ch) {
                long long ss = 1LL << (PRECISION_BITS - 1);
                for (int y = 0; y < ymax; ++y) ss += (long long)in.px[((size_t)(y + ymin) * in.w + xx) * 3 + ch] * k[y];
                out.px[((size_t)yy * in.w + xx) * 3 + ch] = clip8((int)ss);
            }
    }
    return out;
}
Rgb resize_horizontal_first(const Rgb &in, int w, int h, const Coeffs &hz, const Coeffs &vt);
Rgb resize(const Rgb &in, int w, int h) {
    Coeffs hz = coefficients(in.w, w), vt = coefficients(in.h, h);
    const bool need_h = w != in.w, need_v = h != in.h;
    // Order of the passes, as in Pillow 13: vertical first only when the height loses more than double what the
    // width loses
    const bool horizontal_first = !((in.h - h) > 0 && (in.h - h) > (in.w - w) * 2);
    if (!horizontal_first && need_v) {
        Rgb mid = vertical(in, h, vt, 0);
        return need_h ? horizontal(mid, w, hz, 0, mid.h) : mid;
    }
    return resize_horizontal_first(in, w, h, hz, vt);
}
Rgb resize_horizontal_first(const Rgb &in, int w, int h, const Coeffs &hz, const Coeffs &vt) {
    // scale horizontally only the rows the vertical pass will read, then scale vertically
    int first = vt.bounds[0], last = vt.bounds[(size_t)h * 2 - 2] + vt.bounds[(size_t)h * 2 - 1];
    if (w == in.w) return h == in.h ? in : vertical(in, h, vt, 0);
    Rgb mid = horizontal(in, w, hz, first, last - first);
    return h == in.h ? mid : vertical(mid, h, vt, first);
}

void put32(std::string &s, uint32_t v) { for (int i = 0; i < 4; ++i) s += (char)(v >> (8 * i)); }
void put16(std::string &s, uint16_t v) { s += (char)v; s += (char)(v >> 8); }

std::string window_icon(const std::wstring &root, const std::wstring &game) {
    std::wstring icon = join(root, L"launcher\\assets\\window_icon.bmp");
    if (GetFileAttributesW(icon.c_str()) != INVALID_FILE_ATTRIBUTES) return utf8(icon) + "\n";
    std::wstring png = join(game, L"sce_sys\\icon0.png");
    Rgb picture;
    std::string error;
    if (!load_rgb(png, picture, error)) return "\nWindow icon unavailable: " + error;
    Rgb small = resize(picture, 64, 64);
    // The BMP layout Pillow writes: a 14-byte file header, a BITMAPINFOHEADER, then the rows from the bottom up in
    // blue-green-red order, each row padded to a multiple of 4 bytes.
    uint32_t stride = ((uint32_t)small.w * 3 + 3) & ~3u, image = stride * (uint32_t)small.h;
    const uint32_t ppm = 3780;   // 96 dpi, since Pillow drops the PNG's resolution when it resizes
    std::string bmp = "BM";
    put32(bmp, 14 + 40 + image);
    put32(bmp, 0);
    put32(bmp, 14 + 40);
    put32(bmp, 40);
    put32(bmp, (uint32_t)small.w);
    put32(bmp, (uint32_t)small.h);
    put16(bmp, 1);
    put16(bmp, 24);
    put32(bmp, 0);
    put32(bmp, image);
    put32(bmp, ppm);
    put32(bmp, ppm);
    put32(bmp, 0);
    put32(bmp, 0);
    for (int y = small.h - 1; y >= 0; --y) {
        std::string row;
        for (int x = 0; x < small.w; ++x) {
            const uint8_t *p = &small.px[((size_t)y * small.w + x) * 3];
            row += (char)p[2];
            row += (char)p[1];
            row += (char)p[0];
        }
        row.resize(stride, '\0');
        bmp += row;
    }
    make_dirs(icon.substr(0, icon.find_last_of(L'\\')));
    HANDLE f = CreateFileW(icon.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "\nWindow icon unavailable: " + os_error(icon);
    DWORD put = 0;
    bool ok = WriteFile(f, bmp.data(), (DWORD)bmp.size(), &put, nullptr) && put == bmp.size();
    CloseHandle(f);
    if (!ok) return "\nWindow icon unavailable: " + os_error(icon);
    return utf8(icon) + "\n";
}

// ------------------------------------------------------------------------------------------------ the update view
// (replaces gow3_pc/scripts/game_update.py) When a dumped game and its update sit in separate folders, the starter
// builds a temporary folder made of links that shows the game with the update on top. Neither dump is written to, and
// even a 47 GB game needs only a few links. A file is hard-linked when possible (works on the same drive, needs no
// special privilege, and its size reads correctly through every API); otherwise a symbolic link is tried, and as a
// last resort a folder becomes a junction and a file a copy. File names are compared case-insensitively.

struct ValueError { std::string message; };

// Formats a failed link the way Python reports it: "[WinError n] text: 'source' -> 'link'".
std::string os_error2(const std::wstring &source, const std::wstring &link, DWORD code = GetLastError()) {
    std::string shown;
    for (char c : utf8(link)) shown += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return os_error(source, code) + " -> '" + shown + "'";
}

// Same, for an error that names no file: "[WinError n] text".
std::string win_error(DWORD code = GetLastError()) {
    const std::string full = os_error(L"", code);
    return full.substr(0, full.size() - 4);   // cut off the trailing ": ''"
}

bool is_reparse(const std::wstring &p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool exists(const std::wstring &p) {   // true only if a link's target is reachable too (like Path.exists)
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!attributes(p, a)) return false;
    if (!(a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}
bool is_dir_followed(const std::wstring &p) { return exists(p) && is_dir(p); }
bool is_file_followed(const std::wstring &p) { return exists(p) && is_file(p); }

// The real location behind a path, with links followed and any \\?\ prefix removed (what Path.resolve() gives); the
// plain absolute path when that can't be determined.
std::wstring resolved(const std::wstring &p) {
    std::vector<wchar_t> full(32768);
    DWORD n = GetFullPathNameW(p.c_str(), 32768, full.data(), nullptr);
    std::wstring f = n && n < 32768 ? std::wstring(full.data(), n) : p;
    HANDLE h = CreateFileW(f.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return f;
    n = GetFinalPathNameByHandleW(h, full.data(), 32768, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (!n || n >= 32768) return f;
    std::wstring r(full.data(), n);
    if (r.compare(0, 8, L"\\\\?\\UNC\\") == 0) return L"\\" + r.substr(7);
    if (r.compare(0, 4, L"\\\\?\\") == 0) return r.substr(4);
    return r;
}

std::wstring fold(const std::wstring &s) {   // lower case for comparing names (enough of str.casefold for game files)
    std::wstring o(s);
    if (!o.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), (int)s.size(), &o[0], (int)o.size(), nullptr, nullptr, 0);
    return o;
}

// Names inside a folder, in whatever order the file system returns them (as os.scandir), skipping . and ..
std::vector<std::wstring> entries_of(const std::wstring &folder) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(folder, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) throw Failure{os_error(folder)};
    do {
        const std::wstring n = fd.cFileName;
        if (n != L"." && n != L"..") names.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return names;
}

// Creates a junction, the kind of folder link any user may make, with the same reparse data _winapi.CreateJunction
// writes.
bool create_junction(const std::wstring &target, const std::wstring &link) {
    const std::wstring substitute = L"\\??\\" + target;
    const size_t sub = substitute.size() * 2, print = target.size() * 2;
    std::vector<char> buf(16 + sub + 2 + print + 2, 0);
    auto put16 = [&](size_t at, size_t v) { buf[at] = (char)v; buf[at + 1] = (char)(v >> 8); };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(&buf[0], &tag, 4);
    put16(4, buf.size() - 8);   // length of everything after the 8-byte header
    put16(8, 0);                // substitute name: offset, then length
    put16(10, sub);
    put16(12, sub + 2);         // print name: offset, then length
    put16(14, print);
    std::memcpy(&buf[16], substitute.data(), sub);
    std::memcpy(&buf[16 + sub + 2], target.data(), print);
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    DWORD got = 0;
    const bool ok = h != INVALID_HANDLE_VALUE &&
                    DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), (DWORD)buf.size(), nullptr, 0, &got, nullptr);
    const DWORD error = GetLastError();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) {
        RemoveDirectoryW(link.c_str());
        SetLastError(error);
    }
    return ok;
}

// Makes `link` point at `target`, trying in turn: a hard link (files only), a symbolic link, then a junction for a
// folder or a plain copy for a file. (Native version of mods.make_link.)
void make_link(const std::wstring &link, const std::wstring &target) {
    const bool file = is_file_followed(target), dir = is_dir_followed(target);
    if (file && CreateHardLinkW(link.c_str(), target.c_str(), nullptr)) return;
    // tests set UNCHARTED_TEST_NO_SYMLINKS=1 to behave like a PC where symbolic links are not allowed
    if (env(L"UNCHARTED_TEST_NO_SYMLINKS") != L"1" && CreateSymbolicLinkW(link.c_str(), target.c_str(),
                            (dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        return;
    if (dir) {
        if (!create_junction(target, link)) throw Failure{win_error()};
        return;
    }
    if (CreateHardLinkW(link.c_str(), target.c_str(), nullptr)) return;
    if (!CopyFileW(target.c_str(), link.c_str(), FALSE)) throw Failure{os_error2(target, link)};
}

// Deletes one entry of a view (file, link or folder). Links are removed themselves; what they point to inside the
// dumps is never entered or touched. (Was game_update.remove.)
void remove_path(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) throw Failure{os_error(path)};
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(a & FILE_ATTRIBUTE_REPARSE_POINT))   // not a link but an actual folder: empty it before removing it
            for (const std::wstring &child : entries_of(path)) remove_path(join(path, child));
        if (!RemoveDirectoryW(path.c_str())) throw Failure{os_error(path)};
    } else if (!DeleteFileW(path.c_str())) {
        throw Failure{os_error(path)};
    }
}

// Puts the contents of `source` on top of `dest`, whose entries are links into the base game. A file from source
// takes the place of dest's entry with the same name (compared case-insensitively); a folder present on both sides
// is turned into a real folder and merged recursively. (Was game_update.overlay.)
void overlay(const std::wstring &source, const std::wstring &dest) {
    std::map<std::wstring, std::wstring> names;   // lower-cased name -> how dest spells it; a later duplicate wins
    for (const std::wstring &n : entries_of(dest)) names[fold(n)] = n;
    for (const std::wstring &n : entries_of(source)) {
        const std::wstring item = join(source, n);
        auto it = names.find(fold(n));
        const std::wstring match = join(dest, it != names.end() ? it->second : n);
        if (is_dir_followed(item) && is_dir_followed(match)) {
            if (is_reparse(match)) {   // dest only links to the base's folder: replace the link with a folder of links
                const std::wstring inner = resolved(match);
                remove_path(match);
                if (!CreateDirectoryW(match.c_str(), nullptr)) throw Failure{os_error(match)};
                for (const std::wstring &child : entries_of(inner)) make_link(join(match, child), join(inner, child));
            }
            overlay(item, match);
        } else {
            if (GetFileAttributesW(match.c_str()) != INVALID_FILE_ATTRIBUTES) remove_path(match);
            make_link(match, item);
        }
    }
}

bool below(const std::wstring &child, const std::wstring &parent) {   // child lies somewhere inside parent (any case)
    std::wstring c = fold(child), p = fold(parent);
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return c.size() > p.size() + 1 && c.compare(0, p.size(), p) == 0 && (c[p.size()] == L'\\' || c[p.size()] == L'/');
}

// Creates a fresh folder in `dir` named `prefix` plus 8 random characters (the same naming as tempfile.mkdtemp).
std::wstring make_temp_dir(const std::wstring &dir, const std::wstring &prefix) {
    static const wchar_t *chars = L"abcdefghijklmnopqrstuvwxyz0123456789_";
    std::vector<wchar_t> full(32768);
    DWORD n = GetFullPathNameW(dir.c_str(), 32768, full.data(), nullptr);
    const std::wstring base = n && n < 32768 ? std::wstring(full.data(), n) : dir;
    for (int attempt = 0; attempt < 100; ++attempt) {
        unsigned char r[8];
        BCryptGenRandom(nullptr, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        std::wstring name = prefix;
        for (unsigned char c : r) name += chars[c % 37];
        const std::wstring path = join(base, name);
        if (CreateDirectoryW(path.c_str(), nullptr)) return path;
        if (GetLastError() != ERROR_ALREADY_EXISTS) throw Failure{os_error(path)};
    }
    throw Failure{"No usable temporary directory name found"};
}

// Builds, inside `out`, a new folder showing the base game with the update over it, and returns its path. The caller
// deletes it again through remove_view. (Was game_update.create_view.)
std::string game_view(const std::wstring &base_in, const std::wstring &update_in, const std::wstring &out) {
    const std::wstring base = resolved(base_in), update = resolved(update_in);
    const std::string base_id = sfo_text(base, "TITLE_ID"), update_id = sfo_text(update, "TITLE_ID");
    if (base_id.empty() || base_id != update_id)
        throw ValueError{"The game and the update belong to different titles: " +
                         (base_id.empty() ? std::string("None") : base_id) + " / " +
                         (update_id.empty() ? std::string("None") : update_id)};
    if (fold(base) == fold(update) || below(base, update) || below(update, base))
        throw ValueError{"The game and its update have to be in two different folders, neither inside the other"};
    if (!is_file_followed(join(update, L"eboot.bin")))
        throw ValueError{"An update folder must contain eboot.bin and sce_sys/param.sfo"};
    if (!make_dirs(out)) throw Failure{os_error(out)};
    const std::wstring target = make_temp_dir(out, L"game-update-");
    try {
        // Link the folders before the files: on a drive that can't hold links (exFAT, FAT32) the very first folder
        // fails, so no time is spent copying files first.
        std::vector<std::wstring> names = entries_of(base);
        std::vector<std::wstring> folders, files;
        for (const std::wstring &n : names) (is_dir_followed(join(base, n)) ? folders : files).push_back(n);
        folders.insert(folders.end(), files.begin(), files.end());
        for (const std::wstring &n : folders) make_link(join(target, n), join(base, n));
        overlay(update, target);
    } catch (...) {
        try { remove_path(target); } catch (...) {}
        throw;
    }
    return utf8(target);
}

std::wstring arg_at(const Value &args, size_t i) {
    return i < args.items.size() && args.items[i].kind == Value::String ? widen(args.items[i].s) : L"";
}

}  // namespace

extern "C" {

// One entry point for the file chores above: `what` picks the chore, args_json holds its arguments as a JSON array.
//   game_executable [root]               -> the program's path, then a message line
//   user_dir [root, suffix]              -> the folder, then a message line
//   boot_view [root, game, update]       -> the folder holding the copies
//   game_view [game, update, out]        -> the folder of the combined view
//   remove_view [view]                   -> nothing
//   window_icon [root, game]             -> the icon's path, then a message line
// Returns 0 on success; 1 on a file error (`out` has it in Python's OSError wording); 2 for an unknown `what`; 3 when
// the game and update folders can't be combined (`out` says why).
UN_API int un_starter_files(const char *what, const char *args_json, char *out, int cap) {
    std::string w = what ? what : "";
    Value args;
    if (!json::parse(args_json ? args_json : "[]", args) || args.kind != Value::Array) return 2;
    try {
        if (w == "game_executable") return give(game_executable(arg_at(args, 0)), out, cap);
        if (w == "user_dir") return give(user_dir(arg_at(args, 0), utf8(arg_at(args, 1))), out, cap);
        if (w == "boot_view") return give(boot_view(arg_at(args, 0), arg_at(args, 1), arg_at(args, 2)), out, cap);
        if (w == "game_view") return give(game_view(arg_at(args, 0), arg_at(args, 1), arg_at(args, 2)), out, cap);
        if (w == "remove_view") {
            const std::wstring view = arg_at(args, 0);
            if (exists(view)) remove_path(view);
            return give("", out, cap);
        }
        if (w == "window_icon") return give(window_icon(arg_at(args, 0), arg_at(args, 1)), out, cap);
    } catch (const ValueError &e) {
        int status = give(e.message, out, cap);
        return status < 0 ? status : 3;
    } catch (const Failure &f) {
        int status = give(f.message, out, cap);
        return status < 0 ? status : 1;
    }
    return 2;
}

}  // extern "C"
