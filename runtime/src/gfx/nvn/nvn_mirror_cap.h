// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// S38: how much video memory the device-local pool copies ([nvn] pool_mirror) may take. Pure, so the tests can call it.
#pragma once

#include <algorithm>
#include <cstdint>

namespace nxrt::nvn {

struct MirrorCap {
    uint64_t bytes = 0;     // most bytes the copies may take in all (0: no copies)
    const char* why = "";   // for the log line
};

constexpr uint64_t kMirrorCapMax = 2048ull << 20;      // the default cap on a large card
constexpr uint64_t kMirrorMinVram = 4ull << 30;        // below this much video memory there are no copies by default

// vram_bytes: the largest device-local heap (0 when unknown). explicit_mb >= 0: [nvn] pool_mirror_mb was set in the ini or the
// environment, and that value wins; -1: not set, the cap follows the card: min(2048 MiB, a quarter of the heap), none below 4 GiB.
// shared_memory: an integrated GPU or APU, whose "video memory" is system RAM; the copies would only cost RAM there, so none by default.
inline MirrorCap mirror_cap(uint64_t vram_bytes, int64_t explicit_mb, bool shared_memory = false) {
    if (explicit_mb >= 0) return {static_cast<uint64_t>(explicit_mb) << 20, "[nvn] pool_mirror_mb"};
    if (shared_memory) return {0, "integrated GPU (shares system memory)"};
    if (vram_bytes < kMirrorMinVram) return {0, "under 4 GiB of video memory"};
    return {std::min(kMirrorCapMax, vram_bytes / 4), vram_bytes / 4 < kMirrorCapMax ? "a quarter of the video memory" : "2048 MiB"};
}

}  // namespace nxrt::nvn
