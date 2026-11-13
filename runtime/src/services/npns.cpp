// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// npns:u and npns:s (push notifications) on a console that is never online. Facts: Lichii's service/npns (command ids; GetReceiveEvent and
// ListenToByName answered) and nxnative's notes (docs/reference/services.md "Game-linked HIPC clients of system services": Animal Crossing
// links its own npns client, which connects through sm). On Horizon the service is always there, so a game's connect succeeds; here
// nothing is ever received: the listen requests are accepted, the receive and state-change events never fire, and the commands that need
// the network (tokens, Receive) are not answered (the service's "not implemented" result, logged), so the game's request fails.
#include <memory>
#include <mutex>

#include "nxrt/log.h"
#include "nxrt/services.h"
#include "services_internal.h"

namespace nxrt::services {
namespace {

using ipc::Request;
using ipc::Response;
using ipc::Service;

struct NpnsState {
    std::mutex mu;
    std::shared_ptr<ServiceEvent> receive, state_change;
};
NpnsState& npns() {
    static NpnsState s;
    return s;
}
uint32_t event_handle(std::shared_ptr<ServiceEvent> NpnsState::*which) {
    NpnsState& s = npns();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!(s.*which)) s.*which = std::make_shared<ServiceEvent>(false);
    return (s.*which)->new_handle();
}

void setup_npns(Service& s) {
    auto listen = [](const char* what) {
        return [what](Request&, Response&) -> uint32_t {
            NXLOG_D("npns", "%s: accepted (nothing is ever received: the console is offline)", what);
            return 0;
        };
    };
    s.add_cmd(1, "ListenAll", listen("ListenAll"));
    s.add_cmd(2, "ListenTo", listen("ListenTo"));
    s.add_cmd(5, "GetReceiveEvent", [](Request&, Response& rsp) -> uint32_t {
        rsp.out_copy.push_back(event_handle(&NpnsState::receive));
        return 0;
    });
    s.add_cmd(7, "GetStateChangeEvent", [](Request&, Response& rsp) -> uint32_t {
        rsp.out_copy.push_back(event_handle(&NpnsState::state_change));
        return 0;
    });
    s.add_cmd(8, "ListenToByName", listen("ListenToByName"));
    s.add_cmd(26, "ListenToMyApplicationId", listen("ListenToMyApplicationId"));
}

}  // namespace

void register_npns() {
    add("npns:u", [] { return make_iface("npns:u", setup_npns); });
    add("npns:s", [] { return make_iface("npns:s", setup_npns); });
}

void npns_reset() {
    NpnsState& s = npns();
    std::lock_guard<std::mutex> lk(s.mu);
    s.receive.reset();
    s.state_change.reset();
}

}  // namespace nxrt::services
