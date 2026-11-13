// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Focus while a library applet is open, and the focus handling mode (am.cpp update_focus_locked; nxnative services.md "Library applets
// take the application's focus", "Focus handling mode decides whether focus changes are notified").
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "svc_test.h"

using namespace svctest;

namespace {

struct Am {
    Session proxy, common, self;
};

Am open_am() {
    Am a;
    Session oe = open("appletOE");
    nxrt::services::ServiceEvent proc;
    Req r;
    r.put<uint64_t>(0).with_pid();
    r.copy.push_back(proc.new_handle());
    a.proxy = oe.object(oe.call(0, r));             // OpenApplicationProxy
    a.common = a.proxy.object(a.proxy.call(0));     // GetCommonStateGetter
    a.self = a.proxy.object(a.proxy.call(1));       // GetSelfController
    return a;
}

int drain(const Session& common, uint32_t want) {   // ReceiveMessage until the queue is empty; how many were `want`
    int n = 0;
    for (int i = 0; i < 64; ++i) {
        Reply m = common.call(1);   // ReceiveMessage
        if (m.result != 0) break;
        n += m.get<uint32_t>(0) == want;
    }
    return n;
}

uint8_t focus(const Session& common) { return common.call(9).get<uint8_t>(0); }   // GetCurrentFocusState: 1 in focus, 2 out

}  // namespace

SVC_TEST(applet_focus, library_applet_takes_focus) {
    reset();
    Am a = open_am();
    CHECK(a.common.valid() && a.self.valid());
    drain(a.common, 15);   // the start's FocusStateChanged
    CHECK_EQ(focus(a.common), 1);
    nxrt::services::amctl::applet_open(true);
    CHECK_EQ(focus(a.common), 2);
    nxrt::services::amctl::applet_open(false);
    CHECK_EQ(focus(a.common), 1);
    CHECK_EQ(drain(a.common, 15), 2);   // out of focus, then in focus again
}

SVC_TEST(applet_focus, handling_mode_without_notifications) {
    reset();
    Am a = open_am();
    drain(a.common, 15);
    CHECK_SHAPE(a.self.call(13, Req().put<uint8_t>(0).put<uint8_t>(0).put<uint8_t>(1)), 0);   // mode 0 / 3: no notification
    nxrt::services::amctl::applet_open(true);
    nxrt::services::amctl::applet_open(false);
    CHECK_EQ(drain(a.common, 15), 0);
    CHECK_SHAPE(a.self.call(13, Req().put<uint8_t>(1).put<uint8_t>(0).put<uint8_t>(1)), 0);   // mode 2: notified
    nxrt::services::amctl::set_focus(false);
    CHECK_EQ(drain(a.common, 15), 1);
    nxrt::services::amctl::set_focus(true);
    CHECK_EQ(drain(a.common, 15), 1);
}
