// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// IPC SVCs: ConnectToNamedPort (0x1F), SendSyncRequest (0x21), SendSyncRequestWithUserBuffer (0x22). Calling convention: svc.h.
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/svc.h"

namespace nxrt {
namespace {

constexpr uint64_t kTlsMessageBytes = 0x100;   // the message buffer at TPIDRRO_EL0 is 0x100 bytes

// ConnectToNamedPort(const char* name) -> (result, handle in x1). The name is read up to 12 bytes.
void svc_connect_to_named_port(::Ctx* c) {
    uint32_t h = 0;
    const uint32_t rc = ipc::connect_named_port(reinterpret_cast<const char*>(static_cast<uintptr_t>(c->x[1])), &h);
    c->x[1] = h;
    svc_ret(c, rc);
}

// SendSyncRequest(Handle session) with the request in the calling thread's TLS message buffer (TPIDRRO_EL0).
void svc_send_sync_request(::Ctx* c) {
    const uint32_t session = static_cast<uint32_t>(c->x[0]);
    svc_ret(c, ipc::send_sync_request(c, session, c->tpidrro, kTlsMessageBytes));
}

// SendSyncRequestWithUserBuffer(void* buffer, size_t size, Handle session).
void svc_send_sync_request_with_user_buffer(::Ctx* c) {
    const uint32_t session = static_cast<uint32_t>(c->x[2]);
    svc_ret(c, ipc::send_sync_request(c, session, c->x[0], c->x[1]));
}

}  // namespace

void svc_register_ipc_group() {
    svc_register(0x1F, svc_connect_to_named_port);
    svc_register(0x21, svc_send_sync_request);
    svc_register(0x22, svc_send_sync_request_with_user_buffer);
}

}  // namespace nxrt
