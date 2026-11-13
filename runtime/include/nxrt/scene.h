// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The graphics shell's test scene and the frame loop that drives it (S4b). The scene is a clear colour and two textured
// quads. The texture is authored as a block-linear (Tegra) image, tiled and untiled with the tools/tiling library, and uploaded
// from the untiled copy: it exercises the same path guest textures take. Everything a frame shows is a function of the frame
// number, so a screenshot of frame N is reproducible and can be compared with a reference image.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "nxrt/frame_limiter.h"
#include "nxrt/gfx.h"

namespace nxrt::gfx {

class Overlay;

class TestScene {
public:
    static std::unique_ptr<TestScene> create(Device& dev, VkFormat color_format, std::string* error = nullptr);
    ~TestScene();
    TestScene(const TestScene&) = delete;
    TestScene& operator=(const TestScene&) = delete;

    // Records the scene for frame f.frame_number into f.cmd (the colour image is in COLOR_ATTACHMENT_OPTIMAL).
    void record(const FrameInfo& f);

    // The test texture (256 x 256 RGBA8, 9 levels) as pixels, level 0 and all levels back to back (tight rows); and the number
    // of bytes the tiling round trip moved. For tests.
    static std::vector<uint8_t> make_texture_levels(uint32_t* level_count = nullptr);
    uint64_t tiled_bytes() const;
    bool tiling_round_trip_ok() const;

private:
    TestScene() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---- the frame loop ---------------------------------------------------------------------------------------------------
struct SceneRunDesc {
    uint32_t frames = 300;                 // frames to render, then stop (0: until the window is closed)
    bool headless = true;                  // true: offscreen images, no window; false: a window with a swapchain
    uint32_t width = 640, height = 360;    // offscreen size, or the window size
    PresentMode present_mode = PresentMode::Fifo;
    double target_fps = 0;                 // frame limiter (0 = unlimited)
    int validation = -1;                   // see DeviceDesc
    std::string gpu;                       // GPU override
    int screenshot_frame = -1;             // frame number to capture, -1 none
    std::string screenshot_path;           // PNG written for it
    bool overlay = false;                  // create the ImGui overlay (toggle key F1)
    int overlay_visible_from_frame = -1;   // show the overlay from this frame on (synthetic toggle), -1: leave it hidden
    bool window_hidden = false;            // create the window hidden
    bool boost_priority = false;           // run the loop at high priority (timing tests)
    uint32_t warmup_frames = 0;            // frames before the pacing statistics start (pipeline and driver warm-up)
    std::function<void(uint64_t frame, Renderer&)> per_frame;   // called before begin_frame (tests change modes / sizes here)
};

struct SceneRunResult {
    bool ok = false;
    std::string error;
    uint64_t frames = 0;                   // frames rendered
    std::string gpu_name;
    int gpu_type = 0;
    ValidationStats validation;
    FrameLimiter::Stats pacing;            // frame start intervals (FrameLimiter)
    RendererStats renderer;
    double wall_seconds = 0;
    // Input latency of the loop: from the moment input was sampled (right after the limiter wait and the platform pump) to the
    // return of end_frame. The limiter waits BEFORE input is read, so this is the cost of the frame body only (well under one
    // period); if the order were reversed it would approach a whole period.
    double input_to_submit_mean_ms = 0, input_to_submit_max_ms = 0;
    Device::MemoryStats memory_end;
};

// Creates platform (unless headless), device, renderer, scene (and overlay), runs the loop, tears everything down.
SceneRunResult run_scene(const SceneRunDesc& desc);

}  // namespace nxrt::gfx
