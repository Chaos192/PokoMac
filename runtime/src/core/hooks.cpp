// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Site hooks (hooks.h): nx_site_inline / nx_site_midhook / nx_hook_entry, the runtime half of the lifter's site lists.
// Design: an immutable snapshot (address -> hooks of each kind) published through an atomic pointer. Registration copies the snapshot,
// edits the copy and publishes it; retired snapshots stay allocated until hooks_clear() because a guest thread may still be reading one.
// The call path takes no lock. The site-list scheme the lifted callouts come from is ssbu-recomp's tools/lifter/relift_sites.py (via
// recomp nxrecomp/program.py load_sites); nothing of ssbu's code is used here.
#include "nxrt/hooks.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "nxrt/log.h"

namespace nxrt {
namespace {

struct Hook {
    uint32_t id = 0;
    HookKind kind = HookKind::Inline;
    uint64_t addr = 0;
    InlineHookFn inl = nullptr;
    SiteHookFn site = nullptr;
    void* user = nullptr;
    std::atomic<uint64_t> calls{0};
};

struct Chain {
    std::vector<std::shared_ptr<Hook>> inl, mid, entry;
};
using Snapshot = std::unordered_map<uint64_t, Chain>;

std::mutex g_write;
std::atomic<const Snapshot*> g_snap{nullptr};
std::vector<std::unique_ptr<const Snapshot>> g_retired;          // owns every published snapshot (the current one is the last)
std::vector<std::shared_ptr<Hook>> g_all;                        // by registration order, for hook_calls / counting
uint32_t g_next_id = 1;
std::atomic<uint64_t> g_site_calls[3];

std::vector<std::shared_ptr<Hook>>& chain_of(Chain& c, HookKind k) {
    return k == HookKind::Inline ? c.inl : k == HookKind::Mid ? c.mid : c.entry;
}
const std::vector<std::shared_ptr<Hook>>& chain_of(const Chain& c, HookKind k) {
    return k == HookKind::Inline ? c.inl : k == HookKind::Mid ? c.mid : c.entry;
}

void publish(std::unique_ptr<Snapshot> s) {
    const Snapshot* raw = s.get();
    g_retired.push_back(std::move(s));
    g_snap.store(raw, std::memory_order_release);
}

uint32_t add(HookKind kind, uint64_t addr, InlineHookFn inl, SiteHookFn site, void* user) {
    if (!(kind == HookKind::Inline ? inl != nullptr : site != nullptr)) return 0;
    std::lock_guard<std::mutex> lk(g_write);
    auto h = std::make_shared<Hook>();
    h->id = g_next_id++;
    h->kind = kind;
    h->addr = addr;
    h->inl = inl;
    h->site = site;
    h->user = user;
    const Snapshot* cur = g_snap.load(std::memory_order_relaxed);
    auto next = cur ? std::make_unique<Snapshot>(*cur) : std::make_unique<Snapshot>();
    chain_of((*next)[addr], kind).push_back(h);
    publish(std::move(next));
    g_all.push_back(h);
    return h->id;
}

}  // namespace

uint32_t hook_add_inline(uint64_t addr, InlineHookFn fn, void* user) { return add(HookKind::Inline, addr, fn, nullptr, user); }
uint32_t hook_add_mid(uint64_t addr, SiteHookFn fn, void* user) { return add(HookKind::Mid, addr, nullptr, fn, user); }
uint32_t hook_add_entry(uint64_t addr, SiteHookFn fn, void* user) { return add(HookKind::Entry, addr, nullptr, fn, user); }

bool hook_remove(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_write);
    std::shared_ptr<Hook> victim;
    for (auto& h : g_all)
        if (h->id == id) { victim = h; break; }
    if (!victim) return false;
    const Snapshot* cur = g_snap.load(std::memory_order_relaxed);
    auto next = std::make_unique<Snapshot>(*cur);
    auto it = next->find(victim->addr);
    if (it != next->end()) {
        auto& v = chain_of(it->second, victim->kind);
        for (size_t i = 0; i < v.size(); i++)
            if (v[i]->id == id) { v.erase(v.begin() + static_cast<ptrdiff_t>(i)); break; }
        if (it->second.inl.empty() && it->second.mid.empty() && it->second.entry.empty()) next->erase(it);
    }
    publish(std::move(next));
    for (size_t i = 0; i < g_all.size(); i++)
        if (g_all[i]->id == id) { g_all.erase(g_all.begin() + static_cast<ptrdiff_t>(i)); break; }
    return true;
}

size_t hook_count() {
    std::lock_guard<std::mutex> lk(g_write);
    return g_all.size();
}

size_t hook_count_at(uint64_t addr, HookKind kind) {
    const Snapshot* s = g_snap.load(std::memory_order_acquire);
    if (!s) return 0;
    auto it = s->find(addr);
    return it == s->end() ? 0 : chain_of(it->second, kind).size();
}

uint64_t hook_calls(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_write);
    for (auto& h : g_all)
        if (h->id == id) return h->calls.load(std::memory_order_relaxed);
    return 0;
}

uint64_t hook_site_calls(HookKind kind) { return g_site_calls[static_cast<int>(kind)].load(std::memory_order_relaxed); }

void hooks_clear() {
    std::lock_guard<std::mutex> lk(g_write);
    g_snap.store(nullptr, std::memory_order_release);
    g_retired.clear();
    g_all.clear();
    for (auto& c : g_site_calls) c.store(0);
}

}  // namespace nxrt

extern "C" {

void nx_site_inline(Ctx* c, uint64_t addr) {
    nxrt::g_site_calls[0].fetch_add(1, std::memory_order_relaxed);
    const nxrt::Snapshot* s = nxrt::g_snap.load(std::memory_order_acquire);
    if (!s) return;
    auto it = s->find(addr);
    if (it == s->end()) return;
    for (const auto& h : it->second.inl) {
        h->calls.fetch_add(1, std::memory_order_relaxed);
        h->inl(c, addr, h->user);
    }
}

static int run_chain(Ctx* c, uint64_t addr, nxrt::HookKind kind) {
    nxrt::g_site_calls[static_cast<int>(kind)].fetch_add(1, std::memory_order_relaxed);
    const nxrt::Snapshot* s = nxrt::g_snap.load(std::memory_order_acquire);
    if (!s) return 0;
    auto it = s->find(addr);
    if (it == s->end()) return 0;
    for (const auto& h : nxrt::chain_of(it->second, kind)) {
        h->calls.fetch_add(1, std::memory_order_relaxed);
        if (h->site(c, addr, h->user)) return 1;
    }
    return 0;
}

int nx_site_midhook(Ctx* c, uint64_t addr) { return run_chain(c, addr, nxrt::HookKind::Mid); }
int nx_hook_entry(Ctx* c, uint64_t addr) { return run_chain(c, addr, nxrt::HookKind::Entry); }

}  // extern "C"
