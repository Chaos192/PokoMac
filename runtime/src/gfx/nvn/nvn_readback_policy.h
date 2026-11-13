// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] readback_defer's eligibility (nvn_gpu_texture.cpp download_texture): a read may be served from the previous read when the texture
// is the deferred shape, the read repeats the previous one, and nothing changed the storage in between (the stamp).
#pragma once
#include <cstdint>

namespace nxrt::nvn::be::rbdiag {
struct Range {
    uint32_t level = 0, levels = 0, layer = 0, layers = 0;
    bool operator==(const Range&) const = default;
    bool fits(uint32_t level_count, uint32_t layer_count) const {
        return levels && layers && level < level_count && levels <= level_count - level &&
               layer < layer_count && layers <= layer_count - layer;
    }
};
struct Stamp {
    uint32_t texture = 0, cpu_generation = 0;
    uintptr_t storage = 0;
    uint64_t storage_bytes = 0;
    Range range;
    uint64_t sync_epoch = 0;   // a render-thread synchronous read may refresh storage without taking the guest read mutex
    bool operator==(const Stamp&) const = default;
};
inline bool eligible(bool enabled, bool exact_format_and_size, bool recent_repeat, bool have_previous, const Stamp& previous,
                     const Stamp& request) {
    return enabled && exact_format_and_size && recent_repeat && have_previous && request.storage && request.storage_bytes &&
           request.range.fits(1, 1) && previous == request;
}
}  // namespace nxrt::nvn::be::rbdiag
