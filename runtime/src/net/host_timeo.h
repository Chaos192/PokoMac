// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// A host socket's receive or send timeout in milliseconds. Winsock takes a DWORD of milliseconds for SO_RCVTIMEO / SO_SNDTIMEO; BSD
// sockets (nxwin on macOS, Linux, Android: setsockopt is the system's) take a struct timeval, and refuse a 4-byte value with EINVAL, so a
// DWORD passed there leaves the socket with no timeout at all (a recv that never returns). Include after <winsock2.h>.
#pragma once
#include <cstdint>

namespace nxrt::net {

inline int set_host_timeout(SOCKET s, int opt, int64_t ms) {
#if defined(_WIN32)
    const DWORD v = static_cast<DWORD>(ms < 0 ? 0 : ms);
    return ::setsockopt(s, SOL_SOCKET, opt, reinterpret_cast<const char*>(&v), sizeof v);
#else
    timeval tv{};
    if (ms > 0) {
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(ms / 1000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>((ms % 1000) * 1000);
    }
    return ::setsockopt(s, SOL_SOCKET, opt, &tv, sizeof tv);
#endif
}

}  // namespace nxrt::net
