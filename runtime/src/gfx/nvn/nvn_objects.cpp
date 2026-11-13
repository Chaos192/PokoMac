// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Guest/host object binding and the memory pool registry (GPU address -> pool).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <shared_mutex>
#include <string>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nvn_core.h"

namespace nxrt::nvn::be {

void bind_guest(void* guest, Obj* obj) {
    GuestHeader* h = static_cast<GuestHeader*>(guest);
    Obj* old = (h->magic == kGuestMagic) ? h->host : nullptr;
    h->host = obj;
    h->kind = static_cast<uint32_t>(obj->kind);
    h->magic = kGuestMagic;
    if (old) old->unref();
}

void unbind_guest(void* guest) {
    GuestHeader* h = static_cast<GuestHeader*>(guest);
    Obj* old = (h->magic == kGuestMagic) ? h->host : nullptr;
    h->host = nullptr;
    h->magic = 0;
    h->kind = 0;
    if (old) old->unref();
}

namespace {
std::shared_mutex g_pool_mu;
std::map<uint64_t, MemoryPool*> g_pools;   // gpu_base -> pool
// Registry generation: bumped under the unique lock by every change of g_pools (pool_register, pool_unregister; a pool's gpu_base and
// size are set before it is registered and never change while it is, and nvnMemoryPoolMapVirtual remaps tiles inside a pool's range
// without touching it). The last-hit cache of pool_lookup is valid only for the generation it was filled under.
std::atomic<uint64_t> g_pool_gen{1};

// [nvn] pool_cache (default on): pool_lookup first checks the last two pools this thread found (a draw resolves 5-15 addresses, mostly
// in one or two pools) and takes the shared lock and walks the map only on a miss. thread_local: the render thread, the game threads
// and the texture preparation workers all look addresses up.
struct PoolCacheEntry { uint64_t gen = 0, lo = 0, hi = 0; MemoryPool* pool = nullptr; };
thread_local PoolCacheEntry t_pool_cache[2];
bool pool_cache_on() {
    static const bool on = [] {
        const bool v = nxrt::config_get_bool("nvn", "pool_cache", true);
        NXLOG_I("nvn", "pool_lookup last-hit cache %s ([nvn] pool_cache)", v ? "on" : "off");
        return v;
    }();
    return on;
}

// Gauge (nxrt/gauge.h). The callback takes g_pool_mu; it is removed before the map is destroyed.
struct PoolGauges {
    PoolGauges() {
        nxrt::gauge_register("nvn.pool_registry", [] {
            std::shared_lock<std::shared_mutex> lk(g_pool_mu);
            return static_cast<int64_t>(g_pools.size());
        });
    }
    ~PoolGauges() { nxrt::gauge_unregister("nvn.pool_registry"); }
};
PoolGauges g_pool_gauges;
}  // namespace

void pool_register(MemoryPool* p) {
    static const bool report_registered = (diag_add_report([] { return pool_fill_report(); }), true);   // freeze dump and crash report
    (void)report_registered;
    std::unique_lock<std::shared_mutex> lk(g_pool_mu);
    g_pools[p->gpu_base] = p;
    g_pool_gen.fetch_add(1, std::memory_order_release);
}

void pool_unregister(MemoryPool* p) {
    std::unique_lock<std::shared_mutex> lk(g_pool_mu);
    auto it = g_pools.find(p->gpu_base);
    if (it != g_pools.end() && it->second == p) g_pools.erase(it);
    g_pool_gen.fetch_add(1, std::memory_order_release);
}

// ---- pool fill ----------------------------------------------------------------------------------------------------------------
// A game that sizes its pools for the console runs them full at larger render sizes, and its suballocator then fails without a word
// (#119, #164: a 4K world load hung or crashed with nothing in the log). The highest placed end is a lower bound of what the pool needs;
// its timeline in the log shows which pool fills, and when.
namespace {
constexpr uint64_t kFillLogMin = 64ull << 20;                         // smaller pools (uniform rings and the like) are not logged
constexpr int kFillSteps[] = {50, 75, 90, 95, 99};
}  // namespace

void pool_note_placement(MemoryPool* p, uint64_t offset, uint64_t size, const char* kind, int w, int h, int format) {
    if (!p || p->is_virtual || !p->size) return;
    p->placed_count.fetch_add(1, std::memory_order_relaxed);
    const uint64_t end = offset + size;
    uint64_t cur = p->placed_end.load(std::memory_order_relaxed);
    while (end > cur && !p->placed_end.compare_exchange_weak(cur, end, std::memory_order_relaxed)) {}
    if (end <= cur || p->size < kFillLogMin) return;
    const double pct = 100.0 * static_cast<double>(end) / static_cast<double>(p->size);
    uint32_t bits = 0;
    for (size_t i = 0; i < sizeof kFillSteps / sizeof kFillSteps[0]; ++i)
        if (pct >= kFillSteps[i]) bits |= 1u << i;
    const uint32_t before = p->fill_logged.fetch_or(bits, std::memory_order_relaxed);
    if ((before | bits) == before) return;                                  // no new threshold (or another thread logged it)
    char what[64];
    if (w > 0) std::snprintf(what, sizeof what, "%s %dx%d format %d", kind ? kind : "?", w, h, format);
    else std::snprintf(what, sizeof what, "%s", kind ? kind : "?");
    NXLOG_I("nvn.pool", "pool %p, %llu MiB (flags 0x%x): placed objects now reach %.1f MiB, %.1f%% of it (%llu placed; this one: %s, %llu KiB at "
            "offset 0x%llx)", static_cast<void*>(p->base), static_cast<unsigned long long>(p->size >> 20), static_cast<unsigned>(p->flags),
            static_cast<double>(end) / (1 << 20), pct, static_cast<unsigned long long>(p->placed_count.load(std::memory_order_relaxed)),
            what, static_cast<unsigned long long>(size >> 10), static_cast<unsigned long long>(offset));
}

std::string pool_fill_report() {
    std::shared_lock<std::shared_mutex> lk(g_pool_mu, std::try_to_lock);
    if (!lk.owns_lock()) return "graphics pools: (busy)\n";
    std::string out = "graphics pools (highest end of a texture or buffer placed in it / its size):\n";
    size_t n = 0;
    for (const auto& [gpu_base, p] : g_pools) {
        if (p->is_virtual || p->size < (1ull << 20)) continue;
        const uint64_t end = p->placed_end.load(std::memory_order_relaxed);
        char b[256];
        std::snprintf(b, sizeof b, "  pool %p, %llu MiB (flags 0x%x): %.1f MiB, %.1f%%, %llu placed\n", static_cast<void*>(p->base),
                      static_cast<unsigned long long>(p->size >> 20), static_cast<unsigned>(p->flags), static_cast<double>(end) / (1 << 20),
                      100.0 * static_cast<double>(end) / static_cast<double>(p->size),
                      static_cast<unsigned long long>(p->placed_count.load(std::memory_order_relaxed)));
        out += b;
        ++n;
    }
    if (!n) out += "  (none)\n";
    return out;
}

uint64_t pool_registry_gen() { return g_pool_gen.load(std::memory_order_acquire); }

PoolHit pool_lookup(uint64_t gpu_address) {
    static const bool cache = pool_cache_on();
    // The generation is read before the cache and before the map: a change made after this load bumps it again, so an entry filled from a
    // map that already had the change is stored under the old generation and missed next time (never the reverse).
    const uint64_t gen = cache ? g_pool_gen.load(std::memory_order_acquire) : 0;
    if (cache) {
        for (int i = 0; i < 2; ++i) {
            const PoolCacheEntry& e = t_pool_cache[i];
            if (e.gen == gen && gpu_address >= e.lo && gpu_address < e.hi) {
                ft::split_count(ft::kCntPoolHit);
                if (i) std::swap(t_pool_cache[0], t_pool_cache[1]);
                return {t_pool_cache[0].pool, gpu_address - t_pool_cache[0].lo};
            }
        }
        ft::split_count(ft::kCntPoolMiss);
    }
    std::shared_lock<std::shared_mutex> lk(g_pool_mu);
    auto it = g_pools.upper_bound(gpu_address);
    if (it == g_pools.begin()) return {};
    --it;
    MemoryPool* p = it->second;
    if (gpu_address >= p->gpu_base + p->size) return {};
    if (cache) {
        t_pool_cache[1] = t_pool_cache[0];
        t_pool_cache[0] = {gen, p->gpu_base, p->gpu_base + p->size, p};
    }
    return {p, gpu_address - p->gpu_base};
}

}  // namespace nxrt::nvn::be
