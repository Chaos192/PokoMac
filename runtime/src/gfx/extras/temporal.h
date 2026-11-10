// SPDX-License-Identifier: GPL-3.0-or-later
// Temporal upscaling / anti-aliasing on the HDR scene before the game's tone map: AMD FSR 3.1 (FidelityFX API, temporal_ffx.cpp) and NVIDIA DLSS /
// DLAA (Streamline, temporal_dlss.cpp). Both SDKs are loaded at run time; nothing is linked. On macOS, Apple's MetalFX temporal scaler
// (platform/mac/gfx/temporal_metalfx.mm), which runs on its own Metal command queue: the Vulkan recording is split around it (RecordingSplitter).
//
// Inputs (docs/S14.md, "Temporal upscaling"):
//   colour   the HDR scene the tone-map draw samples (the same interception as the spatial scene upscaler, nvn_replay.cpp)
//   depth    the depth target of the G-buffer pass (D24S8 in Pokopia), reversed-Z, NDC z = depth (NVN depth mode NEAR_IS_ZERO)
//   camera   the G-buffer vertex shaders' uniform block c3 (NVN uniform slot 0 of the vertex stage): floats 0..15 the view-projection
//            (column-major, jittered), floats 52..67 the view matrix. The projection is VP * inverse(V); its z-column x / y terms are the
//            sub-pixel jitter (zero without the game's TAA, about +-1 / width with [pkp] game_taa).
//   motion   made here by a compute pass from depth, the current jittered and the previous unjittered view-projection (camera motion only).
// The projection jitter is added by the runtime (temporal_jitter_block): the game's own TAA jitter does not reach the scene camera.
#pragma once
#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace nxrt::gfx { struct Image; }

namespace nxrt::extras {

// One frame's inputs for a backend: every image is owned by the temporal pass, in VK_IMAGE_LAYOUT_GENERAL, readable and writable by compute.
struct TemporalImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageUsageFlags usage = 0;
};
struct TemporalFrame {
    TemporalImage color, depth, motion, output;
    VkExtent2D render = {}, out = {};
    float jitter_px[2] = {};         // the projection's sub-pixel offset in pixels (FSR convention: +x right, +y down in the picture)
    float mv_scale[2] = {};          // motion vectors are in UV units: times the render size gives pixels
    bool reset = false;              // no usable history (first frame, camera cut, output size or backend capacity change)
    bool depth_inverted = false;     // reversed-Z when true, forward depth otherwise
    float frame_ms = 16.7f;
    float near_z = 0.1f, far_z = 1000.0f, fov_y = 1.0f, aspect = 1.0f;
    float sharpness = 0.0f;          // 0..1, FSR's RCAS
    // camera (column-vector convention, stored column-major: the memory layout of a row-major matrix in row-vector convention, as Streamline wants)
    float view_to_clip[16] = {}, clip_to_view[16] = {}, clip_to_prev_clip[16] = {}, prev_clip_to_clip[16] = {};
    float cam_pos[3] = {}, cam_up[3] = {}, cam_right[3] = {}, cam_fwd[3] = {};
};

class TemporalBackend {
public:
    virtual ~TemporalBackend() = default;
    virtual const char* name() const = 0;
    // Records the upscale into cb (outside any render pass). False: nothing recorded (the caller falls back to the game's own picture).
    // A backend whose work runs outside the Vulkan queue (MetalFX) splits the recording (RecordingSplitter) and sets cb to the command buffer
    // the rest of the submission is recorded into.
    virtual bool evaluate(VkCommandBuffer& cb, const TemporalFrame& f) = 0;
    // The temporal pass's images (TemporalFrame: colour, depth, motion, output) are made by the backend when makes_images() says it needs
    // them made its own way (MetalFX: memory Metal can see); otherwise the pass makes them with gfx::Device::create_image. *out gets image,
    // view, format and extent (allocation stays null). free_image frees one it made, once the GPU and wait_idle are done with it.
    virtual bool makes_images() const { return false; }
    virtual bool make_image(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, const char* name, gfx::Image* out) {
        (void)format, (void)extent, (void)usage, (void)name, (void)out;
        return false;
    }
    virtual void free_image(gfx::Image& img) { (void)img; }
    // Returns once the backend's own GPU work (outside the Vulkan queue) is done with every image it was given. Render thread.
    virtual void wait_idle() {}
    // The backend cannot run any more (a lasting failure, logged): the caller hands over to another upscaler.
    virtual bool broken() const { return false; }
};

