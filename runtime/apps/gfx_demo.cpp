// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// nxrt_gfx_demo: runs the graphics shell's test scene (clear colour + tiled-texture quads), windowed or headless.
//   nxrt_gfx_demo [--headless] [--frames N] [--fps X|vrr] [--present-mode fifo|fifo_relaxed|mailbox|immediate] [--size WxH]
//                 [--screenshot file.png] [--shot-frame N] [--overlay] [--gpu discrete|integrated|N|name] [--validation 0|1]
// Windowed with --overlay: F1 shows the settings menu. Prints a one-line result.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nxrt/log.h"
#include "nxrt/nvn.h"
#include "nxrt/platform.h"
#include "nxrt/scene.h"

int main(int argc, char** argv) {
    nxrt::gfx::SceneRunDesc d;
    d.headless = false;
    d.frames = 0;
    d.width = 1280;
    d.height = 720;
    bool want_vrr_cap = false;   // --fps vrr: a cap just under the display refresh rate (VRR friendly)
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
        if (!std::strcmp(argv[i], "--headless")) d.headless = true;
        else if (arg("--frames")) d.frames = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (arg("--fps")) { ++i; if (!std::strcmp(argv[i], "vrr")) want_vrr_cap = true; else d.target_fps = std::atof(argv[i]); }
        else if (arg("--present-mode")) { if (!nxrt::gfx::parse_present_mode(argv[++i], &d.present_mode)) { std::fprintf(stderr, "bad present mode\n"); return 2; } }
        else if (arg("--size")) { unsigned w = 0, h = 0; if (std::sscanf(argv[++i], "%ux%u", &w, &h) == 2) { d.width = w; d.height = h; } }
        else if (arg("--screenshot")) d.screenshot_path = argv[++i];
        else if (arg("--shot-frame")) d.screenshot_frame = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--overlay")) d.overlay = true;
        else if (arg("--overlay-from")) { d.overlay = true; d.overlay_visible_from_frame = std::atoi(argv[++i]); }
        else if (arg("--gpu")) d.gpu = argv[++i];
        else if (arg("--validation")) d.validation = std::atoi(argv[++i]);
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (d.headless && d.frames == 0) d.frames = 300;
    if (!d.screenshot_path.empty() && d.screenshot_frame < 0) d.screenshot_frame = 60;
    nxrt::LogOptions lo;
    lo.path = "nxrt_gfx_demo.log";
    nxrt::log_init(lo);
    if (want_vrr_cap) {
        // the refresh rate of the primary display, read through the platform layer
        nxrt::platform::init();
        nxrt::platform::WindowDesc wd;
        wd.hidden = true;
        nxrt::platform::create_window(wd);
        const float hz = nxrt::platform::display_refresh_hz();
        nxrt::platform::destroy_window();
        nxrt::platform::shutdown();
        d.target_fps = hz > 0 ? nxrt::suggest_vrr_cap(hz) : 0;
        std::printf("display %.2f Hz -> frame cap %.2f fps\n", hz, d.target_fps);
    }
    nxrt::nvn::install();
    const nxrt::gfx::SceneRunResult r = nxrt::gfx::run_scene(d);
    if (!r.ok) { std::fprintf(stderr, "FAILED: %s\n", r.error.c_str()); return 1; }
    std::printf("gpu=\"%s\" frames=%llu fps=%.2f p99=%.3fms gpu_ms=%.3f validation_errors=%llu warnings=%llu\n", r.gpu_name.c_str(),
                static_cast<unsigned long long>(r.frames), r.pacing.fps, r.pacing.p99_ms, r.renderer.gpu_ms_avg,
                static_cast<unsigned long long>(r.validation.errors), static_cast<unsigned long long>(r.validation.warnings));
    return r.validation.errors ? 3 : 0;
}
