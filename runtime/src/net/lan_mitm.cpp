// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// ldn_mitm's LAN protocol and the real-LAN link (lan_mitm.h). Winsock (nxwin on macOS, Linux, Android).
// The wire format and the compression are ported from Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors):
// src/Ryujinx.HLE/HOS/Services/Ldn/UserServiceCreator/LdnMitm/LanProtocol.cs (Compress, Decompress, the header) and LanDiscovery.cs
// (scan, the host's node table, SyncNetwork); see CREDITS.md.
#include "lan_mitm.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "host_timeo.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)   // mstcpip.h, hidden behind some SDK guards (as net/udp.cpp)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "nxrt/log.h"
#include "relay_server.h"

#if defined(_WIN32)
#pragma comment(lib, "ws2_32.lib")
#endif

namespace nxrt::vnet::mitm {
namespace {

bool wsa() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wd;
        ok = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    });
    return ok;
}

sockaddr_in sa4(uint32_t ip, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    return a;
}

// A UDP socket on port: this address on Windows (which delivers the subnet's broadcasts to it), every address elsewhere (BSD sockets get a
// broadcast only on a socket bound to the wildcard or the broadcast address). SO_REUSEADDR (and SO_REUSEPORT) so another program's socket
// on the port does not stop it.
SOCKET udp_on(uint32_t ip, uint16_t port, std::string* why) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        if (why) *why = "no UDP socket";
        return s;
    }
    const BOOL one = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#if defined(SO_REUSEPORT)
    ::setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&one), sizeof one);
#endif
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
#if defined(_WIN32)
    const sockaddr_in a = sa4(ip, port);
#else
    const sockaddr_in a = sa4((ip >> 24) == 127 ? ip : 0, port);
#endif
    if (::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        if (why) *why = "UDP port " + std::to_string(port) + " is taken";
        closesocket(s);
        return INVALID_SOCKET;
    }
    net::set_host_timeout(s, SO_RCVTIMEO, 200);   // reader threads look at their stop flag five times a second
    DWORD no = 0, got = 0;   // an ICMP port-unreachable from an earlier send must not fail the next recvfrom (Windows; nxwin: a no-op)
    WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof no, nullptr, 0, &got, nullptr, nullptr);
    return s;
}

bool send_all(SOCKET s, const std::vector<uint8_t>& b) {
    size_t done = 0;
    while (done < b.size()) {
        const int k = ::send(s, reinterpret_cast<const char*>(b.data() + done), static_cast<int>(b.size() - done), 0);
        if (k <= 0) return false;
        done += static_cast<size_t>(k);
    }
    return true;
}

uint32_t get32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

}  // namespace

// ---- wire ---------------------------------------------------------------------------------------------------------------------------------
bool compress(const uint8_t* p, size_t n, std::vector<uint8_t>* out) {
    out->clear();
    size_t i = 0;
    while (i < n) {
        const uint8_t b = p[i++];
        if (b != 0) {
            out->push_back(b);
            continue;
        }
        uint8_t count = 0;
        while (i < n && p[i] == 0 && count < 0xFF) {
            ++count;
            ++i;
        }
        out->push_back(0);
        out->push_back(count);
        if (out->size() > kBufferSize) return false;
    }
    return out->size() <= kBufferSize;
}

bool decompress(const uint8_t* p, size_t n, size_t want, std::vector<uint8_t>* out) {
    out->clear();
    size_t i = 0;
    while (i < n && out->size() < kBufferSize) {
        const uint8_t b = p[i++];
        out->push_back(b);
        if (b == 0) {
            if (i == n) return false;   // a zero with no count
            const uint8_t count = p[i++];
            for (uint8_t k = 0; k < count && out->size() < kBufferSize; ++k) out->push_back(0);
        }
    }
    return i == n && out->size() == want;
}

