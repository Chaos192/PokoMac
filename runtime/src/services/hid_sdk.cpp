// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The SDK's nn::hid input getters answered on the host ([hid] sdk_hooks = auto | on | off; auto: on for titles with no game module of
// their own). The lifted sdk reads controller state from the hid shared memory, whose layout moves between SDK versions (a Switch 2 SDK
// 22 title may read a newer layout than hid.cpp writes); answering the functions themselves needs no layout. The approach, and the rules
// for which controller shows on which npad (hid_sdk.h), are nxnative's (seedds/nxnative dm/host/hle_hid.c, MIT; no code of it is used):
//  - every history read fills every requested entry, newest first with consecutive sampling numbers (Super Mario Odyssey reads all 16
//    entries whatever count comes back; with only entry 0 written its sticks did nothing). The entries are the npad's real history,
//    recorded at each 5 ms sample (History below), not copies of the current state: a game that finds presses as differences between
//    entries, from several threads, saw none (Ocarina of Time);
//  - a press is latched until the game's next read of one entry, so a press shorter than the game's frame is not lost;
//  - six-axis sensors: two for handheld and a pair of Joy-Cons, one otherwise, every requested entry 5 ms apart (the sensor's 200 Hz);
//  - a vibration device is mounted when the npad's controller has that style;
//  - the controller applet answers at once with the connected players, player 1 selected; fewer than the minimum is a cancel (an OK with
//    fewer is an answer a console never gives: Mario Kart 8 Deluxe opened the applet again every few frames on it);
//  - touches only in handheld mode (the console's screen is out of reach when docked);
//  - the console's own six-axis sensor (SevenSixAxisSensor) reports no states and rest.
// The configuration requests (supported styles and ids, the style update event, vibration values, the hold type) still go to the hid
// service: hid.cpp keeps what the game set, and this file reads it. The sampler (hid.cpp, every 5 ms) hands each sample here, for the
// press latches and to signal an npad's style update event when what this file reports for it changes.
#include "hid_sdk.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "hid_shm.h"
#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/jc2_mouse.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"
#include "nxrt/services_api.h"

