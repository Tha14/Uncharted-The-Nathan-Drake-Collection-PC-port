#include "gow3_mouse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Gow3Mouse {
namespace {

std::mutex mutex;
float acc_x = 0, acc_y = 0;
unsigned held = 0; // bits 0-4: SDL buttons 1-5
std::chrono::steady_clock::time_point wheel_until[2];
bool captured = false;

constexpr unsigned WheelUp = 1u << 5, WheelDown = 1u << 6;

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_MOUSE");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

void Update(SDL_Window* window, bool may_capture) {
    const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool want = Enabled() && may_capture && focused;
    if (want == captured) {
        return;
    }
    captured = want;
    SDL_SetWindowRelativeMouseMode(window, want);
    std::scoped_lock lock{mutex};
    acc_x = acc_y = 0;
    held = 0; // no button stays pressed across a release (Alt+Tab with the trigger held)
}

bool OnEvent(const SDL_Event& event) {
    if (!captured) {
        return false;
    }
    std::scoped_lock lock{mutex};
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        acc_x += event.motion.xrel;
        acc_y += event.motion.yrel;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button >= 1 && event.button.button <= 5) {
            const unsigned bit = 1u << (event.button.button - 1);
            held = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? held | bit : held & ~bit;
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (event.wheel.y != 0) {
            wheel_until[event.wheel.y > 0 ? 0 : 1] =
                std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
        }
        return true;
    default:
        return false;
    }
}

} // namespace Gow3Mouse

extern "C" int gow3gpu_mouse_take(float* dx, float* dy, unsigned* buttons) {
    using namespace Gow3Mouse;
    std::scoped_lock lock{mutex};
    *dx = acc_x;
    *dy = acc_y;
    acc_x = acc_y = 0;
    const auto now = std::chrono::steady_clock::now();
    *buttons = held | (now < wheel_until[0] ? WheelUp : 0) | (now < wheel_until[1] ? WheelDown : 0);
    return captured ? 1 : 0;
}
