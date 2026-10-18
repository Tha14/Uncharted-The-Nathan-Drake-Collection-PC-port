#pragma once
// Mouse input for the pad (runtime_pad.c turns it into the right stick and PS4 buttons). The
// window thread feeds SDL's mouse events here and captures the mouse (relative mode) while the
// game window has the focus and no menu is open; Alt+Tab or the in-game menu releases it.
// The settings' mouse switch turns it off (GOW3_MOUSE=0 at start).

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

// Mouse look (src/runtime_mouselook.c, on the game's camera thread): movement since the last call
// (raw pixels, unscaled); 0 while the mouse is not captured. Movement older than 250 ms (no
// camera update took it: menus, cutscenes) is dropped. Each call marks mouse look active.
extern "C" int gow3gpu_mouse_take_look(float* dx, float* dy);
// Whether the mouse look took movement in the last 200 ms (the pad then leaves the right stick
// to the gamepad).
extern "C" int gow3gpu_mouse_look_recent(void);
// Scripted runs (GOW3_PAD_FILE mx=/my=): adds movement as if the mouse moved; mouse look then
// takes it without a captured mouse.
extern "C" void gow3gpu_mouse_inject(float dx, float dy);
