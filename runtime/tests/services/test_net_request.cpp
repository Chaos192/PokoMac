// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// nifm IRequest evaluation (network.cpp RequestEval; nxnative services.md "Network requests stay on hold before they settle"): the first
// GetRequestState after Submit answers OnHold and fires the state-change events, the next answers settled; every Submit starts over.
#include "nxrt/kernel.h"
#include "svc_test.h"

using namespace svctest;

namespace {

Session request() {
    Session n = open("nifm:u");
    Session gs = n.object(n.call(5, Req().put<uint64_t>(0).with_pid()));   // CreateGeneralService(pid)
    return gs.object(gs.call(4, Req().put<uint32_t>(0)));                  // CreateRequest(requirement preset)
}

uint32_t state(const Session& r) { return r.call(0).get<uint32_t>(0); }   // GetRequestState

bool fired(uint32_t h) {   // WaitSynchronization(h, 0): signalled
    int32_t index = -1;
    return nxrt::kernel::wait_synchronization(&h, 1, 0, &index) == 0;
}

}  // namespace

SVC_TEST(net_request, on_hold_until_the_next_query) {
    Session r = request();
    CHECK_EQ(state(r), 1u);   // Free: nothing submitted
    Reply ev = r.call(2);     // GetSystemEventReadableHandles
    CHECK_EQ(ev.copy.size(), 2u);
    CHECK_SHAPE(r.call(4), 0);   // Submit
    CHECK_EQ(state(r), 2u);      // OnHold: still being evaluated
    if (ev.copy.size() == 2) CHECK(fired(ev.copy[0]));   // the state changes: a waiter wakes and asks again
    CHECK_EQ(state(r), 1u);   // settled: no connection, nothing accepted
    CHECK_EQ(state(r), 1u);

    CHECK_SHAPE(r.call(4), 0);   // a second submission is evaluated again
    CHECK_EQ(state(r), 2u);
    CHECK_EQ(state(r), 1u);

    CHECK_SHAPE(r.call(4), 0);   // GetResult settles it as well (the no-connection result)
    CHECK_EQ(r.call(1).result, nxrt::kernel::make_result(110, 300));
    CHECK_EQ(state(r), 1u);

    CHECK_SHAPE(r.call(4), 0);   // and Cancel
    CHECK_SHAPE(r.call(3), 0);
    CHECK_EQ(state(r), 1u);
}