namespace nxrt::services {

// ---- the npad layout ---------------------------------------------------------------------------------------------------------------------
namespace hidsdk {

int index_of_id(uint32_t id) {
    if (id < 8) return static_cast<int>(id);
    if (id == 0x20) return kIndexHandheld;
    if (id == 0x10) return kIndexOther;
    return -1;
}

uint32_t id_of_index(int index) {
    return index >= 0 && index < 8 ? static_cast<uint32_t>(index) : (index == kIndexHandheld ? 0x20u : 0x10u);
}

namespace {

using nxrt::hid::NpadStyle;

// A plain Joy-Con style with its Joy-Con 2 bit beside it when the game supports that bit and the player holds Joy-Con 2s (or the game
// supports only the Joy-Con 2 form).
uint32_t with_two(uint32_t plain, uint32_t two, const Config& c) {
    return plain | (((c.styles & two) && (c.joy2 || !(c.styles & plain))) ? two : 0u);
}

uint32_t style_of(Kind k, int index, const Config& c) {
    switch (k) {
        case Kind::Pad: return index == kIndexHandheld ? kHandheld : kFullKey;
        case Kind::Dual: return with_two(kJoyDual, kJoyDual2, c);
        case Kind::Left: return with_two(kJoyLeft, kJoyLeft2, c);
        case Kind::Right: return with_two(kJoyRight, kJoyRight2, c);
        default: return 0;
    }
}

NpadView present(int index, int slot, NpadStyle src, const Config& c) {
    NpadView v;
    const uint32_t s = c.styles;
    const bool full_ok = (s & kFullKey) != 0, dual_ok = (s & (kJoyDual | kJoyDual2)) != 0;
    const bool left_ok = (s & (kJoyLeft | kJoyLeft2)) != 0, right_ok = (s & (kJoyRight | kJoyRight2)) != 0;
    if (index == kIndexHandheld) {
        v.kind = Kind::Pad;
    } else {
        switch (src) {
            case NpadStyle::JoyDual: v.kind = dual_ok ? Kind::Dual : full_ok ? Kind::Pad : Kind::None; break;
            case NpadStyle::JoyLeft: v.kind = left_ok ? Kind::Left : full_ok ? Kind::Pad : Kind::None; break;
            case NpadStyle::JoyRight: v.kind = right_ok ? Kind::Right : full_ok ? Kind::Pad : Kind::None; break;
            default:   // a controller of its own: full key; as a pair of Joy-Cons to a game that takes none (or to a game that takes Joy-Con 2s
                       // while the player uses them)
                if (c.joy2 && (s & kJoyDual2)) v.kind = Kind::Dual;
                else if (full_ok) v.kind = Kind::Pad;
                else if (dual_ok) v.kind = Kind::Dual;
                break;
        }
    }
    if (v.kind != Kind::None) {
        v.slot = slot;
        v.style = style_of(v.kind, index, c);
    }
    return v;
}

}  // namespace

Layout resolve(const nxrt::hid::InputSnapshot& snap, const Config& c) {
    Layout L;
    auto supported = [&](int i) { return ((c.index_mask >> i) & 1u) != 0; };
    L.player1 = (!c.docked && (c.styles & kHandheld) && supported(kIndexHandheld)) ? kIndexHandheld : 0;
    // Player 1 is slot 0; a source that only connects its handheld slot feeds player 1 from there.
    const bool from_handheld = !snap.npad[0].connected && snap.npad[nxrt::hid::kNpadSlotHandheld].connected;
    const int p1slot = from_handheld ? static_cast<int>(nxrt::hid::kNpadSlotHandheld) : 0;
    const NpadStyle p1style = from_handheld ? NpadStyle::FullKey : snap.npad[0].style;
    const bool one_player = (c.index_mask & 0xFEu) == 0;
    uint32_t taken = 1u << L.player1;
    for (int p = 1; p < 8; ++p) {
        if (!snap.npad[p].connected) continue;
        if (supported(p)) taken |= 1u << p;
        else if (one_player) L.merged |= 1u << p;
    }
    L.npad[L.player1] = present(L.player1, p1slot, p1style, c);
    for (int p = 1; p < 8; ++p)
        if (((taken >> p) & 1u) && p != L.player1) L.npad[p] = present(p, p, snap.npad[p].style, c);
    // Single assignment splits a pair of Joy-Cons (a controller of its own stays whole, as on the console).
    const bool singles_ok = (c.styles & (kJoyLeft | kJoyLeft2)) && (c.styles & (kJoyRight | kJoyRight2));
    for (int i = 0; i < 8; ++i) {
        NpadView& v = L.npad[i];
        if (v.kind != Kind::Dual || !singles_ok || !((c.single_mask >> i) & 1u)) continue;
        const int slot = v.slot;
        if (snap.npad[slot].style != NpadStyle::JoyDual) continue;
        const bool keep_right = c.single_keep[i] == 2;
        v.kind = keep_right ? Kind::Right : Kind::Left;
        v.style = style_of(v.kind, i, c);
        for (int j = 1; j < 8; ++j) {
            if (!supported(j) || ((taken >> j) & 1u)) continue;
            taken |= 1u << j;
            NpadView& o = L.npad[j];
            o.kind = keep_right ? Kind::Left : Kind::Right;
            o.slot = slot;
            o.style = style_of(o.kind, j, c);
            break;
        }
    }
    return L;
}

// ---- the controller applet's bench (hid_sdk.h hid_applet_open) ------------------------------------------------------------------------
void apply_bench(nxrt::hid::InputSnapshot& snap, uint32_t bench, bool one) {
    auto deeper = [](int32_t a, int32_t b) { return std::abs(a) >= std::abs(b) ? a : b; };
    for (uint32_t p = 1; p < nxrt::hid::kNpadPlayers; ++p) {
        nxrt::hid::NpadInput& m = snap.npad[p];
        if (!((bench >> p) & 1u) || !m.connected) continue;
        if (one) {
            for (uint32_t to : {0u, nxrt::hid::kNpadSlotHandheld}) {
                nxrt::hid::NpadInput& n = snap.npad[to];
                if (!n.connected) continue;
                n.buttons |= m.buttons;
                n.left.x = deeper(n.left.x, m.left.x);
                n.left.y = deeper(n.left.y, m.left.y);
                n.right.x = deeper(n.right.x, m.right.x);
                n.right.y = deeper(n.right.y, m.right.y);
            }
        }
        m = nxrt::hid::NpadInput{};   // not connected
    }
}

int players_shown(const Layout& L) {
    int n = 0;
    for (const NpadView& v : L.npad) n += v.kind != Kind::None;
    return n;
}

namespace {
// Is player p + 1 (input slot p) shown on an npad in this layout?
bool shown(const Layout& L, uint32_t p) {
    for (const NpadView& v : L.npad)
        if (v.kind != Kind::None && v.slot == static_cast<int>(p)) return true;
    return false;
}
Layout benched_layout(const nxrt::hid::InputSnapshot& raw, const Config& c, uint32_t bench) {
    nxrt::hid::InputSnapshot s = raw;
    apply_bench(s, bench, false);   // feeding player 1 changes no npad
    return resolve(s, c);
}
}  // namespace

uint32_t bench_release(const nxrt::hid::InputSnapshot& raw, const Config& c, uint32_t bench, int max) {
    int count = players_shown(benched_layout(raw, c, bench));
    for (uint32_t p = 1; p < nxrt::hid::kNpadPlayers && (max <= 0 || count < max); ++p) {
        if (!((bench >> p) & 1u)) continue;
        bench &= ~(1u << p);
        count = players_shown(benched_layout(raw, c, bench));
    }
    return bench;
}

uint32_t bench_take(const nxrt::hid::InputSnapshot& raw, const Config& c, uint32_t bench, int max) {
    if (max <= 0) return bench;
    Layout L = benched_layout(raw, c, bench);
    int count = players_shown(L);
    for (uint32_t p = nxrt::hid::kNpadPlayers - 1; p > 0 && count > max; --p) {
        if (!shown(L, p)) continue;
        bench |= 1u << p;
        L = benched_layout(raw, c, bench);
        count = players_shown(L);
    }
    // the lowest players stay: one benched before takes the place of a higher player shown now (one that connected since)
    for (uint32_t q = 1; q < nxrt::hid::kNpadPlayers; ++q) {
        if (!((bench >> q) & 1u) || !raw.npad[q].connected) continue;
        for (uint32_t p = nxrt::hid::kNpadPlayers - 1; p > q; --p) {
            if (!shown(L, p)) continue;
            bench = (bench | (1u << p)) & ~(1u << q);
            L = benched_layout(raw, c, bench);
            break;
        }
    }
    return bench;
}

}  // namespace hidsdk

namespace {

using hidsdk::Kind;
using hidsdk::Layout;
using hidsdk::NpadView;
using namespace hidshm;

constexpr uint64_t kSixPeriodNs = 5000000;                       // the six-axis sensor's 200 Hz
constexpr uint64_t kFreshNs = 100000000;                        // a stored sample older than this is not used (the sampler stopped)
constexpr int32_t kStickThreshold = 32767 / 2;                  // a stick past half its travel sets its direction bit
constexpr uint32_t kResultControllerSupportCanceled = 202u | (3101u << 9);   // hid 2202-3101 (nxnative, from the SDK's ShowControllerSupportCore)
constexpr uint32_t kAttrConnected = 1u << 0, kAttrWired = 1u << 1, kAttrLeft = 1u << 2, kAttrLeftWired = 1u << 3, kAttrRight = 1u << 4,
                   kAttrRightWired = 1u << 5;
// The buttons each Joy-Con of a pair has (NpadButton bits), its stick's direction bits and its rail buttons included.
constexpr uint64_t kLeftButtons = (1ull << 4) | (1ull << 6) | (1ull << 8) | (1ull << 11) | (0xFull << 12) | (0xFull << 16) | (3ull << 24);
constexpr uint64_t kRightButtons = 0xFull | (1ull << 5) | (1ull << 7) | (1ull << 9) | (1ull << 10) | (0xFull << 20) | (3ull << 26);
constexpr uint64_t kPadButtons = 0x00FFFFFFull;                 // a controller of its own or a pair has no rail buttons to press

struct Stored {
    std::mutex mu;
    hid::InputSnapshot snap;
    bool have = false;
    uint64_t at_ns = 0;
};
Stored& stored() {
    static Stored* s = new Stored();
    return *s;
}
std::atomic<uint64_t> g_prev[hid::kNpadSlots];
std::atomic<uint64_t> g_latch[hid::kNpadSlots];
std::atomic<uint32_t> g_single_mask{0};
std::atomic<uint8_t> g_single_keep[8];
std::atomic<uint32_t> g_last_style[hidsdk::kIndices];
std::atomic<int64_t> g_sampling{1 << 16};   // starts high, so the history entries below the newest keep positive numbers
std::atomic<bool> g_installed{false};
std::atomic<uint32_t> g_logged{0};          // first-use log lines already written (bit per kind)

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

hidsdk::Config config() {
    hidsdk::Config c;
    hid_supported_sets(&c.styles, &c.index_mask);
    c.docked = amctl::docked();
    c.single_mask = g_single_mask.load(std::memory_order_relaxed);
    for (int i = 0; i < 8; ++i) c.single_keep[i] = g_single_keep[i].load(std::memory_order_relaxed);
    c.joy2 = game::info().platform == game::Platform::Switch2 && hid::jc2_mouse_wants_pointer();
    return c;
}

// The input a read answers from: the sampler's newest sample (at most 5 ms old), or the source itself when the sampler does not run.
struct Read {
    hid::InputSnapshot snap;
    Layout L;
    Read() {
        bool got = false;
        {
            Stored& s = stored();
            std::lock_guard<std::mutex> lk(s.mu);
            if (s.have && now_ns() - s.at_ns < kFreshNs) {
                snap = s.snap;
                got = true;
            }
        }
        if (!got) hid_read_input(&snap);
        L = hidsdk::resolve(snap, config());
    }
    const NpadView* view(uint32_t id) const {
        const int i = hidsdk::index_of_id(id);
        return i < 0 ? nullptr : &L.npad[i];
    }
};

template <class T>
T* gp(uint64_t a) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(a));
}
void ret(::Ctx* c, uint64_t v) { c->x[0] = v; }
void log_once(uint32_t bit, const char* fmt, ...) {
    if (g_logged.fetch_or(bit) & bit) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    NXLOG_I("hid", "%s", buf);
}