std::vector<uint8_t> packet(Type t, const uint8_t* p, size_t n) {
    std::vector<uint8_t> body;
    bool packed = false;
    if (n && compress(p, n, &body)) packed = true;
    else body.assign(p, p + n);
    std::vector<uint8_t> b(kHeader, 0);
    const uint32_t magic = kMagic;
    std::memcpy(b.data(), &magic, 4);
    b[4] = t;
    b[5] = packed ? 1 : 0;
    const uint16_t len = static_cast<uint16_t>(body.size()), dlen = static_cast<uint16_t>(packed ? n : 0);
    std::memcpy(&b[6], &len, 2);
    std::memcpy(&b[8], &dlen, 2);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

int parse(const uint8_t* b, size_t n, Type* t, std::vector<uint8_t>* payload) {
    if (n < kHeader) return 0;
    if (get32(b) != kMagic) return -1;
    uint16_t len, dlen;
    std::memcpy(&len, b + 6, 2);
    std::memcpy(&dlen, b + 8, 2);
    if (kHeader + len > kBufferSize) return -1;
    if (n < kHeader + len) return 0;
    *t = static_cast<Type>(b[4]);
    if (b[5]) {
        if (!decompress(b + kHeader, len, dlen, payload)) return -1;
    } else {
        payload->assign(b + kHeader, b + kHeader + len);
    }
    return static_cast<int>(kHeader + len);
}

bool pick_interface(const std::string& setting, uint32_t* ip, uint32_t* mask, std::string* why) {
    const std::vector<LocalAddr> all = local_ipv4();
    unsigned a, b, c, d;
    char tail;
    if (!setting.empty() && setting != "auto") {
        if (std::sscanf(setting.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
            *why = "[net] lan_ip = " + setting + " is not an IPv4 address";
            return false;
        }
        *ip = (a << 24) | (b << 16) | (c << 8) | d;
        *mask = (*ip >> 24) == 127 ? 0xFF000000u : 0xFFFFFF00u;
        for (const LocalAddr& l : all)
            if (l.ip == *ip && l.mask) *mask = l.mask;
        return true;
    }
    auto priv = [](uint32_t x) {
        const uint32_t h = x >> 24, s = (x >> 16) & 0xFF;
        return h == 10 || (h == 172 && s >= 16 && s <= 31) || (h == 192 && s == 168);
    };
    for (int pass = 0; pass < 2; ++pass)
        for (const LocalAddr& l : all)
            if (l.mask && (pass == 1 || priv(l.ip))) {
                *ip = l.ip;
                *mask = l.mask;
                return true;
            }
    *why = "no IPv4 network interface is up";
    return false;
}

// ---- LanLink ---------------------------------------------------------------------------------------------------------------------------
namespace {
class LanLink final : public Link, public std::enable_shared_from_this<LanLink> {
public:
    LanLink(uint32_t ip, uint32_t mask) : ip_(ip), mask_(mask) {}
    ~LanLink() override {
        std::map<uint16_t, Port> ports;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
            ports.swap(ports_);
        }
        for (auto& [p, port] : ports) {
            if (port.th.joinable()) port.th.join();
            closesocket(port.s);
        }
    }
    uint32_t local_ip() const override { return ip_; }
    uint32_t netmask() const override { return mask_; }
    bool reaches(uint32_t dst) const override { return (dst & mask_) == (ip_ & mask_) || dst == 0xFFFFFFFFu; }
    void open_port(uint16_t port) override { sock(port); }
    bool send(const Datagram& d) override {
        const bool vtcp = d.src_port == 0 && d.dst_port == 0;
        const SOCKET s = sock(vtcp ? kVtcpPort : d.src_port);
        if (s == INVALID_SOCKET) return false;
        const sockaddr_in to = sa4(d.dst_ip, vtcp ? kVtcpPort : d.dst_port);
        return ::sendto(s, reinterpret_cast<const char*>(d.data.data()), static_cast<int>(d.data.size()), 0, reinterpret_cast<const sockaddr*>(&to),
                        sizeof to) >= 0;
    }

private:
    struct Port {
        SOCKET s = INVALID_SOCKET;
        std::thread th;
    };
    SOCKET sock(uint16_t port) {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_ || port == 0) return INVALID_SOCKET;
        auto it = ports_.find(port);
        if (it != ports_.end()) return it->second.s;
        std::string why;
        Port p;
        p.s = udp_on(ip_, port, &why);
        if (p.s == INVALID_SOCKET) {
            NXLOG_W("net", "lan: %s: the game's datagrams on it do not reach the LAN", why.c_str());
            return INVALID_SOCKET;
        }
        const SOCKET s = p.s;
        p.th = std::thread([this, s, port] { read(s, port); });
        ports_.emplace(port, std::move(p));
        return s;
    }
    void read(SOCKET s, uint16_t port) {
        std::vector<uint8_t> buf(65536);
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (stop_) return;
            }
            sockaddr_in from{};
            int fl = sizeof from;
            const int n = ::recvfrom(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n < 0) continue;   // the timeout (or an ICMP error): look at stop_ again
            Datagram d;
            d.src_ip = ntohl(from.sin_addr.s_addr);
            if (d.src_ip == ip_) continue;   // our own broadcast, looped back
            const bool vtcp = port == kVtcpPort;
            d.src_port = vtcp ? 0 : ntohs(from.sin_port);
            d.dst_ip = ip_;
            d.dst_port = vtcp ? 0 : port;
            d.data.assign(buf.begin(), buf.begin() + n);
            deliver(d);
        }
    }
    const uint32_t ip_, mask_;
    std::mutex mu_;
    bool stop_ = false;
    std::map<uint16_t, Port> ports_;
};

