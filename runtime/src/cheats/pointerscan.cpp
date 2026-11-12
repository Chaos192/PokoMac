// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Pointer scanner (pointerscan.h).
#include "pointerscan.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace nxrt::cheats {

std::string PointerPath::text() const {
    char b[64];
    std::string s = module;
    std::snprintf(b, sizeof b, "+0x%llx", static_cast<unsigned long long>(base_offset));
    s += b;
    for (uint64_t o : offsets) {
        std::snprintf(b, sizeof b, " -> +0x%llx", static_cast<unsigned long long>(o));
        s += b;
    }
    return s;
}

namespace {
bool in_any(uint64_t a, const std::vector<ScanRegion>& ranges) {
    for (const auto& r : ranges)
        if (a >= r.addr && a < r.addr + r.size) return true;
    return false;
}
}  // namespace

PointerMap pointer_map_build(MemSource& src, const PointerScanOptions& opt, const std::vector<ScanRegion>& in_ranges, ScanProgress* prog) {
    PointerMap map;
    auto regions = src.regions(opt.scan);
    uint64_t total = 0;
    for (const auto& r : regions) total += r.size;
    if (prog) { prog->bytes_total = total; prog->bytes_done = 0; prog->running = true; }
    const unsigned align = opt.align ? opt.align : 8;

    std::mutex mu;
    std::atomic<size_t> ri{0};
    auto work = [&] {
        std::vector<uint8_t> buf;
        std::vector<std::pair<uint64_t, uint64_t>> local;
        for (;;) {
            const size_t i = ri.fetch_add(1);
            if (i >= regions.size() || (prog && prog->cancel.load())) break;
            const ScanRegion& r = regions[i];
            const size_t chunk = 1 << 20;
            for (uint64_t off = 0; off < r.size; off += chunk) {
                if (prog && prog->cancel.load()) break;
                const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk, r.size - off));
                buf.resize(n);
                if (!src.read(r.addr + off, buf.data(), n)) { if (prog) prog->bytes_done += n; continue; }
                const uint64_t a0 = r.addr + off;
                const uint64_t start = (a0 + (align - 1)) & ~uint64_t(align - 1);
                for (uint64_t a = start; a + 8 <= a0 + n; a += align) {
                    uint64_t val;
                    std::memcpy(&val, buf.data() + (a - a0), 8);
                    if (val && in_any(val, in_ranges)) local.emplace_back(val, a);
                }
                if (prog) prog->bytes_done += n;
            }
        }
        if (!local.empty()) {
            std::lock_guard<std::mutex> lk(mu);
            map.by_value.insert(map.by_value.end(), local.begin(), local.end());
        }
    };
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 4; if (hc > 8) hc = 8;
    std::vector<std::thread> pool;
    for (unsigned k = 1; k < hc; ++k) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    std::sort(map.by_value.begin(), map.by_value.end());
    if (prog) prog->running = false;
    return map;
}

namespace {
// Slots whose value is in [t - maxoff, t]: offset = t - value, 0..maxoff.
void slots_pointing_near(const PointerMap& map, uint64_t t, uint64_t maxoff, std::vector<std::pair<uint64_t, uint64_t>>& out) {
    // find value range [t-maxoff, t]
    const uint64_t lo = t > maxoff ? t - maxoff : 0;
    auto a = std::lower_bound(map.by_value.begin(), map.by_value.end(), std::make_pair(lo, uint64_t(0)));
    for (auto it = a; it != map.by_value.end() && it->first <= t; ++it)
        out.emplace_back(it->first, it->second);   // (value, slot)
}

const StaticRange* static_of(const std::vector<StaticRange>& roots, uint64_t addr) {
    for (const auto& r : roots)
        if (addr >= r.lo && addr < r.hi) return &r;
    return nullptr;
}
}  // namespace

std::vector<PointerPath> pointer_find_paths(const PointerMap& map, const std::vector<StaticRange>& roots, uint64_t target, const PointerScanOptions& opt) {
    std::vector<PointerPath> out;
    // DFS from the target address upward; offsets are collected innermost-first then reversed.
    struct Frame { uint64_t addr; std::vector<uint64_t> offs; unsigned depth; };
    std::vector<Frame> stack;
    stack.push_back({target, {}, 0});
    while (!stack.empty() && out.size() < opt.max_paths) {
        Frame f = std::move(stack.back());
        stack.pop_back();
        if (f.depth >= opt.max_depth) continue;
        std::vector<std::pair<uint64_t, uint64_t>> slots;
        slots_pointing_near(map, f.addr, opt.max_offset, slots);
        for (const auto& s : slots) {
            if (out.size() >= opt.max_paths) break;
            const uint64_t value = s.first, slot = s.second;
            const uint64_t off = f.addr - value;
            std::vector<uint64_t> offs = f.offs;
            offs.push_back(off);                      // innermost offset first
            if (const StaticRange* sr = static_of(roots, slot)) {
                PointerPath p;
                p.module = sr->module;
                p.mem_type = sr->mem_type;
                p.base_offset = slot - sr->base;
                p.offsets.assign(offs.rbegin(), offs.rend());   // outermost (static) first
                out.push_back(std::move(p));
            } else if (f.depth + 1 < opt.max_depth) {
                stack.push_back({slot, offs, f.depth + 1});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const PointerPath& a, const PointerPath& b) { return a.offsets.size() < b.offsets.size(); });
    return out;
}

uint64_t pointer_resolve(MemSource& src, const PointerPath& p, uint64_t (*module_base)(const std::string&)) {
    const uint64_t base = module_base ? module_base(p.module) : 0;
    if (!base && !module_base) return 0;
    uint64_t slot0 = base + p.base_offset;
    uint64_t val = 0;
    if (!src.read(slot0, &val, 8)) return 0;
    if (p.offsets.empty()) return val;
    for (size_t i = 0; i + 1 < p.offsets.size(); ++i) {
        uint64_t next = 0;
        if (!src.read(val + p.offsets[i], &next, 8)) return 0;
        val = next;
    }
    return val + p.offsets.back();
}

std::vector<PointerPath> pointer_rescan(MemSource& src, const std::vector<PointerPath>& paths, uint64_t new_target, uint64_t (*module_base)(const std::string&)) {
    std::vector<PointerPath> kept;
    for (const auto& p : paths)
        if (pointer_resolve(src, p, module_base) == new_target) kept.push_back(p);
    return kept;
}

}  // namespace nxrt::cheats
