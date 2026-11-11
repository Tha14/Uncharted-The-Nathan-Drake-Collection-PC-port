// gow3.ini graphics keys (gow3_settings.cpp, gow3_graphics.h).
#include "test_environment.h" // SetTestEnv
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "gow3_graphics.h"
#include "gow3_settings.h"

namespace G = Gow3Graphics;

static std::string Read(const char* path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

int main() {
    assert(G::ParseDisplayMode("borderless") == G::Borderless && G::ParseDisplayMode("bogus") == G::Windowed);
    assert(G::ParseFpsLimit(240) == G::MaxFpsLimit);  // over the cap clamps to it
    assert(G::ParseFpsLimit(75) == 75);               // a listed limit is kept
    assert(G::ParseResolution("1440p") == 3 && G::ParseResolution("8K") == 0);
    assert(G::ParseEngineFps(240) == 240 && G::ParseEngineFps(90) == 120);
    assert(G::ParseFramesQueued(2) == 2 && G::ParseFramesQueued(0) == 0 && G::ParseFramesQueued(5) == 1);
    assert(G::ParseRcasStrength(100) == 100 && G::ParseRcasStrength(101) == 75 && G::ParseRcasStrength(-1) == 75);
    assert(G::RcasAttenuation(100) == 0.0f && G::RcasAttenuation(0) == 2.0f && G::RcasAttenuation(75) == 0.5f);
    assert(G::FsrPassRuns(true, false, 1280, 720, 2560, 1440) && !G::FsrPassRuns(false, false, 1280, 720, 2560, 1440));
    assert(G::FsrPassRuns(false, true, 1920, 1080, 1920, 1080) && !G::FsrPassRuns(true, false, 1920, 1080, 1920, 1080));
    assert(!G::FsrPassRuns(true, true, 2560, 1440, 1920, 1080));  // larger image: regular scaling
    assert(G::EffectiveFps(0, 120) == 120 && G::EffectiveFps(240, 120) == 120 && G::EffectiveFps(60, 240) == 60);
    // The present thread has its own cap, unrelated to the simulation rate: values within the
    // cap pass through, while 0 and anything faster than the cap become the cap.
    for (const int kept : {120, 30}) assert(G::PresentLimit(kept) == kept);
    for (const int capped : {240, 0, G::MaxFpsLimit + 1}) assert(G::PresentLimit(capped) == G::MaxFpsLimit);

    const char* path = "graphics-settings-test.ini";
    {
        std::ofstream out(path);
        out << "display_mode=borderless\nvsync=0\nfps_limit=240\nasync_shaders=0\n"
               "render_resolution=1440p\nengine_fps=240\ndeferred_readback=0\nstale_readback=1\n"
               "fsr1=1\nrcas=0\nrcas_strength=40\nframes_queued=2\n";
    }
    SetTestEnv("GOW3_CONFIG", path);          // Load() and Save() use the scratch ini
    SetTestEnv("GOW3_ASYNC_SHADERS", "");     // no override: async_shaders comes from the ini
    auto& v = Gow3Settings::Get();
    Gow3Settings::Load();
    assert(v.display_mode == G::Borderless && !v.vsync);
    assert(v.fps_limit == G::MaxFpsLimit && !v.async_shaders);  // 240 was clamped
    assert(v.render_resolution == 3 && v.engine_fps == 240 && !v.deferred_readback && v.stale_readback);
    assert(v.fsr1 && !v.rcas && v.rcas_strength == 40 && v.frames_queued == 2);
    assert(!Gow3Settings::GraphicsNeedRestart());
    assert(v.applied_engine_fps == 240);  // no GOW3_ENGINE_FPS: the configured rate
    SetTestEnv("GOW3_ENGINE_FPS", "60");  // set by run.py when its frame rate patch failed to apply
    Gow3Settings::Load();
    assert(v.engine_fps == 240 && v.applied_engine_fps == 60 && !Gow3Settings::GraphicsNeedRestart());
    SetTestEnv("GOW3_ENGINE_FPS", "");  // drop the override again
    v.engine_fps = 120;
    assert(!Gow3Settings::GraphicsNeedRestart() && "engine_fps changes need no restart");
    v.deferred_readback = !v.deferred_readback;  // was off, now on
    assert(Gow3Settings::GraphicsNeedRestart());
    v.deferred_readback = !v.deferred_readback;  // and off again
    v.fps_limit = 60;  // live: no restart because of it
    v.engine_fps = 240;
    assert(!Gow3Settings::GraphicsNeedRestart());

    Gow3Settings::Save();
    const std::string saved = Read(path);
    for (const char* line : {"display_mode=borderless\n", "vsync=0\n", "fps_limit=60\n", "async_shaders=0\n",
                             "render_resolution=1440p\n", "engine_fps=240\n", "deferred_readback=0\n",
                             "stale_readback=1\n", "fsr1=1\n", "rcas=0\n", "rcas_strength=40\n",
                             "frames_queued=2\n"}) {
        assert(saved.find(line) != std::string::npos);
    }

    {
        std::ofstream out(path);
        out << "display_mode=huge\nfps_limit=7\nrender_resolution=8K\nengine_fps=1\n"
               "rcas_strength=250\nframes_queued=9\n";
    }
    SetTestEnv("GOW3_ASYNC_SHADERS", "1");  // at load time the env var overrides the ini
    v.async_shaders = false;
    Gow3Settings::Load();
    assert(v.display_mode == G::Windowed && v.render_resolution == 0);  // unknown names fall back
    assert(v.fps_limit == 60 && v.engine_fps == 120);                     // unlisted rates fall back too
    assert(v.rcas_strength == 75 && v.frames_queued == 1);
    assert(v.async_shaders);
    {
        std::ofstream out(path);
        out << "cheat_double_jumps=1\ncheat_very_hard=1\n";
    }
    Gow3Settings::Load();
    Gow3Settings::Save();
    const std::string cheats = Read(path);
    assert(cheats.find("cheat_double_jumps=1\n") != std::string::npos);
    assert(cheats.find("cheat_very_hard=1\n") != std::string::npos);
    std::remove(path);
    std::puts("PASS: graphics settings");
}
