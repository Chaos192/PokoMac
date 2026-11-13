// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia, the core part of the game module (pkp_game.h): the data part plus the title facts. Needs the runtime library,
// not the graphics one.
#include "pkp_game.h"

#include "nxrt/game.h"
#include "nxrt/services.h"
#include "nxrt/tt.h"

namespace nxrt::tt::pkp {
const GameProbes& pkp_game_probes();      // pkp_tt_probe.cpp
}

namespace nxrt::games::pkp {

namespace {

// main.npdm of the 2.0.1 update; save sizes are placeholders until control.nacp is known (the boot log shows what the game asks for).
void title_defaults(services::TitleProfile& p) {
    p.program_id = data_info().program_id;
    p.name = data_info().title;
    p.display_version = data_info().version;
    p.supported_languages = data_info().languages;   // control.nacp SupportedLanguage (pkp_data.cpp)
    p.account_save_size = 0x4000000;
    p.account_save_journal = 0x4000000;
    p.device_save_size = 0x4000000;
    p.device_save_journal = 0x4000000;
    p.main_thread_stack = 0x100000;            // main.npdm
    p.main_thread_priority = 44;
    p.main_thread_core = 0;
    p.firmware = "20.5.0";                     // the 2.0.1 update is built with SDK 20.5.25
    p.user_nickname = "Player";
    p.user_uid_hi = 0x0000000000000001ull;
    p.user_uid_lo = 0x41434e4852656370ull;     // "PKPRecmp"
}

// Polling sleeps on the boot path (IDA, 2.0.1; [sched] poll_sleeps, nxrt/game.h). The save manager (the object behind 0x71005ac980 and
// its siblings, run on the loader job thread from 0x710112e2cc) queues each save-file request (SYDAT.BIN and the other save files) on the
// request queue of a worker thread and then waits for it with `while (!request->done) SleepThread(1 s)`, or sets its state word (+0x628)
// and waits `while (state < n) SleepThread(1 s)`. The request takes milliseconds, so every one of them cost a full second: 12 of them on
// a boot to the title screen with a save, ~12 s of the 45 s. The boot state machine at 0x7100230b00 (state 8) sleeps 100 ms and returns
// while what it waits for is not ready (it runs again on the next step), 11 times on that boot. The BCAT polls (500 ms) never run offline
// but are the same kind of loop. All of them only check more often with 1 ms.
constexpr game::PollSleep kPollSleeps[] = {
    {0x5aae44, 1000000}, {0x5abc00, 1000000}, {0x5ac350, 1000000}, {0x5ac9fc, 1000000}, {0x5aca6c, 1000000}, {0x5ad21c, 1000000},
    {0x231580, 1000000},
    {0x545328, 1000000}, {0x545408, 1000000}, {0x583384, 1000000},
};

}  // namespace

const game::Info& core_info() {
    static const game::Info info = [] {
        game::Info g = data_info();
        g.title_defaults = &title_defaults;
        g.probes = &tt::pkp::pkp_game_probes();
        g.poll_sleeps = kPollSleeps;
        g.poll_sleep_count = sizeof kPollSleeps / sizeof kPollSleeps[0];
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::pkp

void nxrt_register_game_pkp_core() { nxrt::game::set_current(&nxrt::games::pkp::core_info()); }