// ---- Net -----------------------------------------------------------------------------------------------------------------------------------
class NetImpl final : public Net {
public:
    NetImpl(uint32_t ip, uint32_t mask, std::vector<uint32_t> peers, Events ev) : ip_(ip), mask_(mask), peers_(std::move(peers)), ev_(std::move(ev)) {}
    ~NetImpl() override {
        host_close();
        disconnect();
        stop_ = true;
        if (udp_th_.joinable()) udp_th_.join();
        if (udp_ != INVALID_SOCKET) closesocket(udp_);
    }
    bool begin(std::string* why) {
        udp_ = udp_on(ip_, kPort, why);
        if (udp_ == INVALID_SOCKET) return false;
        udp_th_ = std::thread([this] { udp_loop(); });
        return true;
    }

    std::vector<Info> scan(int wait_ms, int wait_after_ms) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            found_.clear();
            scanning_ = true;
        }
        const std::vector<uint8_t> p = packet(kScan);
        std::vector<uint32_t> to = peers_;
        to.push_back(ip_ | ~mask_);
        for (uint32_t t : to) {
            const sockaddr_in a = sa4(t, kPort);
            ::sendto(udp_, reinterpret_cast<const char*>(p.data()), static_cast<int>(p.size()), 0, reinterpret_cast<const sockaddr*>(&a), sizeof a);
        }
        std::unique_lock<std::mutex> lk(mu_);
        if (cv_.wait_for(lk, std::chrono::milliseconds(wait_ms), [&] { return !found_.empty(); }))
            cv_.wait_for(lk, std::chrono::milliseconds(wait_after_ms), [] { return false; });
        scanning_ = false;
        std::vector<Info> out;
        for (const auto& [mac, info] : found_) out.push_back(info);
        return out;
    }

    bool host(const Info& info, std::string* why) override {
        SOCKET l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        const BOOL one = TRUE;
        ::setsockopt(l, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
        const sockaddr_in a = sa4(ip_, kPort);
        if (l == INVALID_SOCKET || ::bind(l, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 || ::listen(l, 8) != 0) {
            if (l != INVALID_SOCKET) closesocket(l);
            *why = "TCP port 11452 is taken on this machine";
            return false;
        }
        std::lock_guard<std::mutex> lk(mu_);
        info_ = info;
        listen_ = l;
        hosting_ = true;
        accept_th_ = std::thread([this, l] { accept_loop(l); });
        return true;
    }
    void host_update(const Info& info) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!hosting_) return;
        Info keep = info;   // the stations stay as this side keeps them: the caller changes the network's other fields
        std::memcpy(keep.data() + kInfoNodes + kNodeSize, info_.data() + kInfoNodes + kNodeSize, kNodeSize * 7);
        keep[kInfoNodeCount] = info_[kInfoNodeCount];
        info_ = keep;
        sync_locked();
    }
    void host_close() override {
        std::vector<std::shared_ptr<Session>> ss;
        std::thread th;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!hosting_) return;
            hosting_ = false;
            ss.swap(sessions_);
            if (listen_ != INVALID_SOCKET) {
                ::shutdown(listen_, SD_BOTH);
                closesocket(listen_);
                listen_ = INVALID_SOCKET;
            }
            th.swap(accept_th_);
        }
        for (auto& s : ss) s->close();
        if (th.joinable()) th.join();
        for (auto& s : ss) s->join();
        std::vector<std::shared_ptr<Session>> done;
        {
            std::lock_guard<std::mutex> lk(mu_);
            done.swap(done_);
        }
        for (auto& s : done) s->join();
    }
    bool host_reject(uint32_t ip) override {
        std::shared_ptr<Session> s;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& x : sessions_)
                if (x->ip == ip) s = x;
        }
        if (!s) return false;
        s->close();   // its reader drops its node and syncs the others
        return true;
    }

    int connect(uint32_t host_ip, const Node& self, int timeout_ms) override {
        disconnect();
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return 3;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        const sockaddr_in a = sa4(host_ip, kPort);
        ::connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof a);
        fd_set w, x;
        FD_ZERO(&w);
        FD_ZERO(&x);
        FD_SET(s, &w);
        FD_SET(s, &x);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int so = 0, sl = sizeof so;
        if (::select(0, nullptr, &w, &x, &tv) <= 0 || !FD_ISSET(s, &w) || ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &sl) != 0 ||
            so != 0) {
            closesocket(s);
            return 3;
        }
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        const BOOL one = TRUE;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        if (!send_all(s, packet(kConnect, self.data(), self.size()))) {
            closesocket(s);
            return 3;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            station_ = s;
            synced_ = false;
            closed_ = false;
            station_th_ = std::thread([this, s] { station_loop(s); });
        }
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return synced_ || closed_; });
        if (synced_) return 0;
        const bool turned_away = closed_;
        lk.unlock();
        disconnect();
        return turned_away ? 2 : 1;
    }
    void disconnect() override {
        std::thread th;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (station_ != INVALID_SOCKET) ::shutdown(station_, SD_BOTH);
            leaving_ = true;
            th.swap(station_th_);
        }
        if (th.joinable()) th.join();
        std::lock_guard<std::mutex> lk(mu_);
        if (station_ != INVALID_SOCKET) closesocket(station_);
        station_ = INVALID_SOCKET;
        leaving_ = false;
    }
    Info info() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return info_;
    }

