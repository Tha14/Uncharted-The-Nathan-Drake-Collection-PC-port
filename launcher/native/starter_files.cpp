// starter_files.cpp - the game starter's files (tools/probe_uncharted.py), part of uncharted_native.dll: the runtime as
// Uncharted.exe, a second instance's user folder, the update's copies for drives that hold no links, and the game
// window's icon.

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

// the last Windows error as Python words an OSError: "[WinError n] text: 'path'"
std::string os_error(const std::wstring &path, DWORD code = GetLastError()) {
    wchar_t *text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                   0, (LPWSTR)&text, 0, nullptr);
    std::wstring t = text ? text : L"error";
    if (text) LocalFree(text);
    while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ' || t.back() == L'.')) t.pop_back();
    std::string shown;   // the path as Python's repr shows it
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
uint64_t ticks(const FILETIME &f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }   // 100 ns

bool make_dirs(const std::wstring &p) {
    if (is_dir(p)) return true;
    size_t cut = p.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 2) make_dirs(p.substr(0, cut));
    return CreateDirectoryW(p.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

struct Failure { std::string message; };

// a folder copied with its files' times (shutil.copytree)
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

// The runtime as Uncharted.exe (Task Manager, Windows' graphics settings and overlays show this name), copied from the
// build output whenever they differ. UNCHARTED_TEST_PROBE_EXE=1: the build output itself (tests); UNCHARTED_PROBE_EXE=
// <file>: another build run as Uncharted.exe (copied back at the next normal start). Lines: the executable, a message.
std::string game_executable(const std::wstring &root) {
    std::wstring built = join(root, L"gow3_pc\\out\\gow3-probe.exe"), game_exe = join(root, L"gow3_pc\\out\\Uncharted.exe");
    if (env(L"UNCHARTED_TEST_PROBE_EXE") == L"1") return utf8(built) + "\n";
    std::wstring other = env(L"UNCHARTED_PROBE_EXE");
    if (!other.empty()) built = other;
    WIN32_FILE_ATTRIBUTE_DATA b, g;
    if (!attributes(built, b))   // still running from a previous start: the build output
        return utf8(built) + "\nUsing " + utf8(base_name(built)) + ": " + os_error(built);
    bool have = attributes(game_exe, g);
    if (!have || ticks(b.ftLastWriteTime) != ticks(g.ftLastWriteTime) || size_of(b) != size_of(g)) {
        if (!CopyFileW(built.c_str(), game_exe.c_str(), FALSE))
            return utf8(built) + "\nUsing " + utf8(base_name(built)) + ": " + os_error(game_exe);
    }
    return utf8(game_exe) + "\n";
}

// The user folder (saves, settings, caches) of this instance: a second one (UNCHARTED_INSTANCE=<n>) starts with copies
// of the main one's settings, saves, trophies and shader caches (an empty cache would compile for minutes).
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
    return utf8(user) + "\nInstance " + suffix.substr(2) + ": new user folder " + utf8(user) +
           " (settings, saves and caches copied)";
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

// For drives that hold no links (exFAT, FAT32): copies of the files the prepare scripts read (the executables,
// sce_module, sce_sys), the update's over the game's, in analysis\views (about 90 MB, copied again only when a source
// changes). The runtime reads the update over the game itself (--app0-overlay).
std::string boot_view(const std::wstring &root, const std::wstring &base, const std::wstring &update) {
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> chosen;   // lower relative -> (relative, source)
    std::vector<std::wstring> order;
    auto choose = [&](const std::wstring &relative, const std::wstring &source) {
        std::wstring key = lower(relative);
        if (!chosen.count(key)) order.push_back(key);
        chosen[key] = {relative, source};
    };
    for (const std::wstring &layer : {base, update}) {   // the update last: its files win
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
        // FAT32 keeps write times in 2-second steps.
        if (attributes(dest, current) && size_of(current) == size_of(wanted)) {
            uint64_t a = ticks(current.ftLastWriteTime), b = ticks(wanted.ftLastWriteTime);
            if ((a > b ? a - b : b - a) <= 20000000ULL) continue;
        }
        make_dirs(dest.substr(0, dest.find_last_of(L'\\')));
        // a plain copy, then the source's times: a read-only source attribute would make the next refresh fail
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

// The game's own icon (sce_sys\icon0.png) as a 64x64 24-bit BMP for the window (SDL loads only BMP), made once into
// launcher\assets\window_icon.bmp. Made as the game starter made it with Pillow (convert to RGB, resize with its
// bicubic filter, save as BMP), byte for byte: Pillow's fixed-point resampling and its BMP header.
struct Rgb {
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // R, G, B per pixel, top row first
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

// Pillow's resampling (libImaging/Resample.c) with its bicubic filter (a = -0.5): coefficients per output pixel,
// normalized, as 22-bit fixed point; a horizontal pass, then a vertical one, each rounding to 8 bits.
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
    std::vector<int> bounds;   // per output pixel: first input, count
    std::vector<int> k;        // ksize per output pixel
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
Rgb horizontal(const Rgb &in, int w, const Coeffs &hz, int row0, int rows) {   // rows row0.. of `in`, to width w
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
Rgb vertical(const Rgb &in, int h, const Coeffs &vt, int row0) {   // to height h; vt's rows are counted from row0
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
    // Pillow 13 runs the vertical pass first when the height shrinks by more than twice as much as the width
    const bool horizontal_first = !((in.h - h) > 0 && (in.h - h) > (in.w - w) * 2);
    if (!horizontal_first && need_v) {
        Rgb mid = vertical(in, h, vt, 0);
        return need_h ? horizontal(mid, w, hz, 0, mid.h) : mid;
    }
    return resize_horizontal_first(in, w, h, hz, vt);
}
Rgb resize_horizontal_first(const Rgb &in, int w, int h, const Coeffs &hz, const Coeffs &vt) {
    // the horizontal pass on the rows the vertical pass uses, then the vertical pass
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
    if (!load_rgb(png, picture, error)) return "\nNo window icon: " + error;
    Rgb small = resize(picture, 64, 64);
    // BMP as Pillow writes it: file header, BITMAPINFOHEADER, BGR rows bottom-up, each padded to 4 bytes
    uint32_t stride = ((uint32_t)small.w * 3 + 3) & ~3u, image = stride * (uint32_t)small.h;
    const uint32_t ppm = 3780;   // 96 dpi: Pillow's resized copy does not keep the PNG's resolution
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
    if (f == INVALID_HANDLE_VALUE) return "\nNo window icon: " + os_error(icon);
    DWORD put = 0;
    bool ok = WriteFile(f, bmp.data(), (DWORD)bmp.size(), &put, nullptr) && put == bmp.size();
    CloseHandle(f);
    if (!ok) return "\nNo window icon: " + os_error(icon);
    return utf8(icon) + "\n";
}

// ------------------------------------------------------------------------------------------------ the update view
// (gow3_pc/scripts/game_update.py) A read-only combined view of a dumped base game and its separate update folder: the
// update is laid over the base in a temporary folder of links, so neither dump is changed and a 47 GB game costs a
// handful of links. Files are hard links first (same drive, no privilege, every stat sees the real size), then symbolic
// links, then (folders) junctions or (files) copies. Names are matched without regard to case.

struct ValueError { std::string message; };

// a link error as Python words one: "[WinError n] text: 'source' -> 'link'"
std::string os_error2(const std::wstring &source, const std::wstring &link, DWORD code = GetLastError()) {
    std::string shown;
    for (char c : utf8(link)) shown += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return os_error(source, code) + " -> '" + shown + "'";
}

// an error without a file name: "[WinError n] text"
std::string win_error(DWORD code = GetLastError()) {
    const std::string full = os_error(L"", code);
    return full.substr(0, full.size() - 4);   // without ": ''"
}

bool is_reparse(const std::wstring &p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool exists(const std::wstring &p) {   // following links (Path.exists)
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

// Path.resolve(): the final path with links followed (\\?\ dropped), else the full path.
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

std::wstring fold(const std::wstring &s) {   // str.casefold for the names games use
    std::wstring o(s);
    if (!o.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), (int)s.size(), &o[0], (int)o.size(), nullptr, nullptr, 0);
    return o;
}

// The folder's entries in the file system's order (os.scandir), without . and ..
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

// A junction (a folder link that needs no privilege), as _winapi.CreateJunction makes one.
bool create_junction(const std::wstring &target, const std::wstring &link) {
    const std::wstring substitute = L"\\??\\" + target;
    const size_t sub = substitute.size() * 2, print = target.size() * 2;
    std::vector<char> buf(16 + sub + 2 + print + 2, 0);
    auto put16 = [&](size_t at, size_t v) { buf[at] = (char)v; buf[at + 1] = (char)(v >> 8); };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(&buf[0], &tag, 4);
    put16(4, buf.size() - 8);   // the reparse data after the 8-byte header
    put16(8, 0);                // the substitute name's offset
    put16(10, sub);
    put16(12, sub + 2);         // the print name's offset
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

// mods.make_link: a hard link (files), a symbolic link, a junction (folders) or a copy (files).
void make_link(const std::wstring &link, const std::wstring &target) {
    const bool file = is_file_followed(target), dir = is_dir_followed(target);
    if (file && CreateHardLinkW(link.c_str(), target.c_str(), nullptr)) return;
    // UNCHARTED_TEST_NO_SYMLINKS=1 (tests): as on a PC without the symlink privilege
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

// game_update.remove: a file, link or folder of a view, without following links into the dumps.
void remove_path(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) throw Failure{os_error(path)};
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(a & FILE_ATTRIBUTE_REPARSE_POINT))   // a real folder: its contents first
            for (const std::wstring &child : entries_of(path)) remove_path(join(path, child));
        if (!RemoveDirectoryW(path.c_str())) throw Failure{os_error(path)};
    } else if (!DeleteFileW(path.c_str())) {
        throw Failure{os_error(path)};
    }
}

// game_update.overlay: source laid over dest (its files replace dest's, folders are merged, names case-insensitive).
// dest holds links to the base; a folder both have becomes a real one.
void overlay(const std::wstring &source, const std::wstring &dest) {
    std::map<std::wstring, std::wstring> names;   // folded -> dest's spelling (the last one wins, as in a dict)
    for (const std::wstring &n : entries_of(dest)) names[fold(n)] = n;
    for (const std::wstring &n : entries_of(source)) {
        const std::wstring item = join(source, n);
        auto it = names.find(fold(n));
        const std::wstring match = join(dest, it != names.end() ? it->second : n);
        if (is_dir_followed(item) && is_dir_followed(match)) {
            if (is_reparse(match)) {   // a linked folder of the base: the link becomes a merged copy
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

bool below(const std::wstring &child, const std::wstring &parent) {   // parent in child.parents (case-insensitive)
    std::wstring c = fold(child), p = fold(parent);
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return c.size() > p.size() + 1 && c.compare(0, p.size(), p) == 0 && (c[p.size()] == L'\\' || c[p.size()] == L'/');
}

// tempfile.mkdtemp(prefix, dir): a new folder with 8 random characters after the prefix.
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

// game_update.create_view: the folder holding base with update over it, made in out; remove it with remove_view.
std::string game_view(const std::wstring &base_in, const std::wstring &update_in, const std::wstring &out) {
    const std::wstring base = resolved(base_in), update = resolved(update_in);
    const std::string base_id = sfo_text(base, "TITLE_ID"), update_id = sfo_text(update, "TITLE_ID");
    if (base_id.empty() || base_id != update_id)
        throw ValueError{"Game and update are not the same title: " + (base_id.empty() ? std::string("None") : base_id) +
                         " / " + (update_id.empty() ? std::string("None") : update_id)};
    if (fold(base) == fold(update) || below(base, update) || below(update, base))
        throw ValueError{"Choose two separate folders for the game and its update"};
    if (!is_file_followed(join(update, L"eboot.bin")))
        throw ValueError{"The update folder needs eboot.bin and sce_sys/param.sfo"};
    if (!make_dirs(out)) throw Failure{os_error(out)};
    const std::wstring target = make_temp_dir(out, L"game-update-");
    try {
        // Folders first: a drive without links (exFAT, FAT32) fails at the first one, before any file is copied.
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

// what: game_executable [root] (lines: the executable, a message), user_dir [root, suffix] (lines: the folder, a
// message), boot_view [root, game, update] (the folder), game_view [game, update, out] (the view's folder), remove_view
// [view] (nothing). args_json: the arguments as a JSON array. 0 ok; 1 failed (out: why, worded as Python's OSError);
// 2 unknown what; 3 the folders can't be combined (out: why, game_update.py's ValueError).
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
    } catch (const ValueError &e) {
        int status = give(e.message, out, cap);
        return status < 0 ? status : 3;
        if (w == "window_icon") return give(window_icon(arg_at(args, 0), arg_at(args, 1)), out, cap);
    } catch (const Failure &f) {
        int status = give(f.message, out, cap);
        return status < 0 ? status : 1;
    }
    return 2;
}

}  // extern "C"
