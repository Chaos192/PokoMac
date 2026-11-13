// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Texture content override hook (nxrt/tex_override.h): storage of the hook, the call from the upload path, the re-upload request.
#include <atomic>
#include <mutex>
#include <vector>

#include <nxrt/tex_override.h>

#include "astc2bc7.h"
#include "nvn_gpu.h"
#include "nvn_texdata.h"

namespace nxrt::gfx {
namespace {
std::mutex g_mu;
TexOverrideFn g_fn = nullptr;
void* g_user = nullptr;
std::atomic<bool> g_set{false};
}  // namespace

void set_texture_override(TexOverrideFn fn, void* user) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_fn = fn;
        g_user = user;
        g_set.store(fn != nullptr);
    }
    texture_override_changed();
}

void texture_override_changed() { nvn::be::textures_mark_override_candidates_dirty(); }

}  // namespace nxrt::gfx

namespace nxrt::nvn::be {

bool texture_override_active() { return gfx::g_set.load(); }

bool texture_override_apply(const Texture& t, uint32_t w, uint32_t h, std::vector<uint8_t>& bc7) {
    gfx::TexOverrideFn fn;
    void* user;
    {
        std::lock_guard<std::mutex> lk(gfx::g_mu);
        fn = gfx::g_fn;
        user = gfx::g_user;
    }
    if (!fn || !t.mem) return false;
    gfx::TexOverrideReq req;
    req.width = w;
    req.height = h;
    req.tiled = t.mem;
    req.tiled_size = static_cast<size_t>(t.storage_size);
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    if (!fn(req, rgba.data(), user)) return false;
    a2b_options opt{};
    opt.quality = A2B_DEFAULT_QUALITY;
    opt.threads = 1;
    opt.allow_opaque_mode6 = 0;
    return a2b_encode_bc7(w, h, rgba.data(), &opt, bc7.data(), bc7.size(), nullptr) == A2B_OK;
}

}  // namespace nxrt::nvn::be
