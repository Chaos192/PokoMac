// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Stub services: every command is logged and answered with kernel::kStubService (see include/nxrt/ipc.h).
#include <cstdio>

#include "nxrt/config.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"

namespace nxrt::ipc {

namespace detail {
uint64_t note_unhandled(Service& s, uint32_t cmd);   // ipc.cpp: counts the command, returns the per-(service, cmd) count
}  // namespace detail

uint32_t StubService::handle(Request& req, Response&) {
    // Bring-up probe ([ipc] stub_result / NXRT_IPC_STUB_RESULT): the Result every stub command answers. The default is the recognisable
    // kStubService; 0 makes a stub "succeed" with an empty reply so the boot goes on past the first refused command and lists the next
    // ones (the guest then reads zeros for every output, so what follows is only a map of which services it asks for, in what order).
    static const uint32_t answer = static_cast<uint32_t>(config_get_int("ipc", "stub_result", kernel::kStubService));
    const uint64_t n = detail::note_unhandled(*this, req.cmd);
    // Logged at Warn once per (service, command); the end-of-run summary has the counts.
    if (n == 1) {
        char hex[2 * 32 + 1] = "";                              // the first 32 bytes of the raw arguments: what is needed to name the command
        for (size_t i = 0; i < req.in_size && i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", req.in[i]);
        NXLOG_W("ipc",
                "STUB SERVICE %s cmd %u (in %zu bytes [%s], %zu copy, %zu move handles, buffers a%zu b%zu x%zu c%zu w%zu) -> 0x%x",
                name().c_str(), req.cmd, req.in_size, hex, req.in_copy.size(), req.in_move.size(), req.a.size(), req.b.size(),
                req.x.size(), req.c.size(), req.w.size(), answer);
    }
    return answer;
}

}  // namespace nxrt::ipc
