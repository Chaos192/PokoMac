// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The one UDP socket the relay client's datagram path needs (relay protocol v3), behind a tiny wrapper like tcp.h (Winsock today).
// A connected socket: it talks to the relay's address only and the OS drops packets from anyone else.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace nxrt::udp {

using Handle = uintptr_t;
constexpr Handle kBad = ~Handle{0};

// A UDP socket connected to ip:port (ip in host order, 1.2.3.4 = 0x01020304); recv waits at most recv_timeout_ms.
// kBad with a short English reason in *err on failure.
Handle open(uint32_t ip, uint16_t port, int recv_timeout_ms, std::string* err);
bool send(Handle h, const void* p, size_t n);   // false: not sent (the datagram is lost, as UDP may lose it anyway)
// One datagram into p (cap bytes): its size, 0 when none came in time (or an ICMP error from an earlier send), -1 when the
// socket failed. A datagram longer than cap is cut and reported as cap + 1.
int recv(Handle h, void* p, size_t cap);
void close(Handle h);

}  // namespace nxrt::udp
