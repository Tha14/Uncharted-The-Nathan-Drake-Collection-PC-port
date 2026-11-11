// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: keeps the newest in-play frame for the save list and writes it out as PNG files; see
// gow3_thumbnail.h for when the presenter takes frames.
#include "gow3_thumbnail.h" // Width, Height and the calls the presenter makes
#include "../gow3gpu.h"     // gow3gpu_mouse_look_calls and the C exports defined below

#include <miniz.h> // PNG encoding in memory

#include <chrono>  // steady clock time stamps
#include <cstdio>  // PNG file output and the log line
#include <cstdlib> // GOW3_SAVE_PICTURE
#include <mutex>   // state shared between the present, game and writer threads
#include <string>  // queued file names
#include <thread>  // background PNG writer
#include <vector>  // pixel buffers

using SteadyClock = std::chrono::steady_clock;

namespace Gow3Thumbnail::Internal {

using Moment = SteadyClock::time_point;

// A picture no older than this shows where the save was made.
constexpr std::chrono::seconds RecentEnough{3};
// After a break, play has to run this long (the fade-in is over) before its pictures count.
constexpr std::chrono::seconds SettleTime{2};
// A save still waiting after this long keeps the picture the game made for it.
constexpr std::chrono::minutes WaitLimit{10};
// How long after the save list opened a save takes the picture from before the pause menu.
constexpr std::chrono::minutes SaveListWindow{2};
// Saves that may wait for a picture at the same time.
constexpr std::size_t MaxWaitingSaves = 16;

// gow3 3 Nov 2026: a save written while the game was not in play. A chapter's first autosave, for
// example, comes after its cutscene or loading screen, when the newest picture still shows the
// end of the previous chapter; such saves get the first picture taken once play has settled.
struct WaitingSave {
    std::string file;
    Moment queued;
};

struct Shared {
    std::mutex lock;
    std::vector<unsigned char> latest_rgb; // RGB8, Width x Height; empty until the first capture
    Moment latest_time{};                  // when latest_rgb was captured
    Moment play_resumed{};                 // when the camera hooks started running again
    std::vector<WaitingSave> waiting;
    // gow3 3 Nov 2026: the camera hooks go on running behind the pause menu, so captures continue
    // there too. The picture held when Options was last pressed, from before the menu appeared, is
    // what a save made through the save list (reached from that menu) receives.
    std::vector<unsigned char> paused_rgb;
    Moment save_list_time{};
};
Shared shared;

// Touched by the present thread only.
struct Pacing {
    Moment previous_request{};
    unsigned hook_calls_seen = 0;
};
Pacing pacing;

// gow3gpu_thumbnail_save results.
constexpr int SaveFailed = 0, SaveWritten = 1, SaveDeferred = 2;

bool EncodeAndWrite(const std::string& file, const std::vector<unsigned char>& rgb) {
    std::size_t png_size = 0;
    void* const encoded = tdefl_write_image_to_png_file_in_memory_ex(
        rgb.data(), static_cast<int>(Width), static_cast<int>(Height), 3, &png_size, 6, MZ_FALSE);
    if (encoded == nullptr) return false;
    bool written = false;
    if (FILE* out = std::fopen(file.c_str(), "wb")) {
        written = std::fwrite(encoded, 1, png_size, out) == png_size;
        std::fclose(out);
    }
    mz_free(encoded);
    if (!written) std::remove(file.c_str()); // no half-written PNG left behind
    return written;
}

// GOW3_SAVE_PICTURE=0 switches pictures off: saves keep the one the game made.
bool PicturesEnabled() {
    const char* value = std::getenv("GOW3_SAVE_PICTURE");
    return value == nullptr || value[0] != '0';
}

} // namespace Gow3Thumbnail::Internal

bool Gow3Thumbnail::Wanted() {
    using namespace Internal;
    static const bool on = PicturesEnabled();
    if (!on) return false;
    const Moment now = SteadyClock::now();
    const auto since_previous = now - pacing.previous_request;
    if (since_previous < std::chrono::seconds(1)) return false;
    const unsigned hook_calls = gow3gpu_mouse_look_calls();
    const bool hooks_ran = hook_calls != pacing.hook_calls_seen;
    if (hooks_ran) {
        pacing.hook_calls_seen = hook_calls;
        // Over two seconds without a request means the camera hooks have only just started again.
        if (since_previous > std::chrono::seconds(2)) {
            std::lock_guard guard{shared.lock};
            shared.play_resumed = now;
        }
        pacing.previous_request = now;
    }
    return hooks_ran;
}

void Gow3Thumbnail::Store(const unsigned char* pixels) {
    using namespace Internal;
    constexpr std::size_t pixel_count = std::size_t{Width} * Height;
    std::vector<unsigned char> rgb(pixel_count * 3);
    const unsigned char* in = pixels;
    unsigned char* out = rgb.data();
    for (std::size_t n = 0; n < pixel_count; ++n, in += 4, out += 3) {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
    }
    const Moment now = SteadyClock::now();
    std::vector<std::string> ready;
    {
        std::lock_guard guard{shared.lock};
        shared.latest_rgb = rgb;
        shared.latest_time = now;
        const bool settled = now - shared.play_resumed >= SettleTime;
        if (settled && !shared.waiting.empty()) {
            for (const WaitingSave& save : shared.waiting) {
                if (now - save.queued < WaitLimit) ready.push_back(save.file);
            }
            shared.waiting.clear();
        }
    }
    if (ready.empty()) return;
    // Encoding takes a few milliseconds, so it runs on a thread of its own, not the present one.
    std::thread writer{[files = std::move(ready), image = std::move(rgb)] {
        for (const std::string& file : files) {
            if (EncodeAndWrite(file, image)) {
                std::printf("GPU: save picture written once play went on (%s)\n",
                            file.c_str());
            }
        }
    }};
    writer.detach();
}

void Gow3Thumbnail::SaveListOpened() {
    auto& state = Internal::shared;
    std::lock_guard guard{state.lock};
    state.save_list_time = SteadyClock::now();
}

extern "C" int gow3gpu_thumbnail_save(const char* png_path) {
    using namespace Gow3Thumbnail::Internal;
    if (png_path == nullptr || png_path[0] == '\0') return SaveFailed;
    std::vector<unsigned char> chosen_rgb;
    {
        std::lock_guard guard{shared.lock};
        const Moment now = SteadyClock::now();
        const bool from_save_list = shared.save_list_time != Moment{} &&
                                    now - shared.save_list_time < SaveListWindow &&
                                    !shared.paused_rgb.empty();
        if (from_save_list) {
            // Used for the whole window: the game may write its profile save next to the chosen one.
            chosen_rgb = shared.paused_rgb;
        } else {
            const bool in_play = !shared.latest_rgb.empty() &&
                                 now - shared.latest_time <= RecentEnough &&
                                 now - shared.play_resumed >= SettleTime;
            if (!in_play) {
                // Queue the file for the next good picture, each file name at most once.
                std::erase_if(shared.waiting,
                              [png_path](const WaitingSave& save) { return save.file == png_path; });
                if (shared.waiting.size() < MaxWaitingSaves) {
                    shared.waiting.push_back(WaitingSave{png_path, now});
                }
                return SaveDeferred;
            }
            chosen_rgb = shared.latest_rgb;
        }
    }
    return EncodeAndWrite(png_path, chosen_rgb) ? SaveWritten : SaveFailed;
}

extern "C" void gow3gpu_thumbnail_pause(void) {
    auto& state = Gow3Thumbnail::Internal::shared;
    std::lock_guard guard{state.lock};
    state.paused_rgb = state.latest_rgb;
}
