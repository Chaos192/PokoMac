// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The one host TCP connection the relay client needs, behind a tiny wrapper (Winsock today; the rest of net/ stays socket-API free).
// Blocking sockets with TCP_NODELAY and a send timeout; connect has its own timeout.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace nxrt::tcp {

using Handle = uintptr_t;
constexpr Handle kBad = ~Handle{0};

// Resolves host (name or IPv4) and connects within timeout_ms. kBad with a short English reason in *err on failure.
Handle dial(const std::string& host, uint16_t port, int timeout_ms, int send_timeout_ms, std::string* err);
bool send_all(Handle h, const void* p, size_t n);   // false: the connection failed (or the send timed out)
bool recv_all(Handle h, void* p, size_t n);         // false: closed or failed before n bytes arrived
void shutdown(Handle h);                            // wakes a thread blocked in recv_all on h; the handle stays open
void close(Handle h);
uint32_t peer_ip(Handle h);                         // the peer's IPv4 address in host order (1.2.3.4 = 0x01020304), 0 if unknown

}  // namespace nxrt::tcp
