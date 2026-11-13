// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Texture pixel edit hook (nxrt/tex_edit.h): storage of the hooks, the edited data kept in memory, the background rebuild after a change.
// prepare_texture (nvn_texdata.cpp) does the decode, the host edit and the encode; this file decides when.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

#include <nxrt/config.h>
#include <nxrt/log.h>
#include <nxrt/tex_edit.h>

#include "nvn_gpu.h"
#include "nvn_gpu_internal.h"
#include "nvn_texdata.h"

namespace nxrt::nvn::be {
namespace {

constexpr const char* kTag = "nvn.texedit";

struct MemoKey {
    uint64_t id, gen;
    VkFormat fmt;
    bool operator<(const MemoKey& o) const { return std::tie(id, gen, fmt) < std::tie(o.id, o.gen, o.fmt); }
};
struct Memo {
    bool ready = false;   // false: a thread is making it
    std::vector<uint8_t> data;
};

struct State {
    std::mutex mu;
    std::condition_variable cv_memo;   // a memo entry became ready or was given up
    gfx::TexEditHooks hooks;
    bool have_hooks = false;
    std::map<MemoKey, Memo> memo;
    std::set<uint32_t> edited;         // Texture::id of the textures that were given edited data
    gfx::TexEditStats st;
    // background rebuild
    std::condition_variable cv_work;
    bool want_rebuild = false;
    bool worker_started = false;
};

State& S() {
    static State* s = new State();   // never destroyed: the worker runs until the process ends
    return *s;
}

std::atomic<bool> g_active{false};

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool hooks_copy(gfx::TexEditHooks& h) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.have_hooks) return false;
    h = s.hooks;
    return true;
}

