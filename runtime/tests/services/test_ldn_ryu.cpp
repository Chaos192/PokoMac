// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Local wireless through a Ryujinx LDN master ([net] protocol = ryuldn: services/ldn_ryu.cpp on net/ryuldn.h). A master server of the
// test's own (a TCP listener on 127.0.0.1, an ephemeral port; nothing outside this machine is contacted) speaks the server side of the
// messages: Initialize, Passphrase, CreateAccessPoint, Scan / ScanReply / ScanReplyEnd, Connect / Connected, SyncNetwork, ProxyConfig,
// ProxyData routed between the members, SetAdvertiseData, SetAcceptPolicy, Reject / RejectReply, Disconnect, NetworkError and Ping. Two
// ldn:u service objects in one process (one hosts, one scans and joins) go through the IPC command table as the SDK sends it.
// The master parses with literal offsets: the layouts .NET gives Ryujinx's own struct files (measured: header 0xC, ConnectRequest
// 0x500 with the NetworkInfo at 0x80, CreateAccessPointRequest 0xBC with the NetworkConfig at 0x74 and the RyuNetworkConfig at 0x94), not
// net/ryuldn.h's constants, so a wrong layout there fails here.
// Datagrams: the bsd socket table is process-wide and follows vnet::current_link(), which is the station's link once it joined (the
// host's before). So the station's side goes through bsd:u, and the host's through a vsock::Stack of its own on the host's link (the link
// vnet::current_link() was right after CreateNetwork): one datagram each way and a broadcast, all through the master.
// The ldn:u object is a Service whose commands ldn_ryu_backend() adds (the hook in ldn.cpp's LdnService does the same).
// vsock.h (and the headers that pull in vnet / ipc) before the Winsock headers: nxwin makes select a macro, which vsock::Stack::select
// must not meet (test_net_udp_redirect.cpp)
#include "../../src/net/ryuldn.h"
#include "../../src/net/vnet.h"
#include "../../src/net/vsock.h"
#include "../../src/services/ldn_ryu.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/ipc.h"
#include "nxrt/services.h"
#include "svc_test.h"

using namespace svctest;
namespace vn = nxrt::vnet;
namespace vs = nxrt::vsock;

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint64_t kLcid = 0x0100A55717C40002ull;   // made up: the title's own id, from the test's control.nacp
constexpr uint64_t kOwnLcid = ~0ull;                // -1: what the game passes for its own
constexpr size_t kInfo = 0x480;
constexpr uint32_t kHostIp = 0x0A720001u, kStationIp = 0x0A720002u, kMask = 0xFFFF0000u;   // 10.114.0.1 / .2, /16
constexpr uint16_t kPort = 41000, kTcpPort = 41001;
constexpr uint32_t kDeviceNotAvailable = 203 | (16u << 9), kConnectRejected = 203 | (67u << 9), kConnectNotFound = 203 | (65u << 9);

uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
void wr16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void wr64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

// ---- the master ------------------------------------------------------------------------------------------------------------------------
// Literal layouts (Ryujinx LdnRyu/Types, UserServiceCreator/Types, Ldn/Types as .NET lays them out).
namespace w {
constexpr uint32_t kMagic = 0x4E444C52u;    // "RLDN"
constexpr size_t kHdr = 0xC;               // u32 magic, u8 type, u8 version, 2 pad, s32 size
enum : uint8_t {
    Initialize = 0, Passphrase = 1, CreateAccessPoint = 2, CreateAccessPointPrivate = 3, SyncNetwork = 7, Reject = 8, RejectReply = 9, Scan = 10, ScanReply = 11,
    ScanReplyEnd = 12, Connect = 13, ConnectPrivate = 14, Connected = 15, Disconnect = 16, ProxyConfig = 17, ProxyData = 20, SetAcceptPolicy = 22,
    SetAdvertiseData = 23, Ping = 254, NetworkError = 255,
};
// NetworkInfo
constexpr size_t kLcid = 0x00, kScene = 0x0A, kSession = 0x10, kBssid = 0x20, kSsidLen = 0x26, kSsid = 0x27, kChannel = 0x48, kLink = 0x4A,
                 kType = 0x4B, kSecMode = 0x60, kPolicy = 0x62, kNodeMax = 0x66, kNodeCount = 0x67, kNodes = 0x68, kAdvSize = 0x26A, kAdv = 0x26C;
constexpr size_t kNodeIp = 0, kNodeMac = 4, kNodeId = 0xA, kNodeOn = 0xB, kNodeName = 0xC, kNodeVer = 0x2E;
}  // namespace w

struct ProxyRec {
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    int32_t proto;
    uint32_t len;
    size_t body;
};

class Master {
public:
    Master() {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        ls_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(ls_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(ls_, 8);
        int len = sizeof a;
        getsockname(ls_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        acc_ = std::thread([this] { accept_loop(); });
    }
    ~Master() {
        stop_ = true;
        closesocket(ls_);
        acc_.join();
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& c : conns_) ::shutdown(c->s, SD_BOTH);
        }
        for (auto& c : conns_) {
            c->th.join();
            closesocket(c->s);
        }
    }
    uint16_t port() const { return port_; }

    // ---- what it saw (the test thread) ----
    std::vector<size_t> sizes(uint8_t type) {
        std::lock_guard<std::mutex> lk(mu_);
        return sizes_[type];
    }
    std::vector<uint8_t> last(uint8_t type) {
        std::lock_guard<std::mutex> lk(mu_);
        return last_[type];
    }
    std::vector<ProxyRec> proxied() {
        std::lock_guard<std::mutex> lk(mu_);
        return proxied_;
    }
    std::vector<std::string> passphrases() {
        std::lock_guard<std::mutex> lk(mu_);
        return passes_;
    }
    int pings() { return pings_.load(); }
    int bad() { return bad_.load(); }
    int policy() {
        std::lock_guard<std::mutex> lk(mu_);
        return rooms_.empty() ? -1 : rooms_.back().policy;
    }

private:
    struct Conn {
        SOCKET s = INVALID_SOCKET;
        std::thread th;
        int idx = 0;
        std::string pass;
        int room = -1, node = -1;
        uint32_t ip = 0;
        uint8_t mac[6] = {};
    };
    struct Room {
        bool alive = true;
        std::string pass;
        uint8_t policy = 0;
        std::array<uint8_t, kInfo> info{};
        std::array<Conn*, 8> members{};
    };