int32_t pick(int32_t a, int32_t b) { return std::abs(a) >= std::abs(b) ? a : b; }   // the more deflected of two sources

uint64_t stick_dirs(const hid::StickValue& l, const hid::StickValue& r) {
    uint64_t b = 0;
    if (l.x < -kStickThreshold) b |= 1ull << 16;
    if (l.y > kStickThreshold) b |= 1ull << 17;
    if (l.x > kStickThreshold) b |= 1ull << 18;
    if (l.y < -kStickThreshold) b |= 1ull << 19;
    if (r.x < -kStickThreshold) b |= 1ull << 20;
    if (r.y > kStickThreshold) b |= 1ull << 21;
    if (r.x > kStickThreshold) b |= 1ull << 22;
    if (r.y < -kStickThreshold) b |= 1ull << 23;
    return b;
}

// One npad state (NpadFullKeyState, NpadHandheldState, NpadJoyDualState, NpadJoyLeftState, NpadJoyRightState share the 0x28 layout).
// Not connected in the style asked for: the sampling number only.
// The state of shared-memory entry `index` as the layout shows it (sampling number left 0); returns its style set (0: nothing connected,
// the state stays zero). take_latch: the presses latched since the last such read are added and cleared.
uint32_t compose(const Layout& L, const hid::InputSnapshot& snap, int index, bool take_latch, NpadCommonState* out) {
    std::memset(out, 0, sizeof *out);
    const NpadView* v = index >= 0 && index < hidsdk::kIndices ? &L.npad[index] : nullptr;
    if (!v || v->kind == Kind::None) return 0;
    uint32_t slots = 1u << v->slot;
    if (index == L.player1) slots |= L.merged;
    uint64_t b = 0;
    hid::StickValue l, r;
    for (uint32_t s = 0; s < hid::kNpadSlots; ++s) {
        if (!((slots >> s) & 1u)) continue;
        const hid::NpadInput& n = snap.npad[s];
        b |= n.buttons | (take_latch ? g_latch[s].exchange(0) : 0);
        l.x = pick(l.x, n.left.x);
        l.y = pick(l.y, n.left.y);
        r.x = pick(r.x, n.right.x);
        r.y = pick(r.y, n.right.y);
    }
    uint32_t attr = kAttrConnected;
    switch (v->kind) {
        case Kind::Left:
            r = {};
            b = (b | stick_dirs(l, r)) & kLeftButtons;
            attr |= kAttrLeft;
            break;
        case Kind::Right:
            l = {};
            b = (b | stick_dirs(l, r)) & kRightButtons;
            attr |= kAttrRight;
            break;
        default:
            b = (b | stick_dirs(l, r)) & kPadButtons;
            attr |= kAttrLeft | kAttrRight;
            if (index == hidsdk::kIndexHandheld) attr |= kAttrWired | kAttrLeftWired | kAttrRightWired;
            break;
    }
    out->buttons = b;
    out->lx = l.x;
    out->ly = l.y;
    out->rx = r.x;
    out->ry = r.y;
    out->attributes = attr;
    return v->style;
}

// One npad state read now (with the latched presses): the single-state getters and one-entry history reads.
void fill_npad(NpadCommonState* out, uint32_t id, uint32_t want, const Read& rd) {
    NpadCommonState st;
    const uint32_t style = compose(rd.L, rd.snap, hidsdk::index_of_id(id), true, &st);
    if (!(style & want)) std::memset(&st, 0, sizeof st);
    st.sampling = static_cast<uint64_t>(g_sampling.fetch_add(1) + 1);
    *out = st;
}

// Each npad's history as the console's lifo holds it: the sampler (every 5 ms) records every entry's state with the style it had and a
// sampling number of that npad's own, consecutive. A history read returns these, so the presses and releases between two reads are in it,
// whoever reads: Ocarina of Time reads npad 0's 16 entries from six threads, and with each read making a new number over copies of the
// current state, the entries older than a read's newest were never a different state, so no press edge was ever seen.
constexpr int kHistory = 17;
struct History {
    NpadCommonState e[kHistory];
    uint32_t style[kHistory];
    uint64_t n = 0;          // entries recorded so far (the newest at (n - 1) % kHistory)
    uint64_t next = 0;       // the next sampling number (0: not started)
};
struct Histories {
    std::mutex mu;
    History h[hidsdk::kIndices];
    uint64_t at_ns = 0;      // the last sample
};
Histories& histories() {
    static Histories* h = new Histories();
    return *h;
}

