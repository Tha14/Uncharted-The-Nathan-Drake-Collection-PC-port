/* The games' draw and shadow distances (see runtime_gamesettings.h). */
#include "runtime_gamesettings.h"
#include "gpu/gow3gpu.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Image offsets of the render settings (fade distance scales: temporal +0x150, cutscene +0x168,
 * objects +0x180) and of the light settings (max sun shadow distance +0xc0, point/spot light
 * shadows +0xc8). Found from the debug menu items' variables (U2 0xbade30, U3 0xe16393). */
typedef struct { const char *game; uint64_t render, light; } Layout;
static const Layout layouts[] = {
    {"Uncharted 2", 0x1247b40, 0x1162820},
    {"Uncharted 3", 0x18c6280, 0x1b3c130},
};
enum { TEMPORAL_FADE = 0x150, CUTSCENE_FADE = 0x168, FADE = 0x180, SUN_SHADOW = 0xc0, LIGHT_SHADOW = 0xc8 };

static unsigned char *image;
static uint64_t image_size;
static const Layout *layout;
static unsigned checks;
static float base_fade[3], base_sun, base_light;

void runtime_gamesettings_set_image(unsigned char *base, uint64_t size) {
    image = base;
    image_size = size;
}

int runtime_gamesettings_active(void) { return layout != NULL; }

static float read_float(uint64_t at) {
    float v;
    memcpy(&v, image + at, sizeof(v));
    return v;
}
static void write_float(uint64_t at, float v) {
    if (read_float(at) != v) memcpy(image + at, &v, sizeof(v));
}

void runtime_gamesettings_tick(void) {
    static uint64_t last;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    if (!image || now - last < 500) return;
    last = now;
    if (!layout) {
        /* The game sets its defaults while starting: look for them for two minutes. */
        if (checks++ > 240) return;
        for (size_t i = 0; i < sizeof(layouts) / sizeof(*layouts); ++i) {
            const Layout *l = &layouts[i];
            if (l->render + FADE + 4 > image_size || l->light + LIGHT_SHADOW + 4 > image_size) continue;
            if (read_float(l->render + FADE) != 0.58f || read_float(l->light + SUN_SHADOW) != 40.0f) continue;
            layout = l;
            base_fade[0] = read_float(l->render + TEMPORAL_FADE);
            base_fade[1] = read_float(l->render + CUTSCENE_FADE);
            base_fade[2] = read_float(l->render + FADE);
            base_sun = read_float(l->light + SUN_SHADOW);
            base_light = read_float(l->light + LIGHT_SHADOW);
            printf("Game settings: %s draw distances found (fade scale %.2f, sun shadows %.0f m, light shadows %.0f m)\n",
                   l->game, base_fade[2], base_sun, base_light);
            fflush(stdout);
            break;
        }
        if (!layout) return;
    }
    const float fade = gow3gpu_settings_fade_distance() / 100.0f;
    const float sun = gow3gpu_settings_sun_shadow_distance() / 100.0f;
    const float light = gow3gpu_settings_light_shadow_distance() / 100.0f;
    write_float(layout->render + TEMPORAL_FADE, base_fade[0] * fade);
    write_float(layout->render + CUTSCENE_FADE, base_fade[1] * fade);
    write_float(layout->render + FADE, base_fade[2] * fade);
    write_float(layout->light + SUN_SHADOW, base_sun * sun);
    write_float(layout->light + LIGHT_SHADOW, base_light * light);
}
