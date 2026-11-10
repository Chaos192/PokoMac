// SPDX-License-Identifier: GPL-3.0-or-later
// macOS: Metal's shader compiler, under the Vulkan driver. KosmicKrisp (and MoltenVK) turn every pipeline into MSL source and hand it to
// Apple's compiler service, which by default runs only a couple of compilations at a time per device. The game's pipelines are built on
// several threads at once (the prewarm workers, the async_whole threads, --prepare-pipelines), so the device is told to use as many
// compiler threads as it has. MTLCreateSystemDefaultDevice returns the one shared MTLDevice of the GPU (the same object MTLCopyAllDevices
// lists, which the drivers use), so the setting reaches the driver's device. [gfx] metal_max_compile = false leaves Metal's default.
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"

#import <Metal/Metal.h>

namespace nxrt::platform {

void mac_metal_tune() {
    if (!config_get_bool("gfx", "metal_max_compile", true)) return;
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();   // kept: the device lives as long as the process
        if (!dev) return;
        if (@available(macOS 13.3, *)) {
            dev.shouldMaximizeConcurrentCompilation = YES;
            NXLOG_I("gfx", "Metal: %s compiles up to %lu shaders at once", dev.name.UTF8String,
                    static_cast<unsigned long>(dev.maximumConcurrentCompilationTaskCount));
        }
    }
}

}  // namespace nxrt::platform