void record_history(const Layout& L, const hid::InputSnapshot& snap) {
    Histories& H = histories();
    std::lock_guard<std::mutex> lk(H.mu);
    for (int i = 0; i < hidsdk::kIndices; ++i) {
        History& h = H.h[i];
        if (!h.next) h.next = static_cast<uint64_t>(g_sampling.load() + 1);   // after any number a direct read handed out
        const uint64_t slot = h.n % kHistory;
        h.style[slot] = compose(L, snap, i, false, &h.e[slot]);
        h.e[slot].sampling = h.next++;
        ++h.n;
    }
    H.at_ns = now_ns();
}

// `n` entries of entry `index`'s history, newest first, in the style `want` (an entry of another style reads as nothing connected); entries
// older than the first recorded repeat it with lower numbers. False when there is no fresh history (the sampler is not running).
bool read_history(int index, uint32_t want, NpadCommonState* out, int32_t n) {
    Histories& H = histories();
    std::lock_guard<std::mutex> lk(H.mu);
    if (index < 0 || index >= hidsdk::kIndices || !H.at_ns || now_ns() - H.at_ns > kFreshNs) return false;
    const History& h = H.h[index];
    if (!h.n) return false;
    const uint64_t have = std::min<uint64_t>(h.n, kHistory);
    for (int32_t i = 0; i < n; ++i) {
        const uint64_t j = std::min<uint64_t>(static_cast<uint64_t>(i), have - 1);
        const uint64_t slot = (h.n - 1 - j) % kHistory;
        out[i] = h.e[slot];
        if (!(h.style[slot] & want)) {
            const uint64_t smp = out[i].sampling;
            std::memset(&out[i], 0, sizeof out[i]);
            out[i].sampling = smp;
        }
        out[i].sampling -= static_cast<uint64_t>(i) - j;   // a repeated oldest entry: one number lower each
    }
    return true;
}

// ---- npads -------------------------------------------------------------------------------------------------------------------------------
// [hid] sdk_log = N: the first N npad state reads, one line each (which npad and style, how many entries, what entry 0 holds). Diagnostics.
std::atomic<int64_t> g_sdk_log{-1};
void sdk_log(const char* what, uint32_t id, uint32_t want, int32_t n, const NpadCommonState& s) {
    int64_t left = g_sdk_log.load(std::memory_order_relaxed);
    if (left < 0) {
        left = config_get_int("hid", "sdk_log", 0);
        g_sdk_log.store(left);
    }
    if (left <= 0 || g_sdk_log.fetch_sub(1) <= 0) return;
    NXLOG_I("hid", "%s npad %u style 0x%x n %d: #%llu buttons 0x%llx attr 0x%x", what, id, want, n, static_cast<unsigned long long>(s.sampling),
            static_cast<unsigned long long>(s.buttons), s.attributes);
}

template <uint32_t Want>
void h_npad_states(::Ctx* c) {   // int GetNpadStates(Npad*State* out, int count, const NpadIdType& id)
    NpadCommonState* out = gp<NpadCommonState>(c->x[0]);
    const int32_t n = static_cast<int32_t>(c->x[1]);
    const uint32_t* id = gp<const uint32_t>(c->x[2]);
    if (!out || !id || n <= 0) return ret(c, 0);
    const Read rd;
    if (n == 1 || !read_history(hidsdk::index_of_id(*id), Want, out, n)) {   // one entry: the newest with the latched presses
        fill_npad(&out[0], *id, Want, rd);
        for (int32_t i = 1; i < n; ++i) {   // no sampler: the current state repeated (nxnative's form)
            out[i] = out[0];
            out[i].sampling = out[0].sampling - static_cast<uint64_t>(i);
        }
    }
    sdk_log("GetNpadStates", *id, Want, n, out[0]);
    log_once(1u << 0, "GetNpadStates answered on the host ([hid] sdk_hooks): npad %u style 0x%x -> style set 0x%x", *id, Want,
             rd.view(*id) ? rd.view(*id)->style : 0u);
    ret(c, static_cast<uint32_t>(n));
}

template <uint32_t Want>
void h_npad_state(::Ctx* c) {   // void GetNpadState(Npad*State* out, const NpadIdType& id)
    NpadCommonState* out = gp<NpadCommonState>(c->x[0]);
    const uint32_t* id = gp<const uint32_t>(c->x[1]);
    if (!out || !id) return;
    fill_npad(out, *id, Want, Read());
    sdk_log("GetNpadState", *id, Want, 1, *out);
}

void h_style_set(::Ctx* c) {   // NpadStyleSet GetNpadStyleSet(const NpadIdType& id)
    const uint32_t* id = gp<const uint32_t>(c->x[0]);
    if (!id) return ret(c, 0);
    const Read rd;
    const NpadView* v = rd.view(*id);
    ret(c, v ? v->style : 0u);
}

void joy_assign(uint32_t id, bool single, uint8_t keep) {
    if (id >= 8) return;
    g_single_keep[id].store(keep);
    const uint32_t was = single ? g_single_mask.fetch_or(1u << id) : g_single_mask.fetch_and(~(1u << id));
    if (((was >> id) & 1u) == (single ? 1u : 0u)) return;
    NXLOG_I("hid", "npad %u: %s Joy-Con assignment%s", id, single ? "single" : "dual", single ? (keep == 2 ? " (the right one stays)" : " (the left one stays)") : "");
    for (uint32_t i = 0; i < 8; ++i) hid_signal_style_event(i);
}

