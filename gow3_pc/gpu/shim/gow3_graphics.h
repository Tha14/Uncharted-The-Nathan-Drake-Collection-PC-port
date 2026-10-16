// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: graphics options of the in-game menu (Insert / R3+L2), kept in gow3.ini. The live ones
// apply on the next frame; the startup ones (patches, GPU readback modes) are read by run.py and
// apply through "Apply and restart". The launcher reads and writes the same keys.
#pragma once

#include <array>
#include <string_view>

namespace Gow3Graphics {

enum DisplayMode : int { Windowed = 0, Borderless = 1, Fullscreen = 2, DisplayModeCount };
inline constexpr std::array<std::string_view, DisplayModeCount> DisplayModeKeys = {
    "windowed", "borderless", "fullscreen"};
inline constexpr std::array<const char*, DisplayModeCount> DisplayModeNames = {
    "Windowed", "Borderless", "Fullscreen"};

/// Present rate limits. Uncharted: at most 120; faster, movement timing breaks (Drake's
/// Fortune at 140-160 FPS: running slows, Nate fell through the world).
inline constexpr int MaxFpsLimit = 120;
/// 40-55 for tests (Uncharted 3's cloth flickers at 60 FPS but not at 30, 16 Oct).
inline constexpr std::array<int, 7> FpsLimits = {30, 40, 45, 50, 55, 60, MaxFpsLimit};

/// Game patch suffixes ("Resolution Patch - <name>"); "native" is no resolution patch.
inline constexpr std::array<std::string_view, 6> Resolutions = {"native", "480p", "720p",
                                                                "1440p", "1800p", "4K"};
/// Engine frame rate ceilings: 60 is the game's own, 120 and 240 are frame rate patches.
inline constexpr std::array<int, 3> EngineFps = {60, 120, 240};

template <typename T, std::size_t N>
constexpr int IndexOf(const std::array<T, N>& values, const T& value, int fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (values[i] == value) return int(i);
    }
    return fallback;
}

inline int ParseDisplayMode(std::string_view text) {
    return IndexOf(DisplayModeKeys, text, Windowed);
}
/// A listed limit; unlimited (0) and faster ones (earlier lists had 144-240) are the maximum.
inline int ParseFpsLimit(int value) {
    if (IndexOf(FpsLimits, value, -1) >= 0) return value;
    return value == 0 || value > MaxFpsLimit ? MaxFpsLimit : 60;
}
/// Index into Resolutions; unknown text is native.
inline int ParseResolution(std::string_view text) {
    return IndexOf(Resolutions, text, 0);
}
/// A listed ceiling, else 120 (the default patch).
inline int ParseEngineFps(int value) {
    return IndexOf(EngineFps, value, -1) >= 0 ? value : 120;
}

/// Frames the GPU command thread may queue ahead of the GPU; 0 = unbounded.
inline constexpr std::array<int, 3> FramesQueued = {1, 2, 0};
inline int ParseFramesQueued(int value) {
    return IndexOf(FramesQueued, value, -1) >= 0 ? value : 1;
}
/// RCAS strength in percent; invalid values fall back to 75.
inline int ParseRcasStrength(int value) {
    return value >= 0 && value <= 100 ? value : 75;
}
/// FSR RCAS attenuation for a strength: 0 is the sharpest, 2 the softest.
inline float RcasAttenuation(int strength) {
    return 2.0f * (1.0f - float(ParseRcasStrength(strength)) / 100.0f);
}
/// Whether the FSR pass runs: FSR 1 when the window is larger than the game's image, RCAS
/// alone when they are the same size. A smaller window is left to the regular scaling.
inline bool FsrPassRuns(bool fsr1, bool rcas, int in_w, int in_h, int out_w, int out_h) {
    const bool upscale = in_w < out_w && in_h < out_h;
    const bool same = in_w == out_w && in_h == out_h;
    return (fsr1 && upscale) || (rcas && same);
}

/// What the present limit really allows: the engine ceiling caps it.
inline int EffectiveFps(int limit, int engine_fps) {
    return limit == 0 || limit > engine_fps ? engine_fps : limit;
}

/// The present thread's frame limit, at most MaxFpsLimit (also for 0 and older, faster
/// settings). Above 60 it needs the fast vblank (GOW3_VBLANK_HZ=0); at 60 Hz the vblank paces.
inline int PresentLimit(int limit) {
    return limit == 0 || limit > MaxFpsLimit ? MaxFpsLimit : limit;
}

} // namespace Gow3Graphics
