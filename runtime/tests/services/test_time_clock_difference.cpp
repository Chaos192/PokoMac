// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// time:u's clock-snapshot arithmetic (IStaticService 500 and 501). Pokopia takes a snapshot when the name-entry keyboard opens and
// another when it closes, then asks CalculateStandardUserSystemClockDifferenceByUser (500); without it the stub's failure made the SDK
// svcBreak (the owner's crash at name entry, 11-14). 501 CalculateSpanBetween is the other half of the pair.
// ClockSnapshot (0xD0): user context at 0x00 (offset 0x00, source 0x10), network context at 0x20 (source 0x30), network time 0x48,
// steady time point 0x90, steady clock source 0x98, automatic correction 0xCC.
#include <cstdint>
#include <cstring>

#include "svc_test.h"

using namespace svctest;

namespace {

struct Snap {
    uint8_t b[0xD0];
};

Snap snapshot(const Session& t) {
    Snap s{};
    Reply r = t.call(400, Req().put<uint8_t>(0).buf(C(s.b, sizeof s.b)));   // GetClockSnapshot(type 0)
    CHECK_EQ(r.result, 0u);
    return s;
}

template <class T> void poke(Snap& s, size_t at, T v) { std::memcpy(s.b + at, &v, sizeof v); }

Reply diff(const Session& t, const Snap& a, const Snap& b, uint32_t cmd = 500) {
    return t.call(cmd, Req().buf(X(a.b, sizeof a.b)).buf(X(b.b, sizeof b.b)));
}

}  // namespace

// The game's own sequence: two snapshots of the same clocks, back to back. Answered (not the stub) with no difference.
SVC_TEST(time, name_entry_snapshots_have_no_user_difference) {
    reset();
    Session t = open("time:u");
    const Snap a = snapshot(t), b = snapshot(t);
    Reply r = diff(t, a, b);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.data.size(), 8u);
    CHECK_EQ(r.get<int64_t>(0), int64_t{0});
}

// The player moved the user clock an hour forward between the snapshots: 3600 s in nanoseconds; back by a day: minus 86400 s.
SVC_TEST(time, user_clock_moved_between_snapshots) {
    reset();
    Session t = open("time:u");
    const Snap a = snapshot(t);
    Snap b = a;
    int64_t off;
    std::memcpy(&off, a.b, 8);
    poke<int64_t>(b, 0x00, off + 3600);
    CHECK_EQ(diff(t, a, b).get<int64_t>(0), int64_t{3600} * 1000000000ll);
    poke<int64_t>(b, 0x00, off - 86400);
    CHECK_EQ(diff(t, a, b).get<int64_t>(0), int64_t{-86400} * 1000000000ll);
}

// Contexts from different clock sources cannot be compared: no difference. Automatic correction moving the clock (the network context
// on the steady clock's source) is not the player's doing either.
SVC_TEST(time, other_source_or_automatic_correction_is_no_difference) {
    reset();
    Session t = open("time:u");
    const Snap a = snapshot(t);
    Snap b = a;
    int64_t off;
    std::memcpy(&off, a.b, 8);
    poke<int64_t>(b, 0x00, off + 60);
    b.b[0x10] ^= 0xFF;                                   // another user clock source
    CHECK_EQ(diff(t, a, b).get<int64_t>(0), int64_t{0});
    Snap c = a, d = a;
    poke<int64_t>(d, 0x00, off + 60);
    c.b[0xCC] = d.b[0xCC] = 1;                           // automatic correction on in both
    std::memcpy(c.b + 0x30, c.b + 0x98, 16);             // the network context on the steady clock's source
    std::memcpy(d.b + 0x30, d.b + 0x98, 16);
    CHECK_EQ(diff(t, c, d).get<int64_t>(0), int64_t{0});
}

// A missing snapshot buffer is a well-formed failure (InvalidArgument-like 116-1), never the unimplemented stub.
SVC_TEST(time, missing_snapshot_buffer_is_a_real_error) {
    reset();
    Session t = open("time:u");
    const Snap a = snapshot(t);
    Reply r = t.call(500, Req().buf(X(a.b, sizeof a.b)));
    CHECK(well_formed_error(r));
    Reply s = t.call(501, Req().buf(X(a.b, sizeof a.b)));
    CHECK(well_formed_error(s));
}

// 501: the steady time between snapshots on one steady clock; on two clocks the network times' difference, or TimeNotFound without them.
SVC_TEST(time, span_between_snapshots) {
    reset();
    Session t = open("time:u");
    const Snap a = snapshot(t);
    Snap b = a;
    int64_t st;
    std::memcpy(&st, a.b + 0x90, 8);
    poke<int64_t>(b, 0x90, st + 42);
    Reply r = diff(t, a, b, 501);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int64_t>(0), int64_t{42} * 1000000000ll);
    Snap c = a, d = a;
    d.b[0x98] ^= 0xFF;                                   // another steady clock
    poke<int64_t>(c, 0x48, 1000);
    poke<int64_t>(d, 0x48, 1100);
    CHECK_EQ(diff(t, c, d, 501).get<int64_t>(0), int64_t{100} * 1000000000ll);
    poke<int64_t>(d, 0x48, 0);
    CHECK(well_formed_error(diff(t, c, d, 501)));
}