void h_joy_assignment(::Ctx* c) {   // NpadJoyAssignmentMode GetNpadJoyAssignment(const NpadIdType& id): 0 dual, 1 single
    const uint32_t* id = gp<const uint32_t>(c->x[0]);
    if (!id) return ret(c, 0);
    const Read rd;
    const NpadView* v = rd.view(*id);
    const bool single = (v && (v->kind == Kind::Left || v->kind == Kind::Right)) || (*id < 8 && ((g_single_mask.load() >> *id) & 1u));
    ret(c, single ? 1 : 0);
}
void h_joy_single(::Ctx* c) {   // SetNpadJoyAssignmentModeSingle(const NpadIdType& id)
    if (const uint32_t* id = gp<const uint32_t>(c->x[0])) joy_assign(*id, true, 1);
}
void h_joy_single_type(::Ctx* c) {   // SetNpadJoyAssignmentModeSingle(const NpadIdType& id, NpadJoyDeviceType type): 0 left, 1 right stays
    if (const uint32_t* id = gp<const uint32_t>(c->x[0])) joy_assign(*id, true, static_cast<uint32_t>(c->x[1]) == 1 ? 2 : 1);
}
void h_joy_single_out(::Ctx* c) {   // bool SetNpadJoyAssignmentModeSingle(NpadIdType* other, const NpadIdType& id, NpadJoyDeviceType type)
    uint32_t* other = gp<uint32_t>(c->x[0]);
    const uint32_t* id = gp<const uint32_t>(c->x[1]);
    if (!id) return ret(c, 0);
    joy_assign(*id, true, static_cast<uint32_t>(c->x[2]) == 1 ? 2 : 1);
    const Read rd;
    const NpadView* mine = rd.view(*id);
    int found = -1;
    if (mine && mine->kind != Kind::None)
        for (int j = 0; j < 8 && found < 0; ++j)
            if (static_cast<uint32_t>(j) != *id && rd.L.npad[j].slot == mine->slot && rd.L.npad[j].kind != Kind::None) found = j;
    if (other) *other = found >= 0 ? static_cast<uint32_t>(found) : *id;
    ret(c, found >= 0 ? 1 : 0);
}
void h_joy_dual(::Ctx* c) {   // SetNpadJoyAssignmentModeDual(const NpadIdType& id)
    if (const uint32_t* id = gp<const uint32_t>(c->x[0])) joy_assign(*id, false, 0);
}
void h_joy_merge(::Ctx* c) {   // Result MergeSingleJoyAsDualJoy(const NpadIdType& a, const NpadIdType& b)
    if (const uint32_t* a = gp<const uint32_t>(c->x[0])) joy_assign(*a, false, 0);
    if (const uint32_t* b = gp<const uint32_t>(c->x[1])) joy_assign(*b, false, 0);
    ret(c, 0);
}

void controller_color(const Read& rd, uint32_t id, uint32_t* main_sub) {   // NpadControllerColor: main (body), sub (buttons)
    const NpadView* v = rd.view(id);
    const hid::NpadInput& n = rd.snap.npad[v && v->slot >= 0 ? v->slot : 0];
    main_sub[0] = n.color_body;
    main_sub[1] = n.color_buttons;
}
void h_color(::Ctx* c) {   // Result GetNpadControllerColor(NpadControllerColor* out, const NpadIdType& id)
    uint32_t* out = gp<uint32_t>(c->x[0]);
    const uint32_t* id = gp<const uint32_t>(c->x[1]);
    if (out && id) controller_color(Read(), *id, out);
    ret(c, 0);
}
void h_color2(::Ctx* c) {   // Result GetNpadControllerColor(NpadControllerColor* left, NpadControllerColor* right, const NpadIdType& id)
    uint32_t* l = gp<uint32_t>(c->x[0]);
    uint32_t* r = gp<uint32_t>(c->x[1]);
    const uint32_t* id = gp<const uint32_t>(c->x[2]);
    if (id) {
        const Read rd;
        if (l) controller_color(rd, *id, l);
        if (r) controller_color(rd, *id, r);
    }
    ret(c, 0);
}

// ---- the controller applet ---------------------------------------------------------------------------------------------------------------
// ControllerSupportArg starts { int8 playerCountMin, playerCountMax, bool enableTakeOverConnection, enableLeftJustify, enablePermitJoyDual,
// enableSingleMode, ... }; ControllerSupportResultInfo is { int8 playerCount, pad[3], NpadIdType selectedId }.
uint32_t controller_support(uint8_t* info, const int8_t* arg) {
    // The applet takes the application's focus while it is open, and some games apply its answer only after the InFocus notification
    // that follows (am.cpp update_focus_locked; nxnative "Library applets take the application's focus", Mario Kart 8 Deluxe).
    amctl::applet_open(true);
    struct Close {
        ~Close() { amctl::applet_open(false); }
    } close;
    const int lo = arg ? arg[0] : 0, hi = arg ? arg[1] : 0;
    const bool single = arg && arg[5] != 0;
    // the players it left out before count again while the game takes more than it sees (hid_applet_open)
    if (arg) hid_applet_open(single ? 1 : hi);
    // below the minimum: the prompt stays up over the game until enough controllers are connected or the player closes it (ctrlui), as a
    // Switch's applet does; the caller's thread waits meanwhile, as it does in nn::hid::ShowControllerSupport
    if (!single) ctrlui::wait_for_players(lo, hi, [] { return hid_sdk_players(nullptr); });
    // the players past the maximum are left out, so the game does not see more than it asked for and open the applet again
    uint32_t sel = 0;
    int count = arg ? hid_applet_close(single ? 1 : hi, &sel) : hid_sdk_players(&sel);
    const bool canceled = !single && lo > count;   // single mode needs one controller, and player 1 is always there
    NXLOG_I("hid", "controller applet: %d to %d players wanted%s, %d connected: %s", lo, hi, single ? " (single mode)" : "", count,
            canceled ? "too few, closed as the player would (canceled)" : "player 1 selected");
    if (single && count > 1) count = 1;
    if (hi > 0 && count > hi) count = hi;
    if (info) {
        std::memset(info, 0, 8);
        info[0] = static_cast<uint8_t>(count);
        std::memcpy(info + 4, &sel, 4);
    }
    return canceled ? kResultControllerSupportCanceled : 0;
}
void h_ctrl_support(::Ctx* c) {   // Result ShowControllerSupport(ControllerSupportResultInfo* out, const ControllerSupportArg& arg)
    ret(c, controller_support(gp<uint8_t>(c->x[0]), gp<const int8_t>(c->x[1])));
}
void h_ctrl_support0(::Ctx* c) {   // Result ShowControllerSupport(const ControllerSupportArg& arg)
    ret(c, controller_support(nullptr, gp<const int8_t>(c->x[0])));
}

