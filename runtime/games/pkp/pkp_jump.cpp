// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Jump momentum: a running jump carries the running speed instead of the fixed air speed. [pkp] jump_momentum (on by default),
// [pkp] jump_momentum_strength (0..1, default 1).
//
// How the game moves the player in the air (main 2.0.1a, measured with a per-frame log and write traps in a scripted run):
//  * The character's movement controller is its movement component + 0xbc0. Each frame its +0xf0 (the air position) is set to the
//    character's position (player + 0x20c0, from 0x7100713d4c), the jump action code 0x71006daf30 then adds the frame's moves to it
//    through the controller's virtual +0x40 (0x710070bc44: +0xf0 += v[3], called from 0x71006db9d0, 0x71006dbb74 (the vertical part)
//    and 0x71006dbbb4), the air motion 0x7100707f78 copies +0xf0 into the controller's position (+0x20), and the movement update
//    0x7100714628 records the step (controller position - character position) through 0x71002778cc (x0 = the character, x1 = the
//    step, 4 floats, Y up; the call at 0x7100714aac) into the movement body ([character + 0x18a8] + 0x30) and moves the character.
//  * On the ground the step follows the walk or run animation (about 5.3-6.5 units a frame walking, 7.4-9.1 running at 60 fps); in a jump
//    the horizontal move is a fixed 5.83 units a frame whatever the speed before the take-off, so walking and running jumps cover the
//    same distance. The body's flags (+0xb8) have bit 3 set from the take-off frame until the landing.
//  * Writing the controller's position, its +0xf0 or the recorded step does not last: the next frame starts again from the character's
//    position plus the frame's moves, so the moves are the place to change the speed.
//
// Two entry hooks (both need their sites in recomp/games/pkp/main.sites):
//  * 0x71002778cc, called from 0x7100714aac, for the local player ([main + 0x3561928] + 0x30) only: remembers its movement component,
//    averages the horizontal step of its last ground frames, takes that average at the take-off, and in the air works out the factor
//    that brings the game's own air step up to it (never down, at most x2, scaled by the strength). It changes nothing itself.
//  * 0x710070bc44, for that controller only, in the air: multiplies the move's x and z by the factor (the vertical move is untouched).
//    Direction and air steering stay the game's; standing and walking jumps (ground speed at or under the air speed) are unchanged.
//    Other characters and Link Play visitors (moved by the network) keep the game's motion.
// The factor is applied from the second air frame on (the first one measures the game's air step); it does not depend on the frame rate.
#include "pkp_jump.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kSetStep = 0x2778cc;          // (character, const float step[4]): [character + 0x18a8] + 0x30 = step
constexpr uint64_t kStepCall = 0x714aac;         // return address of the movement update's call (x19 = the movement component)
constexpr uint64_t kAirAdd = 0x70bc44;           // controller virtual +0x40 (controller, const float v[3]): controller + 0xf0 += v
constexpr uint64_t kPlayerManager = 0x3561928;   // qword: the player manager; +0x30 = the local player
constexpr uint64_t kManagerPlayer = 0x30, kBody = 0x18a8, kBodyFlags = 0xb8, kController = 0xbc0;
constexpr uint64_t kAirborne = 0x8;              // body flags: set from the take-off frame to the landing
constexpr int kHistory = 8;                      // ground frames averaged for the take-off speed
constexpr float kMaxScale = 2.0f;

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }

struct State {
    uint64_t base = 0;
    float strength = 1.0f;
    uint64_t player = 0, component = 0;
    float ground[kHistory] = {};
    int n = 0, next = 0;
    bool air = false;
    float carried = 0;      // the ground speed at the take-off (units a frame)
    float k = 1.0f;         // factor for the next air frame
    float applied = 1.0f;   // factor the vector hook used this frame
    uint64_t jumps = 0;
};
State g;

int hook_set_step(::Ctx* c, uint64_t, void*) {
    if (c->x[30] != g.base + kStepCall) return 0;
    const uint64_t ch = c->x[0], src = c->x[1];
    const uint64_t mgr = rd<uint64_t>(g.base + kPlayerManager);
    if (!mgr || ch != rd<uint64_t>(mgr + kManagerPlayer) || !mem::is_mapped(src, 16)) return 0;
    const uint64_t body = rd<uint64_t>(ch + kBody);
    if (!body || !mem::is_mapped(body + kBodyFlags, 8)) return 0;
    if (ch != g.player) { g.player = ch; g.n = g.next = 0; g.air = false; }
    g.component = c->x[19];

    const float applied = g.applied;
    g.applied = 1.0f;
    float step[4];
    std::memcpy(step, reinterpret_cast<const void*>(src), sizeof step);
    const float h = std::hypot(step[0], step[2]);
    if (!std::isfinite(h)) return 0;
    if (!(rd<uint64_t>(body + kBodyFlags) & kAirborne)) {
        g.air = false;
        g.k = 1.0f;
        g.ground[g.next] = h;
        g.next = (g.next + 1) % kHistory;
        g.n = std::min(g.n + 1, kHistory);
        return 0;
    }
    if (!g.air) {   // take-off frame: its step is part ground, part air; measure from the next one
        g.air = true;
        float s = 0;
        for (int i = 0; i < g.n; i++) s += g.ground[i];
        g.carried = g.n ? s / static_cast<float>(g.n) : 0.0f;
        g.n = g.next = 0;
        g.k = 1.0f;
        return 0;
    }
    const float raw = h / applied;   // the game's own air step this frame
    g.k = (raw > 0.05f && g.carried > raw) ? std::min(1.0f + (g.carried / raw - 1.0f) * g.strength, kMaxScale) : 1.0f;
    if (g.jumps < 12 && applied == 1.0f && g.k > 1.0f) {
        g.jumps++;
        NXLOG_I(kTag, "jump momentum: take-off at %.2f a frame, air step %.2f: x%.2f", static_cast<double>(g.carried), static_cast<double>(raw),
                static_cast<double>(g.k));
    }
    return 0;
}

int hook_air_add(::Ctx* c, uint64_t, void*) {
    if (!g.air || g.k <= 1.0f || c->x[0] != g.component + kController) return 0;
    const uint64_t v = c->x[1];
    if (!mem::is_mapped(v, 12)) return 0;
    const float x = rd<float>(v) * g.k, z = rd<float>(v + 8) * g.k;
    std::memcpy(reinterpret_cast<void*>(v), &x, sizeof x);
    std::memcpy(reinterpret_cast<void*>(v + 8), &z, sizeof z);
    g.applied = g.k;
    return 0;
}

}  // namespace

void jump_install(uint64_t base) {
    if (!config_get_bool("pkp", "jump_momentum", true)) return;
    g.base = base;
    g.strength = static_cast<float>(std::clamp(config_get_double("pkp", "jump_momentum_strength", 1.0), 0.0, 1.0));
    hook_add_entry(base + kSetStep, &hook_set_step);
    hook_add_entry(base + kAirAdd, &hook_air_add);
    NXLOG_I(kTag, "jump momentum: on, strength %.2f (hooks main+0x%llx, main+0x%llx)", static_cast<double>(g.strength),
            static_cast<unsigned long long>(kSetStep), static_cast<unsigned long long>(kAirAdd));
}

}  // namespace nxrt::games::pkp
