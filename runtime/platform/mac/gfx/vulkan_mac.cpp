// SPDX-License-Identifier: GPL-3.0-or-later
// macOS: finding Vulkan. There is no system Vulkan loader, so the game folder carries one, next to pkp:
//   libvulkan.1.dylib                       the Khronos loader
//   libvulkan_kosmickrisp.dylib             KosmicKrisp (LunarG's conformant Vulkan 1.4 driver on Metal), the default
//   libMoltenVK.dylib                       MoltenVK (Vulkan portability driver on Metal), [gfx] vulkan_driver = moltenvk
//   vulkan/icd.d/libkosmickrisp_icd.json    their manifests ("library_path" relative to the json: ../../<dylib>)
//   vulkan/icd.d/MoltenVK_icd.json
// Unless the environment already names the drivers (VK_DRIVER_FILES / VK_ICD_FILENAMES: the Vulkan SDK's setup-env.sh, or a user),
// the loader is pointed at the chosen manifest. Without a bundled loader the usual places are tried (the Vulkan SDK in /usr/local/lib or
// on DYLD_LIBRARY_PATH). SDL is told to use the same library, so the window's surface comes from the same loader instance.
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"

#include <SDL3/SDL.h>
#include <volk.h>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>

#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

namespace nxrt::platform {
namespace {

constexpr const char* kTag = "gfx";

std::string exe_dir() {
    uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);
    std::string p(n, '\0');
    if (_NSGetExecutablePath(p.data(), &n) != 0) return ".";
    char real[PATH_MAX];
    std::string s = realpath(p.c_str(), real) ? real : p.c_str();
    const size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? "." : s.substr(0, slash);
}

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

}  // namespace

bool mac_load_vulkan() {
    const std::string dir = exe_dir();
    if (!getenv("VK_DRIVER_FILES") && !getenv("VK_ICD_FILENAMES")) {
        const std::string want = config_get_str("gfx", "vulkan_driver", "kosmickrisp");
        const std::string kk = dir + "/vulkan/icd.d/libkosmickrisp_icd.json", mvk = dir + "/vulkan/icd.d/MoltenVK_icd.json";
        std::string pick;
        if (want == "moltenvk" && exists(mvk)) pick = mvk;
        else if (exists(kk)) pick = kk;
        else if (exists(mvk)) pick = mvk;
        if (!pick.empty()) {
            setenv("VK_DRIVER_FILES", pick.c_str(), 1);
            NXLOG_I(kTag, "Vulkan driver: %s ([gfx] vulkan_driver = %s)", pick.c_str(), want.c_str());
        }
    } else {
        NXLOG_I(kTag, "Vulkan drivers from the environment: %s", getenv("VK_DRIVER_FILES") ? getenv("VK_DRIVER_FILES") : getenv("VK_ICD_FILENAMES"));
    }
    const std::vector<std::string> loaders = {dir + "/libvulkan.1.dylib", "libvulkan.1.dylib", "libvulkan.dylib", "/usr/local/lib/libvulkan.1.dylib"};
    for (const std::string& l : loaders) {
        void* h = dlopen(l.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vkGetInstanceProcAddr"));
        if (!gipa) {
            dlclose(h);
            continue;
        }
        volkInitializeCustom(gipa);
        SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, l.c_str());
        NXLOG_I(kTag, "Vulkan loader: %s", l.c_str());
        return true;
    }
    NXLOG_E(kTag, "no Vulkan loader: put libvulkan.1.dylib next to pkp (the release does) or install the Vulkan SDK");
    return false;
}

}  // namespace nxrt::platform
