// SPDX-License-Identifier: GPL-3.0-or-later
// macOS: Apple's MetalFX temporal scaler as a temporal upscaler (src/gfx/extras/temporal.h). The temporal pass (temporal.cpp) records its
// input pass (colour and depth copies, motion vectors from depth and the camera) into the replay's command buffer as for FSR 3; MetalFX then
// runs on a Metal command queue of its own, between two parts of that submission:
//
//   images   the pass's four images are made here (make_image) with dedicated memory exported to Metal (VK_EXT_external_memory_metal). An
//            MTLTEXTURE handle is the driver's own texture (MoltenVK). KosmicKrisp exports only MTLHEAP: the heap is the image's memory, and a
//            texture made in it at offset 0 with the image's format and size reads and writes the same texels (checked byte for byte on both
//            drivers, every format here, odd sizes too). MetalFX's output must be a private texture of exactly the output size, so unless the
//            image's own texture is one, MetalFX writes a texture of its own and a blit copies it into the image's top-left corner.
//   split    the recording is split there (RecordingSplitter, nvn_gpu.cpp split_recording): the first part goes to the queue at once and
//            signals our timeline semaphore = 2k - 1 (frame k); the rest waits for 2k before it runs. Image ownership moves to
//            VK_QUEUE_FAMILY_EXTERNAL at the end of the first part and back at the start of the rest; the images stay in GENERAL.
//   sync     MoltenVK (VK_EXT_metal_objects) gives the semaphore's MTLSharedEvent: the Metal command buffer waits for 2k - 1 and signals 2k on
//            the GPU. KosmicKrisp has no such export: the Metal command buffer is committed waiting for an event of ours, a helper thread
//            waits for 2k - 1 on the CPU and sets that event, and the command buffer's completion handler signals 2k (vkSignalSemaphore).
//            Neither path blocks the render thread. The semaphore only ever moves forward: the first part of frame k + 1 is submitted after
//            the rest of frame k, which waits for 2k.
//   lifetime the submission's serial is signalled by its last part, which waits for MetalFX, so everything the replay keeps alive until the
//            serial completes outlives MetalFX's use of it; images are freed only after wait_idle (this queue) and a device wait.
//
// Anything that keeps MetalFX from starting (no MetalFX on the GPU, no Metal interop in the driver, a scaler that cannot be made) is logged
// once and makes the backend broken(): temporal.cpp then hands over to FSR 1.
#include "extras/temporal.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "nxrt/config.h"
#include "nxrt/extras.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#if !__has_feature(objc_arc)
#error "temporal_metalfx.mm is compiled with ARC (cmake/gfx.cmake)"
#endif

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "metalfx";

MTLPixelFormat mtl_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
        case VK_FORMAT_R32_SFLOAT: return MTLPixelFormatR32Float;
        case VK_FORMAT_R16G16_SFLOAT: return MTLPixelFormatRG16Float;
        default: return MTLPixelFormatInvalid;
    }
}

// Why MetalFX does not run (temporal_metalfx_why, the F1 page). Written once by the render thread, read by the UI thread.
std::mutex g_why_mu;
std::string g_why;
void set_why(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void set_why(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_why_mu);
    if (!g_why.empty()) return;
    g_why = buf;
    NXLOG_W(kTag, "MetalFX off: %s", buf);
}

