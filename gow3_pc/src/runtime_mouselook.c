/* Mouse look handlers (see runtime_mouselook.h). They run on the game's thread inside its camera
 * update, once per frame, so they take the mouse movement since the last frame and apply it to
 * the camera state the function is about to use. Small stacks: the game runs this in job fibers
 * with 64 KiB stacks. */
#include "runtime_mouselook.h"
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned char *image;

void runtime_mouselook_set_image(unsigned char *base) { image = base; }

int runtime_mouselook_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        /* Also with the mouse off: the hooks give the controller camera speed too. Without a
         * captured mouse (off, or no focus) they take no mouse movement. */
        const char *look = getenv("GOW3_MOUSE_LOOK");
        enabled = !(look && look[0] == '0');
    }
    return enabled;
}

/* Radians per mouse count at sensitivity 1 (the settings' mouse sensitivity scales it, live;
 * 0.0011 rad is about 0.063 degrees: a 2,000-count swipe turns about 126 degrees). */
static float look_scale(void) {
    const float s = gow3gpu_controls_mouse_sens();
    return 0.0011f * (s > 0.0f ? s : 1.0f);
}
static float look_invert(void) {
    return gow3gpu_controls_mouse_invert() ? -1.0f : 1.0f;
}

/* Controller camera and aim speed (menu Controls tab, GOW3_STICK_SPEED). Uncharted 2's and 3's
 * camera input functions read the right stick once, at their start, from stick + 8 and + 0xc
 * (floats, -1..1; nothing else of it). They get a scaled copy instead (the stub passes the
 * returned pointer as their third argument): the camera turns faster or slower at every
 * deflection, aiming included, with the game's ramp and invert options kept. The caller's stick
 * stays as it was. */
static int look_debug(void);
static void stick_debug(const char *game, const float *look, const float *stick);
static _Thread_local float scaled_stick[4];
static void *scale_stick(const float *stick) {
    const float speed = gow3gpu_controls_stick_speed();
    if (!stick || !(speed > 0.0f) || speed == 1.0f) return NULL;
    memcpy(scaled_stick, stick, sizeof(scaled_stick));
    scaled_stick[2] *= speed;
    scaled_stick[3] *= speed;
    return scaled_stick;
}

/* Uncharted 3 (1.02), camera input at 0x4e42f0(camera, look direction, stick input, ...):
 * yaw rotates the look direction (x, y, z, 0) about +Y by -stickX * ramp * scale * dt degrees;
 * pitch is a 0..1 position on the camera's vertical curve, stepped by the game's own function
 * 0x100ef90(camera + 0x7750, pitch, delta) with delta = speed(0x7588) * dt * stickY *
 * scale(0x7608). The mouse goes through the same paths without the ramp, dead zone or speed cap.
 * The vertical step per count matches the horizontal one through the stick rates' ratio. */
typedef ABI float (*U3PitchStep)(void *limits, float pitch, float delta);
static ABI void *u3_camera_input(unsigned char *camera, float *look, void *stick) {
    if (look_debug()) stick_debug("U3", look, stick);
    float dx = 0, dy = 0;
    if (!gow3gpu_mouse_take_look(&dx, &dy) || (dx == 0.0f && dy == 0.0f) || !camera || !look) {
        return scale_stick(stick);
    }
    const float per_count = look_scale();
    if (dx != 0.0f) {
        const float angle = -dx * per_count;
        const float c = cosf(angle), s = sinf(angle);
        const float x = look[0], z = look[2];
        look[0] = x * c + z * s;
        look[2] = z * c - x * s;
    }
    if (dy != 0.0f) {
        const float *f = (const float *)camera;
        const float yaw_full = f[0x7578 / 4] * f[0x7604 / 4] * 0.017453292f; /* rad/s at full stick */
        const float pitch_full = f[0x7588 / 4] * f[0x7608 / 4];             /* curve units/s */
        if (yaw_full > 1e-4f && pitch_full > 0.0f && isfinite(yaw_full) && isfinite(pitch_full)) {
            const float delta = dy * per_count * look_invert() * (pitch_full / yaw_full);
            float *pitch = (float *)(camera + 0x7874);
            const U3PitchStep step = (U3PitchStep)(image + 0x100ef90);
            const float next = step(camera + 0x7750, *pitch, delta);
            if (isfinite(next)) {
                *pitch = next;
            }
        }
    }
    return scale_stick(stick);
}

