// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// macOS: Metal's shader compiler, under the Vulkan driver. KosmicKrisp (and MoltenVK) turn every pipeline into MSL source and hand it to
// Apple's compiler service, which by default runs only a couple of compilations at a time per device. The game's pipelines are built on
// several threads at once (the prewarm workers, the async_whole threads, --prepare-pipelines), so the device is told to use as many
// compiler threads as it has. MTLCreateSystemDefaultDevice returns the one shared MTLDevice of the GPU (the same object MTLCopyAllDevices
// lists, which the drivers use), so the setting reaches the driver's device. [gfx] metal_max_compile = false leaves Metal's default.
//
// Metal's shader cache. What Apple's compiler builds is kept on disk under <user cache folder>/<program identity>/com.apple.metal/, one
// folder for every program outside an app bundle that has no identity (no embedded Info.plist), and inside it in numbered files
// (functions.data, functions1.data, ...): a process takes the first file no other running process of that folder holds, at its first
// compilation, and looks only in that file. The game has an identity of its own (game-build/release_b/pkp_build.py write_identity), so
// the pipelines --prepare-pipelines compiles are in the file the game takes next; mac_metal_cache_report says which file a process got.
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <libproc.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#import <Foundation/Foundation.h>
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

namespace {

// The program's identity (CFBundleIdentifier of the main bundle; for a plain executable, of its embedded Info.plist). Empty: none.
std::string program_identity() {
    @autoreleasepool {
        NSString* id = [[NSBundle mainBundle] bundleIdentifier];
        return id ? std::string(id.UTF8String) : std::string();
    }
}

// The Metal cache file of compiled functions this process holds open, and its number (functions.data is 0, functionsN.data is N).
// Empty until the process compiled its first shader.
bool open_cache_file(std::string* path, int* index) {
    const pid_t pid = getpid();
    const int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes <= 0) return false;
    std::vector<proc_fdinfo> fds(static_cast<size_t>(bytes) / sizeof(proc_fdinfo) + 32);
    const int got = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
    for (int i = 0; got > 0 && i < got / static_cast<int>(sizeof(proc_fdinfo)); ++i) {
        if (fds[i].proc_fdtype != PROX_FDTYPE_VNODE) continue;
        char p[MAXPATHLEN];
        if (fcntl(fds[i].proc_fd, F_GETPATH, p) == -1 || !std::strstr(p, "/com.apple.metal/")) continue;
        const char* base = std::strrchr(p, '/');
        if (!base || std::strncmp(base + 1, "functions", 9) != 0) continue;
        const char* num = base + 10;
        const char* dot = std::strstr(num, ".data");
        if (!dot || dot[5]) continue;
        *path = p;
        *index = dot == num ? 0 : std::atoi(num);
        return true;
    }
    return false;
}

}  // namespace

void mac_metal_cache_report() {
    static std::atomic<int> tries{0};
    static std::atomic<bool> done{false};
    if (done.load(std::memory_order_relaxed) || tries.fetch_add(1, std::memory_order_relaxed) >= 64) return;
    std::string path;
    int index = 0;
    if (!open_cache_file(&path, &index) || done.exchange(true)) return;   // not open yet: asked again after the next pipeline
    const std::string id = program_identity();
    NXLOG_I("gfx", "Metal shader cache: %s (file %d of %s)", path.c_str(), index,
            id.empty() ? "the cache every program without an identity shares" : id.c_str());
    if (id.empty())
        NXLOG_W("gfx", "this program has no identity of its own (linked by an older Set up): it shares Metal's shader cache with every program "
                       "outside an app and finds what Set up compiled only when it gets the same cache file; run Set up again");
    else if (index > 0)
        NXLOG_W("gfx", "another process of this program holds Metal's first cache file (a game or a preparation from the same folder?): "
                       "the pipelines compiled into that file are compiled again in this one");
}

std::string mac_metal_cache_key() {
    char osv[64] = {};
    size_t n = sizeof osv - 1;
    if (sysctlbyname("kern.osversion", osv, &n, nullptr, 0) != 0) std::strcpy(osv, "?");
    const std::string id = program_identity();
    char base[MAXPATHLEN] = {};
    std::string dir = confstr(_CS_DARWIN_USER_CACHE_DIR, base, sizeof base) ? std::string(base) : std::string();
    if (!dir.empty() && dir.back() != '/') dir += '/';
    dir += (id.empty() ? std::string() : id + "/") + "com.apple.metal";
    struct stat st = {};
    char born[48] = "none";
    if (stat(dir.c_str(), &st) == 0) std::snprintf(born, sizeof born, "%lld", static_cast<long long>(st.st_birthtimespec.tv_sec));
    return "metal=" + (id.empty() ? std::string("shared") : id) + "/" + osv + "/" + born;
}

}  // namespace nxrt::platform