// What the hand-off is doing, for the freeze report (temporal_metalfx_report) and the stall lines below. Written by the render thread (issued,
// committed), the helper thread (waiting, released) and Metal's completion thread (completed, status); read by the watchdog.
struct Diag {
    std::atomic<uint64_t> issued{0};        // frame k last handed to MetalFX (its first part signals 2k - 1, its rest waits for 2k)
    std::atomic<uint64_t> committed_ns{0};  // when that frame's Metal command buffer was committed
    std::atomic<uint64_t> waiting{0};       // helper thread: the semaphore value it waits for now (0: idle)
    std::atomic<uint64_t> waiting_ns{0};    // since when
    std::atomic<uint64_t> released{0};      // helper thread: frames whose Metal command buffer it let start
    std::atomic<uint64_t> completed{0};     // Metal: the last frame whose command buffer completed
    std::atomic<int64_t> status{-1};        // its MTLCommandBufferStatus (4 completed, 5 error)
};
Diag g_diag;
uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The host round trip (no MTLSharedEvent of the semaphore: KosmicKrisp). Each job waits on the CPU until the first part of a frame has run
// (the semaphore reaches `wait`) and then sets our event, which the already committed Metal command buffer waits for. One thread, jobs in
// frame order; it waits in slices so a lost device cannot keep it forever.
class Bridge {
public:
    void start(gfx::Device* dev, VkSemaphore sem) {
        dev_ = dev;
        sem_ = sem;
        th_ = std::thread([this] { run(); });
    }
    void push(uint64_t wait, id<MTLSharedEvent> ev, uint64_t ev_value) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push_back({wait, ev, ev_value});
        }
        cv_.notify_one();
    }
    void stop() {
        if (!th_.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_one();
        th_.join();
    }

private:
    struct Job {
        uint64_t wait;
        id<MTLSharedEvent> ev;
        uint64_t ev_value;
    };
    void run() {
        log_set_thread_name("metalfx-sync");
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // the GPU idles between the parts while this thread sleeps
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (q_.empty()) return;   // stop with nothing left
                j = q_.front();
                q_.pop_front();
            }
            VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &sem_;
            wi.pValues = &j.wait;
            const uint64_t t0 = now_ns();
            g_diag.waiting_ns.store(t0);
            g_diag.waiting.store(j.wait);
            bool told = false;
            for (;;) {
                const VkResult r = vkWaitSemaphores(dev_->device(), &wi, 100'000'000ull);
                if (r == VK_TIMEOUT && !dev_->device_lost()) {
                    // a frame's first part takes a few ms: a quarter of a second means the Vulkan queue is not reaching it
                    if (!told && now_ns() - t0 > 250'000'000ull) {
                        told = true;
                        uint64_t v = 0;
                        const VkResult rv = vkGetSemaphoreCounterValue(dev_->device(), sem_, &v);
                        NXLOG_W(kTag, "frame %llu: the Vulkan work before MetalFX has not finished after 250 ms (semaphore %llu%s, waiting for %llu); "
                                      "MetalFX frames released %llu, completed %llu (status %lld), issued %llu",
                                static_cast<unsigned long long>(j.ev_value), static_cast<unsigned long long>(v), rv == VK_SUCCESS ? "" : " (unreadable)",
                                static_cast<unsigned long long>(j.wait), static_cast<unsigned long long>(g_diag.released.load()),
                                static_cast<unsigned long long>(g_diag.completed.load()), static_cast<long long>(g_diag.status.load()),
                                static_cast<unsigned long long>(g_diag.issued.load()));
                    }
                    continue;
                }
                if (r != VK_SUCCESS && !dev_->device_lost()) NXLOG_E(kTag, "vkWaitSemaphores failed (%d): MetalFX goes on without the wait", static_cast<int>(r));
                if (r != VK_SUCCESS)
                    NXLOG_E(kTag, "frame %llu: the wait for its first part (semaphore %llu) ended with %d after %.0f ms", static_cast<unsigned long long>(j.ev_value),
                            static_cast<unsigned long long>(j.wait), static_cast<int>(r), static_cast<double>(now_ns() - t0) / 1e6);
                break;
            }
            g_diag.waiting.store(0);
            g_diag.released.store(j.ev_value);
            j.ev.signaledValue = j.ev_value;   // (also after a failure: the Metal command buffer and the rest of the frame must not hang)
        }
    }
    gfx::Device* dev_ = nullptr;
    VkSemaphore sem_ = VK_NULL_HANDLE;
    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    bool stop_ = false;
};

class MetalFx final : public TemporalBackend {
public:
    const char* name() const override { return "MetalFX"; }
    bool makes_images() const override { return true; }
    bool broken() const override { return broken_.load(); }