    void accept_loop() {
        for (;;) {
            SOCKET c = ::accept(ls_, static_cast<sockaddr*>(nullptr), static_cast<int*>(nullptr));   // (casts: the BSD build has two accepts)
            if (c == INVALID_SOCKET || stop_) {
                if (c != INVALID_SOCKET) closesocket(c);
                return;
            }
            std::lock_guard<std::mutex> lk(mu_);
            auto conn = std::make_unique<Conn>();
            conn->s = c;
            conn->idx = static_cast<int>(conns_.size());
            const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, static_cast<uint8_t>(conn->idx + 1)};
            std::memcpy(conn->mac, mac, 6);
            Conn* p = conn.get();
            conn->th = std::thread([this, p] { serve(*p); });
            conns_.push_back(std::move(conn));
        }
    }
    static bool recv_all(SOCKET s, void* p, size_t n) {
        char* c = static_cast<char*>(p);
        while (n) {
            const int k = ::recv(s, c, static_cast<int>(n), 0);
            if (k <= 0) return false;
            c += k;
            n -= static_cast<size_t>(k);
        }
        return true;
    }
    // caller holds mu_
    void send(Conn& c, uint8_t type, const void* p = nullptr, size_t n = 0) {
        std::vector<uint8_t> f(w::kHdr + n, 0);
        wr32(&f[0], w::kMagic);
        f[4] = type;
        f[5] = 1;
        wr32(&f[8], static_cast<uint32_t>(n));
        if (n) std::memcpy(&f[w::kHdr], p, n);
        size_t off = 0;
        while (off < f.size()) {
            const int k = ::send(c.s, reinterpret_cast<const char*>(f.data() + off), static_cast<int>(f.size() - off), 0);
            if (k <= 0) return;
            off += static_cast<size_t>(k);
        }
    }
    void error(Conn& c, int32_t e) { send(c, w::NetworkError, &e, 4); }
    void sync(Room& r, Conn* except) {
        for (Conn* m : r.members)
            if (m && m != except) send(*m, w::SyncNetwork, r.info.data(), kInfo);
    }
    void put_node(Room& r, int node, Conn& c, const uint8_t* name, uint16_t version) {
        uint8_t* n = r.info.data() + w::kNodes + 0x40 * node;
        std::memset(n, 0, 0x40);
        wr32(n + w::kNodeIp, c.ip);
        std::memcpy(n + w::kNodeMac, c.mac, 6);
        n[w::kNodeId] = static_cast<uint8_t>(node);
        n[w::kNodeOn] = 1;
        std::memcpy(n + w::kNodeName, name, 32);
        wr16(n + w::kNodeVer, version);
        r.members[static_cast<size_t>(node)] = &c;
        r.info[w::kNodeCount] = static_cast<uint8_t>(r.info[w::kNodeCount] + 1);
        c.node = node;
    }
    void drop_node(Room& r, int node) {
        std::memset(r.info.data() + w::kNodes + 0x40 * node, 0, 0x40);
        r.members[static_cast<size_t>(node)] = nullptr;
        r.info[w::kNodeCount] = static_cast<uint8_t>(r.info[w::kNodeCount] - 1);
    }
    void leave(Conn& c) {
        if (c.room < 0) return;
        Room& r = rooms_[static_cast<size_t>(c.room)];
        if (c.node == 0) {   // the host: the room ends for everyone
            for (Conn* m : r.members)
                if (m && m != &c) {
                    const uint32_t ip = m->ip;
                    send(*m, w::Disconnect, &ip, 4);
                    m->room = -1;
                }
            r.alive = false;
        } else {
            drop_node(r, c.node);
            sync(r, nullptr);
        }
        c.room = -1;
        c.node = -1;
    }
    void join(Conn& c, const uint8_t* session, const uint8_t* name, uint16_t version) {
        for (size_t i = 0; i < rooms_.size(); ++i) {
            Room& r = rooms_[i];
            if (!r.alive || r.pass != c.pass || std::memcmp(r.info.data() + w::kSession, session, 16) != 0) continue;
            if (r.policy == 1) return error(c, 8);                         // ConnectRejected
            int node = -1;
            for (int k = 1; k < r.info[w::kNodeMax] && node < 0; ++k)
                if (!r.members[static_cast<size_t>(k)]) node = k;
            if (node < 0) return error(c, 2);                              // TooManyPlayers
            c.ip = (kHostIp & kMask) | static_cast<uint32_t>(node + 1);
            c.room = static_cast<int>(i);
            put_node(r, node, c, name, version);
            const uint32_t cfg[2] = {c.ip, kMask};
            send(c, w::ProxyConfig, cfg, 8);
            send(c, w::Connected, r.info.data(), kInfo);
            sync(r, &c);
            return;
        }
        error(c, 6);   // ConnectNotFound
    }
    void serve(Conn& c) {
        std::vector<uint8_t> b;
        for (;;) {
            uint8_t h[w::kHdr];
            if (!recv_all(c.s, h, sizeof h)) break;
            const int32_t size = static_cast<int32_t>(rd32(h + 8));
            if (rd32(h) != w::kMagic || h[5] != 1 || size < 0 || size > 131072) {
                ++bad_;
                break;
            }
            b.assign(static_cast<size_t>(size), 0);
            if (size && !recv_all(c.s, b.data(), b.size())) break;
            std::lock_guard<std::mutex> lk(mu_);
            sizes_[h[4]].push_back(b.size());
            last_[h[4]] = b;
            handle(c, h[4], b);
        }
        std::lock_guard<std::mutex> lk(mu_);
        leave(c);
    }
    void handle(Conn& c, uint8_t type, const std::vector<uint8_t>& b) {
        switch (type) {
            case w::Passphrase:
                c.pass.assign(reinterpret_cast<const char*>(b.data()), strnlen(reinterpret_cast<const char*>(b.data()), b.size()));
                passes_.push_back(c.pass);
                return;
            case w::Initialize: {
                uint8_t m[0x16] = {};
                m[0] = static_cast<uint8_t>(c.idx + 1);
                std::memcpy(m + 16, c.mac, 6);
                send(c, w::Initialize, m, sizeof m);
                const uint8_t ping[2] = {0, 0x5A};   // the server asks; the client must send it back
                send(c, w::Ping, ping, 2);
                return;
            }
            case w::Ping:
                if (b.size() == 2 && b[0] == 0 && b[1] == 0x5A) ++pings_;
                return;
            case w::CreateAccessPoint:
            case w::CreateAccessPointPrivate: {
                // CreateAccessPointRequest 0xBC: SecurityConfig @0, UserConfig @0x44, NetworkConfig @0x74, RyuNetworkConfig @0x94.
                // CreateAccessPointPrivateRequest 0x13C: SecurityConfig @0, SecurityParameter @0x44, UserConfig @0x64, NetworkConfig @0x94,
                // AddressList @0xB4, RyuNetworkConfig @0x114. The advertise data follows either.
                const bool priv = type == w::CreateAccessPointPrivate;
                const size_t size = priv ? 0x13C : 0xBC, user = priv ? 0x64 : 0x44, cfg = priv ? 0x94 : 0x74;
                if (b.size() < size) return void(++bad_);
                leave(c);
                Room r;
                r.pass = c.pass;
                uint8_t* i = r.info.data();
                wr64(i + w::kLcid, rd64(&b[cfg]));
                wr16(i + w::kScene, rd16(&b[cfg + 0xA]));
                const uint8_t sid[16] = {'r', 'y', 'u', 't', static_cast<uint8_t>(rooms_.size() + 1)};
                std::memcpy(i + w::kSession, priv ? &b[0x54] : sid, 16);
                if (priv) std::memcpy(i + 0x50, &b[0x44], 16);   // the security parameter's data
                std::memcpy(i + w::kBssid, c.mac, 6);
                i[w::kSsidLen] = 16;
                std::memcpy(i + w::kSsid, "ryuldn-test-room", 16);
                wr16(i + w::kChannel, 6);
                i[w::kLink] = 3;
                i[w::kType] = 2;
                wr16(i + w::kSecMode, rd16(&b[0]));
                i[w::kNodeMax] = b[cfg + 0x12];
                const size_t adv = b.size() - size;
                wr16(i + w::kAdvSize, static_cast<uint16_t>(adv));
                if (adv) std::memcpy(i + w::kAdv, &b[size], std::min<size_t>(adv, 384));
                c.ip = kHostIp;
                c.room = static_cast<int>(rooms_.size());
                rooms_.push_back(r);
                Room& rr = rooms_.back();
                put_node(rr, 0, c, &b[user], rd16(&b[cfg + 0x14]));
                const uint32_t pc[2] = {c.ip, kMask};
                send(c, w::ProxyConfig, pc, 8);
                send(c, w::Connected, rr.info.data(), kInfo);
                return;
            }
            case w::Scan: {
                if (b.size() != 0x60) return void(++bad_);
                const uint64_t lcid = rd64(&b[0]);
                for (Room& r : rooms_)
                    if (r.alive && r.pass == c.pass && (!(b[0x5C] & 1) || rd64(r.info.data() + w::kLcid) == lcid))
                        send(c, w::ScanReply, r.info.data(), kInfo);
                send(c, w::ScanReplyEnd);
                return;
            }
            case w::Connect:
                if (b.size() != 0x500) return void(++bad_);
                join(c, &b[0x80 + w::kSession], &b[0x44], static_cast<uint16_t>(rd32(&b[0x74])));
                return;
            case w::ConnectPrivate:   // ConnectPrivateRequest 0xC0: SecurityParameter @0x44 (session id @0x54), UserConfig @0x64, version @0x94
                if (b.size() != 0xC0) return void(++bad_);
                join(c, &b[0x54], &b[0x64], static_cast<uint16_t>(rd32(&b[0x94])));
                return;
            case w::SetAdvertiseData:
            case w::SetAcceptPolicy: {
                if (c.room < 0 || c.node != 0) return void(++bad_);
                Room& r = rooms_[static_cast<size_t>(c.room)];
                if (type == w::SetAcceptPolicy) {
                    if (b.size() != 1) return void(++bad_);
                    r.policy = b[0];
                    r.info[w::kPolicy] = b[0];
                } else {
                    std::memset(r.info.data() + w::kAdv, 0, 384);
                    wr16(r.info.data() + w::kAdvSize, static_cast<uint16_t>(b.size()));
                    std::memcpy(r.info.data() + w::kAdv, b.data(), std::min<size_t>(b.size(), 384));
                }
                sync(r, nullptr);
                return;
            }
            case w::Reject: {
                if (b.size() != 8 || c.room < 0 || c.node != 0) return void(++bad_);
                Room& r = rooms_[static_cast<size_t>(c.room)];
                const uint32_t node = rd32(&b[0]);
                Conn* m = node < 8 ? r.members[node] : nullptr;
                if (!m) {
                    error(c, 9);   // RejectFailed
                } else {
                    send(*m, w::Reject, b.data(), 8);
                    const uint32_t ip = m->ip;
                    send(*m, w::Disconnect, &ip, 4);
                    drop_node(r, static_cast<int>(node));
                    m->room = -1;
                    m->node = -1;
                    sync(r, nullptr);
                }
                send(c, w::RejectReply);
                return;
            }
            case w::Disconnect:
                if (b.size() != 4) ++bad_;
                leave(c);
                return;
            case w::ProxyData: {
                if (b.size() < 0x14 || c.room < 0) return void(++bad_);
                ProxyRec p{};
                p.src_ip = rd32(&b[0]);
                p.dst_ip = rd32(&b[6]);
                p.len = rd32(&b[0x10]);
                p.body = b.size();
                p.src_port = rd16(&b[4]);
                p.dst_port = rd16(&b[0xA]);
                p.proto = static_cast<int32_t>(rd32(&b[0xC]));
                proxied_.push_back(p);
                Room& r = rooms_[static_cast<size_t>(c.room)];
                const bool bcast = p.dst_ip == ((kHostIp & kMask) | ~kMask) || p.dst_ip == 0xFFFFFFFFu;
                for (Conn* m : r.members)
                    if (m && m != &c && (bcast || m->ip == p.dst_ip)) send(*m, w::ProxyData, b.data(), b.size());
                return;
            }
            default:
                return;
        }
    }

    SOCKET ls_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread acc_;
    std::mutex mu_;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::vector<Room> rooms_;
    std::map<uint8_t, std::vector<size_t>> sizes_;
    std::map<uint8_t, std::vector<uint8_t>> last_;
    std::vector<ProxyRec> proxied_;
    std::vector<std::string> passes_;
    std::atomic<int> pings_{0}, bad_{0};
};

