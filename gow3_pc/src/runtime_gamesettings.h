/* The games' own draw and shadow distances, scaled from the in-game menu (Image tab): Uncharted 2
 * and 3 (1.02) keep them in two settings blocks in the image (the debug menu's "Fade Distance
 * Scale" family, "Max Sunlight Shadow Dist", "Max Shadow Dist (point/spot lights)"). The layout is
 * recognised by the games' defaults before anything is written. */
#pragma once
#include <stdint.h>

void runtime_gamesettings_set_image(unsigned char *image, uint64_t size);
/* Called often (the pad's sampling, on the game's thread): applies the menu's values, at most
 * twice a second. */
void runtime_gamesettings_tick(void);
/* 1 once this game's settings were found. */
int runtime_gamesettings_active(void);