// ---- six-axis sensors and vibration devices ----------------------------------------------------------------------------------------------
// SixAxisSensorHandle and VibrationDeviceHandle: byte 0 the npad style index (full key 3, handheld 4, pair 5, left 6, right 7), byte 1 the
// npad id, byte 2 the device (left / right of two).
uint32_t style_index(uint32_t style) {
    using namespace hidsdk;
    if (style & kFullKey) return 3;
    if (style & kHandheld) return 4;
    if (style & (kJoyDual | kJoyDual2)) return 5;
    if (style & (kJoyLeft | kJoyLeft2)) return 6;
    if (style & (kJoyRight | kJoyRight2)) return 7;
    return 0;
}
uint32_t style_of_index(uint32_t i) { return i >= 3 && i <= 7 ? 1u << (i - 3) : 0u; }

// The npad a handle names, when its controller is connected in the handle's style.
const NpadView* handle_view(const Read& rd, uint32_t h) {
    const uint32_t want = style_of_index(h & 0xFF), dev = (h >> 16) & 0xFF;
    const NpadView* v = rd.view((h >> 8) & 0xFF);
    if (!want || !v || v->kind == Kind::None || !(v->style & want) || dev > 1) return nullptr;
    return v;
}

void h_six_handles(::Ctx* c) {   // int GetSixAxisSensorHandles(SixAxisSensorHandle* out, int count, const NpadIdType& id, NpadStyleSet style)
    uint32_t* out = gp<uint32_t>(c->x[0]);
    const int32_t n = static_cast<int32_t>(c->x[1]);
    const uint32_t* id = gp<const uint32_t>(c->x[2]);
    const uint32_t si = style_index(static_cast<uint32_t>(c->x[3]));
    if (!out || !id || n <= 0 || !si) return ret(c, 0);
    const int32_t k = std::min<int32_t>(si == 4 || si == 5 ? 2 : 1, n);
    for (int32_t i = 0; i < k; ++i) out[i] = si | ((*id & 0xFF) << 8) | (static_cast<uint32_t>(i) << 16);
    ret(c, static_cast<uint32_t>(k));
}

int six_states(SixAxisState* out, int n, uint32_t h) {
    if (!out || n <= 0) return 0;
    const Read rd;
    const NpadView* v = handle_view(rd, h);
    if (!v) {
        std::memset(out, 0, sizeof *out * static_cast<size_t>(n));
        return 0;
    }
    const hid::SixAxisSample& m = rd.snap.npad[v->slot].six_axis;
    const uint64_t now = now_ns() / kSixPeriodNs;
    for (int i = 0; i < n; ++i) {
        SixAxisState& s = out[i];
        std::memset(&s, 0, sizeof s);
        s.delta_time = kSixPeriodNs;
        s.sampling = now - static_cast<uint64_t>(i);
        std::memcpy(s.accel, m.accel, sizeof s.accel);
        std::memcpy(s.gyro, m.gyro, sizeof s.gyro);
        std::memcpy(s.angle, m.angle, sizeof s.angle);
        std::memcpy(s.direction, m.direction, sizeof s.direction);
        s.attributes = 1;   // connected
    }
    return n;
}
void h_six_states(::Ctx* c) {   // int GetSixAxisSensorStates(SixAxisSensorState* out, int count, const SixAxisSensorHandle& h)
    const uint32_t* h = gp<const uint32_t>(c->x[2]);
    ret(c, static_cast<uint32_t>(h ? six_states(gp<SixAxisState>(c->x[0]), static_cast<int32_t>(c->x[1]), *h) : 0));
}
void h_six_state(::Ctx* c) {   // void GetSixAxisSensorState(SixAxisSensorState* out, const SixAxisSensorHandle& h)
    if (const uint32_t* h = gp<const uint32_t>(c->x[1])) six_states(gp<SixAxisState>(c->x[0]), 1, *h);
}
void h_six_at_rest(::Ctx* c) {   // bool IsSixAxisSensorAtRest(const SixAxisSensorHandle& h)
    const uint32_t* h = gp<const uint32_t>(c->x[0]);
    if (!h) return ret(c, 1);
    const Read rd;
    const NpadView* v = handle_view(rd, *h);
    ret(c, !v || rd.snap.npad[v->slot].six_axis.at_rest ? 1 : 0);
}

void h_vib_mounted(::Ctx* c) {   // bool IsVibrationDeviceMounted(const VibrationDeviceHandle& h), and the Switch 2's IsOunceVibrationDeviceMounted
    const uint32_t* h = gp<const uint32_t>(c->x[0]);
    if (!h) return ret(c, 0);
    const Read rd;
    const NpadView* v = handle_view(rd, *h);
    const bool on = v && !(v->kind == Kind::Left && ((*h >> 16) & 0xFF) != 0);   // a left Joy-Con has one device, the first
    ret(c, on ? 1 : 0);
}

// The console's own six-axis sensor (SevenSixAxisSensor: the body's orientation in handheld mode; Breath of the Wild, SDK 20's form). Its
// samples are a lifo in the game's transfer memory (hid 306 InitializeSevenSixAxisSensor), which the service never fills, so the SDK's
// reader took whatever the buffer held as a lifo header. This console has no such sensor: no states, at rest (nxnative).
void h_seven_states(::Ctx* c) { ret(c, 0); }   // int GetSevenSixAxisSensorStates(SevenSixAxisSensorState* out, int count)
void h_seven_at_rest(::Ctx* c) { ret(c, 1); }  // bool IsSevenSixAxisSensorAtRest()

// ---- mouse, keyboard, touch screen -------------------------------------------------------------------------------------------------------
void mouse_state(MouseState* s, const hid::InputSnapshot& snap, uint64_t sampling) {
    std::memset(s, 0, sizeof *s);
    s->sampling = sampling;
    const hid::MouseInput& m = snap.mouse;
    s->x = m.x;
    s->y = m.y;
    s->dx = m.dx;
    s->dy = m.dy;
    s->wheel_dx = m.wheel_dx;
    s->wheel_dy = m.wheel_dy;
    s->buttons = m.buttons;
    s->attributes = m.connected ? 2u : 0u;   // bit 1 connected
}
void h_mouse_state(::Ctx* c) {   // void GetMouseState(MouseState* out)
    if (MouseState* s = gp<MouseState>(c->x[0])) mouse_state(s, Read().snap, static_cast<uint64_t>(g_sampling.fetch_add(1) + 1));
}
void h_mouse_states(::Ctx* c) {   // int GetMouseStates(MouseState* out, int count)
    MouseState* s = gp<MouseState>(c->x[0]);
    const int32_t n = static_cast<int32_t>(c->x[1]);
    if (!s || n <= 0) return ret(c, 0);
    const Read rd;
    const uint64_t top = static_cast<uint64_t>(g_sampling.fetch_add(1) + 1);
    for (int32_t i = 0; i < n; ++i) mouse_state(&s[i], rd.snap, top - static_cast<uint64_t>(i));
    ret(c, static_cast<uint32_t>(n));
}