// ---- the ldn:u objects -------------------------------------------------------------------------------------------------------------------
class LdnObject final : public nxrt::ipc::Service {
public:
    LdnObject() : Service("nn::ldn::detail::IUserLocalCommunicationService") { b_ = nxrt::services::ldn_ryu_backend(*this); }
    void on_close() override {
        if (b_) b_->shutdown();
    }

private:
    std::shared_ptr<nxrt::services::LdnRyuBackend> b_;
};

Session ldn_object() {
    Session s(nxrt::ipc::create_session_handle(std::make_shared<LdnObject>()));
    CHECK(s.call(400, Req().put<uint64_t>(0).with_pid()).result == 0);   // Initialize
    return s;
}

uint32_t state_of(const Session& s) {
    Reply r = s.call(0);
    return r.ok && r.result == 0 ? r.get<uint32_t>(0) : 0xFFFFFFFFu;
}

bool wait_for(const std::function<bool()>& f, int ms = 3000) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (!f()) {
        if (Clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

std::vector<uint8_t> raw_config(const char* name, uint8_t node_max) {   // CreateNetwork: SecurityConfig, UserConfig, 4 pad, NetworkConfig
    std::vector<uint8_t> raw(0x98, 0);
    wr16(&raw[0], 2);                                  // a debug security mode: the master must get Retail
    std::memcpy(&raw[0x44], name, std::strlen(name));
    wr64(&raw[0x78], kOwnLcid);                        // the title's own: the master must get the NACP's
    wr16(&raw[0x78 + 0xA], 7);                         // scene
    wr16(&raw[0x78 + 0x10], 11);                       // channel: the master must get 0
    raw[0x78 + 0x12] = node_max;
    wr16(&raw[0x78 + 0x14], 3);                        // local communication version
    return raw;
}

std::vector<uint8_t> network_info(const Session& s) {
    std::vector<uint8_t> ni(kInfo);
    if (s.call(1, Req().buf(B(ni.data(), ni.size()))).result != 0) ni.clear();   // GetNetworkInfo
    return ni;
}
int nodes_in(const std::vector<uint8_t>& ni) {
    int n = 0;
    for (int i = 0; i < 8 && !ni.empty(); ++i) n += ni[w::kNodes + 0x40 * i + w::kNodeOn] ? 1 : 0;
    return n;
}

// Scan for the title's networks (LocalCommunicationId -1 with the filter flag): the count and the first one.
uint32_t scan(const Session& g, std::vector<uint8_t>* first) {
    std::vector<uint8_t> f(0x60, 0);
    wr64(&f[0], kOwnLcid);
    wr32(&f[0x5C], 1);   // LocalCommunicationId
    std::vector<uint8_t> found(kInfo * 4);
    Reply r = g.call(102, Req().put<int16_t>(0).bytes("\0\0\0\0\0\0", 6).bytes(f.data(), f.size()).buf(B(found.data(), found.size())));
    if (r.result != 0) return 0xFFFFFFFFu;
    if (first) first->assign(found.begin(), found.begin() + kInfo);
    return r.get<uint32_t>(0);
}

uint32_t connect(const Session& g, const std::vector<uint8_t>& ni, const char* name) {
    std::vector<uint8_t> raw(0x7C, 0);
    wr16(&raw[0], 1);
    std::memcpy(&raw[0x44], name, std::strlen(name));
    wr32(&raw[0x74], 3);   // version
    wr32(&raw[0x78], 0);   // option
    return g.call(302, Req().bytes(raw.data(), raw.size()).buf(A(ni.data(), kInfo))).result;   // Connect
}

uint32_t join(const Session& g, const char* name) {
    std::vector<uint8_t> ni;
    if (scan(g, &ni) != 1) return 0xFFFFFFFFu;
    return connect(g, ni, name);
}

// ---- bsd:u (FreeBSD sockaddr_in as bsd carries it) ---------------------------------------------------------------------------------------
struct Sockaddr {
    uint8_t len = 16, family = 2;
    uint16_t port_be = 0;
    uint32_t ip_be = 0;
    uint8_t zero[8] = {};
};
Sockaddr sa(uint32_t ip, uint16_t port) {
    Sockaddr a;
    a.port_be = htons(port);
    a.ip_be = htonl(ip);
    return a;
}

struct Bsd {
    Session s = svctest::open("bsd:u");
    int32_t socket(int32_t type) const { return s.call(2, Req().put<int32_t>(2).put<int32_t>(type).put<int32_t>(0)).get<int32_t>(0); }
    int32_t bind(int32_t fd, uint16_t port) const {
        Sockaddr a = sa(0, port);
        return s.call(13, Req().put(fd).buf(A(&a, sizeof a))).get<int32_t>(0);
    }
    bool send(int32_t fd, uint32_t ip, uint16_t port, const std::string& m) const {
        Sockaddr a = sa(ip, port);
        Reply r = s.call(11, Req().put(fd).put<int32_t>(0).buf(A(m.data(), m.size())).buf(A(&a, sizeof a)));   // SendTo
        return r.get<int32_t>(0) == static_cast<int32_t>(m.size());
    }
    // One datagram within ms: its text and sender ("" when none came).
    std::string recv(int32_t fd, int ms, uint32_t* ip, uint16_t* port) const {
        struct {
            int32_t fd;
            int16_t events, revents;
        } p{fd, 1, 0};
        s.call(6, Req().put<uint32_t>(1).put<int32_t>(ms).buf(A(&p, sizeof p)).buf(B(&p, sizeof p)));   // Poll
        char buf[512];
        Sockaddr from;
        Reply r = s.call(9, Req().put(fd).put<int32_t>(0x80).buf(B(buf, sizeof buf)).buf(B(&from, sizeof from)));   // RecvFrom
        if (r.get<int32_t>(0) <= 0) return "";
        *ip = ntohl(from.ip_be);
        *port = ntohs(from.port_be);
        return std::string(buf, static_cast<size_t>(r.get<int32_t>(0)));
    }
    // (ret, errno) of Connect
    std::pair<int32_t, int32_t> connect(int32_t fd, uint32_t ip, uint16_t port) const {
        Sockaddr a = sa(ip, port);
        Reply r = s.call(14, Req().put(fd).buf(A(&a, sizeof a)));
        return {r.get<int32_t>(0), r.get<int32_t>(4)};
    }
    void close(int32_t fd) const { s.call(26, Req().put(fd)); }
};

// Config keys the cases change, put back at the end.
struct ConfigGuard {
    std::vector<std::pair<std::string, std::string>> keys;
    std::vector<std::string> old;
    explicit ConfigGuard(std::vector<std::pair<std::string, std::string>> k) : keys(std::move(k)) {
        for (auto& [s, n] : keys) old.push_back(nxrt::config_get_str(s.c_str(), n.c_str(), ""));
    }
    ~ConfigGuard() {
        for (size_t i = 0; i < keys.size(); ++i) nxrt::config_set(keys[i].first.c_str(), keys[i].second.c_str(), old[i].c_str());
        nxrt::services::profile_reload();
    }
};

// A control.nacp with the title's LocalCommunicationId list (0x30B0) and display version (0x3060).
std::string write_nacp() {
    std::vector<uint8_t> d(0x4000, 0);
    wr64(&d[0x30B0], kLcid);
    std::memcpy(&d[0x3060], "1.2.3", 5);
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "nxrt_ldn_ryu_test.nacp";
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (f) {
        std::fwrite(d.data(), 1, d.size(), f);
        std::fclose(f);
    }
    return p.string();
}

}  // namespace

// The framing and the config checks on their own.
SVC_TEST(ldn_ryu, wire_framing) {
    namespace ryu = vn::ryuldn;
    const uint8_t ping[2] = {0, 7};
    const std::vector<uint8_t> f = ryu::encode(ryu::Packet::Ping, ping, 2);
    const uint8_t want[] = {'R', 'L', 'D', 'N', 0xFE, 1, 0, 0, 2, 0, 0, 0, 0, 7};
    CHECK(f.size() == sizeof want && std::memcmp(f.data(), want, sizeof want) == 0);
    ryu::Header h{};
    CHECK_EQ(ryu::parse(f.data(), f.size(), &h), 14);
    CHECK_EQ(ryu::parse(f.data(), 13, &h), 0);              // not complete yet
    std::vector<uint8_t> bad = f;
    bad[5] = 2;                                             // another protocol version
    CHECK_EQ(ryu::parse(bad.data(), bad.size(), &h), -1);
    bad = f;
    bad[0] = 'X';
    CHECK_EQ(ryu::parse(bad.data(), bad.size(), &h), -1);
    bad = f;
    wr32(&bad[8], 131072 - 12);                             // header + body must stay below 131072
    CHECK_EQ(ryu::parse(bad.data(), bad.size(), &h), -1);
    CHECK(ryu::valid_passphrase(""));
    CHECK(ryu::valid_passphrase("Ryujinx-0123abcd"));
    CHECK(!ryu::valid_passphrase("Ryujinx-0123ABCD"));
    CHECK(!ryu::valid_passphrase("Ryujinx-0123abc"));
    CHECK(!ryu::valid_passphrase("secret"));
    std::string host;
    uint16_t port = 0;
    CHECK(ryu::parse_server("ldn.example.org", &host, &port) && host == "ldn.example.org" && port == 30456);
    CHECK(ryu::parse_server(" 127.0.0.1:4000 ", &host, &port) && host == "127.0.0.1" && port == 4000);
    CHECK(!ryu::parse_server("", &host, &port));
    CHECK(!ryu::parse_server("host:0", &host, &port));
    CHECK(!ryu::parse_server("host:70000", &host, &port));
}

// No master: [net] ryuldn_server unset, or nobody listening there, or a passphrase Ryujinx would not take.
SVC_TEST(ldn_ryu, without_a_master) {
    ConfigGuard g({{"net", "ryuldn_server"}, {"net", "ryuldn_passphrase"}});
    nxrt::config_set("net", "ryuldn_server", "");
    nxrt::config_set("net", "ryuldn_passphrase", "");
    {
        Session s = ldn_object();
        CHECK(s.call(200).result == 0);                                     // OpenAccessPoint
        const std::vector<uint8_t> raw = raw_config("Host", 4);
        CHECK_EQ(s.call(202, Req().bytes(raw.data(), raw.size())).result, kDeviceNotAvailable);
        CHECK(s.call(201).result == 0);                                     // CloseAccessPoint
        CHECK(s.call(300).result == 0);                                     // OpenStation
        CHECK_EQ(scan(s, nullptr), 0u);
        std::vector<uint8_t> ni(kInfo, 0);
        CHECK_EQ(connect(s, ni, "G"), kConnectNotFound);
        CHECK(s.call(401).result == 0);                                     // Finalize
        s.close();
    }
    uint16_t closed = 0;   // a port nobody listens on
    {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        SOCKET t = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(t, reinterpret_cast<sockaddr*>(&a), sizeof a);
        int len = sizeof a;
        getsockname(t, reinterpret_cast<sockaddr*>(&a), &len);
        closed = ntohs(a.sin_port);
        closesocket(t);
    }
    nxrt::config_set("net", "ryuldn_server", ("127.0.0.1:" + std::to_string(closed)).c_str());
    {
        Session s = ldn_object();
        CHECK(s.call(200).result == 0);
        const std::vector<uint8_t> raw = raw_config("Host", 4);
        CHECK_EQ(s.call(202, Req().bytes(raw.data(), raw.size())).result, kDeviceNotAvailable);
        CHECK_EQ(state_of(s), 2u);
        s.call(401);
        s.close();
    }
    nxrt::config_set("net", "ryuldn_passphrase", "secret");   // not Ryujinx's form: off, never the public rooms
    {
        Session s = ldn_object();
        CHECK(s.call(200).result == 0);
        const std::vector<uint8_t> raw = raw_config("Host", 4);
        CHECK_EQ(s.call(202, Req().bytes(raw.data(), raw.size())).result, kDeviceNotAvailable);
        s.call(401);
        s.close();
    }
}

// The whole round: host, scan, join, datagrams both ways and a broadcast through the master, a refused TCP connect, the accept policy,
// Reject, a station leaving, the host destroying the room, and a passphrase that sees other rooms.
SVC_TEST(ldn_ryu, host_and_station_through_a_master) {
    ConfigGuard g({{"net", "protocol"}, {"net", "ryuldn_server"}, {"net", "ryuldn_passphrase"}, {"fs", "nacp"}, {"title", "display_version"}});
    Master master;
    CHECK(master.port() != 0);
    const std::string nacp = write_nacp();
    nxrt::config_set("fs", "nacp", nacp.c_str());
    nxrt::config_set("title", "display_version", "1.2.3");   // PokoMac's profile takes it from [title], not from the NACP
    nxrt::services::profile_reload();
    nxrt::config_set("net", "protocol", "ryuldn");
    nxrt::config_set("net", "ryuldn_server", ("127.0.0.1:" + std::to_string(master.port())).c_str());
    nxrt::config_set("net", "ryuldn_passphrase", "");

    // ---- the host
    Session host = ldn_object();
    CHECK(host.call(200).result == 0);                                         // OpenAccessPoint
    const char adv[] = "ryuldn advertise data";
    CHECK(host.call(206, Req().buf(A(adv, sizeof adv))).result == 0);          // SetAdvertiseData
    const std::vector<uint8_t> raw = raw_config("Host", 4);
    CHECK_EQ(host.call(202, Req().bytes(raw.data(), raw.size())).result, 0u);  // CreateNetwork
    CHECK_EQ(state_of(host), 3u);
    const std::shared_ptr<vn::Link> host_link = vn::current_link();
    CHECK(host_link && host_link->local_ip() == kHostIp);
    Reply ip = host.call(2);                                                   // GetIpv4Address: the ProxyConfig's
    CHECK(ip.result == 0 && ip.get<uint32_t>(0) == kHostIp && ip.get<uint32_t>(4) == kMask);
    std::vector<uint8_t> hi = network_info(host);
    CHECK(hi.size() == kInfo && nodes_in(hi) == 1 && rd64(&hi[w::kLcid]) == kLcid);
    CHECK(!hi.empty() && std::strcmp(reinterpret_cast<const char*>(&hi[w::kNodes + w::kNodeName]), "Host") == 0);
    // what the master got: Passphrase (0x80, empty), Initialize (0x16), CreateAccessPoint (0xBC + the advertise data) with the NACP's
    // id for -1, channel 0, security mode Retail and the display version as the game version
    CHECK(master.sizes(w::Passphrase).size() == 1 && master.sizes(w::Passphrase)[0] == 0x80);
    CHECK(master.sizes(w::Initialize).size() == 1 && master.sizes(w::Initialize)[0] == 0x16);
    const std::vector<uint8_t> ca = master.last(w::CreateAccessPoint);
    CHECK_EQ(ca.size(), 0xBC + sizeof adv);
    if (ca.size() == 0xBC + sizeof adv) {
        CHECK_EQ(rd64(&ca[0x74]), kLcid);
        CHECK_EQ(rd16(&ca[0x74 + 0x10]), 0u);
        CHECK_EQ(rd16(&ca[0]), 1u);
        CHECK(std::strcmp(reinterpret_cast<const char*>(&ca[0x94]), "1.2.3") == 0);
        CHECK(std::memcmp(&ca[0xBC], adv, sizeof adv) == 0);
        CHECK(std::strcmp(reinterpret_cast<const char*>(&ca[0x44]), "Host") == 0);
    }

    // ---- the station: scan, join
    Session sta = ldn_object();
    CHECK(sta.call(300).result == 0);                                          // OpenStation
    std::vector<uint8_t> found;
    CHECK_EQ(scan(sta, &found), 1u);
    CHECK(master.last(w::Scan).size() == 0x60 && rd64(master.last(w::Scan).data()) == kLcid);
    CHECK_EQ(rd16(&found[w::kAdvSize]), sizeof adv);
    CHECK(std::memcmp(&found[w::kAdv], adv, sizeof adv) == 0);
    CHECK_EQ(connect(sta, found, "Guest"), 0u);
    CHECK_EQ(state_of(sta), 5u);
    CHECK(master.sizes(w::Connect).size() == 1 && master.sizes(w::Connect)[0] == 0x500);
    Reply sip = sta.call(2);
    CHECK(sip.result == 0 && sip.get<uint32_t>(0) == kStationIp && sip.get<uint32_t>(4) == kMask);
    CHECK_EQ(nodes_in(network_info(sta)), 2);
    CHECK(wait_for([&] { return nodes_in(network_info(host)) == 2; }));        // the master's SyncNetwork
    {
        std::vector<uint8_t> ni(kInfo), up(64);
        CHECK(host.call(101, Req().buf(B(ni.data(), ni.size())).buf(B(up.data(), up.size()))).result == 0);   // GetNetworkInfoLatestUpdate
        CHECK_EQ(up[8] & 1, 1);                                                // node 1: Connect
    }
    // the host changes its advertise data: the master syncs the room to the station
    const char adv2[] = "new advertise data, longer than the first";
    CHECK(host.call(206, Req().buf(A(adv2, sizeof adv2))).result == 0);
    CHECK(wait_for([&] {
        const std::vector<uint8_t> ni = network_info(sta);
        return !ni.empty() && rd16(&ni[w::kAdvSize]) == sizeof adv2 && std::memcmp(&ni[w::kAdv], adv2, sizeof adv2) == 0;
    }));
    CHECK(!master.sizes(w::SetAdvertiseData).empty() && master.sizes(w::SetAdvertiseData).back() == sizeof adv2);
    const std::shared_ptr<vn::Link> sta_link = vn::current_link();
    CHECK(sta_link && sta_link != host_link && sta_link->local_ip() == kStationIp);
    CHECK_EQ(master.pings(), 2);                                               // both clients sent the server's Ping back

    // ---- datagrams through the master: the station on bsd:u, the host on its own table on the host's link
    {
        Bsd bsd;
        const int32_t sfd = bsd.socket(2);
        CHECK(sfd >= 0 && bsd.bind(sfd, kPort) == 0);
        vs::Stack hs(host_link);
        const int hfd = hs.socket(vs::kAfInet, vs::kSockDgram, 0).ret;
        CHECK(hfd >= 0 && hs.bind(hfd, {0, kPort}).err == 0);
        const int32_t on = 1;
        CHECK_EQ(hs.setsockopt(hfd, vs::kSolSocket, vs::kSoBroadcast, &on, sizeof on).err, 0u);
        const int64_t tv[2] = {3, 0};
        hs.setsockopt(hfd, vs::kSolSocket, vs::kSoRcvTimeo, tv, sizeof tv);

        const std::string down = "host to station";
        vs::Addr to{kStationIp, kPort};
        CHECK_EQ(hs.sendto(hfd, down.data(), down.size(), 0, &to).ret, static_cast<int32_t>(down.size()));
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        CHECK(bsd.recv(sfd, 3000, &from_ip, &from_port) == down);
        CHECK(from_ip == kHostIp && from_port == kPort);

        const std::string upm = "station to host";
        CHECK(bsd.send(sfd, kHostIp, kPort, upm));
        char buf[256];
        vs::Addr from{};
        const vs::Ret r = hs.recvfrom(hfd, buf, sizeof buf, 0, &from);
        CHECK(r.ret == static_cast<int32_t>(upm.size()) && std::string(buf, static_cast<size_t>(r.ret > 0 ? r.ret : 0)) == upm);
        CHECK(from.ip == kStationIp && from.port == kPort);

        const std::string all = "to everyone";
        vs::Addr bc{0xFFFFFFFFu, kPort};                                       // leaves as the room's broadcast, 10.114.255.255
        CHECK_EQ(hs.sendto(hfd, all.data(), all.size(), 0, &bc).ret, static_cast<int32_t>(all.size()));
        CHECK(bsd.recv(sfd, 3000, &from_ip, &from_port) == all);

        const std::vector<ProxyRec> px = master.proxied();
        CHECK_EQ(px.size(), 3u);
        if (px.size() == 3) {
            CHECK(px[0].src_ip == kHostIp && px[0].src_port == kPort && px[0].dst_ip == kStationIp && px[0].dst_port == kPort);
            CHECK(px[1].src_ip == kStationIp && px[1].dst_ip == kHostIp && px[1].len == upm.size() && px[1].body == 0x14 + upm.size());
            CHECK(px[2].dst_ip == 0x0A72FFFFu);
            for (const ProxyRec& p : px) CHECK_EQ(p.proto, 17);
        }

        // TCP to a peer: refused at once (TCP between nodes is v2), not a 12 s SYN retry
        const int32_t tfd = bsd.socket(1);
        const auto t0 = Clock::now();
        const auto rc = bsd.connect(tfd, kHostIp, kTcpPort);
        CHECK(rc.first == -1 && rc.second == 111);                             // ECONNREFUSED
        CHECK(Clock::now() - t0 < std::chrono::seconds(1));
        bsd.close(tfd);
        bsd.close(sfd);
        hs.close(hfd);
    }

    // ---- Reject: the station is turned out with DisconnectReason Rejected
    CHECK_EQ(host.call(205, Req().put<uint32_t>(1)).result, 0u);
    CHECK(master.sizes(w::Reject).size() == 1 && master.sizes(w::Reject)[0] == 8);
    CHECK(wait_for([&] { return state_of(sta) == 4; }));
    CHECK_EQ(sta.call(3).get<int16_t>(0), 5);
    CHECK(wait_for([&] { return nodes_in(network_info(host)) == 1; }));
    CHECK(vn::current_link() != sta_link);                                     // the station's link is no longer current

    // ---- the accept policy: reject all, then accept all again
    CHECK(host.call(207, Req().put<uint8_t>(1)).result == 0);
    CHECK(wait_for([&] { return master.policy() == 1; }));
    CHECK_EQ(join(sta, "Guest"), kConnectRejected);
    CHECK_EQ(state_of(sta), 4u);
    CHECK(host.call(207, Req().put<uint8_t>(0)).result == 0);
    CHECK(wait_for([&] { return master.policy() == 0; }));

    // ---- the station joins and leaves on its own
    CHECK_EQ(join(sta, "Guest"), 0u);
    CHECK(wait_for([&] { return nodes_in(network_info(host)) == 2; }));
    CHECK(sta.call(304).result == 0);                                          // Disconnect
    CHECK_EQ(state_of(sta), 4u);
    CHECK_EQ(sta.call(3).get<int16_t>(0), 1);                                  // DisconnectedByUser
    CHECK(wait_for([&] { return nodes_in(network_info(host)) == 1; }));

    // ---- the station joins, the host destroys the room: the station lands in Station with DestroyedByUser
    CHECK_EQ(join(sta, "Guest"), 0u);
    CHECK(host.call(204).result == 0);                                         // DestroyNetwork
    CHECK_EQ(state_of(host), 2u);
    CHECK(wait_for([&] { return state_of(sta) == 4; }));
    CHECK_EQ(sta.call(3).get<int16_t>(0), 3);

    // ---- another passphrase sees other rooms: a new room is not listed for it
    CHECK_EQ(host.call(202, Req().bytes(raw.data(), raw.size())).result, 0u);
    CHECK_EQ(scan(sta, nullptr), 1u);
    nxrt::config_set("net", "ryuldn_passphrase", "Ryujinx-0123abcd");
    Session other = ldn_object();
    CHECK(other.call(300).result == 0);
    CHECK_EQ(scan(other, nullptr), 0u);
    {
        const std::vector<std::string> ps = master.passphrases();
        CHECK(!ps.empty() && ps.back() == "Ryujinx-0123abcd");
    }

    CHECK_EQ(master.bad(), 0);
    for (Session* s : {&host, &sta, &other}) {
        s->call(401);   // Finalize
        s->close();
    }
    std::error_code ec;
    std::filesystem::remove(nacp, ec);
}

// A private room: CreateNetworkPrivate with the game's SecurityParameter and AddressList, ConnectPrivate by its session id (no Scan), on a
// private passphrase.
SVC_TEST(ldn_ryu, private_room) {
    ConfigGuard g({{"net", "ryuldn_server"}, {"net", "ryuldn_passphrase"}, {"fs", "nacp"}, {"title", "display_version"}});
    Master master;
    const std::string nacp = write_nacp();
    nxrt::config_set("fs", "nacp", nacp.c_str());
    nxrt::config_set("title", "display_version", "1.2.3");   // PokoMac's profile takes it from [title], not from the NACP
    nxrt::services::profile_reload();
    nxrt::config_set("net", "ryuldn_server", ("127.0.0.1:" + std::to_string(master.port())).c_str());
    nxrt::config_set("net", "ryuldn_passphrase", "Ryujinx-0a1b2c3d");
    uint8_t sp[0x20];   // SecurityParameter: 16 B data, 16 B session id
    for (int i = 0; i < 16; ++i) {
        sp[i] = static_cast<uint8_t>(0x10 + i);
        sp[16 + i] = static_cast<uint8_t>(0xC0 + i);
    }
    uint8_t addr[0x60];   // AddressList: 8 x (u32 ip, 6 B MAC, u16)
    for (int i = 0; i < 0x60; ++i) addr[i] = static_cast<uint8_t>(i ^ 0x5A);

    Session host = ldn_object();
    CHECK(host.call(200).result == 0);
    std::vector<uint8_t> raw(0xB8, 0);   // SecurityConfig, SecurityParameter @0x44, UserConfig @0x64, 4 pad, NetworkConfig @0x98
    wr16(&raw[0], 1);
    std::memcpy(&raw[0x44], sp, sizeof sp);
    std::memcpy(&raw[0x64], "PrivHost", 8);
    wr64(&raw[0x98], kOwnLcid);
    raw[0x98 + 0x12] = 2;
    wr16(&raw[0x98 + 0x14], 3);
    CHECK_EQ(host.call(203, Req().bytes(raw.data(), raw.size()).buf(X(addr, sizeof addr))).result, 0u);   // CreateNetworkPrivate
    CHECK_EQ(state_of(host), 3u);
    const std::vector<uint8_t> cp = master.last(w::CreateAccessPointPrivate);
    CHECK_EQ(cp.size(), 0x13Cu);
    if (cp.size() == 0x13C) {
        CHECK(std::memcmp(&cp[0x44], sp, sizeof sp) == 0);
        CHECK(std::strcmp(reinterpret_cast<const char*>(&cp[0x64]), "PrivHost") == 0);
        CHECK_EQ(rd64(&cp[0x94]), kLcid);
        CHECK(std::memcmp(&cp[0xB4], addr, sizeof addr) == 0);
        CHECK(std::strcmp(reinterpret_cast<const char*>(&cp[0x114]), "1.2.3") == 0);
    }
    CHECK(master.passphrases().size() == 1 && master.passphrases()[0] == "Ryujinx-0a1b2c3d");
    Reply sec = host.call(4);   // GetSecurityParameter: the game's
    CHECK(sec.result == 0 && sec.data.size() >= 0x20 && std::memcmp(sec.data.data(), sp, sizeof sp) == 0);
    Reply cfg = host.call(5);   // GetNetworkConfig
    CHECK(cfg.result == 0 && cfg.get<uint64_t>(0) == kLcid && cfg.get<uint8_t>(0x12) == 2);

    Session sta = ldn_object();
    CHECK(sta.call(300).result == 0);
    std::vector<uint8_t> cr(0xC0, 0);   // SecurityConfig, SecurityParameter @0x44, UserConfig @0x64, version @0x94, option @0x98, NetworkConfig @0xA0
    wr16(&cr[0], 1);
    std::memcpy(&cr[0x44], sp, sizeof sp);
    std::memcpy(&cr[0x64], "PrivGuest", 9);
    wr32(&cr[0x94], 3);
    wr64(&cr[0xA0], kOwnLcid);
    CHECK_EQ(sta.call(303, Req().bytes(cr.data(), cr.size())).result, 0u);   // ConnectPrivate
    CHECK_EQ(state_of(sta), 5u);
    const std::vector<uint8_t> pc = master.last(w::ConnectPrivate);
    CHECK(pc.size() == 0xC0 && rd64(&pc[0xA0]) == kLcid && std::memcmp(&pc[0x44], sp, sizeof sp) == 0);
    Reply sip = sta.call(2);
    CHECK(sip.result == 0 && sip.get<uint32_t>(0) == kStationIp);
    CHECK(wait_for([&] { return nodes_in(network_info(host)) == 2; }));
    CHECK_EQ(master.bad(), 0);
    for (Session* s : {&host, &sta}) {
        s->call(401);
        s->close();
    }
    std::error_code ec;
    std::filesystem::remove(nacp, ec);
}
