#pragma once
// Mouse input for the pad (runtime_pad.c turns it into the right stick and PS4 buttons). The
// window thread feeds SDL's mouse events here and captures the mouse (relative mode) while the
// game window has the focus and no menu is open; Alt+Tab or the in-game menu releases it.
// GOW3_MOUSE=0 turns the mouse off.

#include <SDL3/SDL.h>

namespace Gow3Mouse {

bool Enabled();
// Window thread: whether the mouse should be captured now, and capturing/releasing it.
void Update(SDL_Window* window, bool may_capture);
// Window thread: a mouse event; true when it was used (the mouse is captured).
bool OnEvent(const SDL_Event& event);

} // namespace Gow3Mouse

// Pad side (C): movement since the last call (pixels) and the held sources: bit 0 left, 1 middle,
// 2 right, 3 X1, 4 X2, 5 wheel up, 6 wheel down (a wheel step holds its bit for 100 ms).
extern "C" int gow3gpu_mouse_take(float* dx, float* dy, unsigned* buttons);