void keyboard_state(KeyboardState* s, const hid::InputSnapshot& snap, uint64_t sampling) {
    std::memset(s, 0, sizeof *s);
    s->sampling = sampling;
    if (!snap.keyboard_connected) return;   // no keyboard: idle and not connected, as the shared memory ring (hid.cpp sample_keyboard)
    s->modifiers = snap.keyboard.modifiers;
    s->attributes = 1u;                     // KeyboardAttribute bit 0: connected
    for (int i = 0; i < 4; ++i) s->keys[i] = snap.keyboard.keys[i];
}
void h_keyboard_state(::Ctx* c) {   // void GetKeyboardState(KeyboardState* out)
    if (KeyboardState* s = gp<KeyboardState>(c->x[0])) keyboard_state(s, Read().snap, static_cast<uint64_t>(g_sampling.fetch_add(1) + 1));
}
void h_keyboard_states(::Ctx* c) {   // int GetKeyboardStates(KeyboardState* out, int count)
    KeyboardState* s = gp<KeyboardState>(c->x[0]);
    const int32_t n = static_cast<int32_t>(c->x[1]);
    if (!s || n <= 0) return ret(c, 0);
    const Read rd;
    const uint64_t top = static_cast<uint64_t>(g_sampling.fetch_add(1) + 1);
    for (int32_t i = 0; i < n; ++i) keyboard_state(&s[i], rd.snap, top - static_cast<uint64_t>(i));
    ret(c, static_cast<uint32_t>(n));
}

// TouchScreenState<N> = { int64 sampling number, int32 count, int32 reserved, TouchState[N] }: the touches in handheld mode only.
void touch_state(uint8_t* p, uint32_t cap) {
    const Read rd;
    const uint64_t sampling = static_cast<uint64_t>(g_sampling.fetch_add(1) + 1);
    std::memcpy(p, &sampling, 8);
    const uint32_t n = amctl::docked() ? 0 : std::min<uint32_t>(rd.snap.touch.count, cap);
    const int32_t count = static_cast<int32_t>(n), zero = 0;
    std::memcpy(p + 8, &count, 4);
    std::memcpy(p + 12, &zero, 4);
    for (uint32_t i = 0; i < cap; ++i) {
        TouchState t{};
        if (i < n) {
            const hid::TouchFinger& f = rd.snap.touch.fingers[i];
            t.finger = f.id;
            t.x = f.x;
            t.y = f.y;
            t.diameter_x = f.diameter_x;
            t.diameter_y = f.diameter_y;
            t.rotation_angle = static_cast<uint32_t>(f.angle);
        }
        std::memcpy(p + 16 + i * sizeof(TouchState), &t, sizeof t);
    }
}
template <uint32_t N>
void h_touch_state(::Ctx* c) {   // void GetTouchScreenState<N>(TouchScreenState<N>* out)
    if (uint8_t* p = gp<uint8_t>(c->x[0])) touch_state(p, N);
}
template <uint32_t N>
void h_touch_states(::Ctx* c) {   // int GetTouchScreenStates<N>(TouchScreenState<N>* out, int count): one entry
    uint8_t* p = gp<uint8_t>(c->x[0]);
    if (!p || static_cast<int32_t>(c->x[1]) <= 0) return ret(c, 0);
    touch_state(p, N);
    ret(c, 1);
}

// ---- the table -----------------------------------------------------------------------------------------------------------------------------
struct Hook {
    const char* name;
    HleFn fn;
};