    bool init(gfx::Device& dev) {
        dev_ = &dev;
        mdev_ = MTLCreateSystemDefaultDevice();
        if (!mdev_) return fail("no Metal device");
        if (![MTLFXTemporalScalerDescriptor supportsDevice:mdev_]) return fail("%s has no MetalFX temporal scaler", mdev_.name.UTF8String);
        min_scale_ = [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:mdev_];
        max_scale_ = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:mdev_];
        if (!dev.features().external_memory_metal || !vkGetMemoryMetalHandleEXT)
            return fail("the Vulkan driver offers no VK_EXT_external_memory_metal (images cannot be shared with Metal)");
        const RecordingSplitter& sp = temporal_splitter();
        if (!sp.prepare || !sp.split) return fail("the renderer cannot split its recording");
        {   // the driver, for the log
            VkPhysicalDeviceDriverProperties dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp};
            vkGetPhysicalDeviceProperties2(dev.physical(), &pp);
            driver_ = dp.driverName[0] ? dp.driverName : pp.properties.deviceName;
        }
        interop_ = config_get_str("upscale", "metalfx_interop", "auto");
        const std::string sync = config_get_str("upscale", "metalfx_sync", "auto");
        const bool can_events = dev.features().metal_objects && vkExportMetalObjectsEXT && sync != "host";
        VkExportMetalObjectCreateInfoEXT xo = {VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT};
        xo.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT;
        VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, can_events ? &xo : nullptr};
        tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
        if (vkCreateSemaphore(dev.device(), &sci, nullptr, &sem_) != VK_SUCCESS) return fail("no timeline semaphore");
        dev.set_name(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(sem_), "MetalFX timeline");
        if (can_events) {
            VkExportMetalSharedEventInfoEXT ei = {VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT};
            ei.semaphore = sem_;
            VkExportMetalObjectsInfoEXT oi = {VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &ei};
            vkExportMetalObjectsEXT(dev.device(), &oi);
            vk_event_ = ei.mtlSharedEvent;
            if (vk_event_ && vk_event_.device && vk_event_.device != mdev_) vk_event_ = nil;   // (MoltenVK's events report no device)
        }
        if (!vk_event_) {
            if (sync != "host")
                NXLOG_I(kTag, "no MTLSharedEvent of the semaphore (VK_EXT_metal_objects %s, export %s): the host round trip is used",
                        dev.features().metal_objects ? "on" : "off", can_events ? "gave none or another device's" : "not tried");
            own_event_ = [mdev_ newSharedEvent];
            if (!own_event_) return fail("no MTLSharedEvent");
            bridge_.start(dev_, sem_);
        }
        queue_ = [mdev_ newCommandQueue];
        if (!queue_) return fail("no Metal command queue");
        queue_.label = @"MetalFX";
        NXLOG_I(kTag, "MetalFX ready on %s (%s driver): scale %.2f to %.2f per side, sync %s", mdev_.name.UTF8String, driver_.c_str(),
                static_cast<double>(min_scale_), static_cast<double>(max_scale_), vk_event_ ? "on the GPU (the semaphore's MTLSharedEvent)" : "by a host round trip");
        return true;
    }

    bool make_image(VkFormat format, VkExtent2D e, VkImageUsageFlags usage, const char* name, gfx::Image* out) override {
        const MTLPixelFormat pf = mtl_format(format);
        if (pf == MTLPixelFormatInvalid) return fail_image(name, "format %d has no Metal twin here", static_cast<int>(format));
        // a colour attachment's texture can be MetalFX's own output (the output texture must be a render target)
        if (format == VK_FORMAT_R16G16B16A16_SFLOAT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT;
        if (interop_ == "heap" || !exportable(format, usage, ht)) ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT;
        if (interop_ == "texture" && ht != VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT) return fail_image(name, "[upscale] metalfx_interop = texture, and the driver exports no MTLTexture");
        if (!exportable(format, usage, ht)) return fail_image(name, "the driver exports this image's memory neither as an MTLTexture nor as an MTLHeap");
        VkDevice d = dev_->device();
        VkExternalMemoryImageCreateInfo xi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        xi.handleTypes = ht;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &xi};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {e.width, e.height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Made m;
        VkImage img = VK_NULL_HANDLE;
        if (vkCreateImage(d, &ci, nullptr, &img) != VK_SUCCESS) return fail_image(name, "vkCreateImage failed");
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(d, img, &mr);
        const uint32_t type = memory_type(mr.memoryTypeBits);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = img;
        VkExportMemoryAllocateInfo xa = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded};
        xa.handleTypes = ht;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &xa};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type;
        VkImageView view = VK_NULL_HANDLE;
        auto undo = [&] {
            m.tex = nil;
            m.heap = nil;
            if (view) vkDestroyImageView(d, view, nullptr);
            vkDestroyImage(d, img, nullptr);
            if (m.mem) vkFreeMemory(d, m.mem, nullptr);
        };
        if (type == UINT32_MAX || vkAllocateMemory(d, &ai, nullptr, &m.mem) != VK_SUCCESS) {
            undo();
            return fail_image(name, "no exportable memory (%llu bytes)", static_cast<unsigned long long>(mr.size));
        }
        if (vkBindImageMemory(d, img, m.mem, 0) != VK_SUCCESS) {
            undo();
            return fail_image(name, "vkBindImageMemory failed");
        }
        VkMemoryGetMetalHandleInfoEXT gi = {VK_STRUCTURE_TYPE_MEMORY_GET_METAL_HANDLE_INFO_EXT};
        gi.memory = m.mem;
        gi.handleType = ht;
        void* h = nullptr;
        if (vkGetMemoryMetalHandleEXT(d, &gi, &h) != VK_SUCCESS || !h) {
            undo();
            return fail_image(name, "vkGetMemoryMetalHandleEXT gave nothing");
        }
        if (ht == VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT) {
            m.tex = (__bridge id<MTLTexture>)h;
        } else {
            m.heap = (__bridge id<MTLHeap>)h;
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:e.width height:e.height mipmapped:NO];
            td.storageMode = m.heap.storageMode;
            td.cpuCacheMode = m.heap.cpuCacheMode;
            td.hazardTrackingMode = m.heap.hazardTrackingMode;
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? MTLTextureUsageRenderTarget : 0);
            if ([m.heap.device heapTextureSizeAndAlignWithDescriptor:td].size > mr.size) {
                undo();
                return fail_image(name, "a texture of its size does not fit the memory the driver gave it");
            }
            m.tex = [m.heap newTextureWithDescriptor:td offset:0];
        }
        if (!m.tex || m.tex.pixelFormat != pf || m.tex.width != e.width || m.tex.height != e.height || m.tex.device != mdev_) {
            undo();
            return fail_image(name, "the Metal texture does not match the image");
        }
        if (m.heap) m.tex.label = [NSString stringWithUTF8String:name ? name : "temporal"];
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(d, &vi, nullptr, &view) != VK_SUCCESS) {
            view = VK_NULL_HANDLE;
            undo();
            return fail_image(name, "vkCreateImageView failed");
        }
        if (name) {
            dev_->set_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(img), name);
            dev_->set_name(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(view), name);
        }
        m.path = ht == VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT ? "MTLTexture export" : "MTLHeap alias";
        *out = gfx::Image{};
        out->image = img;
        out->view = view;
        out->format = format;
        out->extent = {e.width, e.height, 1};
        made_[img] = m;
        return true;
    }

    void free_image(gfx::Image& img) override {
        auto it = made_.find(img.image);
        if (it == made_.end()) return;
        VkDevice d = dev_->device();
        it->second.tex = nil;     // ours first: the heap's memory goes with the VkDeviceMemory
        it->second.heap = nil;
        if (img.view) vkDestroyImageView(d, img.view, nullptr);
        vkDestroyImage(d, img.image, nullptr);
        vkFreeMemory(d, it->second.mem, nullptr);
        made_.erase(it);
        img = gfx::Image{};
    }

    void wait_idle() override {
        if (temporal_splitter().flush) temporal_splitter().flush();   // the parts waiting for MetalFX reach the queue: a device wait covers them
        if (last_) [last_ waitUntilCompleted];
        last_ = nil;
    }

    bool evaluate(VkCommandBuffer& cb, const TemporalFrame& f) override {
        if (broken_.load()) return false;
        const Made* in[4] = {find(f.color), find(f.depth), find(f.motion), find(f.output)};
        if (!in[0] || !in[1] || !in[2] || !in[3]) return once(told_foreign_, "the temporal images were not made for MetalFX: skipped");
        if (f.render.width > f.out.width || f.render.height > f.out.height) return once(told_bigger_, "the scene is larger than the output: MetalFX skipped");
        const double scale = std::max(static_cast<double>(f.out.width) / f.render.width, static_cast<double>(f.out.height) / f.render.height);
        if (scale > max_scale_ + 1e-3) {
            if (!told_scale_) {
                told_scale_ = true;
                NXLOG_W(kTag, "%ux%u -> %ux%u is %.2fx per side, beyond MetalFX's %.2fx: the scene is stretched by the game instead (raise the scene resolution)",
                        f.render.width, f.render.height, f.out.width, f.out.height, scale, static_cast<double>(max_scale_));
            }
            return false;
        }
        if (!ensure_scaler(f)) return false;
        // MetalFX writes the output image's own texture when it is a private render target of exactly the output size (MoltenVK, no
        // scene part), else a texture of ours that is copied into the image
        id<MTLTexture> dst = in[3]->tex;
        const MTLTextureUsage ou = scaler_.outputTextureUsage;
        const bool direct = dst.storageMode == MTLStorageModePrivate && (dst.usage & ou) == ou && dst.width == f.out.width && dst.height == f.out.height;
        if (!direct && !ensure_own_output(f.out)) return false;
        const RecordingSplitter& sp = temporal_splitter();
        if (!sp.prepare(cb)) return false;   // (it says why, once)
        const VkImage imgs[4] = {f.color.image, f.depth.image, f.motion.image, f.output.image};
        ownership(cb, imgs, true);
        const uint64_t k = ++frame_, ready = 2 * k - 1, done = 2 * k;
        const VkCommandBuffer next = sp.split(cb, sem_, ready, sem_, done);
        if (!next) {   // (cannot happen after prepare): the images come back in the same command buffer
            --frame_;
            ownership(cb, imgs, false);
            return false;
        }
        encode(f, in, direct ? dst : own_out_, direct ? nil : dst, ready, done, k);
        ownership(next, imgs, false);
        cb = next;
        if (!logged_start_) {
            logged_start_ = true;
            const bool aa = f.render.width == f.out.width && f.render.height == f.out.height;
            NXLOG_I(kTag, "MetalFX %s: %ux%u -> %ux%u, colour RGBA16Float (HDR, auto exposure), depth R32Float %s, motion RG16Float; %s driver, images as %s, "
                          "output %s, sync %s",
                    aa ? "anti-aliasing at native size" : "temporal upscaling", f.render.width, f.render.height, f.out.width, f.out.height,
                    f.depth_inverted ? "reversed" : "forward", driver_.c_str(), in[0]->path, direct ? "written in place" : "copied into the image",
                    vk_event_ ? "on the GPU (MTLSharedEvent)" : "by a host round trip");
        }
        return true;
    }

    void shutdown() {
        wait_idle();
        bridge_.stop();
        scaler_ = nil;
        own_out_ = nil;
        queue_ = nil;
        vk_event_ = nil;
        own_event_ = nil;
        if (sem_) vkDestroySemaphore(dev_->device(), sem_, nullptr);
        sem_ = VK_NULL_HANDLE;
        broken_.store(true);
    }

