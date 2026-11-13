// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Structure adapted from SuperMarioWonderRecomp runtime/src/ipc.cpp (MIT, Copyright (c) 2026 BelmanteGu); see THIRD_PARTY_NOTICES.md.
// The service manager "sm:": CMIF commands 0 RegisterClient, 1 GetServiceHandle, 2 RegisterService, 3 UnregisterService, 4 DetachClient.
// The same command ids serve as TIPC commands (message type - 16).
#include <memory>
#include <string>

#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"

namespace nxrt::ipc {

std::shared_ptr<Service> make_sm() {
    auto s = std::make_shared<Service>("sm:");
    s->add_cmd(0, "RegisterClient", [](Request& req, Response&) -> uint32_t {
        NXLOG_D("sm", "RegisterClient(pid %llx)", static_cast<unsigned long long>(req.pid));
        return 0;
    });
    s->add_cmd(1, "GetServiceHandle", [](Request& req, Response& rsp) -> uint32_t {
        // The name is 8 bytes, NUL padded: read up to the first NUL.
        char name[9] = {};
        for (size_t i = 0; i < 8; ++i) {
            name[i] = static_cast<char>(req.arg<uint8_t>(i));
            if (!name[i]) break;
        }
        const std::string n(name);
        if (n.empty()) return kernel::make_result(21, 6);   // sm InvalidServiceName (0xC15)
        NXLOG_I("sm", "sm: GetServiceHandle(%s)", n.c_str());
        rsp.push_object(make_service(n));                    // a move handle to the new session
        return 0;
    });
    s->add_cmd(2, "RegisterService", [](Request&, Response&) -> uint32_t {
        NXLOG_W("sm", "sm: RegisterService: guest-provided services are not supported");
        return kernel::kStubService;
    });
    s->add_cmd(3, "UnregisterService", [](Request&, Response&) -> uint32_t {
        NXLOG_W("sm", "sm: UnregisterService: guest-provided services are not supported");
        return kernel::kStubService;
    });
    s->add_cmd(4, "DetachClient", [](Request&, Response&) -> uint32_t { return 0; });
    return s;
}

}  // namespace nxrt::ipc
