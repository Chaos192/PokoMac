// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/gfx/nvn/nvn_readback_policy.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace nxrt::nvn::be::rbdiag;
static int checks = 0;
static void check(bool condition, const char* description) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
}
int main() {
    const Stamp original{286, 7, 0x1000, 4096, {0, 1, 0, 1}, 3};
    auto allowed = [&](const Stamp& request) { return eligible(true, true, true, true, original, request); };
    check(allowed(original), "the deferred shape with matching previous storage is served from the previous read");
    check(!eligible(false, true, true, true, original, original), "readback_defer off stays on the normal path");
    check(!eligible(true, false, true, true, original, original), "wrong format or extent stays synchronous");
    check(!eligible(true, true, false, true, original, original), "a first or late read stays synchronous");
    check(!eligible(true, true, true, false, original, original), "no previous data must never be returned as valid");
    Stamp request = original; ++request.cpu_generation;
    check(!allowed(request), "CPU write plus completed upload still invalidates older data");
    request = original; ++request.sync_epoch;
    check(!allowed(request), "a synchronous render-thread refresh invalidates an older copy");
    request = original; request.storage = 0x2000;
    check(!allowed(request), "storage replacement invalidates the old snapshot");
    request = original; request.storage_bytes *= 2;
    check(!allowed(request), "storage size change invalidates the old snapshot");
    request = original; request.texture = 287;
    check(!allowed(request), "another texture of the shape is not served the first one's data");
    request = original; request.range.levels = 0;
    check(!allowed(request), "empty ranges never reuse cached data");
    request = original; request.range.layers = 2;
    check(!allowed(request), "an incompatible range stays synchronous");
    request = original; request.storage = 0;
    check(!eligible(true, true, true, true, request, request), "null storage is invalid even if stamps match");
    request = original; request.range.levels = 0;
    check(!eligible(true, true, true, true, request, request), "matching empty ranges are still invalid");
    check(!Range{0, 0, 0, 1}.fits(1, 1), "zero levels are rejected");
    check(!Range{0, 1, 0, 0}.fits(1, 1), "zero layers are rejected");
    check(!Range{std::numeric_limits<uint32_t>::max() - 1, 3, 0, 1}.fits(std::numeric_limits<uint32_t>::max(), 1), "overflowing level end is rejected");
    check(!Range{0, 1, std::numeric_limits<uint32_t>::max(), 2}.fits(1, std::numeric_limits<uint32_t>::max()), "overflowing layer end is rejected");
    std::cout << "PASS: " << checks << " eligibility and invalidation checks.\n";
}