/* Yaw about +Y, shared by Uncharted 2 and 3: the same angle convention as their stick path
 * (positive stick X = negative angle = turning right). */
static void rotate_look(float *look, float dx, float per_count) {
    const float angle = -dx * per_count;
    const float c = cosf(angle), s = sinf(angle);
    const float x = look[0], z = look[2];
    look[0] = x * c + z * s;
    look[2] = z * c - x * s;
}

/* Uncharted 2 (1.02), camera input at 0x428f40 with the same arguments and math as Uncharted 3's,
 * other offsets: tuning at *(camera + 0x8d00) (yaw ramp max +0x70, pitch speed +0x80), yaw scale
 * camera + 0x9fa8, pitch scale + 0x9fac, pitch at + 0x9e60 stepped by 0x4288d0(camera, delta),
 * which returns the new pitch. */
typedef ABI float (*U2PitchStep)(void *camera, float delta);
static ABI void *u2_camera_input(unsigned char *camera, float *look, void *stick) {
    if (look_debug()) stick_debug("U2", look, stick);
    float dx = 0, dy = 0;
    if (!gow3gpu_mouse_take_look(&dx, &dy) || (dx == 0.0f && dy == 0.0f) || !camera || !look) {
        return scale_stick(stick);
    }
    const float per_count = look_scale();
    if (dx != 0.0f) {
        rotate_look(look, dx, per_count);
    }
    const float *tune = *(const float *const *)(camera + 0x8d00);
    if (dy != 0.0f && tune) {
        const float *f = (const float *)camera;
        const float yaw_full = tune[0x70 / 4] * f[0x9fa8 / 4] * 0.017453292f;
        const float pitch_full = tune[0x80 / 4] * f[0x9fac / 4];
        if (yaw_full > 1e-4f && pitch_full > 0.0f && isfinite(yaw_full) && isfinite(pitch_full)) {
            const float delta = dy * per_count * look_invert() * (pitch_full / yaw_full);
            const U2PitchStep step = (U2PitchStep)(image + 0x4288d0);
            const float next = step(camera, delta);
            if (isfinite(next)) {
                *(float *)(camera + 0x9e60) = next;
            }
        }
    }
    return scale_stick(stick);
}

/* Uncharted 1 (1.02): the cameras' input step is a virtual (vtable +0x98) called from 0xb62a5
 * with params: +0x10 the pad input (floats: left x, left y, right x, right y; flags +0x30). The
 * orbit camera, 0xb7430, turns by right x * factor * the game's camera speed (0xa4cb10) * 180
 * degrees per second and pitches by right y * factor * speed * -90 (factor 1, or 1.5 / 0.75 for
 * input flags 0x100 / 0x300), over the frame time at 0xa67000; the mouse adds the stick value
 * that turns exactly its angle this frame (no dead zone, response curve or speed cap; the game's
 * own camera speed cancels out). GOW3_MOUSE_LOOK_DEBUG=1 logs the values once a second. */
static int look_debug(void) {
    static int debug = -1;
    if (debug < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_DEBUG");
        debug = env && env[0] == '1';
    }
    return debug;
}
/* GOW3_MOUSE_LOOK_DEBUG=1: once a second the camera's heading (degrees about +Y from the look
 * direction at entry), how far it turned since the last line, and the right stick it reads. */
