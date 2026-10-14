#include "gow3_movie.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

extern "C" void* runtime_map_guest_memory(std::uint64_t size);

namespace Gow3Movie {
namespace {

struct Size {
    std::uint32_t width = 0, height = 0;
};

const Size& Display() {
    static const Size size = [] {
        Size s;
        if (const char* env = std::getenv("GOW3_MOVIE_SIZE")) {
            if (std::sscanf(env, "%ux%u", &s.width, &s.height) != 2 || s.width < 64 || s.height < 64 ||
                s.width > 7680 || s.height > 4320) {
                s = {};
            }
        }
        return s;
    }();
    return size;
}

constexpr std::uint32_t Align64(std::uint32_t value) {
    return (value + 63) & ~63u;
}

// The frame buffers of the open decoders (a game cycles through a handful): base, and the layout
// packed as width | height << 16 | pitch << 32 | rows << 48; and their scratch frames.
struct Frame {
    std::atomic<std::uint64_t> base{0};
    std::atomic<std::uint64_t> layout{0};
};
std::array<Frame, 32> frames;
std::atomic<std::uint32_t> next{0};

struct Scratch {
    std::atomic<std::uint64_t> base{0};
    std::atomic<std::uint64_t> address{0};
};
std::array<Scratch, 16> scratch;
std::mutex scratch_mutex;

std::uint64_t FindScratch(std::uint64_t base) {
    for (auto& entry : scratch) {
        if (entry.base.load(std::memory_order_acquire) == base) {
            return entry.address.load(std::memory_order_acquire);
        }
    }
    return 0;
}

} // namespace

bool DisplaySize(std::uint32_t& width, std::uint32_t& height) {
    width = Display().width;
    height = Display().height;
    return width != 0;
}

std::uint32_t ScaledPitch() {
    return Align64(Display().width);
}

std::uint32_t ScaledRows() {
    return Align64(Display().height);
}

void RecordFrame(std::uint64_t base, std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
                 std::uint32_t rows) {
    if (!Display().width || !base) {
        return;
    }
    const std::uint64_t layout = std::uint64_t(width & 0xffff) | std::uint64_t(height & 0xffff) << 16 |
                                 std::uint64_t(pitch & 0xffff) << 32 | std::uint64_t(rows & 0xffff) << 48;
    for (auto& frame : frames) {
        if (frame.base.load(std::memory_order_acquire) == base) {
            frame.layout.store(layout, std::memory_order_release);
            return;
        }
    }
    auto& frame = frames[next.fetch_add(1, std::memory_order_relaxed) % frames.size()];
    frame.base.store(0, std::memory_order_release);
    frame.layout.store(layout, std::memory_order_release);
    frame.base.store(base, std::memory_order_release);
}

std::uint8_t* ScratchFrame(std::uint64_t base) {
    if (!Display().width || !base) {
        return nullptr;
    }
    if (const std::uint64_t address = FindScratch(base)) {
        return reinterpret_cast<std::uint8_t*>(address);
    }
    std::scoped_lock lock{scratch_mutex};
    if (const std::uint64_t address = FindScratch(base)) {
        return reinterpret_cast<std::uint8_t*>(address);
    }
    for (auto& entry : scratch) {
        if (entry.base.load(std::memory_order_acquire)) {
            continue;
        }
        const std::uint64_t bytes = std::uint64_t(ScaledPitch()) * ScaledRows() * 3 / 2;
        void* address = runtime_map_guest_memory(bytes);
        if (!address) {
            std::printf("Movie: no scratch frame (%llu bytes) for %#llx; movies are shown as decoded\n",
                        (unsigned long long)bytes, (unsigned long long)base);
            std::fflush(stdout);
            return nullptr;
        }
        entry.address.store(reinterpret_cast<std::uint64_t>(address), std::memory_order_release);
        entry.base.store(base, std::memory_order_release);
        std::printf("Movie: frame buffer %#llx read as %ux%u from %p\n", (unsigned long long)base,
                    Display().width, Display().height, address);
        std::fflush(stdout);
        return static_cast<std::uint8_t*>(address);
    }
    return nullptr;
}

bool Remap(std::uint64_t& address, std::uint32_t& width, std::uint32_t& height, std::uint32_t& pitch,
           std::uint32_t bits) {
    const Size& display = Display();
    if (!display.width) {
        return false;
    }
    const bool luma = bits == 8 && width == display.width && height == display.height;
    const bool chroma = bits == 16 && width == display.width / 2 && height == display.height / 2;
    if (!luma && !chroma) {
        return false;
    }
    // The player's chroma offset: (width * align(height, 64)) & ~63, in 32 bits.
    const std::uint64_t chroma_offset = std::uint32_t(display.width * Align64(display.height)) & ~63u;
    for (auto& frame : frames) {
        const std::uint64_t base = frame.base.load(std::memory_order_acquire);
        if (!base || address != (luma ? base : base + chroma_offset)) {
            continue;
        }
        if (const std::uint64_t copy = FindScratch(base)) {
            address = luma ? copy : copy + std::uint64_t(ScaledPitch()) * ScaledRows();
            pitch = luma ? ScaledPitch() : ScaledPitch() / 2;
            return true;
        }
        const std::uint64_t layout = frame.layout.load(std::memory_order_acquire);
        const std::uint32_t w = layout & 0xffff, h = (layout >> 16) & 0xffff;
        const std::uint32_t p = (layout >> 32) & 0xffff, rows = (layout >> 48) & 0xffff;
        if (luma) {
            width = w;
            height = h;
            pitch = p;
        } else {
            address = base + std::uint64_t(p) * rows;
            width = w / 2;
            height = h / 2;
            pitch = p / 2;
        }
        return true;
    }
    return false;
}

} // namespace Gow3Movie
