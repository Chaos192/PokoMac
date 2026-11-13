// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// The Switch 2 vibration commands of IHidServer (services/hid.cpp): 213, which Star Fox's SDK sends as its vibration starts and aborts on
// when it fails (the stub service result ended its boot 3.5 s in), answers success with nothing out, as Lichii does.
#include <cstdint>

#include "svc_test.h"

using namespace svctest;

SVC_TEST(hid_vibration, switch2_cmd_213) {
    Session hid = open("hid");
    CHECK(hid.valid());
    Req r;
    for (int i = 0; i < 9; ++i) r.put<uint32_t>(0);   // 36 bytes in, as the SDK sends it
    const Reply rp = hid.call(213, r);
    CHECK_SHAPE(rp, 0);
}
