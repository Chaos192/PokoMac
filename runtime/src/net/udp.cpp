// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Winsock implementation of net/udp.h.
#include "udp.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "host_timeo.h"
#include <mstcpip.h>

#if defined(_WIN32)   // Windows link directives (an ELF linker such as lld would look for these libraries)
#pragma comment(lib, "ws2_32.lib")
#endif

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)   // mstcpip.h, hidden behind some SDK guards
#endif

namespace nxrt::udp {

Handle open(uint32_t ip, uint16_t port, int recv_timeout_ms, std::string* err) {
    WSADATA wd;   // one WSAStartup per socket, undone in close() (Winsock counts them, so this sits fine next to tcp.cpp's)
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        *err = "Winsock unavailable";
        return kBad;
    }
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        *err = "no socket";
        return kBad;
    }
    // Windows reports an ICMP port-unreachable from an earlier send as WSAECONNRESET on the next recv: switch that off
    BOOL off = FALSE;
    DWORD got = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        closesocket(s);
        WSACleanup();
        *err = "cannot reach the relay's UDP port";
        return kBad;
    }
    nxrt::net::set_host_timeout(s, SO_RCVTIMEO, recv_timeout_ms);   // a DWORD is no timeout at all on BSD sockets (host_timeo.h)
    return static_cast<Handle>(s);
}

bool send(Handle h, const void* p, size_t n) {
    if (h == kBad) return false;
    return ::send(static_cast<SOCKET>(h), static_cast<const char*>(p), static_cast<int>(n), 0) == static_cast<int>(n);
}

int recv(Handle h, void* p, size_t cap) {
    if (h == kBad) return -1;
    const int k = ::recv(static_cast<SOCKET>(h), static_cast<char*>(p), static_cast<int>(cap), 0);
    if (k >= 0) return k;
    switch (WSAGetLastError()) {
        case WSAETIMEDOUT: case WSAEWOULDBLOCK: case WSAECONNRESET: case WSAEINTR: return 0;
        case WSAEMSGSIZE: return static_cast<int>(cap) + 1;
        default: return -1;
    }
}

void close(Handle h) {
    if (h != kBad) {
        closesocket(static_cast<SOCKET>(h));
        WSACleanup();
    }
}

}  // namespace nxrt::udp