static void stick_debug(const char *game, const float *look, const float *stick) {
    static uint64_t last;
    static float last_heading;
    static unsigned calls;
    ++calls;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    if (now - last < 1000 || !look || !stick) return;
    const float heading = atan2f(look[0], look[2]) * 57.29578f;
    float turned = heading - last_heading;
    while (turned > 180.0f) turned -= 360.0f;
    while (turned < -180.0f) turned += 360.0f;
    printf("Camera debug: %s heading %.1f, turned %.1f over %u calls, right stick %.2f %.2f, speed %.2f\n", game,
           heading, last ? turned : 0.0f, calls, stick[2], stick[3], gow3gpu_controls_stick_speed());
    fflush(stdout);
    last = now;
    last_heading = heading;
    calls = 0;
}
static void u1_debug(const char *site, const unsigned char *params, float dx, float dy) {
    static uint64_t last[2];
    static unsigned calls[2];
    const int i = site[0] == 'o' ? 0 : 1;
    ++calls[i];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    if (now - last[i] < 1000 || !params) return;
    last[i] = now;
    const float *input = *(const float *const *)(params + 0x10);
    printf("Mouse look debug: %s %u calls, params+0x18 %g, dt %g, speed %g", site, calls[i],
           *(const float *)(params + 0x18), *(const float *)(image + 0xa67000), *(const float *)(image + 0xa4cb10));
    if (input)
        printf(", input %g %g %g %g flags %#x", input[0], input[1], input[2], input[3],
               *(const unsigned *)((const unsigned char *)input + 0x30));
    printf(", mouse %g %g\n", dx, dy);
    fflush(stdout);
    calls[i] = 0;
}
/* Not verified in game yet: on only with GOW3_MOUSE_LOOK_U1=1 (otherwise the mouse stays the stick). */
static int u1_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_U1");
        enabled = env && env[0] == '1';
    }
    return enabled;
}
static ABI void *u1_orbit_camera_input(unsigned char *self, unsigned char *camera, unsigned char *params) {
    (void)self;
    (void)camera;
    if (!u1_enabled() && !look_debug()) return NULL;
    float dx = 0, dy = 0;
    const int took = u1_enabled() ? gow3gpu_mouse_take_look(&dx, &dy) : 0;
    if (look_debug()) u1_debug("orbit", params, dx, dy);
    if (!took || (dx == 0.0f && dy == 0.0f) || !params) return NULL;
    float *input = *(float **)(params + 0x10);
    if (!input) return NULL;
    const unsigned mode = *(const unsigned *)((const unsigned char *)input + 0x30) & 0x300;
    const float factor = mode == 0x100 ? 1.5f : mode == 0x300 ? 0.75f : 1.0f;
    const float speed = *(const float *)(image + 0xa4cb10), dt = *(const float *)(image + 0xa67000);
    if (!(speed > 1e-3f && speed < 100.0f) || !(dt > 1e-4f && dt < 0.5f)) return NULL;
    const float degrees = 57.29578f * look_scale();
    input[2] += dx * degrees / (factor * speed * 180.0f * dt);
    input[3] += dy * look_invert() * degrees / (factor * speed * 90.0f * dt);
    return NULL;
}
static ABI void *u1_follow_camera_input(unsigned char *self, unsigned char *camera, unsigned char *params) {
    (void)self;
    (void)camera;
    if (look_debug()) u1_debug("follow", params, 0, 0);
    return NULL;
}

static const MouseLookSite sites[] = {
    {"Uncharted 1 (orbit camera)",
     0xb7430,
     6, /* push rbp; mov rbp, rsp; push r15 */
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0x28, 0x01, 0x00, 0x00, 0x49, 0x89, 0xd7, 0x48},
     24,
     (void *)u1_orbit_camera_input},
    {"Uncharted 1 (follow camera)",
     0xaff80,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0x78, 0x02, 0x00, 0x00, 0x48, 0x89, 0xd3, 0x49},
     24,
     (void *)u1_follow_camera_input},
    {"Uncharted 2",
     0x428f40,
     6, /* push rbp; mov rbp, rsp; push r15 */
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
      0x78, 0xc5, 0xfa, 0x11, 0x85, 0x70, 0xff, 0xff},
     24,
     (void *)u2_camera_input},
    {"Uncharted 3",
     0x4e42f0,
     6, /* push rbp; mov rbp, rsp; push r15 */
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0xc8, 0x00, 0x00, 0x00, 0x45, 0x89, 0xc7, 0x48},
     24,
     (void *)u3_camera_input},
};

const MouseLookSite *runtime_mouselook_sites(unsigned *count) {
    *count = sizeof(sites) / sizeof(sites[0]);
    return sites;
}