private:
    struct Made {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        id<MTLTexture> tex = nil;     // the image's texels as Metal sees them
        id<MTLHeap> heap = nil;       // MTLHEAP export: the image's memory (kept while tex is)
        const char* path = "";
    };

    bool fail(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        set_why("%s", buf);
        broken_.store(true);
        return false;
    }
    bool fail_image(const char* name, const char* fmt, ...) __attribute__((format(printf, 3, 4))) {
        char buf[400];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        return fail("%s: %s", name ? name : "temporal image", buf);
    }
    bool once(bool& told, const char* what) {
        if (!told) {
            told = true;
            NXLOG_W(kTag, "%s", what);
        }
        return false;
    }

    // The driver can make an image of this format and usage whose memory exports as `ht`.
    bool exportable(VkFormat format, VkImageUsageFlags usage, VkExternalMemoryHandleTypeFlagBits ht) const {
        VkPhysicalDeviceExternalImageFormatInfo xi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        xi.handleType = ht;
        VkPhysicalDeviceImageFormatInfo2 fi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &xi};
        fi.format = format;
        fi.type = VK_IMAGE_TYPE_2D;
        fi.tiling = VK_IMAGE_TILING_OPTIMAL;
        fi.usage = usage;
        VkExternalImageFormatProperties xp = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 fp = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &xp};
        return vkGetPhysicalDeviceImageFormatProperties2(dev_->physical(), &fi, &fp) == VK_SUCCESS &&
               (xp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
    }

    // Device-local memory, the kind no CPU maps first (MoltenVK: private storage), else any device-local type the image allows.
    uint32_t memory_type(uint32_t bits) const {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(dev_->physical(), &mp);
        uint32_t any = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags fl = mp.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || !(fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || (fl & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) continue;
            if (!(fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return i;
            if (any == UINT32_MAX) any = i;
        }
        return any;
    }

    const Made* find(const TemporalImage& t) const {
        auto it = made_.find(t.image);
        return it == made_.end() ? nullptr : &it->second;
    }

    // A scaler for this input and output size (made again when either changes; MetalFX keeps its history inside it).
    bool ensure_scaler(const TemporalFrame& f) {
        if (scaler_ && scaler_in_.width == f.render.width && scaler_in_.height == f.render.height && scaler_out_.width == f.out.width &&
            scaler_out_.height == f.out.height)
            return true;
        if (last_) [last_ waitUntilCompleted];   // the old scaler may still be in use on our queue
        scaler_ = nil;
        MTLFXTemporalScalerDescriptor* d = [MTLFXTemporalScalerDescriptor new];
        d.colorTextureFormat = MTLPixelFormatRGBA16Float;
        d.depthTextureFormat = MTLPixelFormatR32Float;
        d.motionTextureFormat = MTLPixelFormatRG16Float;
        d.outputTextureFormat = MTLPixelFormatRGBA16Float;
        d.inputWidth = f.render.width;
        d.inputHeight = f.render.height;
        d.outputWidth = f.out.width;
        d.outputHeight = f.out.height;
        d.autoExposureEnabled = YES;   // the input is the linear HDR scene before the game's tone map
        scaler_ = [d newTemporalScalerWithDevice:mdev_];
        if (!scaler_)
            return fail("no MetalFX temporal scaler for %ux%u -> %ux%u (RGBA16Float colour, R32Float depth, RG16Float motion)", f.render.width,
                        f.render.height, f.out.width, f.out.height);
        const MTLTextureUsage need = scaler_.colorTextureUsage | scaler_.depthTextureUsage | scaler_.motionTextureUsage;
        for (const TemporalImage* t : {&f.color, &f.depth, &f.motion}) {
            const Made* m = find(*t);
            if (m && (m->tex.usage & need) != need)
                return fail("an input texture lacks the usage MetalFX needs (0x%lx of 0x%lx)", static_cast<unsigned long>(m->tex.usage), static_cast<unsigned long>(need));
        }
        scaler_in_ = f.render;
        scaler_out_ = f.out;
        fresh_ = true;
        own_out_ = nil;
        return true;
    }

    bool ensure_own_output(VkExtent2D out) {
        if (own_out_ && own_out_.width == out.width && own_out_.height == out.height) return true;
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:out.width height:out.height mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = scaler_.outputTextureUsage | MTLTextureUsageShaderRead;
        own_out_ = [mdev_ newTextureWithDescriptor:td];
        if (!own_out_) return fail("no %ux%u output texture", out.width, out.height);
        own_out_.label = @"MetalFX output";
        return true;
    }

    // The images go to Metal at the end of the first part (release) and come back at the start of the rest (acquire). GENERAL throughout.
    void ownership(VkCommandBuffer cb, const VkImage (&imgs)[4], bool release) const {
        VkImageMemoryBarrier2 b[4];
        for (int i = 0; i < 4; ++i) {
            b[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            if (release) {
                b[i].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                b[i].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                b[i].srcQueueFamilyIndex = dev_->queue_family();
                b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            } else {
                b[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                b[i].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                b[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
                b[i].dstQueueFamilyIndex = dev_->queue_family();
            }
            b[i].oldLayout = b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].image = imgs[i];
            b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 4;
        di.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(cb, &di);
    }

    // Our Metal command buffer of frame k: waits for the first part (`ready`), upscales, copies into the image when MetalFX wrote a
    // texture of ours, and lets the rest of the submission go (`done`). Committed now; it starts when the first part has run.
    void encode(const TemporalFrame& f, const Made* const (&in)[4], id<MTLTexture> out, id<MTLTexture> copy_to, uint64_t ready, uint64_t done, uint64_t k) {
        @autoreleasepool {
            id<MTLCommandBuffer> mcb = [queue_ commandBuffer];
            if (!mcb) {   // nothing on the GPU: the rest of the frame is let go from the CPU once the first part has run
                fail("no Metal command buffer");
                release_from_cpu(ready, done);
                return;
            }
            mcb.label = @"MetalFX";
            if (vk_event_) [mcb encodeWaitForEvent:vk_event_ value:ready];
            else [mcb encodeWaitForEvent:own_event_ value:k];
            scaler_.colorTexture = in[0]->tex;
            scaler_.depthTexture = in[1]->tex;
            scaler_.motionTexture = in[2]->tex;
            scaler_.outputTexture = out;
            scaler_.inputContentWidth = f.render.width;
            scaler_.inputContentHeight = f.render.height;
            // f.jitter_px: the projection's offset in pixels, +y down the picture (FSR's convention). MetalFX wants the offset of the
            // sample from the pixel centre, the other way round ([upscale] jitter_sign flips the axes again for checking).
            scaler_.jitterOffsetX = -f.jitter_px[0];
            scaler_.jitterOffsetY = -f.jitter_px[1];
            // the motion vectors are previous minus current position in UV units, y down: times the render size, MetalFX's pixels
            scaler_.motionVectorScaleX = f.mv_scale[0];
            scaler_.motionVectorScaleY = f.mv_scale[1];
            scaler_.depthReversed = f.depth_inverted;
            scaler_.preExposure = 1.0f;
            scaler_.reset = f.reset || fresh_;
            fresh_ = false;
            [scaler_ encodeToCommandBuffer:mcb];
            if (copy_to) {
                id<MTLBlitCommandEncoder> be = [mcb blitCommandEncoder];
                [be copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(f.out.width, f.out.height, 1)
                          toTexture:copy_to destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [be endEncoding];
            }
            if (vk_event_) [mcb encodeSignalEvent:vk_event_ value:done];
            const bool gpu_signal = vk_event_ != nil;
            MetalFx* me = this;   // the backend lives as long as the process
            [mcb addCompletedHandler:^(id<MTLCommandBuffer> b) {
                g_diag.completed.store(k);
                g_diag.status.store(static_cast<int64_t>(b.status));
                const bool ok = b.status == MTLCommandBufferStatusCompleted;
                if (!ok) me->failed_on_gpu(b);
                if (!gpu_signal || !ok) me->signal_done(done);   // KosmicKrisp: the rest of the frame runs after this
            }];
            [mcb commit];
            g_diag.issued.store(k);
            g_diag.committed_ns.store(now_ns());
            last_ = mcb;
            if (!vk_event_) bridge_.push(ready, own_event_, k);
        }
    }

    // Metal's completion thread. The rest of the frame waits for `done`: always reached, even after a failure.
    void signal_done(uint64_t done) {
        uint64_t now = 0;
        if (vkGetSemaphoreCounterValue(dev_->device(), sem_, &now) == VK_SUCCESS && now >= done) return;
        VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        ssi.semaphore = sem_;
        ssi.value = done;
        const VkResult r = vkSignalSemaphore(dev_->device(), &ssi);
        if (r != VK_SUCCESS && !dev_->device_lost()) NXLOG_E(kTag, "vkSignalSemaphore(%llu) failed (%d)", static_cast<unsigned long long>(done), static_cast<int>(r));
    }

    void failed_on_gpu(id<MTLCommandBuffer> b) {
        if (gpu_failed_.exchange(true)) return;
        NXLOG_E(kTag, "the MetalFX command buffer failed (status %ld): %s", static_cast<long>(b.status),
                b.error ? b.error.localizedDescription.UTF8String : "no error given");
        set_why("the MetalFX command buffer failed on the GPU");
        broken_.store(true);
    }

    // The rest of the frame let go without Metal work (no command buffer could be made): after the first part, from a helper.
    void release_from_cpu(uint64_t ready, uint64_t done) {
        gfx::Device* dev = dev_;
        VkSemaphore sem = sem_;
        std::thread([this, dev, sem, ready, done] {
            VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &sem;
            wi.pValues = &ready;
            while (vkWaitSemaphores(dev->device(), &wi, 100'000'000ull) == VK_TIMEOUT && !dev->device_lost()) {}
            signal_done(done);
        }).detach();
    }

    gfx::Device* dev_ = nullptr;
    id<MTLDevice> mdev_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLFXTemporalScaler> scaler_ = nil;
    id<MTLTexture> own_out_ = nil;
    id<MTLCommandBuffer> last_ = nil;
    id<MTLSharedEvent> vk_event_ = nil;    // the semaphore's own event (MoltenVK)
    id<MTLSharedEvent> own_event_ = nil;   // set by the bridge (host round trip)
    Bridge bridge_;
    VkSemaphore sem_ = VK_NULL_HANDLE;
    uint64_t frame_ = 0;
    VkExtent2D scaler_in_ = {}, scaler_out_ = {};
    bool fresh_ = true;
    float min_scale_ = 1.0f, max_scale_ = 3.0f;
    std::string driver_, interop_;
    std::unordered_map<VkImage, Made> made_;
    std::atomic<bool> broken_{false}, gpu_failed_{false};
    bool logged_start_ = false, told_foreign_ = false, told_bigger_ = false, told_scale_ = false;
};

MetalFx* g_inst = nullptr;
bool g_tried = false;

}  // namespace

TemporalBackend* temporal_metalfx(void* gfx_device) {
    if (!g_tried && gfx_device) {
        g_tried = true;
        auto* m = new MetalFx();
        m->init(*static_cast<gfx::Device*>(gfx_device));
        g_inst = m;   // kept even when it failed: broken() tells temporal.cpp to hand over (and the semaphore, if any, is freed at shutdown)
    }
    return g_inst && !g_inst->broken() ? g_inst : nullptr;
}

bool temporal_metalfx_available() {
    static const bool ok = [] {
        @autoreleasepool {
            id<MTLDevice> d = MTLCreateSystemDefaultDevice();
            return d && [MTLFXTemporalScalerDescriptor supportsDevice:d];
        }
    }();
    std::lock_guard<std::mutex> lk(g_why_mu);
    return ok && g_why.empty();
}

const char* temporal_metalfx_why() {
    std::lock_guard<std::mutex> lk(g_why_mu);
    return g_why.empty() ? nullptr : g_why.c_str();
}

std::string temporal_metalfx_report() {
    if (!g_inst || !g_diag.issued.load()) return {};
    char b[512];
    const uint64_t now = now_ns(), w = g_diag.waiting.load(), c = g_diag.committed_ns.load();
    std::snprintf(b, sizeof b, "MetalFX: frame %llu issued %.0f ms ago, released %llu, completed %llu (status %lld)%s",
                  static_cast<unsigned long long>(g_diag.issued.load()), c ? static_cast<double>(now - c) / 1e6 : 0.0,
                  static_cast<unsigned long long>(g_diag.released.load()), static_cast<unsigned long long>(g_diag.completed.load()),
                  static_cast<long long>(g_diag.status.load()), w ? "" : ", helper thread idle\n");
    std::string out = b;
    if (w) {
        std::snprintf(b, sizeof b, ", helper thread waiting for semaphore %llu for %.0f ms\n", static_cast<unsigned long long>(w),
                      static_cast<double>(now - g_diag.waiting_ns.load()) / 1e6);
        out += b;
    }
    return out;
}

void temporal_metalfx_shutdown() {
    if (g_inst) g_inst->shutdown();
}

}  // namespace nxrt::extras