#define TOUCH_HOOKS(N)                                                                                     \
    {"_ZN2nn3hid19GetTouchScreenStateILm" #N "EEEvPNS0_16TouchScreenStateIXT_EEE", h_touch_state<N>},     \
    {"_ZN2nn3hid20GetTouchScreenStatesILm" #N "EEEiPNS0_16TouchScreenStateIXT_EEEi", h_touch_states<N>},

const Hook kHooks[] = {
    {"_ZN2nn3hid15GetNpadStyleSetERKj", h_style_set},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", h_npad_states<hidsdk::kFullKey>},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", h_npad_states<hidsdk::kHandheld>},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", h_npad_states<hidsdk::kJoyDual>},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", h_npad_states<hidsdk::kJoyLeft>},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", h_npad_states<hidsdk::kJoyRight>},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", h_npad_state<hidsdk::kFullKey>},
    {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", h_npad_state<hidsdk::kHandheld>},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", h_npad_state<hidsdk::kJoyDual>},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", h_npad_state<hidsdk::kJoyLeft>},
    {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", h_npad_state<hidsdk::kJoyRight>},
    {"_ZN2nn3hid20GetNpadJoyAssignmentERKj", h_joy_assignment},
    {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", h_joy_single},
    {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE", h_joy_single_type},
    {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleEPjRKjNS0_17NpadJoyDeviceTypeE", h_joy_single_out},
    {"_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj", h_joy_dual},
    {"_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_", h_joy_merge},
    {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj", h_color},
    {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj", h_color2},
    {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", h_ctrl_support},
    {"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", h_ctrl_support0},
    {"_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", h_six_handles},
    {"_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", h_six_states},
    {"_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", h_six_state},
    {"_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", h_six_at_rest},
    {"_ZN2nn3hid24IsVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE", h_vib_mounted},
    {"_ZN2nn3hid29IsOunceVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE", h_vib_mounted},
    {"_ZN2nn3hid27GetSevenSixAxisSensorStatesEPNS0_23SevenSixAxisSensorStateEi", h_seven_states},
    {"_ZN2nn3hid26IsSevenSixAxisSensorAtRestEv", h_seven_at_rest},
    {"_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE", h_mouse_state},
    {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", h_mouse_states},
    {"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", h_keyboard_state},
    {"_ZN2nn3hid17GetKeyboardStatesEPNS0_13KeyboardStateEi", h_keyboard_states},
    TOUCH_HOOKS(1) TOUCH_HOOKS(2) TOUCH_HOOKS(3) TOUCH_HOOKS(4) TOUCH_HOOKS(5) TOUCH_HOOKS(6) TOUCH_HOOKS(7) TOUCH_HOOKS(8)
    TOUCH_HOOKS(9) TOUCH_HOOKS(10) TOUCH_HOOKS(11) TOUCH_HOOKS(12) TOUCH_HOOKS(13) TOUCH_HOOKS(14) TOUCH_HOOKS(15) TOUCH_HOOKS(16)
};
#undef TOUCH_HOOKS

}  // namespace

void hid_sdk_on_sample(const nxrt::hid::InputSnapshot& snap) {
    for (uint32_t s = 0; s < hid::kNpadSlots; ++s) {
        const uint64_t b = snap.npad[s].buttons;
        const uint64_t prev = g_prev[s].exchange(b);
        if (b & ~prev) g_latch[s].fetch_or(b & ~prev);
    }
    {
        Stored& st = stored();
        std::lock_guard<std::mutex> lk(st.mu);
        st.snap = snap;
        st.have = true;
        st.at_ns = now_ns();
    }
    if (!g_installed.load(std::memory_order_acquire)) return;
    const Layout L = hidsdk::resolve(snap, config());
    record_history(L, snap);
    for (int i = 0; i < hidsdk::kIndices; ++i)
        if (g_last_style[i].exchange(L.npad[i].style) != L.npad[i].style) hid_signal_style_event(static_cast<uint32_t>(i));
}

int hid_sdk_players(uint32_t* selected_id) {
    const Read rd;
    int n = 0;
    for (const NpadView& v : rd.L.npad) n += v.kind != Kind::None;
    if (selected_id) *selected_id = hidsdk::id_of_index(rd.L.player1);
    return n;
}

// ---- the controller applet's bench (hid_sdk.h) ----------------------------------------------------------------------------------------
namespace {
std::atomic<uint32_t> g_bench{0};          // bit p: player p + 1 left out by the controller applet
std::atomic<bool> g_bench_one{false};      // the applet was opened for one player: the benched players feed player 1

void bench_set(uint32_t now, int max, const char* when) {
    const uint32_t was = g_bench.exchange(now);
    if (was == now) return;
    for (uint32_t p = 1; p < hid::kNpadPlayers; ++p) {
        const bool in = (now >> p) & 1u, before = (was >> p) & 1u;
        if (in != before)
            NXLOG_I("hid", "controller applet (%s, at most %d players): player %u %s", when, max, p + 1,
                    in ? (g_bench_one.load() ? "is left out, its controller feeds player 1" : "is left out (more players than the game takes)")
                       : "counts again");
    }
    hid_sample_once();   // their npads connect or disconnect now: the style update events tell the game before the applet answers
}
}  // namespace

void hid_applet_open(int max) {
    if (!g_bench.load()) return;
    hid::InputSnapshot raw;
    hid_read_raw_input(&raw);
    bench_set(hidsdk::bench_release(raw, config(), g_bench.load(), max), max, "opened");
}

int hid_applet_close(int max, uint32_t* selected_id) {
    hid::InputSnapshot raw;
    hid_read_raw_input(&raw);
    const uint32_t now = hidsdk::bench_take(raw, config(), g_bench.load(), max);
    g_bench_one.store(max == 1);
    bench_set(now, max, "answers");
    return hid_sdk_players(selected_id);
}

void hid_bench_apply(hid::InputSnapshot& snap) {
    uint32_t b = g_bench.load(std::memory_order_relaxed);
    if (!b) return;
    uint32_t gone = 0;   // a controller that went away is no longer left out: the next one on its place is a player
    for (uint32_t p = 1; p < hid::kNpadPlayers; ++p)
        if (((b >> p) & 1u) && !snap.npad[p].connected) gone |= 1u << p;
    if (gone) {
        b = g_bench.fetch_and(~gone) & ~gone;
        for (uint32_t p = 1; p < hid::kNpadPlayers; ++p)
            if ((gone >> p) & 1u) NXLOG_I("hid", "controller applet: player %u, left out, has no controller any more: that place counts again", p + 1);
    }
    hidsdk::apply_bench(snap, b, g_bench_one.load(std::memory_order_relaxed));
}

void hid_bench_clear(const char* why) {
    if (g_bench.exchange(0)) NXLOG_I("hid", "controller applet: every player it left out counts again (%s)", why);
}

uint32_t hid_bench_mask() { return g_bench.load(); }

void hid_sdk_reset() {
    g_bench.store(0);
    g_bench_one.store(false);
    for (auto& a : g_prev) a.store(0);
    for (auto& a : g_latch) a.store(0);
    for (auto& a : g_single_keep) a.store(0);
    for (auto& a : g_last_style) a.store(0);
    g_single_mask.store(0);
    g_logged.store(0);
    {
        Histories& H = histories();
        std::lock_guard<std::mutex> lk(H.mu);
        for (History& h : H.h) h = History{};
        H.at_ns = 0;
    }
    Stored& st = stored();
    std::lock_guard<std::mutex> lk(st.mu);
    st.have = false;
}

}  // namespace nxrt::services

namespace nxrt::hid {

bool install_sdk_hooks() {
    std::string mode = config_get_str("hid", "sdk_hooks", "auto");
    for (char& ch : mode) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const char* id = game::info().id;
    const bool generic = !id || !*id || !std::strcmp(id, "generic");
    bool on = mode == "on" || (mode != "off" && generic);
    if (mode != "on" && mode != "off" && mode != "auto") NXLOG_W("hid", "[hid] sdk_hooks = %s: not auto, on or off; using auto", mode.c_str());
    if (!on) {
        NXLOG_I("hid", "[hid] sdk_hooks %s: the SDK reads the hid shared memory%s", mode.c_str(), generic ? "" : " (the title's game module uses it)");
        return false;
    }
    size_t defined = 0;
    for (const services::Hook& h : services::kHooks) {
        hle_register(h.name, h.fn);
        module_override_symbol(h.name);
        if (const uint64_t def = nxmodule_find_export(h.name)) {   // the sdk's own definition, for a call by address
            fn_register(def, h.fn);
            ++defined;
        }
    }
    services::g_installed.store(true, std::memory_order_release);
    NXLOG_I("hid", "[hid] sdk_hooks on: %zu nn::hid functions answered on the host (%zu defined by the title's sdk)",
            sizeof services::kHooks / sizeof services::kHooks[0], defined);
    return true;
}

}  // namespace nxrt::hid
