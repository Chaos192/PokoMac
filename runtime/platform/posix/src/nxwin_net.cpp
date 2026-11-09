// SPDX-License-Identifier: GPL-3.0-or-later
// nxwin: Winsock helpers (startup, ioctls, select) and GetAdaptersAddresses over getifaddrs.
#include "nxwin_internal.h"

#include <iphlpapi.h>
#include <mstcpip.h>
#include <winsock2.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/if_dl.h>

#undef select

int WSAStartup(WORD version, LPWSADATA data) {
    signal(SIGPIPE, SIG_IGN);
    if (data) {
        std::memset(data, 0, sizeof *data);
        data->wVersion = data->wHighVersion = version;
        std::snprintf(data->szDescription, sizeof data->szDescription, "BSD sockets");
    }
    return 0;
}
int WSACleanup() { return 0; }

int WSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD in_n, LPVOID, DWORD, LPDWORD got, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE) {
    if (got) *got = 0;
    if (code == SIO_UDP_CONNRESET) return 0;
    if (code == SIO_KEEPALIVE_VALS && in && in_n >= sizeof(tcp_keepalive)) {
        const auto* k = static_cast<const tcp_keepalive*>(in);
        int on = k->onoff ? 1 : 0;
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
        if (on) {
            int idle = static_cast<int>(std::max<ULONG>(1, k->keepalivetime / 1000));
            int intvl = static_cast<int>(std::max<ULONG>(1, k->keepaliveinterval / 1000));
            setsockopt(s, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle);
            setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
        }
        return 0;
    }
    errno = EOPNOTSUPP;
    return SOCKET_ERROR;
}

int nxwin_ws_select(int, fd_set* r, fd_set* w, fd_set* x, const struct timeval* tv) {
    int maxfd = -1;
    for (int fd = 0; fd < FD_SETSIZE; ++fd)
        if ((r && FD_ISSET(fd, r)) || (w && FD_ISSET(fd, w)) || (x && FD_ISSET(fd, x))) maxfd = fd;
    struct timeval t;
    if (tv) t = *tv;
    return ::select(maxfd + 1, r, w, x, tv ? &t : nullptr);
}

ULONG GetAdaptersAddresses(ULONG family, ULONG, PVOID, PIP_ADAPTER_ADDRESSES out, PULONG len) {
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return ERROR_NOT_ENOUGH_MEMORY;
    struct Iface {
        std::string name;
        unsigned flags = 0;
        DWORD type = IF_TYPE_OTHER;
        std::vector<std::pair<sockaddr_storage, int>> addrs;   // address, prefix length
    };
    std::vector<Iface> ifs;
    auto find = [&](const char* name) -> Iface& {
        for (auto& i : ifs)
            if (i.name == name) return i;
        ifs.push_back({name});
        return ifs.back();
    };
    for (auto* p = ifa; p; p = p->ifa_next) {
        Iface& i = find(p->ifa_name);
        i.flags = p->ifa_flags;
        if (i.flags & IFF_LOOPBACK) i.type = IF_TYPE_SOFTWARE_LOOPBACK;
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_LINK) {
            const auto* sdl = reinterpret_cast<const sockaddr_dl*>(p->ifa_addr);
            if (sdl->sdl_type == IFT_ETHER) i.type = IF_TYPE_ETHERNET_CSMACD;
            else if (sdl->sdl_type == IFT_LOOP) i.type = IF_TYPE_SOFTWARE_LOOPBACK;
        }
        if (!p->ifa_addr) continue;
        const int fam = p->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6) continue;
        if (family != AF_UNSPEC && static_cast<ULONG>(fam) != family) continue;
        sockaddr_storage ss{};
        std::memcpy(&ss, p->ifa_addr, fam == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
        int prefix = 0;
        if (p->ifa_netmask) {
            const auto* m = reinterpret_cast<const uint8_t*>(fam == AF_INET ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(p->ifa_netmask)->sin_addr)
                                                                            : static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(p->ifa_netmask)->sin6_addr));
            const int n = fam == AF_INET ? 4 : 16;
            for (int k = 0; k < n; ++k) prefix += __builtin_popcount(m[k]);
        }
        i.addrs.push_back({ss, prefix});
    }
    freeifaddrs(ifa);

    // Layout: adapters, then unicast records, then sockaddrs, then names.
    auto r16 = [](size_t n) { return (n + 15) & ~size_t{15}; };
    size_t need = 0;
    for (auto& i : ifs) {
        need += r16(sizeof(IP_ADAPTER_ADDRESSES)) + r16(i.name.size() + 1) + r16((i.name.size() + 1) * sizeof(wchar_t));
        need += i.addrs.size() * (r16(sizeof(IP_ADAPTER_UNICAST_ADDRESS)) + r16(sizeof(sockaddr_storage)));
    }
    if (ifs.empty()) return ERROR_NO_DATA;
    if (!out || *len < need) {
        *len = static_cast<ULONG>(need);
        return ERROR_BUFFER_OVERFLOW;
    }
    std::memset(out, 0, need);
    auto* cur = reinterpret_cast<uint8_t*>(out);
    auto take = [&](size_t n) {
        uint8_t* p = cur;
        cur += (n + 15) & ~size_t{15};
        return p;
    };
    IP_ADAPTER_ADDRESSES* prev = nullptr;
    for (auto& i : ifs) {
        auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(take(sizeof(IP_ADAPTER_ADDRESSES)));
        if (prev) prev->Next = a;
        prev = a;
        a->Length = sizeof *a;
        a->IfIndex = if_nametoindex(i.name.c_str());
        a->IfType = i.type;
        a->OperStatus = (i.flags & IFF_UP) && (i.flags & IFF_RUNNING) ? IfOperStatusUp : IfOperStatusDown;
        a->Mtu = 1500;
        auto* an = reinterpret_cast<char*>(take(i.name.size() + 1));
        std::memcpy(an, i.name.c_str(), i.name.size() + 1);
        a->AdapterName = an;
        auto* fn = reinterpret_cast<wchar_t*>(take((i.name.size() + 1) * sizeof(wchar_t)));
        for (size_t k = 0; k <= i.name.size(); ++k) fn[k] = static_cast<wchar_t>(static_cast<unsigned char>(i.name.c_str()[k]));
        a->FriendlyName = fn;
        a->Description = fn;
        IP_ADAPTER_UNICAST_ADDRESS* uprev = nullptr;
        for (auto& [ss, prefix] : i.addrs) {
            auto* u = reinterpret_cast<IP_ADAPTER_UNICAST_ADDRESS*>(take(sizeof(IP_ADAPTER_UNICAST_ADDRESS)));
            auto* sa = reinterpret_cast<sockaddr_storage*>(take(sizeof(sockaddr_storage)));
            *sa = ss;
            u->Length = sizeof *u;
            u->Address.lpSockaddr = reinterpret_cast<sockaddr*>(sa);
            u->Address.iSockaddrLength = ss.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
            u->OnLinkPrefixLength = static_cast<UINT8>(prefix);
            if (uprev) uprev->Next = u;
            else a->FirstUnicastAddress = u;
            uprev = u;
        }
    }
    return NO_ERROR;
}
