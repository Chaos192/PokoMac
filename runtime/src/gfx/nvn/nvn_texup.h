// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Background upgrade of the ASTC -> BC7 disk cache ([gfx] astc_quality = high; implementation nvn_texup.cpp).
//
// The first time a texture is needed and the cache has no entry of the high preset for it, the render thread (or a texprep job) uses what is there
// (an entry of the fast preset, or a fast transcode made on the spot, exactly as before) and hands a snapshot of the texture's source bytes to this pool.
// A worker thread untiles the snapshot, decodes the ASTC and encodes it again with the slow bc7e preset, then writes the cache entry. The next start
// finds the entry and the texture comes out sharper at no cost. The workers run at below-normal priority, a few threads only (the game keeps the
// cores), and stop within a fraction of a second when the GPU layer shuts down; half-finished work is dropped, never written.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>

namespace nxrt::nvn::be::texup {

// The work of a job. stop becomes true when the pool shuts down; the function should return false quickly then. true: the job did its work.
using Work = std::function<bool(const std::atomic<bool>& stop)>;

// Any thread. key identifies the job (a texture's cache key; one job per key per run), bytes is the memory the job holds while it waits and runs.
// Returns false when the job was not taken: the pool is off, the key was submitted before, or too much work is waiting already (then a later
// submit of the same key may succeed; the texture is simply upgraded in a later run).
bool submit(uint64_t key, uint64_t bytes, Work work);
// Cheap pre-check before a snapshot is made: false when submit(key, ...) would not take the job (pool off or stopping, key already submitted).
bool wants(uint64_t key);

// Stops the workers (a job in flight is asked to stop), drops the queue, logs the totals. A later submit starts the pool again.
void shutdown();

struct Stats {
    uint64_t submitted = 0, done = 0, failed = 0, dropped_budget = 0, waiting = 0;
};
Stats stats();

}  // namespace nxrt::nvn::be::texup
