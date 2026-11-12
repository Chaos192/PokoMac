// SPDX-License-Identifier: GPL-3.0-or-later
// The NVN-on-Vulkan back end (S4d). install_backend() replaces the logging stubs of the NVN shell (nvn.h) with real
// implementations and starts the render thread that replays the game's command lists on a gfx::Device.
//   * Memory pools are the game's own memory: the back end imports it as Vulkan memory (VK_EXT_external_memory_host), so a GPU
//     address is a host address and buffers are zero-copy.
//   * Game threads only record. nvnQueueSubmitCommands hands the lists to the render thread and returns when they are recorded
//     into Vulkan (not when the GPU has run them); fences, nvnQueueFinish and the memory a game polls follow the GPU timeline.
//   * nvnQueuePresentTexture draws the window texture into the Renderer's frame (honouring the shell's FrameLimiter when one is
//     given) and presents it.
// Configuration (config.h, environment wins): [nvn] shader_cache (translated SPIR-V folder), bc7_cache (ASTC->BC7 cache folder),
// bc7_quality, log_unimplemented; see docs/CONFIG.md.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "nxrt/gfx.h"

namespace nxrt {
class FrameLimiter;
}

namespace nxrt::nvn {

struct BackendDesc {
    gfx::Device* device = nullptr;            // required; must outlive the back end
    gfx::Renderer* renderer = nullptr;        // optional: without it nvnQueuePresentTexture only waits for the GPU
    FrameLimiter* limiter = nullptr;          // optional: begin_frame() is called before each presented frame
    bool keep_aspect = false;                 // present with letterbox bars instead of stretching to the window
    bool register_entry_points = true;        // false for tests that call the Vulkan layer directly
};

bool install_backend(const BackendDesc& desc);   // idempotent per process: a second call with another device fails
void shutdown_backend();                         // waits for the GPU, destroys every Vulkan object of the back end
bool backend_installed();
// Writes the on-disk caches (the pipeline cache) now. For the paths that end the process without shutting the back end down (window closed).
void backend_flush_caches();
// Shows the game's last frame again (with the overlay on top) when the game has not presented for 50 ms and no present is queued. For while
// the game waits on the player in a library applet (the on-screen keyboard) and presents nothing. Any thread; does not wait.
void present_last_frame_again();

// Traces the next whole presented frame like [nvn] trace_frame (the scripted `@shot nvntrace_<name>` calls this).
void backend_trace_next_frame();

// Shader preparation (S24): compiles the shader parts of every program in a list before the game runs, so the driver already has them when the
// game asks. The list is written by the setup tool from the player's own game files (<shader cache folder>/../programs.bin, "MPRPRG01").
// Needs the back end installed (any mode; `register_entry_points = false` is enough) and an open shader cache.
struct ShaderPrepareOptions {
    std::string programs_file;                                    // empty: programs.bin next to the shader cache folder
    bool only_if_needed = false;                                  // do nothing when the state file says this GPU and driver were prepared already
    bool force = false;                                           // ignore the state file
    int threads = 0;                                              // 0: all hardware threads but two
    std::function<bool(uint64_t done, uint64_t total)> progress;  // called often; returning false cancels
};
struct ShaderPrepareResult {
    uint64_t programs = 0;          // in the list
    uint64_t prepared = 0;          // programs whose parts were all made
    uint64_t missing_shaders = 0;   // skipped: a stage has no translated shader in the cache
    uint64_t failed = 0;            // a part could not be made
    uint64_t parts = 0;
    uint64_t fast_parts = 0;        // parts made in under 2 ms: the driver already had them
    double seconds = 0;
    bool cancelled = false;
    bool already_ready = false;     // only_if_needed and nothing to do
    std::string error;              // why nothing could be done
};
bool prepare_shaders(const ShaderPrepareOptions& o, ShaderPrepareResult* r);

// Pipeline preparation (--prepare-pipelines; the macOS setup and every Play run it): merges the seed key file that ships with the release
// ([nvn] pipeline_seed) into the player's prewarm key file ([nvn] pipeline_keys) and builds every pipeline of that key file once, whole,
// before the game runs: the same records the game's prewarm builds at start, with the same function (nvn_prewarm.cpp build_record). On
// Metal the first build of a pipeline is Apple's compiler working through MSL source (hundreds of ms; a few take minutes), and macOS keeps
// what it compiled in its own shader cache (per program identity: pkp_build.py write_identity), so the game's prewarm at start finds the
// pipelines there instead of compiling them while the game loads. A state file per driver next to the key file (pipelines-<driver>.state)
// lists what was built for which GPU, driver version and Metal cache; only_if_needed builds only what it does not list. Needs the back end
// installed and an open shader cache.
struct PipelinePrepareOptions {
    bool only_if_needed = false;                                  // skip the records the state file lists as built for this GPU, driver and Metal cache
    int threads = 0;                                              // 0: all hardware threads but two
    std::function<bool(uint64_t done, uint64_t total)> progress;  // called often; returning false cancels
};
struct PipelinePrepareResult {
    uint64_t pipelines = 0;         // key file records to build this time (graphics and compute)
    uint64_t built = 0;
    uint64_t missing_shaders = 0;   // a stage has no translated shader yet (one the game makes while it runs): tried again next time
    uint64_t failed = 0;
    uint64_t slow = 0;              // took 3 s or more
    uint64_t added = 0;             // seed records the key file did not have
    double seconds = 0;
    bool cancelled = false;
    bool already_ready = false;     // only_if_needed and nothing to do
    std::string error;
};
bool prepare_pipelines(const PipelinePrepareOptions& o, PipelinePrepareResult* r);

struct BackendStats {
    uint64_t submits = 0, lists_replayed = 0, draws = 0, draws_skipped = 0, dispatches = 0, clears = 0, copies = 0, presents = 0;
    uint64_t passes = 0, pass_breaks = 0, pipelines_created = 0, pipeline_cache_hits = 0, descriptor_sets = 0;
    uint64_t pipelines_in_play = 0;   // pipelines made after the first present, prewarm included (the F1 menu's pins)
    uint64_t textures_created = 0, texture_uploads = 0, texture_upload_bytes = 0, bc7_transcodes = 0, textures_failed = 0;
    uint64_t pools_imported = 0, unresolved_addresses = 0, missing_handles = 0, unsupported_ops = 0;
    uint64_t submit_replay_ns = 0;
};
BackendStats backend_stats();
// Operations that were skipped because they are not implemented, with how often (for the log and docs/S4d.md).
std::string backend_unsupported_report();

}  // namespace nxrt::nvn
