// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// nxwin <winsock2.h>: Winsock over BSD sockets. SOCKET is an fd; WSAGetLastError is errno (WSAE* are the errno values), with
// EINPROGRESS reported as WSAEWOULDBLOCK the way Winsock reports a non-blocking connect. select() ignores its first argument as
// Winsock does: the highest descriptor in the sets is used instead. WSAStartup also turns SIGPIPE off for the process.
#pragma once
#include "windows.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SD_RECEIVE SHUT_RD
#define SD_SEND SHUT_WR
#define SD_BOTH SHUT_RDWR
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
typedef struct timeval TIMEVAL;
typedef struct sockaddr SOCKADDR;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct sockaddr_in6 SOCKADDR_IN6;
typedef struct sockaddr_storage SOCKADDR_STORAGE;
typedef struct addrinfo ADDRINFOA;
typedef struct in_addr IN_ADDR;
typedef struct hostent HOSTENT;
typedef struct linger LINGER;
typedef struct WSAData { WORD wVersion, wHighVersion; char szDescription[257], szSystemStatus[129]; unsigned short iMaxSockets, iMaxUdpDg; char* lpVendorInfo; } WSADATA, *LPWSADATA;
typedef void* LPWSAOVERLAPPED;
typedef void (*LPWSAOVERLAPPED_COMPLETION_ROUTINE)(DWORD, DWORD, LPWSAOVERLAPPED, DWORD);

#define WSAEINTR EINTR
#define WSAEBADF EBADF
#define WSAEACCES EACCES
#define WSAEFAULT EFAULT
#define WSAEINVAL EINVAL
#define WSAEMFILE EMFILE
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAEALREADY EALREADY
#define WSAENOTSOCK ENOTSOCK
#define WSAEDESTADDRREQ EDESTADDRREQ
#define WSAEMSGSIZE EMSGSIZE
#define WSAEPROTOTYPE EPROTOTYPE
#define WSAENOPROTOOPT ENOPROTOOPT
#define WSAEPROTONOSUPPORT EPROTONOSUPPORT
#define WSAEOPNOTSUPP EOPNOTSUPP
#define WSAEAFNOSUPPORT EAFNOSUPPORT
#define WSAEADDRINUSE EADDRINUSE
#define WSAEADDRNOTAVAIL EADDRNOTAVAIL
#define WSAENETDOWN ENETDOWN
#define WSAENETUNREACH ENETUNREACH
#define WSAENETRESET ENETRESET
#define WSAECONNABORTED ECONNABORTED
#define WSAECONNRESET ECONNRESET
#define WSAENOBUFS ENOBUFS
#define WSAEISCONN EISCONN
#define WSAENOTCONN ENOTCONN
#define WSAESHUTDOWN ESHUTDOWN
#define WSAETIMEDOUT ETIMEDOUT
#define WSAECONNREFUSED ECONNREFUSED
#define WSAEHOSTDOWN EHOSTDOWN
#define WSAEHOSTUNREACH EHOSTUNREACH
#define WSANOTINITIALISED 10093
#define WSAHOST_NOT_FOUND 11001

#define IOC_IN 0x80000000u
#define IOC_VENDOR 0x18000000u
#define _WSAIOW(x, y) (IOC_IN | (x) | (y))
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#define SIO_KEEPALIVE_VALS _WSAIOW(IOC_VENDOR, 4)

int WSAStartup(WORD version, LPWSADATA data);
int WSACleanup();
inline int WSAGetLastError() { return errno == EINPROGRESS ? EWOULDBLOCK : errno; }
inline void WSASetLastError(int e) { errno = e; }
inline int closesocket(SOCKET s) { return ::close(s); }
inline int ioctlsocket(SOCKET s, long cmd, u_long* arg) {
    if (cmd == static_cast<long>(FIONBIO)) {
        const int fl = ::fcntl(s, F_GETFL, 0);
        if (fl < 0) return -1;
        return ::fcntl(s, F_SETFL, *arg ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) < 0 ? -1 : 0;
    }
    int v = 0;
    const int r = ::ioctl(s, static_cast<unsigned long>(cmd), &v);
    *arg = static_cast<u_long>(v);
    return r;
}
// SIO_UDP_CONNRESET: no-op (BSD sockets do not report ICMP errors on unconnected UDP sockets); SIO_KEEPALIVE_VALS: TCP keepalive options.
int WSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD in_n, LPVOID out, DWORD out_n, LPDWORD got, LPWSAOVERLAPPED ov,
             LPWSAOVERLAPPED_COMPLETION_ROUTINE cr);
int nxwin_ws_select(int n, fd_set* r, fd_set* w, fd_set* x, const struct timeval* tv);
#define select(n, r, w, x, tv) nxwin_ws_select((n), (r), (w), (x), (tv))

// Winsock takes `int*` lengths where BSD takes socklen_t*.
inline int accept(int s, sockaddr* a, int* n) { socklen_t l = n ? static_cast<socklen_t>(*n) : 0; const int r = ::accept(s, a, n ? &l : nullptr); if (n) *n = static_cast<int>(l); return r; }
inline int getsockname(int s, sockaddr* a, int* n) { socklen_t l = static_cast<socklen_t>(*n); const int r = ::getsockname(s, a, &l); *n = static_cast<int>(l); return r; }
inline int getpeername(int s, sockaddr* a, int* n) { socklen_t l = static_cast<socklen_t>(*n); const int r = ::getpeername(s, a, &l); *n = static_cast<int>(l); return r; }
inline int getsockopt(int s, int lv, int o, void* v, int* n) { socklen_t l = static_cast<socklen_t>(*n); const int r = ::getsockopt(s, lv, o, v, &l); *n = static_cast<int>(l); return r; }
inline ssize_t recvfrom(int s, void* b, size_t len, int f, sockaddr* a, int* n) {
    socklen_t l = n ? static_cast<socklen_t>(*n) : 0;
    const ssize_t r = ::recvfrom(s, b, len, f, a, n ? &l : nullptr);
    if (n) *n = static_cast<int>(l);
    return r;
}
