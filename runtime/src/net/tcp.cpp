// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Winsock implementation of net/tcp.h.
#include "tcp.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "host_timeo.h"

#include <mutex>

#if defined(_WIN32)   // Windows link directives (an ELF linker such as lld would look for these libraries)
#pragma comment(lib, "ws2_32.lib")
#endif

namespace nxrt::tcp {
namespace {

bool startup() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wd;
        ok = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    });
    return ok;
}

const char* reason(int e) {
    switch (e) {
        case WSAECONNREFUSED: return "connection refused (is the server running on that port?)";
        case WSAETIMEDOUT: return "timed out";
        case WSAENETUNREACH: case WSAEHOSTUNREACH: return "network unreachable";
        default: return "connection failed";
    }
}

}  // namespace

Handle dial(const std::string& host, uint16_t port, int timeout_ms, int send_timeout_ms, std::string* err) {
    if (!startup()) {
        *err = "Winsock unavailable";
        return kBad;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        *err = "cannot resolve the host name";
        return kBad;
    }
    SOCKET s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        *err = "no socket";
        return kBad;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int rc = ::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    int e = rc == 0 ? 0 : WSAGetLastError();
    if (e == WSAEWOULDBLOCK) {
        fd_set w, x;
        FD_ZERO(&w);
        FD_ZERO(&x);
        FD_SET(s, &w);
        FD_SET(s, &x);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        const int n = ::select(0, nullptr, &w, &x, &tv);
        if (n == 0) e = WSAETIMEDOUT;
        else if (n < 0) e = WSAGetLastError();
        else {
            int so = 0, len = sizeof so;
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &len);
            e = FD_ISSET(s, &x) && so == 0 ? WSAECONNREFUSED : so;
        }
    }
    if (e != 0) {
        closesocket(s);
        *err = reason(e);
        return kBad;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    const BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);   // small game datagrams: no Nagle delay
    nxrt::net::set_host_timeout(s, SO_SNDTIMEO, send_timeout_ms);   // a DWORD is no timeout at all on BSD sockets (host_timeo.h)
    return static_cast<Handle>(s);
}

bool send_all(Handle h, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
        const int k = ::send(static_cast<SOCKET>(h), c, static_cast<int>(n > 0x10000 ? 0x10000 : n), 0);
        if (k <= 0) return false;
        c += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}

bool recv_all(Handle h, void* p, size_t n) {
    char* c = static_cast<char*>(p);
    while (n) {
        const int k = ::recv(static_cast<SOCKET>(h), c, static_cast<int>(n > 0x10000 ? 0x10000 : n), 0);
        if (k <= 0) return false;
        c += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}

void shutdown(Handle h) {
    if (h != kBad) ::shutdown(static_cast<SOCKET>(h), SD_BOTH);
}

void close(Handle h) {
    if (h != kBad) closesocket(static_cast<SOCKET>(h));
}

uint32_t peer_ip(Handle h) {
    sockaddr_in a{};
    int len = sizeof a;
    if (h == kBad || getpeername(static_cast<SOCKET>(h), reinterpret_cast<sockaddr*>(&a), &len) != 0 || a.sin_family != AF_INET) return 0;
    return ntohl(a.sin_addr.s_addr);
}

}  // namespace nxrt::tcp