private:
    struct Session {
        SOCKET s = INVALID_SOCKET;
        uint32_t ip = 0;
        int slot = -1;
        std::mutex send_mu;
        std::thread th;
        std::atomic<bool> closing{false};
        void close() {
            if (!closing.exchange(true)) ::shutdown(s, SD_BOTH);
        }
        void join() {
            if (th.joinable()) th.join();
        }
        bool send(const std::vector<uint8_t>& b) {
            std::lock_guard<std::mutex> lk(send_mu);
            return send_all(s, b);
        }
    };

    void udp_loop() {
        std::vector<uint8_t> buf(kBufferSize + 64);
        while (!stop_) {
            sockaddr_in from{};
            int fl = sizeof from;
            const int n = ::recvfrom(udp_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n <= 0) continue;
            if (ntohl(from.sin_addr.s_addr) == ip_) continue;   // our own scan, looped back
            Type t;
            std::vector<uint8_t> pl;
            if (parse(buf.data(), static_cast<size_t>(n), &t, &pl) <= 0) continue;
            std::unique_lock<std::mutex> lk(mu_);
            if (t == kScan && hosting_) {
                const std::vector<uint8_t> p = packet(kScanResponse, info_.data(), info_.size());
                lk.unlock();
                ::sendto(udp_, reinterpret_cast<const char*>(p.data()), static_cast<int>(p.size()), 0, reinterpret_cast<const sockaddr*>(&from), fl);
            } else if (t == kScanResponse && scanning_ && pl.size() == kInfoSize) {
                Info info;
                std::memcpy(info.data(), pl.data(), kInfoSize);
                if (info[kInfoNodes + kNodeName] == 0) continue;   // a host with no name: not a network (Ryujinx drops these too)
                std::array<uint8_t, 6> bssid;
                std::memcpy(bssid.data(), info.data() + 0x20, 6);
                found_[bssid] = info;
                cv_.notify_all();
            }
        }
    }

    // The host's node table: a station takes the first free slot under node_count_max, if the accept policy lets it; its node info as it
    // sent it, with its slot as node id, connected, and its address as the host sees it when it gave none.
    bool admit_locked(Session& s, const std::vector<uint8_t>& node) {
        if (node.size() != kNodeSize) return false;
        const int max = std::clamp<int>(info_[kInfoNodeCountMax], 1, 8);
        int slot = -1;
        for (int i = 1; i < max && slot < 0; ++i)
            if (info_[kInfoNodes + kNodeSize * i + kNodeConnected] == 0) slot = i;
        if (slot < 0) return false;
        uint8_t* n = info_.data() + kInfoNodes + kNodeSize * slot;
        std::memcpy(n, node.data(), kNodeSize);
        if (get32(n + kNodeIp) == 0) std::memcpy(n + kNodeIp, &s.ip, 4);
        n[kNodeId] = static_cast<uint8_t>(slot);
        n[kNodeConnected] = 1;
        s.slot = slot;
        recount_locked();
        return true;
    }
    void recount_locked() {
        uint8_t c = 0;
        for (int i = 0; i < 8; ++i) c = static_cast<uint8_t>(c + (info_[kInfoNodes + kNodeSize * i + kNodeConnected] ? 1 : 0));
        info_[kInfoNodeCount] = c;
    }
    void sync_locked() {
        const std::vector<uint8_t> p = packet(kSyncNetwork, info_.data(), info_.size());
        for (const auto& s : sessions_)
            if (s->slot >= 0) s->send(p);
    }

    void accept_loop(SOCKET l) {
        for (;;) {
            sockaddr_in from{};
            int fl = sizeof from;
            const SOCKET c = ::accept(l, reinterpret_cast<sockaddr*>(&from), &fl);
            if (c == INVALID_SOCKET) return;   // closed: host_close
            auto s = std::make_shared<Session>();
            s->s = c;
            s->ip = ntohl(from.sin_addr.s_addr);
            const BOOL one = TRUE;
            ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
            std::lock_guard<std::mutex> lk(mu_);
            if (!hosting_) {
                closesocket(c);
                return;
            }
            sessions_.push_back(s);
            s->th = std::thread([this, s] { session_loop(s); });
        }
    }

    void session_loop(std::shared_ptr<Session> s) {
        std::vector<uint8_t> buf, chunk(4096);
        bool changed = false;
        for (;;) {
            const int k = ::recv(s->s, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
            if (k <= 0) break;
            buf.insert(buf.end(), chunk.begin(), chunk.begin() + k);
            Type t;
            std::vector<uint8_t> pl;
            int used;
            bool refuse = false;
            while ((used = parse(buf.data(), buf.size(), &t, &pl)) > 0) {
                buf.erase(buf.begin(), buf.begin() + used);
                if (t != kConnect || s->slot >= 0) continue;
                Node node{};
                if (pl.size() == kNodeSize) std::memcpy(node.data(), pl.data(), kNodeSize);
                const bool ok = pl.size() == kNodeSize && (!ev_.admit || ev_.admit(node));   // the policy, outside our lock
                std::lock_guard<std::mutex> lk(mu_);
                if (!ok || !admit_locked(*s, pl)) {
                    refuse = true;
                    break;
                }
                sync_locked();
                changed = true;
                NXLOG_I("net", "lan: station %s joined as node %d", vnet::ip_str(s->ip).c_str(), s->slot);
            }
            if (changed && ev_.changed) ev_.changed();
            changed = false;
            if (used < 0 || refuse) break;
        }
        bool was_node = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const auto it = std::find(sessions_.begin(), sessions_.end(), s);
            if (it != sessions_.end()) {   // ended by itself (not by host_close, which joins what it took): joined later from done_
                sessions_.erase(it);
                done_.push_back(s);
            }
            if (s->slot >= 0 && hosting_) {
                std::memset(info_.data() + kInfoNodes + kNodeSize * s->slot, 0, kNodeSize);
                recount_locked();
                sync_locked();
                was_node = true;
            }
        }
        closesocket(s->s);
        if (was_node && ev_.changed) ev_.changed();
    }

    void station_loop(SOCKET s) {
        std::vector<uint8_t> buf, chunk(4096);
        bool orderly = false;
        for (;;) {
            const int k = ::recv(s, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
            if (k <= 0) {
                orderly = k == 0;
                break;
            }
            buf.insert(buf.end(), chunk.begin(), chunk.begin() + k);
            Type t;
            std::vector<uint8_t> pl;
            int used;
            bool changed = false;
            while ((used = parse(buf.data(), buf.size(), &t, &pl)) > 0) {
                buf.erase(buf.begin(), buf.begin() + used);
                if (t != kSyncNetwork || pl.size() != kInfoSize) continue;
                std::lock_guard<std::mutex> lk(mu_);
                std::memcpy(info_.data(), pl.data(), kInfoSize);
                changed = synced_;
                synced_ = true;
                cv_.notify_all();
            }
            if (changed && ev_.changed) ev_.changed();
            if (used < 0) break;
        }
        bool tell;
        {
            std::lock_guard<std::mutex> lk(mu_);
            tell = synced_ && !leaving_;
            closed_ = true;
            cv_.notify_all();
        }
        if (tell && ev_.lost) ev_.lost(orderly);
    }

    const uint32_t ip_, mask_;
    const std::vector<uint32_t> peers_;
    const Events ev_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    SOCKET udp_ = INVALID_SOCKET;
    std::thread udp_th_;
    Info info_{};
    bool scanning_ = false;
    std::map<std::array<uint8_t, 6>, Info> found_;   // scan answers by BSSID
    bool hosting_ = false;
    SOCKET listen_ = INVALID_SOCKET;
    std::thread accept_th_;
    std::vector<std::shared_ptr<Session>> sessions_;
    std::vector<std::shared_ptr<Session>> done_;   // sessions that ended by themselves, joined by host_close
    SOCKET station_ = INVALID_SOCKET;
    std::thread station_th_;
    bool synced_ = false, closed_ = false, leaving_ = false;
};
}  // namespace

std::shared_ptr<Link> make_lan_link(uint32_t ip, uint32_t mask) {
    if (!wsa()) return nullptr;
    auto l = std::make_shared<LanLink>(ip, mask);
    l->open_port(kVtcpPort);   // TCP between AnySwitch nodes arrives here before this node ever sends on it
    return l;
}

std::unique_ptr<Net> Net::start(uint32_t ip, uint32_t mask, std::vector<uint32_t> peers, Events ev, std::string* why) {
    if (!wsa()) {
        *why = "no sockets";
        return nullptr;
    }
    auto n = std::make_unique<NetImpl>(ip, mask, std::move(peers), std::move(ev));
    if (!n->begin(why)) return nullptr;
    return n;
}

}  // namespace nxrt::vnet::mitm