// ---- splitting the recording (nvn_gpu.cpp, split_prepare / split_recording) -----------------------------------------------------------
// A backend that runs on another queue (MetalFX) needs what the submission recorded so far on the GPU before it starts, and the rest of the
// submission must wait for it. The NVN back end registers these at start-up (all null without it). Render thread, inside a recording.
struct RecordingSplitter {
    // True when the recording that cb belongs to can be split now (it is the command buffer being recorded, no query is open in it, and the
    // back end can carry its state over); the command buffer the rest goes into is reserved. False: no split, cb is untouched.
    bool (*prepare)(VkCommandBuffer cb) = nullptr;
    // After a successful prepare: ends cb and submits it (after every earlier submission) so that it signals `signal` = signal_value when
    // done, then begins the reserved command buffer, which waits for `wait` >= wait_value before it runs, and returns it. The submission's
    // serial (the game's sync) is signalled by its last part, so everything the game waits for includes the work between the parts.
    VkCommandBuffer (*split)(VkCommandBuffer cb, VkSemaphore signal, uint64_t signal_value, VkSemaphore wait, uint64_t wait_value) = nullptr;
    // Hands the submissions recorded before the current one to the GPU ([nvn] submit_batch may hold them back), so a device wait covers them.
    void (*flush)() = nullptr;
};
void temporal_set_splitter(const RecordingSplitter& s);
const RecordingSplitter& temporal_splitter();

// ---- what the replay calls (nvn_replay.cpp) --------------------------------------------------------------------------------------
bool temporal_wanted();   // a temporal mode is selected
// A G-buffer draw: `words` is the vertex stage's uniform slot 0 (bytes / 4 words). Keeps the first block that holds a valid camera this frame.
void temporal_note_gbuffer(const uint32_t* words, size_t n_words);
bool temporal_camera_ready();   // a camera was kept since the last evaluation
// The scene camera's jitter. Pokopia's own TAA jitter (0x7102574a18, written into a view object's projection at +0xc0) never reaches the
// G-buffer camera block (its view-projection stays unjittered with the enable forced, run b7), so the runtime jitters every vertex uniform
// slot 0 block that holds a valid camera: adds a Halton(2, 3) offset of the frame (8 phases at native size, more when upscaling, +-0.5 px of
// width x height) to the view-projection in `words` (a copy the draw then binds). False: not a camera block, already jittered, or
// [upscale] jitter = 0.
uint32_t temporal_jitter_phase_count();
bool temporal_jitter_block(uint32_t* words, size_t n_words, uint32_t width, uint32_t height, uint64_t frame);
// The tone-map draw: colour (view in SHADER_READ_ONLY_OPTIMAL) and depth (depth-aspect view, layout depth_layout) of the scene, the part of
// them that is upscaled being `render` from texel 0, 0; depth_neg_one: NVN depth mode NEAR_IS_MINUS_W (NDC z = 2 d - 1). Returns the result
// (`out` large, in the top-left of an image of out_tex when that is larger; GENERAL layout, readable by fragment shaders after the recorded
// barrier) or null (nothing recorded: the draw keeps its own input).
VkImageView temporal_evaluate(void* gfx_device, VkCommandBuffer cb, VkImageView color, VkImageView depth, VkImageLayout depth_layout, VkExtent2D render,
                              VkExtent2D out, bool depth_neg_one, VkExtent2D out_tex = {});

// ---- backends --------------------------------------------------------------------------------------------------------------------
// gfx_device: the gfx::Device. Null when the SDK cannot be loaded or the GPU does not support it (logged once).
TemporalBackend* temporal_ffx(void* gfx_device);
TemporalBackend* temporal_dlss(void* gfx_device, bool dlaa);
// True when the backend's DLLs can be found (for the F1 page; does not load anything).
bool temporal_ffx_available();
bool temporal_dlss_available();
#if defined(__APPLE__)
// MetalFX (platform/mac/gfx/temporal_metalfx.mm). Null when the GPU or the Vulkan driver cannot run it (logged once, temporal_metalfx_why).
TemporalBackend* temporal_metalfx(void* gfx_device);
bool temporal_metalfx_available();     // the GPU supports MetalFX's temporal scaler (for the F1 page; the driver check runs at the first frame)
const char* temporal_metalfx_why();    // why MetalFX does not run, or null while nothing failed
void temporal_shutdown();              // the temporal pass's and MetalFX's objects (NVN back end tear-down: render thread, device idle)
void temporal_metalfx_shutdown();      // MetalFX's part of it
std::string temporal_metalfx_report(); // the state of MetalFX's hand-off for the freeze report (empty when it never ran)
#endif
bool temporal_dlss_started();   // Streamline was initialised before the Vulkan instance (needed for DLSS; set by streamline_preinit)

// Streamline must own vkCreateInstance / vkCreateDevice / the swapchain calls: when the start-up mode is DLSS or DLAA, this loads
// sl.interposer.dll, runs slInit and returns its vkGetInstanceProcAddr for volkInitializeCustom (vk_device.cpp). Null: use the normal loader.
PFN_vkGetInstanceProcAddr streamline_preinit();

}  // namespace nxrt::extras
