// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Input event queue, raw mouse accumulator and gamepad info table (declarations in include/nxrt/input.h).
// Every container is guarded by its own mutex: the platform thread pushes, any thread may drain or read.
#include "nxrt/input.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace nxrt::input {
namespace {

// Fixed ring. head is the index of the oldest event, count the number of queued events.
struct Queue {
    std::mutex mu;
    Event ring[kQueueCapacity];
    size_t head = 0;
    size_t count = 0;
    uint64_t dropped = 0;
    uint64_t pushed = 0;
};
Queue g_queue;

struct DeltaAcc {
    std::mutex mu;
    MouseDelta sum;
};
DeltaAcc g_delta;

struct PadTable {
    std::mutex mu;
    std::vector<GamepadInfo> pads;
};
PadTable g_pads;

}  // namespace

// ---- queue ------------------------------------------------------------------------------------------------------------

void queue_push(const Event& e) {
    std::lock_guard<std::mutex> lk(g_queue.mu);
    if (g_queue.count == kQueueCapacity) {  // full: the oldest event makes room
        g_queue.head = (g_queue.head + 1) % kQueueCapacity;
        --g_queue.count;
        ++g_queue.dropped;
    }
    g_queue.ring[(g_queue.head + g_queue.count) % kQueueCapacity] = e;
    ++g_queue.count;
    ++g_queue.pushed;
}

size_t queue_drain(Event* out, size_t max) {
    std::lock_guard<std::mutex> lk(g_queue.mu);
    if (!out || max == 0) return 0;
    const size_t n = std::min(max, g_queue.count);
    for (size_t i = 0; i < n; ++i) out[i] = g_queue.ring[(g_queue.head + i) % kQueueCapacity];
    g_queue.head = (g_queue.head + n) % kQueueCapacity;
    g_queue.count -= n;
    return n;
}

bool queue_pop(Event* out) {
    std::lock_guard<std::mutex> lk(g_queue.mu);
    if (g_queue.count == 0) return false;
    if (out) *out = g_queue.ring[g_queue.head];
    g_queue.head = (g_queue.head + 1) % kQueueCapacity;
    --g_queue.count;
    return true;
}

size_t queue_size() {
    std::lock_guard<std::mutex> lk(g_queue.mu);
    return g_queue.count;
}

uint64_t queue_dropped() {
    std::lock_guard<std::mutex> lk(g_queue.mu);
    return g_queue.dropped;
}

uint64_t queue_pushed() {
    std::lock_guard<std::mutex> lk(g_queue.mu);
    return g_queue.pushed;
}

void queue_clear() {
    std::lock_guard<std::mutex> lk(g_queue.mu);
    g_queue.head = 0;
    g_queue.count = 0;
    g_queue.dropped = 0;
    g_queue.pushed = 0;
}

// ---- raw mouse accumulator ------------------------------------------------------------------------------------------

void mouse_delta_add(double dx, double dy) {
    std::lock_guard<std::mutex> lk(g_delta.mu);
    g_delta.sum.dx += dx;
    g_delta.sum.dy += dy;
    ++g_delta.sum.events;
}

MouseDelta mouse_delta_take() {
    std::lock_guard<std::mutex> lk(g_delta.mu);
    const MouseDelta out = g_delta.sum;
    g_delta.sum = MouseDelta{};
    return out;
}

MouseDelta mouse_delta_peek() {
    std::lock_guard<std::mutex> lk(g_delta.mu);
    return g_delta.sum;
}

// ---- gamepads -----------------------------------------------------------------------------------------------------

size_t gamepad_count() {
    std::lock_guard<std::mutex> lk(g_pads.mu);
    return g_pads.pads.size();
}

size_t gamepad_list(GamepadInfo* out, size_t max) {
    std::lock_guard<std::mutex> lk(g_pads.mu);
    if (!out) return 0;
    const size_t n = std::min(max, g_pads.pads.size());
    for (size_t i = 0; i < n; ++i) out[i] = g_pads.pads[i];
    return n;
}

PadKind gamepad_kind_of(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_pads.mu);
    for (const GamepadInfo& p : g_pads.pads) {
        if (p.id == id) return p.kind;
    }
    return PadKind::Other;
}

void gamepad_set_info(const GamepadInfo& info) {
    std::lock_guard<std::mutex> lk(g_pads.mu);
    for (GamepadInfo& p : g_pads.pads) {
        if (p.id == info.id) {
            p = info;
            return;
        }
    }
    g_pads.pads.push_back(info);
}

void gamepad_remove_info(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_pads.mu);
    g_pads.pads.erase(std::remove_if(g_pads.pads.begin(), g_pads.pads.end(),
                                     [id](const GamepadInfo& p) { return p.id == id; }),
                      g_pads.pads.end());
}

}  // namespace nxrt::input
