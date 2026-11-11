// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: numbers for the performance overlay (gow3_overlay.cpp). A thread started once samples
// processor and graphics card load plus their temperatures (PDH counters, NVML), the game's RAM,
// and VRAM through VK_EXT_memory_budget. It runs once a second, and only while the overlay shows
// at least one of those items (20 Oct 2026); GOW3_PERF_SAMPLER_PAUSE=0 makes it run every 500 ms
// without pausing. Anything that could not be measured is left negative and printed as "n/a".
// Since 24 Oct 2026 the temperatures follow the load figures (the card's from nvml.dll, the
// processor's from the ACPI thermal zone), and the card is labelled plain "GPU" rather than by
// its model name, which kept the one-line counter from wrapping.
// 24 Oct 2026: RAM is the game's own memory (its working set), shown without the system's total
// ("RAM 6.1 GB") so the line stays short.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace Vulkan {
class Instance;
}

namespace Gow3PerfStats {

/// Overlay items (Gow3Settings::Values::overlay_items).
enum Item : unsigned {
    Fps = 1u << 0,
    FrameTime = 1u << 1,
    Upscaler = 1u << 2,
    Gpu = 1u << 3,
    Cpu = 1u << 4,
    Ram = 1u << 5,
    Vram = 1u << 6,
    AllItems = (1u << 7) - 1,
};
inline constexpr unsigned DefaultItems = Fps | FrameTime | Upscaler;

struct Sample {
    float fps = 0.0f, frame_ms = 0.0f;
    std::string upscaler;
    std::string gpu_name;
    float gpu_percent = -1.0f, cpu_percent = -1.0f;
    float gpu_temp_c = -1.0f; ///< graphics card, degrees Celsius
    float cpu_temp_c = -1.0f; ///< processor (ACPI zone), degrees Celsius
    // RAM used = this process's working set, RAM total = installed memory; the VRAM total is the
    // budget the driver grants.
    double ram_used_gb = -1.0, ram_total_gb = -1.0, vram_used_gb = -1.0, vram_total_gb = -1.0;
};

/// Process CPU time over wall time on every core, 0 to 100; -1 without a wall interval.
inline float CpuPercent(uint64_t process_delta, uint64_t wall_delta, unsigned cores) {
    if (!wall_delta || !cores) {
        return -1.0f;
    }
    return std::clamp(float(double(process_delta) * 100.0 / (double(wall_delta) * cores)), 0.0f, 100.0f);
}

/// The overlay text for the items in `mask`: one line each, or one line when `row`.
inline std::string Format(const Sample& s, unsigned mask, bool row) {
    std::string out;
    char part[160];
    const auto add = [&] {
        if (!out.empty()) {
            out += row ? "   " : "\n";
        }
        out += part;
    };
    const auto pair = [](char* to, size_t size, const char* label, double used, double total) {
        if (used < 0.0) {
            std::snprintf(to, size, "%s n/a", label);
        } else if (total < 0.0) {
            std::snprintf(to, size, "%s %.1f GB", label, used);
        } else {
            std::snprintf(to, size, "%s %.1f / %.1f GB", label, used, total);
        }
    };
    // Load in percent, then the temperature when one is known. The degree sign is UTF-8 (U+00B0),
    // a glyph DejaVu Sans, the overlay font, provides.
    const auto load = [](char* to, size_t size, const char* label, float percent, float celsius) {
        if (percent < 0.0f) {
            std::snprintf(to, size, "%s n/a", label);
        } else if (celsius < 0.0f) { // no sensor reading
            std::snprintf(to, size, "%s %.0f%%", label, percent);
        } else {
            std::snprintf(to, size, "%s %.0f%% %.0f\xc2\xb0" "C", label, percent, celsius);
        }
    };
    if (mask & Fps) {
        std::snprintf(part, sizeof(part), "%.0f FPS", s.fps);
        add();
    }
    if (mask & FrameTime) {
        std::snprintf(part, sizeof(part), "%.1f ms", s.frame_ms);
        add();
    }
    if (mask & Upscaler) {
        std::snprintf(part, sizeof(part), "%s", s.upscaler.empty() ? "Native" : s.upscaler.c_str());
        add();
    }
    if (mask & Gpu) {
        // Labelled "GPU", not by s.gpu_name, so the single-line counter stays short.
        load(part, sizeof(part), "GPU", s.gpu_percent, s.gpu_temp_c);
        add();
    }
    if (mask & Cpu) {
        load(part, sizeof(part), "CPU", s.cpu_percent, s.cpu_temp_c);
        add();
    }
    if (mask & Ram) {
        // Only the amount in use is printed, never the total (24 Oct 2026).
        constexpr double kNoTotal = -1.0;
        pair(part, sizeof(part), "RAM", s.ram_used_gb, kNoTotal);
        add();
    }
    if (mask & Vram) {
        pair(part, sizeof(part), "VRAM", s.vram_used_gb, s.vram_total_gb);
        add();
    }
    return out;
}

/// Starts the sampling thread the first time. Every call also keeps it sampling: it sleeps
/// once no call came for 2 s (the overlay calls it on each frame that shows a sampled item).
void Start(const Vulkan::Instance& instance);
/// Most recent sampler output: load, temperature and memory figures. FPS, frame time and the
/// upscaler name are left for the caller to fill in.
Sample Latest();

} // namespace Gow3PerfStats
