// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia's test-tooling probes (nxrt/tt.h GameProbes, registered in pkp_core.cpp): the player's position, heading and action state,
// the stage id, the play time and the in-game time of day, read from guest memory at each frame boundary. No hooks: the frame source is
// `tick` (.weaver/play.sh). The script steps that need game functions (@warp @give @call ...) are not provided.
//
// Where the values live (main-relative offsets of the 2.0.1 update):
//   player manager  [main + 0x3561928] (26344 bytes, made in 0x71000ae380); +0x30 = the local player (+0x38 the second one). The follow
//                   camera (0x7100087840) and its reset (0x7100086000) read it the same way.
//   player          vtable slot 2 (0x71002e40d8) returns player + 0x20c0, the world position (3 floats, Y up); slot 3 (0x71002e4478) returns
//                   the heading at player + 0x2404 (radians); the local player's own overrides are below (kPlayerMode). [player + 0x18b8] + 0x984 is the u16 action state (0x71002779c8; the camera
//                   tests it against 136).
//   save data       [main + 0x3561930]: +0x13d373c = the time of day in seconds (float), +0x13d3740 year (u16), +0x13d3742 month,
//                   +0x13d3743 day (bytes); the play report (0x71001145fc) reads them so. +0x4a5418 = the play time in seconds (double,
//                   0x71011358e0 "PlayHour"). +0xc91f20 = m_stageId (byte, the save serialiser 0x71005d2c40). +0x6d7cec = m_tutorialHistory
//                   (1024 bytes, serialiser 0x71005ca320): `prog` counts its set bits.
#include <bit>
#include <cmath>
#include <cstring>

#include "nxrt/log.h"
#include "tt_internal.h"

namespace nxrt::tt::pkp {
namespace {

using nxrt::tt::detail::game_base;
using nxrt::tt::detail::read_mem;

constexpr uint64_t kPlayerManager = 0x3561928;
constexpr uint64_t kManagerPlayer = 0x30;
constexpr uint64_t kPlayerPos = 0x20c0;
constexpr uint64_t kPlayerYaw = 0x2404;
// The local player's class (vtable main+0x322fda8) overrides both getters (0x71002e4084, 0x71002e4420): while its mode word at +0x5a34 is 0 and
// the byte at +0x5a39 is 0 (vtable slots 504/512), the position and heading come from the save data instead (+0x7419f0, +0x741a10).
constexpr uint64_t kPlayerMode = 0x5a34;
constexpr uint64_t kPlayerModeByte = 0x5a39;
constexpr uint64_t kSaveAltPos = 0x7419f0;
constexpr uint64_t kSaveAltYaw = 0x741a10;
constexpr uint64_t kPlayerAction = 0x18b8;
constexpr uint64_t kActionState = 0x984;
constexpr uint64_t kSaveData = 0x3561930;
constexpr uint64_t kSaveTimeOfDay = 0x13d373c;
constexpr uint64_t kSavePlayTime = 0x4a5418;
constexpr uint64_t kSaveStageId = 0xc91f20;
constexpr uint64_t kSaveTutorials = 0x6d7cec;   // m_tutorialHistory, 1024 bytes (serialiser 0x71005ca320); its set bits are `prog`

template <class T>
bool rd(uint64_t a, T* v) { return read_mem(a, v, sizeof(T)); }
uint64_t rd64(uint64_t a) { uint64_t v = 0; rd(a, &v); return v; }

bool read_state(GameState* out) {
    GameState g;
    const uint64_t base = game_base();
    const uint64_t mgr = rd64(base + kPlayerManager);
    const uint64_t player = mgr ? rd64(mgr + kManagerPlayer) : 0;
    const uint64_t save = rd64(base + kSaveData);
    if (player) {
        float p[3], yaw = 0;
        uint32_t mode = 1;
        uint8_t mode_byte = 1;
        const bool alt = save && rd(player + kPlayerMode, &mode) && rd(player + kPlayerModeByte, &mode_byte) && mode == 0 && mode_byte == 0;
        const uint64_t pos_at = alt ? save + kSaveAltPos : player + kPlayerPos;
        const uint64_t yaw_at = alt ? save + kSaveAltYaw : player + kPlayerYaw;
        if (rd(pos_at, &p) && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
            std::memcpy(g.pos, p, sizeof p);
            if (rd(yaw_at, &yaw) && std::isfinite(yaw)) g.yaw = yaw;
            const uint64_t act = rd64(player + kPlayerAction);
            uint16_t st = 0;
            if (act && rd(act + kActionState, &st)) g.move_state = st;
            g.have_player = true;
        }
    }
    if (save) {
        float tod = 0;
        if (rd(save + kSaveTimeOfDay, &tod) && std::isfinite(tod) && tod >= 0 && tod < 86400.0f * 2) { g.tod = tod / 3600.0f; g.have_tod = true; }
        double play = 0;
        if (rd(save + kSavePlayTime, &play) && std::isfinite(play) && play >= 0) { g.igt = play; g.have_igt = true; }
        uint8_t stage = 0;
        if (rd(save + kSaveStageId, &stage)) { g.area[0] = stage; g.have_world = true; }
        uint64_t bits[128];
        if (rd(save + kSaveTutorials, &bits)) {
            int n = 0;
            for (uint64_t w : bits) n += std::popcount(w);
            g.prog = n;
            g.have_prog = true;
        }
    }
    static bool logged = false;
    if (!logged && player) {      // once: which class the player is (its vtable, main-relative) and where slot 2 points, to check the layout above
        logged = true;
        const uint64_t vt = rd64(player);
        NXLOG_I("tt", "pkp probe: player %llx vtable main+0x%llx slot2 main+0x%llx", static_cast<unsigned long long>(player),
                static_cast<unsigned long long>(vt - base), static_cast<unsigned long long>(rd64(vt + 16) - base));
    }
    *out = g;
    return g.have_player || g.have_tod || g.have_world;
}

}  // namespace

const GameProbes& pkp_game_probes() {
    static const GameProbes p = [] {
        GameProbes t;
        t.read_state = &read_state;
        return t;
    }();
    return p;
}

}  // namespace nxrt::tt::pkp
