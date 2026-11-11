// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: thumbnails for the save list. While the player is in control (the camera hooks are being
// called, so neither the pause menu nor a cutscene is up), the presenter shrinks roughly one game
// frame per second, before the overlay goes on top. Saves are given the latest one as thumb.png.
#ifndef GOW3_SHIM_THUMBNAIL_H
#define GOW3_SHIM_THUMBNAIL_H

namespace Gow3Thumbnail {

/// Picture size: double the 228x128 the save list displays.
inline constexpr unsigned Width = 456;
inline constexpr unsigned Height = 256;

/// Present thread. True when this frame should be captured: a second or more has passed since
/// the previous request and the camera hooks were called in the meantime.
[[nodiscard]] bool Wanted();

/// Present thread. Takes a finished capture: Width x Height RGBA8 pixels, top row first.
void Store(const unsigned char* pixels);

/// The game opened its "Save game" list. A save written in the following two minutes is given
/// the picture set aside when Options was pressed (gow3gpu_thumbnail_pause).
void SaveListOpened();

} // end of namespace Gow3Thumbnail

#endif // GOW3_SHIM_THUMBNAIL_H