// The host's match on the game's storage, under SEH (a page the game unmapped must not take the process down).
uint64_t match_guarded(const gfx::TexEditHooks& h, const gfx::TexEditReq& r) {
    __try {
        return h.match(r, h.user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool prepare_guarded(const Texture& t, const TexCaps& caps, PreparedTexture& out) {
    __try {
        return prepare_texture(t, caps, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool candidate(const Texture& t, void*) {
    return t.mem && !t.is_display && t.fmt && t.fmt->astc && t.layers == 1 && t.b.samples <= 1 && t.b.target == NVN_TEXTURE_TARGET_2D;
}

gfx::TexEditReq req_of(const Texture& t, const uint8_t* src) {
    gfx::TexEditReq r;
    r.width = static_cast<uint32_t>(t.b.width);
    r.height = static_cast<uint32_t>(t.b.height);
    r.levels = t.levels;
    r.block_w = t.fmt ? t.fmt->block_w : 0;
    r.block_h = t.fmt ? t.fmt->block_h : 0;
    r.srgb = t.fmt && t.fmt->srgb;
    r.tiled = src;
    r.tiled_size = static_cast<size_t>(t.storage_size);
    return r;
}

// One pass after texture_edit_changed: new data for every live texture that matches, each uploaded again once its data is ready, and the ones
// edited before that no longer match uploaded again as the game's (they ask prepare_texture again, which finds the new data kept here).
void rebuild_once() {
    gfx::TexEditHooks h;
    const bool have = hooks_copy(h);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Ref<Texture>> all = texture_registry_collect(&candidate, nullptr);
    std::set<uint32_t> was_edited;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        was_edited = S().edited;
    }
    TexCaps caps;
    bool caps_ok = false;
    if (gpu::active()) {
        const gfx::DeviceFeatures& f = gpu::device().features();
        caps.bc = f.bc;
        caps.astc = f.astc_ldr;
        caps.bc7_transcode = true;
        caps_ok = true;
    }
    std::vector<Texture*> dirty;
    std::vector<uint32_t> not_edited;   // edited before, the game's own data from now on
    uint32_t made = 0;
    for (const Ref<Texture>& r : all) {
        Texture* t = r.get();
        if (t->destroyed.load()) continue;
        const uint64_t id = have ? match_guarded(h, req_of(*t, t->mem)) : 0;
        if (id && caps_ok && !caps.astc) {
            PreparedTexture tmp;
            if (prepare_guarded(*t, caps, tmp)) ++made;   // keeps the edited data in the memo
        }
        if (id || was_edited.count(t->id)) {
            // uploaded again as soon as its own data is ready: the first textures change while the others are still being made
            dirty.push_back(t);
            t->cpu_dirty.store(true);
            gpu::mark_texture_cpu_dirty(t);
        }
        if (!id && was_edited.count(t->id)) not_edited.push_back(t->id);
    }
    {
        std::lock_guard<std::mutex> lk(S().mu);
        State& s = S();
        for (uint32_t i : not_edited) s.edited.erase(i);
        s.st.rebuilds++;
        s.st.last_rebuild_ms = ms_since(t0);
        s.st.last_rebuild_textures = made;
    }
    NXLOG_I(kTag, "rebuild: %u textures edited, %zu uploaded again, %.1f ms (%zu ASTC textures looked at)", made, dirty.size(), ms_since(t0), all.size());
}

void worker_main() {
    log_set_thread_name("nvn-texedit");
    State& s = S();
    std::unique_lock<std::mutex> lk(s.mu);
    for (;;) {
        s.cv_work.wait(lk, [&] { return s.want_rebuild; });
        s.want_rebuild = false;
        lk.unlock();
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        rebuild_once();
        lk.lock();
    }
}

}  // namespace

// ---- prepare_texture side (nvn_texdata.h) ---------------------------------------------------------------------------------------
bool tex_edit_active() { return g_active.load(std::memory_order_acquire); }

TexEditKey tex_edit_match(const Texture& t, const uint8_t* src) {
    TexEditKey k;
    gfx::TexEditHooks h;
    if (!hooks_copy(h) || !h.match) return k;
    k.id = h.match(req_of(t, src), h.user);   // prepare_texture already runs guarded where it runs off the render thread
    if (k.id) k.gen = h.generation ? h.generation(h.user) : 0;
    return k;
}

bool tex_edit_lookup(const TexEditKey& k, VkFormat f, std::vector<uint8_t>& data) {
    State& s = S();
    std::unique_lock<std::mutex> lk(s.mu);
    const MemoKey mk{k.id, k.gen, f};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        auto it = s.memo.find(mk);
        if (it == s.memo.end()) {
            s.memo[mk];   // this thread makes it: tex_edit_store or tex_edit_abandon follows
            return false;
        }
        if (it->second.ready) {
            data = it->second.data;
            s.st.reuses++;
            return true;
        }
        if (s.cv_memo.wait_until(lk, deadline) == std::cv_status::timeout) return false;   // make it here as well rather than wait longer
    }
}

void tex_edit_store(const TexEditKey& k, VkFormat f, const std::vector<uint8_t>& data, double ms) {
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        const uint64_t cur = s.have_hooks && s.hooks.generation ? s.hooks.generation(s.hooks.user) : k.gen;
        Memo& m = s.memo[MemoKey{k.id, k.gen, f}];
        m.ready = true;
        m.data = data;
        // data of older answers is not asked for again
        for (auto it = s.memo.begin(); it != s.memo.end();) {
            if (it->second.ready && it->first.gen != cur) it = s.memo.erase(it);
            else ++it;
        }
        s.st.edits++;
        s.st.last_edit_ms = ms;
    }
    s.cv_memo.notify_all();
}

void tex_edit_abandon(const TexEditKey& k, VkFormat f) {
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        auto it = s.memo.find(MemoKey{k.id, k.gen, f});
        if (it != s.memo.end() && !it->second.ready) s.memo.erase(it);
    }
    s.cv_memo.notify_all();
}

void tex_edit_level(uint64_t id, uint32_t level, uint32_t w, uint32_t h, uint8_t* rgba) {
    gfx::TexEditHooks hk;
    if (hooks_copy(hk) && hk.edit) hk.edit(id, level, w, h, rgba, hk.user);
}

void tex_edit_note(const Texture& t) {
    std::lock_guard<std::mutex> lk(S().mu);
    S().edited.insert(t.id);
}

int tex_edit_quality() {
    static const int q = static_cast<int>(std::clamp<int64_t>(config_get_int("nvn", "tex_edit_quality", 1), 0, 6));
    return q;
}

}  // namespace nxrt::nvn::be

namespace nxrt::gfx {

void set_texture_edit(const TexEditHooks* hooks) {
    using namespace nxrt::nvn::be;
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.have_hooks = hooks && hooks->match;
        s.hooks = hooks ? *hooks : TexEditHooks{};
        s.memo.clear();
        g_active.store(s.have_hooks, std::memory_order_release);
    }
    texture_edit_changed();
}

void texture_edit_changed() {
    using namespace nxrt::nvn::be;
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.have_hooks && s.edited.empty()) return;
        s.want_rebuild = true;
        if (!s.worker_started) {
            s.worker_started = true;
            std::thread(&worker_main).detach();
        }
    }
    s.cv_work.notify_one();
}

TexEditStats texture_edit_stats() {
    using namespace nxrt::nvn::be;
    std::lock_guard<std::mutex> lk(S().mu);
    return S().st;
}

}  // namespace nxrt::gfx
