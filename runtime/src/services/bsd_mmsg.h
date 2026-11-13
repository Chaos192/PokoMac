// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// bsd:u 29 RecvMMsg and 30 SendMMsg over one descriptor's receive / send / wait (the offline socket table, net/vsock.h, through
// offline(); a redirected host socket, net/redirect.h, through services/net_redirect.cpp). After Lichii's
// src/core/hle/service/sockets/bsd.cpp (GPL; facts only, our own code): ReadMessages / MessageParser (:127-200), RecvMMsg / SendMMsg
// (:1174-1235), RecvMMsgImpl / SendMMsgImpl (:2091-2470).
//   RecvMMsg raw arguments {s32 fd, u32 vlen, u32 flags, u32 reserved, s64 timeout_sec, s64 timeout_nsec}; SendMMsg {s32 fd, u32 vlen,
//   u32 flags}. Both take one B buffer, read and written back: a header byte the system ignores, then vlen messages, each a u32 name size
//   and the name (a sockaddr_in, 16 bytes; up to 0x100 accepted), a u32 iov count and per iov a u64 size and its data, a u32 control size
//   and the control data, a u32 msg_flags and a u32 msg_len. The reply is (s32 ret, u32 errno): the messages done, -1 on a failure before
//   the first; the buffer is written back only when ret > 0.
//   RecvMMsg writes back the header byte and the messages received, in the request's layout: the name cut to the sender's sockaddr (no
//   name for a stream), every iov with the data scattered over it in order, control size 0, msg_flags MSG_TRUNC (0x10) when a datagram
//   was longer than its iovs (the rest is lost), msg_len the bytes received. vlen 0: 0. A timeout below zero or nanoseconds past
//   999999999: EINVAL; flags other than MSG_PEEK, MSG_DONTWAIT, MSG_WAITFORONE (0x80000): EOPNOTSUPP; a buffer that does not hold vlen
//   messages: EINVAL. A non-zero timeout bounds the wait for the first message whatever the socket's mode (none came: 0); a zero one
//   leaves that wait to the socket (blocking, SO_RCVTIMEO, MSG_DONTWAIT). Later messages wait as the socket does unless MSG_WAITFORONE
//   or MSG_DONTWAIT; a receive that fails after the first message ends the call with the count so far.
//   SendMMsg writes back the whole buffer with each sent message's msg_len. Flags other than MSG_DONTWAIT and MSG_NOSIGNAL (0x20000):
//   EOPNOTSUPP; control data: EOPNOTSUPP; a name of under 16 or over 0x100 bytes: EINVAL; not AF_INET: EAFNOSUPPORT; no name: the
//   connected peer. The iovs are gathered into one datagram. A send that fails after the first message ends the call with the count so
//   far; a stream that took part of a message counts it and ends the call.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "nxrt/ipc.h"
#include "../net/vsock.h"

namespace nxrt::services::mmsg {

constexpr uint32_t kMsgTrunc = 0x10, kMsgNosignal = 0x20000, kMsgWaitforone = 0x80000;   // FreeBSD numbering

// One descriptor's operations.
struct Sock {
    std::function<vsock::Ret(void* p, size_t n, int flags, vsock::Addr* from)> recvfrom;           // from null for a stream
    std::function<vsock::Ret(const void* p, size_t n, int flags, const vsock::Addr* to)> sendto;   // to null: the connected peer
    std::function<int16_t(int timeout_ms)> wait_in;                                                // POLLIN revents within timeout_ms
    bool valid = false;    // a socket (EBADF otherwise)
    bool stream = false;   // connection-based: no names
};

// The work, on the buffer's bytes (rewritten when ret > 0).
vsock::Ret recv(const Sock& s, uint32_t vlen, uint32_t flags, int64_t timeout_sec, int64_t timeout_nsec, std::vector<uint8_t>& buf);
vsock::Ret send(const Sock& s, uint32_t vlen, uint32_t flags, std::vector<uint8_t>& buf);

// The commands: the arguments and the B buffer from req, (ret, errno) into rsp, the buffer written back. Result code 0.
uint32_t recvmmsg(ipc::Request& req, ipc::Response& rsp, const Sock& s);
uint32_t sendmmsg(ipc::Request& req, ipc::Response& rsp, const Sock& s);

// The destination of a SendMMsg request's first message, false when it has none (or the buffer does not parse).
bool first_destination(const ipc::Request& req, vsock::Addr* a);

// The offline socket table's descriptor fd (vsock::stack()).
Sock offline(int fd);

}  // namespace nxrt::services::mmsg
