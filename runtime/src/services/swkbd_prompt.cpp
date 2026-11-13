// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The software keyboard's question to the player (am.cpp, swkbdui in services_api.h). The applet's host thread posts the request and waits;
// the overlay (gfx/overlay.cpp) draws an on-screen keyboard inside the game window and answers. No second OS window: the old native prompt
// lost its keys under Wine/Proton (issue #29), and a pop-up does not get focus in gamescope at all.
#include <condition_variable>
#include <mutex>

#include "nxrt/services_api.h"

namespace nxrt::services::swkbdui {

namespace {
struct Question {
    std::mutex mu;
    std::condition_variable cv;
    uint64_t id = 0;          // the open question, 0 = none
    uint64_t next = 1;
    Request req;
    bool ok = false;
    std::string text;
};
Question& q() {
    static Question s;
    return s;
}
}  // namespace

bool ask(const Request& r, std::string* text) {
    Question& s = q();
    std::unique_lock<std::mutex> lk(s.mu);
    s.cv.wait(lk, [&] { return s.id == 0; });   // one question at a time
    const uint64_t id = s.next++;
    s.id = id;
    s.req = r;
    s.cv.wait(lk, [&] { return s.id != id; });
    if (s.ok) *text = s.text;
    return s.ok;
}

uint64_t pending(Request* r) {
    Question& s = q();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.id && r) *r = s.req;
    return s.id;
}

void answer(uint64_t id, bool ok, const std::string& text) {
    Question& s = q();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.id != id || id == 0) return;
        s.ok = ok;
        s.text = text;
        s.id = 0;
    }
    s.cv.notify_all();
}

}  // namespace nxrt::services::swkbdui
